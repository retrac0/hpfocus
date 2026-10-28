-- license:BSD-3-Clause
-- copyright-holders:Joel Heikkila
--
-- Regression harness for the hp9k520 / hp9k540 drivers, run as MAME's
-- -autoboot_script. tests/run_tests.py writes a small wrapper per test
-- that loads this file and calls H.run with the test's steps.
--
-- A step is a table; the first matching key decides what it does:
--
--   { wait = "text", on = SOURCE, within = s }   until a visible line
--                                                contains the text
--   { line = "text", on = SOURCE, within = s }   until a visible line,
--                                                trimmed, IS the text
--   { sleep = s }                                let emulated time pass
--   { post = "keys", to = TAG }                  type through the natural
--                                                keyboard; TAG picks the
--                                                keyboard device (else all)
--   { posted = true }                            until the typing is done
--   { type = "keys", rate = s, to = TAG }         post a character at a time
--   { show = SOURCE }                            print what a source shows
--   { absent = TEXT, on = SOURCE }               fail if any line holds TEXT
--   { press = "NAME" }                           hold the key whose input
--                                                field is NAME, for a key
--                                                with no character (STORE)
--   { insert = PATH, into = TAG }                put a disc in a drive while
--                                                the machine runs
--   { reset = SPEC }                             load an ISA case at a reset
--   { setup = SPEC }                             load one into a running CPU
--   { expect = { [addr] = word } }               compare words, see H.cpu_check
--
-- SOURCE is "screen" for the 520's display, or the tag of a serial
-- terminal device for a 540 console or multiplexer port. Results are
-- printed as one "HARNESS PASS name" or "HARNESS FAIL name: why" line,
-- followed on failure by what the source showed.

local H = {}

local function now()
	return manager.machine.time:as_double()
end

local function trim(s)
	return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- The 520's display: the frame buffer through the CRTC's display start
-- and row count, the way the hardware scans it out.
function H.screen_lines()
	local dim = manager.machine.devices[":dim"]
	local vram = manager.machine.memory.shares[":dim:vram"]
	local crtc = emu.item(dim.items["0/crtc"])
	local start = ((crtc:read(12) << 8) | crtc:read(13)) & 4095
	local rows = crtc:read(6)
	if rows == 0 or rows > 26 then rows = 26 end
	local lines = {}
	for r = 0, rows - 1 do
		local t = {}
		for c = 0, 79 do
			local ch = vram:read_u16(((start + r * 80 + c) & 4095) * 2) & 0xff
			t[#t + 1] = (ch >= 32 and ch < 127) and string.char(ch) or " "
		end
		lines[#lines + 1] = table.concat(t)
	end
	return lines
end

-- A serial terminal's text buffer, 80 x 24.
function H.terminal_lines(tag)
	local dev = manager.machine.devices[tag]
	local buf = emu.item(dev.items["0/m_buffer"])
	local raw = buf:read_block(0, buf.count)
	local lines = {}
	for r = 0, (#raw // 80) - 1 do
		lines[#lines + 1] = raw:sub(r * 80 + 1, r * 80 + 80):gsub("[^\32-\126]", " ")
	end
	return lines
end

local function lines_of(source)
	if source == "screen" then
		return H.screen_lines()
	end
	return H.terminal_lines(source)
end

local function input_field(name)
	for _, port in pairs(manager.machine.ioport.ports) do
		local f = port.fields[name]
		if f then return f end
	end
end

local function select_keyboard(tag)
	for _, kbd in pairs(manager.machine.natkeyboard.keyboards) do
		kbd.enabled = (tag == nil) or (kbd.tag:sub(1, #tag) == tag)
	end
end

-- An ISA check: load words into the CPU's space, set registers at the next
-- reset, run, and compare words afterwards. The CPU stops when it runs off
-- the end of the code, which is the end of the test.
--   { load = { [addr] = word, ... }, regs = { P = ..., ... },
--     run = seconds, expect = { [addr] = word, ... } }
function H.cpu_check(spec)
	local cpu = manager.machine.devices[":cpu"]
	local space = cpu.spaces["program"]
	for addr, word in pairs(spec.load) do
		space:write_u32(addr, word)
	end
	for name, value in pairs(spec.regs) do
		cpu.state[name].value = value
	end
end

function H.run(name, steps, limit)
	local i = 1
	local mark = now()
	local seen = {}
	local done = false
	local sub

	local function finish(ok, why, source)
		if done then return end
		done = true
		if ok then
			print("HARNESS PASS " .. name)
		else
			print("HARNESS FAIL " .. name .. ": " .. why)
			if source then
				local ok2, lines = pcall(lines_of, source)
				if ok2 then
					for _, l in ipairs(lines) do
						if trim(l) ~= "" then print("  | " .. l) end
					end
				end
			end
		end
		manager.machine:exit()
	end

	local function advance()
		i = i + 1
		mark = now()
		if i > #steps then finish(true) end
	end

	sub = emu.add_machine_frame_notifier(function()
		if done then return end
		local t = now()
		local s = steps[i]
		if t > limit then
			finish(false, string.format("out of time at step %d", i), s and s.on)
			return
		end
		if s.wait then
			for _, l in ipairs(lines_of(s.on)) do
				if l:find(s.wait, 1, true) then advance() return end
			end
			if s.within and t - mark > s.within then
				finish(false, string.format("no '%s' within %ds", s.wait, s.within), s.on)
			end
		elseif s.line then
			for _, l in ipairs(lines_of(s.on)) do
				if trim(l) == s.line then advance() return end
			end
			if (s.within or 0) <= t - mark then
				finish(false, string.format("no line '%s'", s.line), s.on)
			end
		elseif s.absent then
			for _, l in ipairs(lines_of(s.on)) do
				if l:find(s.absent, 1, true) then
					finish(false, string.format("'%s' on %s", s.absent, s.on or "screen"), s.on)
					return
				end
			end
			advance()
		elseif s.sleep then
			if t - mark >= s.sleep then advance() end
		elseif s.post then
			select_keyboard(s.to)
			manager.machine.natkeyboard:post(s.post)
			advance()
		elseif s.type then
			-- One character at a time, for input a program reads slower
			-- than the natural keyboard types.
			s.at = s.at or 1
			s.next = s.next or t
			if t >= s.next then
				if s.at == 1 then select_keyboard(s.to) end
				manager.machine.natkeyboard:post(s.type:sub(s.at, s.at))
				s.at = s.at + 1
				s.next = t + (s.rate or 0.25)
				if s.at > #s.type then advance() end
			end
		elseif s.show then
			print("-- " .. s.show .. " at " .. string.format("%.1f", t))
			for _, l in ipairs(lines_of(s.show)) do
				if trim(l) ~= "" then print("  | " .. l) end
			end
			advance()
		elseif s.posted then
			if manager.machine.natkeyboard.empty then advance() end
		elseif s.insert then
			local err = manager.machine.images[s.into]:load(s.insert)
			if err then
				finish(false, "cannot insert " .. s.insert .. ": " .. tostring(err))
			else
				advance()
			end
		elseif s.press then
			local f = input_field(s.press)
			if not f then
				finish(false, "no key " .. s.press)
			elseif not s.down then
				f:set_value(1)
				s.down = t
			elseif t - s.down >= 0.1 then
				f:clear_value()
				advance()
			end
		elseif s.expect then
			local space = manager.machine.devices[":cpu"].spaces["program"]
			for addr, want in pairs(s.expect) do
				local got = space:read_u32(addr)
				if got ~= want then
					local cpu = manager.machine.devices[":cpu"]
					local regs = {}
					for _, r in ipairs({ "P", "PB", "PL", "S", "Q", "DB", "STATUS", "CIR" }) do
						regs[#regs + 1] = string.format("%s=%x", r, cpu.state[r].value)
					end
					finish(false, string.format("%s: word %06x is %d, want %d (%s)", s.what or "cpu", addr, got, want, table.concat(regs, " ")))
					return
				end
			end
			advance()
		elseif s.setup then
			H.cpu_check(s.setup)
			advance()
		elseif s.reset then
			-- Set up the next ISA case at the reset it triggers, after the
			-- devices have reset and the loader has had its turn.
			local spec = s.reset
			local once = true
			H.reset_sub = emu.add_machine_reset_notifier(function()
				if once then
					once = false
					H.cpu_check(spec)
				end
			end)
			manager.machine:soft_reset()
			advance()
		else
			finish(false, "unknown step " .. i)
		end
	end)
	H.sub = sub
end

return H
