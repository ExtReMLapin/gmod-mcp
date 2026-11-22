-- gmod-mcp: runtime core (serializer, dispatch, budgeted execution, output capture).
local A = __mcpagent
local J = A.json
local native = __mcpagent_native or {}
A.native = native
A.realm = native.realm or (MENU_DLL and "menu" or SERVER and "server" or "client")
A.mode = native.mode or "client"
A.version = native.version or "?"
A.generation = native.generation or 0
A.handlers = A.handlers or {}
A.injected_at = SysTime()

local H = A.handlers
local type, pairs, tostring, select, pcall, xpcall, getmetatable, rawget = type, pairs, tostring, select, pcall, xpcall, getmetatable, rawget
local SysTime = SysTime

-- Structured errors: A.fail raises an object recognized by dispatch.
local FAIL_MT = {}
function A.fail(code, message, details)
	error(setmetatable({ code = code, message = message, details = details }, FAIL_MT), 0)
end
function A.is_fail(e) return getmetatable(e) == FAIL_MT end

local function vec(x) return { x = x[1] or x.x, y = x[2] or x.y, z = x[3] or x.z } end

-- Arguments: conversions from JSON.
function A.to_vector(t, name)
	if t == nil then return nil end
	if type(t) ~= "table" then A.fail("invalid_params", (name or "vector") .. ": expected an [x,y,z] array or an {x,y,z} object") end
	local v = vec(t)
	if type(v.x) ~= "number" or type(v.y) ~= "number" or type(v.z) ~= "number" then
		A.fail("invalid_params", (name or "vector") .. ": expected numeric components")
	end
	return Vector(v.x, v.y, v.z)
end
function A.to_angle(t, name)
	if t == nil then return nil end
	if type(t) ~= "table" then A.fail("invalid_params", (name or "angle") .. ": expected a [p,y,r] array or a {p,y,r} object") end
	local p, y, r = t[1] or t.p or t.pitch, t[2] or t.y or t.yaw, t[3] or t.r or t.roll
	if type(p) ~= "number" or type(y) ~= "number" or type(r) ~= "number" then
		A.fail("invalid_params", (name or "angle") .. ": expected numeric components")
	end
	return Angle(p, y, r)
end
function A.vec_out(v) return J.array({ v.x, v.y, v.z }) end

-- debug_draw "arrow": the shaft and two head wings as {a = {x,y,z}, b = {x,y,z}} segments (plain numbers; the render hook
-- draws them as lines). The head is a quarter of the length, at most 16 units. Zero length or a non-finite end: the shaft only.
function A.arrow_segments(from, to, head)
	local segs = { { a = { x = from.x, y = from.y, z = from.z }, b = { x = to.x, y = to.y, z = to.z } } }
	local dx, dy, dz = to.x - from.x, to.y - from.y, to.z - from.z
	local len = math.sqrt(dx * dx + dy * dy + dz * dz)
	if not (len > 0 and len < math.huge) then return segs end
	dx, dy, dz = dx / len, dy / len, dz / len
	-- side = d x up (or d x forward when d is nearly vertical)
	local ux, uy, uz = 0, 0, 1
	if math.abs(dz) > 0.99 then ux, uy, uz = 1, 0, 0 end
	local sx, sy, sz = dy * uz - dz * uy, dz * ux - dx * uz, dx * uy - dy * ux
	local sl = math.sqrt(sx * sx + sy * sy + sz * sz)
	sx, sy, sz = sx / sl, sy / sl, sz / sl
	local h = head or math.min(len * 0.25, 16)
	for _, k in ipairs({ 1, -1 }) do
		segs[#segs + 1] = { a = { x = to.x, y = to.y, z = to.z },
			b = { x = to.x - dx * h + sx * h * 0.5 * k, y = to.y - dy * h + sy * h * 0.5 * k, z = to.z - dz * h + sz * h * 0.5 * k } }
	end
	return segs
end
function A.ang_out(a) return J.array({ a.p, a.y, a.r }) end

-- Serializer: any Lua value -> JSON-encodable value.
local function func_info(f)
	local info = debug.getinfo(f, "S")
	if not info then return { __type = "function" } end
	return { __type = "function", what = info.what, source = info.short_src, line = info.linedefined }
end

local function ent_info(e)
	local ok, valid = pcall(IsValid, e)
	if not ok or not valid then
		local idx = -1
		pcall(function() idx = e:EntIndex() end)
		if e == NULL or idx == -1 then return { __type = type(e), null = true } end
		if idx == 0 then return { __type = type(e), id = 0, class = "worldspawn", world = true } end
		return { __type = type(e), valid = false, id = idx }
	end
	local o = { __type = type(e), id = e:EntIndex(), class = e:GetClass() }
	local m = e:GetModel()
	if m and m ~= "" then o.model = m end
	if e:IsPlayer() then o.name = e:Nick() end
	return o
end

function A.serialize(v, opts, depth, seen)
	opts = opts or {}
	depth = depth or 0
	seen = seen or {}
	-- No cut by default: depth, limit and maxstr apply only when the caller asks (every cut is counted in opts.cut and
	-- the result says so at the top level). The only guard is the total size (opts.max_nodes values, opts.max_bytes of
	-- strings): past it, the call fails with result_too_large instead of building a huge result. (A client _G is far below:
	-- about 4.5 MB of JSON.)
	local maxdepth = opts.depth or 200
	local limit = opts.limit or math.huge
	local maxstr = opts.maxstr or math.huge
	local tv = type(v)
	opts.nodes = (opts.nodes or 0) + 1
	if opts.nodes > (opts.max_nodes or 1000000) then
		A.fail("result_too_large", "the result has more than " .. (opts.max_nodes or 1000000) .. " values: return only what you " ..
			"need (`fields`, a summary, a count), pass depth/limit/max_string, or write it to a file (file.Write) and read that")
	end
	if v == nil then return J.null end
	if tv == "boolean" then return v end
	if tv == "number" then
		if v ~= v then return { __type = "number", value = "nan" } end
		if v == math.huge then return { __type = "number", value = "inf" } end
		if v == -math.huge then return { __type = "number", value = "-inf" } end
		return v
	end
	if tv == "string" then
		opts.bytes = (opts.bytes or 0) + math.min(#v, maxstr)
		if opts.bytes > (opts.max_bytes or 67108864) then
			A.fail("result_too_large", "the result holds more than " .. math.floor((opts.max_bytes or 67108864) / 1048576) ..
				" MiB of strings: return only what you need (`fields`, a summary), pass max_string, or write it to a file")
		end
		if #v > maxstr then
			opts.cut = (opts.cut or 0) + 1
			return { __type = "string", truncated = true, length = #v, value = string.sub(v, 1, maxstr) }
		end
		return v
	end
	if tv == "Vector" then return { __type = "Vector", x = v.x, y = v.y, z = v.z } end
	if tv == "Angle" then return { __type = "Angle", p = v.p, y = v.y, r = v.r } end
	if tv == "function" then return func_info(v) end
	if tv == "thread" then return { __type = "thread", status = coroutine.status(v) } end
	if (IsEntity and IsEntity(v)) or tv == "Entity" or tv == "Player" or tv == "Weapon" or tv == "NPC" or tv == "Vehicle" or tv == "NextBot" then
		return ent_info(v)
	end
	if tv == "table" then
		if IsColor and IsColor(v) then return { __type = "Color", r = v.r, g = v.g, b = v.b, a = v.a } end
		if seen[v] then return { __type = "cycle" } end
		local mt = getmetatable(v)
		local metaname = type(mt) == "table" and rawget(mt, "MetaName") or nil
		local count, isarr = 0, true
		for k in pairs(v) do
			count = count + 1
			if type(k) ~= "number" or k < 1 or k % 1 ~= 0 then isarr = false end
		end
		if isarr then for i = 1, count do if rawget(v, i) == nil then isarr = false break end end end
		if depth >= maxdepth then
			opts.cut = (opts.cut or 0) + 1
			return { __type = "table", truncated = true, count = count, metatable = metaname }
		end
		seen[v] = true
		local out
		if isarr and count > 0 then
			out = J.array({})
			for i = 1, math.min(count, limit) do out[i] = A.serialize(rawget(v, i), opts, depth + 1, seen) end
			if count > limit then
				opts.cut = (opts.cut or 0) + 1
				out[#out + 1] = { __type = "truncated", remaining = count - limit }
			end
		else
			out = J.object({})
			local n = 0
			for k, val in pairs(v) do
				n = n + 1
				if n > limit then
					opts.cut = (opts.cut or 0) + 1
					out.__truncated = count - limit
					break
				end
				local key = type(k) == "string" and k or ("[" .. (type(k) == "number" and tostring(k) or type(k) .. ":" .. tostring(k)) .. "]")
				out[key] = A.serialize(val, opts, depth + 1, seen)
			end
			if metaname then out.__metatable = metaname end
		end
		seen[v] = nil
		return out
	end
	-- Other userdata (IMaterial, PhysObj, CEffectData...)
	local o = { __type = tv }
	local ok, s = pcall(tostring, v)
	if ok then o.tostring = s end
	if tv == "IMaterial" then pcall(function() o.name = v:GetName() end) end
	if tv == "PhysObj" then pcall(function() o.valid = v:IsValid() end) end
	return o
end

-- Serializes returned values into out.values / out.n; out.truncated = number of cut values (strings over max_string,
-- tables deeper than depth or longer than limit), with a note telling how to get them whole.
function A.serialize_values(res, opts, out)
	local values = J.array({})
	for i = 1, res.n do values[i] = A.serialize(res[i], opts) end
	out.values, out.n = values, res.n
	if (opts.cut or 0) > 0 then
		out.truncated = opts.cut
		out.truncated_note = "some values were cut by the depth / limit / max_string you passed: marked __type = \"truncated\" " ..
			"or truncated = true. Never read a cut value as data; call again without these limits to get it whole."
	end
	return out
end

-- Output capture during an execution.
local capture_stack = {}
local function fmt_args(...)
	local n = select("#", ...)
	local t = {}
	for i = 1, n do t[i] = tostring((select(i, ...))) end
	return table.concat(t, "\t")
end

local function pack_values(...) return { n = select("#", ...), ... } end

function A.capture_begin()
	local buf = {}
	local saved = { print = print, Msg = Msg, MsgN = MsgN, MsgC = MsgC, RunConsoleCommand = RunConsoleCommand }
	local frame = { buf = buf, saved = saved, size = 0, dropped = 0, blocked = {}, blocked_seen = {}, inc_failed = {}, inc_seen = {} }
	capture_stack[#capture_stack + 1] = frame
	-- Captured output is capped at 1 MiB (a print loop must not exhaust memory); the rest is counted as dropped.
	local function add(s)
		if frame.size + #s > 1048576 then
			frame.dropped = frame.dropped + #s
			return
		end
		frame.size = frame.size + #s
		buf[#buf + 1] = s
	end
	print = function(...) add(fmt_args(...) .. "\n") return saved.print(...) end
	Msg = function(...) add(fmt_args(...):gsub("\t", "")) return saved.Msg(...) end
	MsgN = function(...) add(fmt_args(...):gsub("\t", "") .. "\n") return saved.MsgN(...) end
	MsgC = function(...)
		local parts = {}
		for i = 1, select("#", ...) do
			local x = select(i, ...)
			if not (type(x) == "table" and IsColor and IsColor(x)) then parts[#parts + 1] = tostring(x) end
		end
		add(table.concat(parts))
		return saved.MsgC(...)
	end
	-- Issue #11: the engine ignores RunConsoleCommand for the commands on its Lua blacklist, without a word. Note them
	-- (IsConCommandBlocked is the engine's own test for exactly that list) so the result can say so.
	-- Issue #19: include() of a file that is not under lua/ returns false without a word. Note the paths that failed, so the
	-- result can say so. The call is passed through; a path found only in the calling file's folder is handed over under its lua/
	-- name (the engine would resolve it against this wrapper, not against the real caller).
	saved.include = include
	if saved.include and type(file) == "table" and file.Exists then
		include = function(p, ...)
			local target = p
			local resolved = false
			if type(p) == "string" then
				local okx, found = pcall(file.Exists, p, "LUA")
				if okx and found then
					resolved = true
				else
					local info = debug.getinfo(2, "S")
					local src = info and info.source or ""
					local dir = src:match("^@lua/(.*)/[^/]*$") or src:match("^@addons/[^/]+/lua/(.*)/[^/]*$")
					if dir then
						local q = dir .. "/" .. p
						local okq, foundq = pcall(file.Exists, q, "LUA")
						if okq and foundq then target, resolved = q, true end
					end
				end
			end
			local r = pack_values(saved.include(target, ...))
			if r[1] == false and not resolved and type(p) == "string" and not frame.inc_seen[p] then
				frame.inc_seen[p] = true
				frame.inc_failed[#frame.inc_failed + 1] = p
			end
			return unpack(r, 1, r.n)
		end
	end
	if saved.RunConsoleCommand and IsConCommandBlocked then
		RunConsoleCommand = function(cmd, ...)
			if type(cmd) == "string" and not frame.blocked_seen[cmd] and IsConCommandBlocked(cmd) then
				frame.blocked_seen[cmd] = true
				frame.blocked[#frame.blocked + 1] = cmd
			end
			return saved.RunConsoleCommand(cmd, ...)
		end
	end
end

-- The blocked commands noted by a capture, as the fields a result carries (nil when there are none).
function A.blocked_fields(blocked)
	if not blocked or #blocked == 0 then return nil end
	return J.array(blocked), "RunConsoleCommand(" .. table.concat(blocked, ", ") .. ") is blocked from Lua by the engine and did nothing"
		.. " (it does so silently): use console_exec or convar_set, which go through the engine, then read the value back."
end

-- Fields a result carries for what a capture noted: blocked_console_commands (Issue #11), include_failed (Issue #19) and the one
-- `warning` that explains them. Nothing is set when nothing was noted.
function A.apply_capture_fields(out, blocked, failed_includes)
	local arr, warn = A.blocked_fields(blocked)
	out.blocked_console_commands, out.warning = arr, warn
	if failed_includes and #failed_includes > 0 then
		out.include_failed = J.array(failed_includes)
		local w = "include(" .. table.concat(failed_includes, ", ") .. ") returned false: include() only resolves paths under lua/ " ..
			"(the game's lua folder, addons and the mounted folder), so a file anywhere else is not loaded, silently. " ..
			"lua_exec_file with an absolute path loads a file from anywhere on the game's disk."
		out.warning = out.warning and (out.warning .. " " .. w) or w
	end
	return out
end

function A.capture_end()
	local top = table.remove(capture_stack)
	if not top then return "" end
	for k, f in pairs(top.saved) do _G[k] = f end
	local out = table.concat(top.buf)
	if top.dropped > 0 then out = out .. string.format("\n[mcpagent: output truncated, %d more bytes dropped]\n", top.dropped) end
	return out, top.blocked, top.inc_failed
end

-- Cuts the traceback before the runtime's internal frames (xpcall of run_budget / dispatch).
function A.trim_traceback(tb)
	local out = {}
	for line in string.gmatch(tb, "[^\n]+") do
		if string.find(line, "in function 'xpcall'", 1, true) or string.find(line, "mcpagent/10_core.lua", 1, true) then break end
		out[#out + 1] = line
	end
	return table.concat(out, "\n")
end

-- Budgeted execution (debug.sethook): returns ok, results (packed table), err, traceback.
local function pack(...) return { n = select("#", ...), ... } end

-- budget nil/false: no time limit and nothing changed for the compiler (an infinite loop freezes the game; the caller
-- notices through its own timeout and status). With a budget (seconds): best effort. A count hook stops the code past
-- the deadline; count hooks never fire inside compiled code, so the compiler is turned off for fn's own body only (its
-- top-level loops), never for the functions it defines (hooks, timers): they run compiled, like the addon's own code.
-- A loop inside a compiled function it calls is not stopped.
function A.run_budget(fn, budget, ...)
	local hooked = budget ~= nil and budget ~= false
	if hooked and jit and jit.off then pcall(jit.off, fn) end
	local deadline = SysTime() + (hooked and budget or 0)
	local oldf, oldmask, oldcount = debug.gethook()
	local exceeded = false
	if hooked then
		debug.sethook(function()
			if SysTime() > deadline then
				exceeded = true
				debug.sethook()
				error("execution budget exceeded (" .. budget .. " s)", 2)
			end
		end, "", 1000)
	end
	local tb
	local res = pack(xpcall(fn, function(e)
		if A.is_fail(e) then return e end
		tb = A.trim_traceback(debug.traceback(tostring(e), 2))
		return e
	end, ...))
	debug.sethook(oldf, oldmask or "", oldcount or 0)
	if not oldf then debug.sethook() end
	if res[1] then
		local out = { n = res.n - 1 }
		for i = 2, res.n do out[i - 1] = res[i] end
		return true, out
	end
	return false, nil, res[2], tb, exceeded
end

-- Dispatch: entry point called by the DLL.
function A.dispatch(payload)
	local ok, req = pcall(J.decode, payload)
	if not ok then return J.encode({ ok = false, error = { code = "bad_request", message = tostring(req) } }) end
	local h = H[req.tool]
	if not h then
		return J.encode({ ok = false, error = { code = "unknown_tool", message = "unknown tool in realm " .. A.realm .. ": " .. tostring(req.tool) } })
	end
	A.blob = nil
	local tb
	local res = pack(xpcall(h, function(e)
		if A.is_fail(e) then return e end
		tb = debug.traceback(tostring(e), 2)
		return e
	end, req.args or {}))
	local out
	if res[1] then
		out = { ok = true, result = res[2] or J.object({}) }
		if A.blob ~= nil then out.blob = true end
	else
		local e = res[2]
		if A.is_fail(e) then
			out = { ok = false, error = { code = e.code, message = e.message, details = e.details } }
		else
			out = { ok = false, error = { code = "internal", message = tostring(e), details = { traceback = tb } } }
		end
		A.blob = nil
	end
	local okenc, s = pcall(J.encode, out)
	if not okenc then
		return J.encode({ ok = false, error = { code = "internal", message = "JSON encoding failed: " .. tostring(s) } })
	end
	return s
end

-- Convars: always the engine's current object. GMod's GetConVar caches the ConVar userdata per name
-- (includes/util.lua, ConVarCache); in the menu state, which survives map changes, a cached ConVar of a convar that an
-- addon re-creates at every map load points to freed memory after the change, and calling it crashed the game (seen
-- in 0.3.2: console_exec -> convar_get from the menu). GetConVar_Internal is the uncached engine lookup.
function A.convar(name)
	if GetConVar_Internal then return GetConVar_Internal(name) end
	return GetConVar and GetConVar(name)
end

-- Compilation: native.compile (luaL_loadbuffer, exact chunk name); falls back to CompileString. Returns a function, or nil and the
-- compiler's message. Whatever the compiler answers that is not a function is a failure to compile (live verification of 0.3.7: code
-- with an invalid escape came back as an empty result): a compiler that gives the message as its first value, as CompileString does
-- with handleError false, or nothing at all, never gets to run anything.
function A.compile(code, chunk)
	local f, e
	if native.compile then
		f, e = native.compile(code, chunk)
	else
		f = CompileString(code, chunk, false)
	end
	if type(f) == "function" then return f end
	if e == nil and type(f) == "string" then e = f end
	if e == nil or tostring(e) == "" then e = (chunk or "?") .. ": the code did not compile (the compiler gave no message)" end
	return nil, tostring(e)
end

-- Public API for scripts: mcpagent.report(channel, value), mcpagent.event(type, data).
mcpagent = mcpagent or {}
-- Reports are streamed and kept in memory: each item is bounded (depth 8, 5000 entries per table, strings of 1 MiB,
-- 20000 values, 1 MiB of strings in all). Returns the serialized item, whether anything was cut, and the string bytes
-- kept. An item the serializer refuses (too many values) becomes a marker instead of an error in the script.
function A.serialize_report(value)
	local opts = { depth = 8, limit = 5000, maxstr = 1048576, max_nodes = 20000, max_bytes = 1048576 }
	local ok, out = pcall(A.serialize, value, opts, 0, {})
	if not ok then
		local msg = A.is_fail(out) and out.message or tostring(out)
		return { __type = "report_too_large", truncated = true, message = msg }, true, 0
	end
	return out, (opts.cut or 0) > 0, opts.bytes or 0
end
function A.send_report(channel, serialized)
	if native.report then native.report(channel, J.encode(serialized), A.realm) end
end
function mcpagent.report(channel, value)
	if type(channel) ~= "string" or channel == "" then error("mcpagent.report: expected a channel (string)", 2) end
	A.send_report(channel, (A.serialize_report(value)))
end
function mcpagent.event(etype, data)
	if type(etype) ~= "string" then error("mcpagent.event: expected a type (string)", 2) end
	if native.event then native.event("user:" .. etype, J.encode(A.serialize(data or {}, { depth = 6 }))) end
end
mcpagent.realm = A.realm

-- Internal tool: runtime ping.
function H.ping(args) return { realm = A.realm, generation = A.generation, time = SysTime() } end
