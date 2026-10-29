-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Joel Heikkila
--
-- stopfile.lua -- stop the emulator the clean way when a file appears.
--
-- tools/runhpux.sh runs MAME with  -autoboot_script tools/mame/stopfile.lua
-- and HPFOCUS_STOPFILE=<path> in the environment. When that path exists
-- the script removes it and asks MAME to exit, exactly as closing the
-- window or reaching -seconds_to_run does: every exit notifier runs, so
-- the disc and the battery are written as a clean exit writes them.
--
-- Why this exists: a headless run (-video none) pumps no SDL events, and
-- MAME's SDL front end turns SIGINT and SIGTERM into a quit EVENT, so
-- before the alpha ^C and kill did nothing to a headless run, and kill -9
-- lost every write the run had made (the image was written back only at
-- exit). runhpux.sh traps the signal itself and creates this file instead.
-- A headless MAME still takes no notice of SIGINT or SIGTERM, so this is
-- the clean stop; for a ^C that reaches a windowed MAME as well it is
-- redundant but never wrong: two requests to exit are one exit. MAME goes on calling the periodic hook while the emulation is
-- paused, so a paused machine is stopped too (measured: emu.pause() from a
-- script, then SIGTERM to runhpux.sh alone).

local path = os.getenv("HPFOCUS_STOPFILE")
if path == nil or path == "" then
	return
end

local calls = 0
emu.register_periodic(function()
	-- Called once per emulated frame, which unthrottled is hundreds of
	-- times a second; looking for the file every 16th call is plenty.
	calls = calls + 1
	if calls % 16 ~= 0 then
		return
	end
	local f = io.open(path, "r")
	if f ~= nil then
		f:close()
		os.remove(path)
		print("[stopfile] stop requested; exiting cleanly")
		manager.machine:exit()
	end
end)
