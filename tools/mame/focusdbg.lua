-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Joel Heikkila
--
-- focusdbg.lua -- headless debugger harness for the FOCUS driver.
--
-- Run with:  focus hp9020 -quik <image> -debug -debugger none \
--                -autoboot_script tools/mame/focusdbg.lua \
--                -video none -sound none -nothrottle
--
-- It drives MAME's own debugger from Lua through debug:command(), which
-- means the whole command set -- bpset, wpset, trace, memory dumps -- is
-- available while the machine runs freely (the "none" backend does not
-- block on a stop, so a Lua periodic hook does the polling instead).
--
-- What to trace is read from the environment: FOCUSDBG_CMDS is a
-- semicolon-separated list of debugger commands run once at startup, and
-- FOCUSDBG_RUN caps the run in emulated seconds.

local mac  = manager.machine
local dbg  = mac.debugger
local cpu  = mac.devices[":cpu"]

local function log(s) print("[focusdbg] " .. s) end

-- Startup commands.
-- Commands come from a file (FOCUSDBG_FILE), one per line, so command
-- bodies may contain semicolons and braces freely.
local cmdfile = os.getenv("FOCUSDBG_FILE")
if cmdfile then
	for line in io.lines(cmdfile) do
		if line ~= "" and not line:match("^%s*%-%-") then
			log("cmd: " .. line)
			dbg:command(line)
		end
	end
end
-- Let it run: the driver's quickload already handed off, so just go.
dbg:command("go")

-- Drain the debugger console log to stdout so command output (printf,
-- memory dumps, trace echoes) is visible in a headless run.
local clog = dbg.consolelog
local seen = 0
local function drain()
	local n = #clog
	while seen < n do
		seen = seen + 1
		print("[dbg] " .. clog[seen])
	end
end

-- Time cap.
local runsecs = tonumber(os.getenv("FOCUSDBG_RUN") or "8")
emu.register_periodic(function()
	drain()
	if mac.time:as_double() >= runsecs then
		drain()
		log(string.format("stop at %.2fs, PC=%08X",
			mac.time:as_double(), cpu.state["P"].value))
		mac:exit()
	end
end)
log("armed, run cap " .. runsecs .. "s")
