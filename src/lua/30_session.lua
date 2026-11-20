-- gmod-mcp: session (maps, disconnect), wait conditions, console, convars, errors.
local A = __mcpagent
local J = A.json
local H = A.handlers
local native = A.native

-- mcpagent_setup: prints the ready-to-paste client configuration (host state only). On a dedicated server it answers
-- the server console only, never a connected player (the output contains the token).
if native.setup_info and concommand and concommand.Add then
	concommand.Add("mcpagent_setup", function(ply)
		if IsValid(ply) then return end
		MsgN(native.setup_info())
	end, nil, "Print the command that connects Claude Code (or another MCP client) to this game")
end
if native.uninstall and concommand and concommand.Add then
	concommand.Add("mcpagent_uninstall", function(ply)
		if IsValid(ply) then return end
		MsgN(native.uninstall())
	end, nil, "Remove the gmod-mcp loader from lua/menu/menu.lua")
end

-- Runs a console command from the current realm, using the right API.
local function run_cmd(parts)
	if A.realm == "server" then
		local line = {}
		for i, p in ipairs(parts) do line[i] = (string.find(p, "[%s;\"]") and ('"' .. p .. '"') or p) end
		game.ConsoleCommand(table.concat(line, " ") .. "\n")
	else
		RunConsoleCommand(unpack(parts))
	end
end
A.run_cmd = run_cmd

-- Splits a command line, honoring quotes.
local function split_cmd(s)
	local out, cur, inq, any = {}, {}, false, false
	for i = 1, #s do
		local c = string.sub(s, i, i)
		if c == '"' then
			inq = not inq
			any = true
		elseif not inq and string.find(c, "%s") then
			if any or #cur > 0 then out[#out + 1] = table.concat(cur) end
			cur, any = {}, false
		else
			cur[#cur + 1] = c
		end
	end
	if any or #cur > 0 then out[#out + 1] = table.concat(cur) end
	return out
end
A.split_cmd = split_cmd

-- Maps (menu or server).
function H.map_list(args)
	local filter = string.lower(args.filter or "")
	local files = file.Find("maps/*.bsp", "GAME")
	local out = J.array({})
	local limit = args.limit or 1000
	local total = 0
	table.sort(files)
	for _, f in ipairs(files) do
		local name = string.StripExtension and string.StripExtension(f) or string.gsub(f, "%.bsp$", "")
		if filter == "" or string.find(string.lower(name), filter, 1, true) then
			total = total + 1
			if #out < limit then out[#out + 1] = name end
		end
	end
	return { maps = out, count = total, truncated = total > #out, filter = args.filter or "" }
end

function H.map_exists(args)
	local map = args.map
	if type(map) ~= "string" or map == "" or string.find(map, "[^%w_%-%.]") then
		A.fail("invalid_params", "invalid map name: " .. tostring(map))
	end
	return { exists = file.Exists("maps/" .. map .. ".bsp", "GAME") }
end

function H.map_start(args)
	if A.realm ~= "menu" and A.realm ~= "server" then A.fail("wrong_realm", "map_start: requires the menu or server realm") end
	-- maxplayers cannot change while a server is running: the DLL disconnects first (separate step).
	if args.maxplayers then run_cmd({ "maxplayers", tostring(args.maxplayers) }) end
	if args.gamemode then run_cmd({ "gamemode", args.gamemode }) end
	if A.realm == "server" then
		run_cmd({ "changelevel", args.map })
	else
		run_cmd({ "map", args.map })
	end
	return { started = true }
end

-- Has the menu finished loading (loading screen torn down)?
function H.menu_idle(args)
	return { loading = (IsInLoading and IsInLoading()) or false, in_game = (IsInGame and IsInGame()) or false }
end

function H.disconnect(args)
	if A.realm ~= "menu" then A.fail("wrong_realm", "disconnect: requires the menu realm") end
	RunConsoleCommand("disconnect")
	return { requested = true }
end

-- Test clients started by the bridge (instances_launch passes -mcp_test_client; native.test_client) run with archived
-- convars set on their command line (fps_max...). A clean quit makes the engine write every archived convar into
-- cfg/config.cfg, which the user's own game shares. Before such a client quits, each one gets back the value the
-- user's game has in config.cfg at that moment (the user may have changed it since the launch), or the convar's
-- default when config.cfg has no line for it.
A.test_client_convars = { "fps_max", "fps_max_nofocus", "engine_no_focus_sleep", "snd_mute_losefocus" }

-- Issue #10: instances_launch {fps_max, commands, launch_args} can change other archived convars. The bridge lists their
-- names on the command line (-mcp_restore_convars a,b: native.restore_convars, a comma-separated string) and they are
-- given back like the built-in ones. Built-in names first, then the extra ones (lower case, once each, plain names only).
function A.test_client_convar_names()
	local out, seen = {}, {}
	for _, n in ipairs(A.test_client_convars) do out[#out + 1] = n seen[n] = true end
	local extra = native.restore_convars
	if type(extra) == "string" then
		for n in string.gmatch(extra, "[^,]+") do
			n = string.lower(n)
			if string.match(n, "^[%w_%.]+$") and #n <= 64 and not seen[n] then out[#out + 1] = n seen[n] = true end
		end
	end
	return out
end

-- name -> value for the `name "value"` (or `name value`) lines of a config.cfg text; the last line of a name wins, as
-- when the engine executes the file. Names are lower case. Binds and aliases are not convars.
local NOT_CONVARS = { bind = true, unbind = true, alias = true, unbindall = true }
function A.parse_config_values(text)
	local out = {}
	if type(text) ~= "string" then return out end
	for line in string.gmatch(text, "[^\r\n]+") do
		local name, rest = string.match(line, "^%s*([%w_%.]+)%s+(.-)%s*$")
		if name and not NOT_CONVARS[string.lower(name)] then
			local v = string.match(rest, '^"([^"]*)"') or string.match(rest, "^(%S+)")
			if v then out[string.lower(name)] = v end
		end
	end
	return out
end

-- A value that cannot end the command or start another one.
local function safe_value(v) return type(v) == "string" and v ~= "" and not string.find(v, '[";\r\n]') end

-- The changes to make: {{name, value}, ...}. `get(name)` returns {current, default} or nil (unknown convar).
function A.plan_convar_restore(names, cfg_text, get)
	local cfg, plan = A.parse_config_values(cfg_text), {}
	for _, name in ipairs(names) do
		local info = get(name)
		if info then
			local want = cfg[name]
			if not safe_value(want) then want = info.default end
			if safe_value(want) and tostring(info.current) ~= want and tonumber(info.current) ~= tonumber(want) then
				plan[#plan + 1] = { name = name, value = want }
			end
		end
	end
	return plan
end

-- Queues the changes (they run before the `quit` that follows: the console buffer is ordered). Number of changes.
function A.restore_test_client_convars()
	if not native.test_client then return 0 end
	-- Read natively: the Lua file library answers nil for cfg/config.cfg (live test 2026-11-15).
	local text = native.user_config and native.user_config() or ""
	local builtin = {}
	for _, n in ipairs(A.test_client_convars) do builtin[n] = true end
	local plan = A.plan_convar_restore(A.test_client_convar_names(), text, function(name)
		local ok, cv = pcall(A.convar, name)
		if not ok or not cv then return nil end
		-- A convar the bridge listed (Issue #10) is restored only if archived: only those reach config.cfg. The built-in
		-- ones always are (as before); GetFlags may be missing on some builds: then the convar is kept.
		if not builtin[name] then
			local fok, flags = pcall(function() return cv.GetFlags and cv:GetFlags() end)
			if fok and type(flags) == "number" and bit.band(flags, 128) == 0 then return nil end
		end
		return { current = cv:GetString(), default = cv:GetDefault() }
	end)
	for _, c in ipairs(plan) do
		if native.engine_cmd then native.engine_cmd(c.name .. ' "' .. c.value .. '"') else RunConsoleCommand(c.name, c.value) end
	end
	return #plan
end

-- True when a console line is a clean quit of the game.
local function is_quit(line)
	local w = string.lower(string.match(line or "", "^%s*(%S+)") or "")
	return w == "quit" or w == "exit"
end

-- The main menu's Quit button calls RunGameUICommand("quit"), which does not pass through the console buffer: on a test
-- client the call is wrapped so that it restores first and then sends the quit through the same ordered path as game_quit.
function A.wrap_menu_quit()
	if not native.test_client or not native.engine_cmd or not RunGameUICommand or A.quit_wrapped then return false end
	local orig = RunGameUICommand
	RunGameUICommand = function(cmd, ...)
		if string.lower(tostring(cmd)) == "quit" then
			A.restore_test_client_convars()
			if native.engine_cmd("quit") then return end
		end
		return orig(cmd, ...)
	end
	A.quit_wrapped = true
	return true
end
if A.realm == "menu" then A.wrap_menu_quit() end

function H.restore_convars(args) return { restored = A.restore_test_client_convars() } end

function H.quit(args)
	local restored = A.restore_test_client_convars()
	if A.realm == "menu" then
		if native.engine_cmd and native.engine_cmd("quit") then return { requested = true, via = "engine", restored = restored } end
		RunGameUICommand("quit")
		return { requested = true, via = "gameui", restored = restored }
	end
	-- srcds: the engine's own blacklist blocks `quit` from server Lua, so game.ConsoleCommand does nothing and the answer would be
	-- a lie. The module then ends the process itself (SIGTERM: srcds's own clean shutdown); without that, an error.
	if IsConCommandBlocked and IsConCommandBlocked("quit") then
		if native.request_quit and native.request_quit(500) then return { requested = true, via = "signal", restored = restored } end
		A.fail("blocked", "the console command quit is blocked from server Lua and this platform has no other clean way to end a dedicated server: stop it from its own console or with the system's tools (game_terminate ends the process at once, without a clean shutdown)")
	end
	game.ConsoleCommand("quit\n")
	return { requested = true, via = "server", restored = restored }
end

-- Condition for wait_for: an expression or a block with return.
local cond_cache = {}
function H.eval_cond(args)
	local code = args.code
	local fn = cond_cache[code]
	if not fn then
		local f, err = A.compile("return (" .. code .. ")", "=mcpagent:wait_for")
		if not f then f, err = A.compile(code, "=mcpagent:wait_for") end
		if not f then A.fail("lua_syntax_error", tostring(err)) end
		fn = f
		cond_cache[code] = f
	end
	local ok, res, err, tb = A.run_budget(fn, args.budget or 1)
	if not ok then A.fail("lua_error", tostring(err), { traceback = tb }) end
	local v = res[1]
	return { truthy = v ~= nil and v ~= false, value = A.serialize(v, { depth = 2, limit = 20 }) }
end

-- Console.
-- Returns {blocked=true} without running anything if the command is on the realm's Lua blacklist;
-- the DLL then reroutes it to the engine (IVEngineClient::ClientCmd).
function H.console_exec(args)
	local parts = split_cmd(args.cmd or "")
	if #parts == 0 then A.fail("invalid_params", "empty command") end
	if is_quit(parts[1]) then A.restore_test_client_convars() end
	if IsConCommandBlocked and IsConCommandBlocked(parts[1]) then return { blocked = true, sent = parts[1] } end
	-- Menu RunConsoleCommand("connect", "ip:port") fails with "Bad server address": use the engine path instead
	-- (the DLL has already checked that the target is loopback).
	if A.realm == "menu" and string.lower(parts[1]) == "connect" then return { blocked = true, sent = parts[1] } end
	run_cmd(parts)
	return { sent = parts[1], args = #parts - 1 }
end

function H.engine_cmd(args)
	if not native.engine_cmd then A.fail("unsupported", "engine command unavailable in this realm/mode") end
	if is_quit(args.cmd) then A.restore_test_client_convars() end
	local ok, err = native.engine_cmd(args.cmd)
	if not ok then A.fail("engine_error", tostring(err)) end
	return { sent = args.cmd }
end

-- Convars.
local FLAGS = {
	[1] = "UNREGISTERED", [2] = "DEVELOPMENTONLY", [4] = "GAMEDLL", [8] = "CLIENTDLL", [16] = "HIDDEN", [32] = "PROTECTED",
	[64] = "SPONLY", [128] = "ARCHIVE", [256] = "NOTIFY", [512] = "USERINFO", [1024] = "PRINTABLEONLY", [2048] = "UNLOGGED",
	[4096] = "NEVER_AS_STRING", [8192] = "REPLICATED", [16384] = "CHEAT", [65536] = "DEMO", [131072] = "DONTRECORD",
	[262144] = "LUA_CLIENT", [524288] = "LUA_SERVER", [1048576] = "NOT_CONNECTED", [2097152] = "ARCHIVE_XBOX",
	[268435456] = "SERVER_CAN_EXECUTE", [536870912] = "SERVER_CANNOT_QUERY", [1073741824] = "CLIENTCMD_CAN_EXECUTE",
}
local function decode_flags(f)
	local out = J.array({})
	for bit_, name in pairs(FLAGS) do if bit.band(f, bit_) ~= 0 then out[#out + 1] = name end end
	table.sort(out)
	return out
end

function H.convar_get(args)
	local name = args.name
	if not ConVarExists or not ConVarExists(name) then A.fail("not_found", "unknown convar: " .. tostring(name)) end
	local cv = A.convar(name)
	if not cv then
		-- Exists for the engine but inaccessible from Lua (FCVAR_PROTECTED, rcon_password...) or a plain command.
		A.fail("protected", "convar protected or inaccessible from Lua (or a command, not a convar): " .. tostring(name))
	end
	local flags = cv.GetFlags and cv:GetFlags() or 0
	local o = { name = cv:GetName(), flags = flags, flag_names = decode_flags(flags), help = cv:GetHelpText(), default = cv:GetDefault() }
	if bit.band(flags, 32) ~= 0 then
		o.value = J.null
		o.protected = true
	else
		o.value = cv:GetString()
		-- The same number can be spelled "0.0" or "0.000000": compare `number`, not the string.
		o.number = tonumber(o.value)
	end
	if cv.GetMin then o.min = cv:GetMin() end
	if cv.GetMax then o.max = cv:GetMax() end
	return o
end

function H.convar_set(args)
	local name = args.name
	if not ConVarExists(name) then A.fail("not_found", "unknown convar: " .. tostring(name)) end
	local cv = A.convar(name)
	if not cv or bit.band(cv:GetFlags(), 32) ~= 0 then A.fail("protected", "convar protected or inaccessible from Lua: " .. name) end
	run_cmd({ name, tostring(args.value) })
	return { requested = true, previous = cv and cv:GetString() or J.null }
end

function H.concommand_list(args)
	local filter = string.lower(args.filter or "")
	local out = J.array({})
	local cmds = concommand and concommand.GetTable and concommand.GetTable() or {}
	for name in pairs(cmds) do
		if filter == "" or string.find(string.lower(name), filter, 1, true) then out[#out + 1] = name end
	end
	table.sort(out)
	return { lua_commands = out }
end

-- Lua errors from all realms, as seen from the menu state.
if A.realm == "menu" then
	hook.Add("OnLuaError", "__mcpagent", function(err, realm, stack, name, id)
		local st = J.array({})
		if type(stack) == "table" then
			for i, fr in ipairs(stack) do
				st[i] = { file = fr.File or fr.short_src or fr.file, line = fr.Line or fr.currentline or fr.line, ["function"] = fr.Function or fr.name }
			end
		end
		local e = { realm = realm, message = tostring(err), stack = st, addon = name, addon_id = id }
		if native.push_error then native.push_error(J.encode(e)) end
		if native.event then native.event("lua_error", J.encode({ realm = realm, message = tostring(err) })) end
	end)
end
