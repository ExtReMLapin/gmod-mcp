-- gmod-mcp bootstrap. Ships as garrysmod/lua/autorun/server/mcpagent_boot.lua (the release zip already does).
-- Loads the gmod-mcp module on the server state: on dedicated servers at every map, and in singleplayer/listen games
-- on the first map after installation, after which the module installs its own loader into the main menu.
-- Does nothing if the module is missing; the module itself stays inactive unless the game runs with -mcp.
pcall(function()
	if file.Exists("mcpagent/load_in_progress.txt", "DATA") then
		MsgN("[mcpagent] not loaded: the previous load did not finish (crash?). Delete garrysmod/data/mcpagent/load_in_progress.txt to retry.")
		return
	end
	local suffix = (system.IsWindows() and "win" or system.IsLinux() and "linux" or "osx") .. (jit.arch == "x64" and "64" or (system.IsWindows() and "32" or ""))
	local path = "lua/bin/gmsv_mcpagent_" .. suffix .. ".dll"
	local f = file.Open(path, "rb", "GAME")
	if not f then return end
	-- Quick sanity check: avoids the blocking Windows "bad image" dialog for an obviously wrong or empty file.
	local head, size = f:Read(4) or "", f:Size()
	f:Close()
	if size < 65536 or (string.sub(head, 1, 2) ~= "MZ" and head ~= "\127ELF") then
		MsgN("[mcpagent] not loaded: ", path, " is not a valid module (corrupted download?)")
		return
	end
	-- Release builds: the CI replaces the placeholder below with the CRC32 of the DLL shipped in the same zip, which
	-- catches truncated or mismatched files. Local builds keep the placeholder and skip this check.
	local expected = "@MCPAGENT_DLL_CRC@"
	if string.sub(expected, 1, 1) ~= "@" and util and util.CRC then
		local crc = util.CRC(file.Read(path, "GAME") or "")
		if crc ~= expected then
			MsgN("[mcpagent] not loaded: ", path, " does not match this release (checksum ", crc, ", expected ", expected, "). Re-extract the release zip.")
			return
		end
	end
	local ok, err = pcall(require, "mcpagent")
	if not ok then MsgN("[mcpagent] failed to load: ", err) end
end)
