-- gmod-mcp: code execution, inspection, per-realm status.
local A = __mcpagent
local J = A.json
local H = A.handlers

function H.lua_exec(args)
	local code = args.code
	if type(code) ~= "string" then A.fail("invalid_params", "code: expected a string") end
	local chunk = args.chunkname or ("=mcpagent:" .. A.realm)
	local fn, cerr = A.compile(code, chunk)
	if not fn then A.fail("lua_syntax_error", tostring(cerr), { chunkname = chunk }) end
	local opts = { depth = args.depth or 4, limit = args.limit or 200 }
	A.capture_begin()
	local t0 = SysTime()
	local ok, res, err, tb, exceeded = A.run_budget(fn, args.budget or 5)
	local dt = (SysTime() - t0) * 1000
	local output = A.capture_end()
	if not ok then
		if A.is_fail(err) then A.fail(err.code, err.message, err.details) end
		A.fail(exceeded and "budget_exceeded" or "lua_error", tostring(err),
			{ traceback = tb, output = output, duration_ms = dt })
	end
	local values = J.array({})
	for i = 1, res.n do values[i] = A.serialize(res[i], opts) end
	return { values = values, n = res.n, output = output, duration_ms = dt }
end

function H.lua_inspect(args)
	local expr = args.expr
	if type(expr) ~= "string" then A.fail("invalid_params", "expr: expected a string") end
	local fn, cerr = A.compile("return " .. expr, "=mcpagent:inspect")
	if not fn then A.fail("lua_syntax_error", tostring(cerr)) end
	local ok, res, err, tb = A.run_budget(fn, 2)
	if not ok then A.fail("lua_error", tostring(err), { traceback = tb }) end
	local v = res[1]
	local depth = args.depth or 1
	local limit = args.limit or 100
	local out = { type = type(v), value = A.serialize(v, { depth = depth, limit = limit }) }
	if type(v) == "table" then
		local count, keys = 0, J.array({})
		for k, val in pairs(v) do
			count = count + 1
			if #keys < limit then keys[#keys + 1] = { key = type(k) == "string" and k or tostring(k), key_type = type(k), type = type(val) } end
		end
		out.count = count
		out.keys = keys
		out.truncated = count > limit
	end
	local mt = getmetatable(v)
	if mt ~= nil then
		local mto = { type = type(mt) }
		if type(mt) == "table" then
			mto.name = rawget(mt, "MetaName")
			local names = J.array({})
			for k in pairs(mt) do if #names < limit then names[#names + 1] = tostring(k) end end
			table.sort(names)
			mto.keys = names
			local idx = rawget(mt, "__index")
			if type(idx) == "table" and idx ~= mt then
				local m = J.array({})
				for k, f in pairs(idx) do if type(f) == "function" and #m < limit then m[#m + 1] = tostring(k) end end
				table.sort(m)
				mto.methods = m
			end
		end
		out.metatable = mto
	end
	return out
end

-- Realm-specific status.
function H.status(args)
	local s = { realm = A.realm, generation = A.generation, lua_memory_kb = collectgarbage("count"), runtime_age_s = SysTime() - A.injected_at }
	if A.realm == "menu" then
		s.version = VERSION
		s.version_str = VERSIONSTR
		s.branch = BRANCH
		s.in_game = IsInGame and IsInGame() or false
		s.loading = IsInLoading and IsInLoading() or false
		s.gamemode = engine and engine.ActiveGamemode and engine.ActiveGamemode() or nil
		local okc, cv = pcall(GetConVar, "maxplayers")
		s.maxplayers = okc and cv and cv:GetInt() or nil
		s.jit = { os = jit and jit.os, arch = jit and jit.arch, version = jit and jit.version }
	else
		s.map = game.GetMap()
		s.maxplayers = game.MaxPlayers()
		s.gamemode = engine.ActiveGamemode()
		s.gamemode_loaded = GAMEMODE ~= nil
		s.entity_count = ents.GetCount()
		s.players = #player.GetAll()
		s.curtime = CurTime()
		s.tickinterval = engine.TickInterval()
		if A.realm == "server" then
			s.dedicated = game.IsDedicated()
			s.singleplayer = game.SinglePlayer()
		else
			s.local_player_valid = IsValid(LocalPlayer())
		end
	end
	return s
end

-- Readiness for map_load: server with the gamemode loaded, client with a valid LocalPlayer.
function H.ready(args)
	if A.realm == "server" then
		return { ready = GAMEMODE ~= nil and #player.GetHumans() + #player.GetBots() >= (game.IsDedicated() and 0 or 1), map = game.GetMap() }
	elseif A.realm == "client" then
		return { ready = GAMEMODE ~= nil and IsValid(LocalPlayer()), map = game.GetMap() }
	end
	return { ready = true }
end
