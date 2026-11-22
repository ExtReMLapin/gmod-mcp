-- gmod-mcp: code execution, inspection, per-realm status.
local A = __mcpagent
local J = A.json
local H = A.handlers

function H.lua_exec(args)
	local code = args.code
	if type(code) ~= "string" then A.fail("invalid_params", "code: expected a string") end
	local chunk = args.chunkname or ("=mcpagent:" .. A.realm)
	-- Issue #28: assign_to keeps the first returned value in a global of this realm (lua_exec_file of a data file).
	local assign = args.assign_to
	if assign ~= nil and (type(assign) ~= "string" or not string.find(assign, "^[%a_][%w_]*$")) then
		A.fail("invalid_params", "assign_to: a plain global name (letters, digits, underscore), not a path or an expression")
	end
	local tc = SysTime()
	local fn, cerr = A.compile(code, chunk)
	local compile_ms = (SysTime() - tc) * 1000
	if not fn then A.fail("lua_syntax_error", tostring(cerr), { chunkname = chunk, line = (A.compile_error_line(cerr)) }) end
	local opts = { depth = args.depth, limit = args.limit, maxstr = args.max_string }
	if assign then opts.depth, opts.limit = opts.depth or 2, opts.limit or 20 end  -- the global holds it whole: the answer is a preview
	-- No budget unless asked (jit: true, the old way to turn it off, wins).
	local budget = args.jit ~= true and tonumber(args.budget) or nil
	A.capture_begin()
	A.guard_begin(args.hook_guard)  -- Issue #15: hooks added by this chunk are removed after N identical errors in a row
	local t0 = SysTime()
	local ok, res, err, tb, exceeded = A.run_budget(fn, budget)
	local dt = (SysTime() - t0) * 1000
	A.guard_end()
	local output, blocked, failed_includes = A.capture_end()
	if not ok then
		if A.is_fail(err) then A.fail(err.code, err.message, err.details) end
		A.fail(exceeded and "budget_exceeded" or "lua_error", tostring(err),
			{ traceback = tb, output = output, duration_ms = dt })
	end
	local out = A.serialize_values(res, opts, { output = output, duration_ms = dt, compile_ms = compile_ms, chunk_bytes = #code,
		chunkname = chunk, budget_s = budget, generation = A.generation })
	A.apply_capture_fields(out, blocked, failed_includes)
	if assign then
		if res.n >= 1 and res[1] ~= nil then
			_G[assign] = res[1]
			out.assigned_to, out.assigned_type = assign, type(res[1])
			if out.truncated then
				out.truncated_note = "preview only: " .. assign .. " holds the whole value; pass depth / limit to see more of it here."
			end
		else
			out.note = "the chunk returned nothing, so " .. assign .. " was left as it was: end the file with `return <value>`."
		end
	end
	return out
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
	if type(v) == "function" then
		-- Where it was defined: the C side adds the real disk path and whether the file came from the mounted folder (Issue #9).
		local info = debug.getinfo(v, "S")
		if info and info.what ~= "C" then
			out.source, out.linedefined, out.lastlinedefined = info.source, info.linedefined, info.lastlinedefined
		end
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

-- Bytecode dumps for lua_function_source: the C side compares the live function with the file compiled from disk.
local function encode_dump(d)
	if util and util.Base64Encode then return util.Base64Encode(d, true), "base64" end
	return (string.gsub(d, ".", function(c) return string.format("%02x", string.byte(c)) end)), "hex"
end

function H.function_dump(args)
	local fn, cerr = A.compile("return " .. tostring(args.expr), "=mcpagent:function_source")
	if not fn then A.fail("lua_syntax_error", tostring(cerr)) end
	local ok, res, err, tb = A.run_budget(fn, 2)
	if not ok then A.fail("lua_error", tostring(err), { traceback = tb }) end
	local f = res[1]
	if type(f) ~= "function" then A.fail("invalid_params", "the expression gives a " .. type(f) .. ", not a function") end
	local info = debug.getinfo(f, "S")
	if info.what == "C" then A.fail("not_lua", "a C function: it has no Lua source") end
	local out = { source = info.source, linedefined = info.linedefined, lastlinedefined = info.lastlinedefined }
	local okd, d = pcall(string.dump, f)
	if okd then out.dump, out.encoding = encode_dump(d) else out.dump_error = tostring(d) end
	return out
end

function H.chunk_dump(args)
	local f, cerr = A.compile(args.code, args.chunkname)
	if not f then A.fail("lua_syntax_error", tostring(cerr)) end
	local out = {}
	out.dump, out.encoding = encode_dump(string.dump(f))
	return out
end

-- Issue #9: does this realm's file system find files of the mounted folder? The module hands over a few names (as file.Exists
-- takes them, from the "LUA" path: no lua/ prefix); file.Time tells which copy answered (the folder's file time is known).
function H.mount_probe(args)
	local files = args.files
	if type(files) ~= "table" or #files > 8 then A.fail("invalid_params", "files: expected a list of at most 8 {name}") end
	if type(file) ~= "table" or type(file.Exists) ~= "function" or type(file.Time) ~= "function" then
		A.fail("unsupported", "this realm has no file library")
	end
	local answers = J.array({})
	for i, f in ipairs(files) do
		if type(f) ~= "table" or type(f.name) ~= "string" then A.fail("invalid_params", "files[" .. i .. "].name: expected a string") end
		local exists = file.Exists(f.name, "LUA") and true or false
		answers[#answers + 1] = { exists = exists, time = exists and file.Time(f.name, "LUA") or nil }
	end
	return { answers = answers }
end

-- Realm-specific status.
function H.status(args)
	local s = { realm = A.realm, generation = A.generation, lua_memory_kb = collectgarbage("count"), runtime_age_s = SysTime() - A.injected_at }
	local function cv(name)
		local c = A.convar(name)
		return c and c:GetFloat() or nil
	end
	s.fps_max, s.host_timescale = cv("fps_max"), cv("host_timescale")
	if A.realm == "menu" then
		s.version = VERSION
		s.version_str = VERSIONSTR
		s.branch = BRANCH
		s.in_game = IsInGame and IsInGame() or false
		s.loading = IsInLoading and IsInLoading() or false
		s.gamemode = engine and engine.ActiveGamemode and engine.ActiveGamemode() or nil
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
