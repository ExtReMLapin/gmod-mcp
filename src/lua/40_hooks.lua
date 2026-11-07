-- gmod-mcp: hook listing, tracing (without changing behavior), per-function cost.
local A = __mcpagent
local J = A.json
local H = A.handlers

local function fn_src(f)
	if type(f) ~= "function" then return { type = type(f) } end
	local i = debug.getinfo(f, "S")
	return { source = i and i.short_src, line = i and i.linedefined, what = i and i.what }
end

function H.hook_list(args)
	local t = hook.GetTable()
	local out = J.object({})
	local count = 0
	for ev, hooks in pairs(t) do
		if not args.event or ev == args.event then
			local list = J.array({})
			for id, f in pairs(hooks) do
				local o = fn_src(f)
				o.id = type(id) == "string" and id or A.serialize(id, { depth = 1 })
				list[#list + 1] = o
				count = count + 1
			end
			out[ev] = list
		end
	end
	local gm_has = false
	local gm = GAMEMODE or GM
	if args.event and gm and type(gm[args.event]) == "function" then gm_has = true end
	local r = { hooks = out, count = count }
	if args.event then
		r.event = args.event
		r.known = (t[args.event] ~= nil) or gm_has
		r.gamemode_function = gm_has and fn_src(gm[args.event]) or J.null
	end
	return r
end

-- Tracers: a recorder added via hook.Add (unique identifier, returns nothing => does not alter the game).
-- The engine does not call the global hook.Call (it holds an internal reference): a hook.Call detour would see nothing.
-- Limitation: if a hook called before the recorder returns a value, the chain stops and the call is not seen.
local function pack(...) return { n = select("#", ...), ... } end

A.traces = A.traces or {}
local traces = A.traces
local next_id = 0

function A.trace_start(event, max, depth, transform)
	next_id = next_id + 1
	local id = A.realm .. "-" .. next_id
	local tr = { event = event, max = max or 100, depth = depth or 2, calls = {}, total = 0, started = SysTime(), transform = transform,
		hook_id = "__mcpagent_trace_" .. id }
	traces[id] = tr
	hook.Add(event, tr.hook_id, function(...)
		tr.total = tr.total + 1
		if #tr.calls < tr.max then
			local args = J.array({})
			for i = 1, select("#", ...) do args[i] = A.serialize((select(i, ...)), { depth = tr.depth, limit = 50 }) end
			local c = { t = SysTime() - tr.started, args = args }
			if tr.transform then tr.transform(c, ...) end
			tr.calls[#tr.calls + 1] = c
		end
		if #tr.calls >= tr.max then tr.done = true end
		-- no return: behavior unchanged
	end)
	return id
end

function H.trace_start(args)
	if type(args.event) ~= "string" or args.event == "" then A.fail("invalid_params", "event is required") end
	return { id = A.trace_start(args.event, args.max_calls, args.depth) }
end

function H.trace_poll(args)
	local tr = traces[args.id]
	if not tr then A.fail("not_found", "unknown trace: " .. tostring(args.id)) end
	return { recorded = #tr.calls, total = tr.total, done = tr.done or false }
end

function H.trace_stop(args)
	local tr = traces[args.id]
	if not tr then A.fail("not_found", "unknown trace (realm recreated?): " .. tostring(args.id)) end
	traces[args.id] = nil
	hook.Remove(tr.event, tr.hook_id)
	local still = hook.GetTable()[tr.event] and hook.GetTable()[tr.event][tr.hook_id] ~= nil
	return { event = tr.event, calls = J.array(tr.calls), recorded = #tr.calls, total_calls = tr.total,
		capped = tr.total > #tr.calls, duration_s = SysTime() - tr.started, tracer_removed = not still }
end

-- Per-function cost of an event: temporarily wraps each entry of hook.GetTable()[event] (and the gamemode function).
A.perf = A.perf or {}
function H.perf_start(args)
	local ev = args.event
	local id = "perf-" .. ev .. "-" .. math.floor(SysTime() * 1000)
	local stats = {}
	local wrapped = {}
	local hooks = table.Copy(hook.GetTable()[ev] or {})
	for hid, f in pairs(hooks) do
		if type(f) == "function" then
			local key = type(hid) == "string" and hid or tostring(hid)
			local st = { id = key, calls = 0, total_ms = 0, max_ms = 0, src = fn_src(f) }
			stats[#stats + 1] = st
			local w = function(...)
				local t0 = SysTime()
				local r = pack(f(...))
				local dt = (SysTime() - t0) * 1000
				st.calls = st.calls + 1
				st.total_ms = st.total_ms + dt
				if dt > st.max_ms then st.max_ms = dt end
				return unpack(r, 1, r.n)
			end
			wrapped[#wrapped + 1] = { hid = hid, orig = f, w = w }
			hook.Add(ev, hid, w)
		end
	end
	local gm = GAMEMODE
	local gmw
	if gm and type(gm[ev]) == "function" then
		local f = gm[ev]
		local st = { id = "GAMEMODE:" .. ev, calls = 0, total_ms = 0, max_ms = 0, src = fn_src(f) }
		stats[#stats + 1] = st
		local w = function(...)
			local t0 = SysTime()
			local r = pack(f(...))
			local dt = (SysTime() - t0) * 1000
			st.calls = st.calls + 1
			st.total_ms = st.total_ms + dt
			if dt > st.max_ms then st.max_ms = dt end
			return unpack(r, 1, r.n)
		end
		gm[ev] = w
		gmw = { orig = f, w = w }
	end
	A.perf[id] = { event = ev, stats = stats, wrapped = wrapped, gmw = gmw, started = SysTime() }
	return { id = id, functions = #stats }
end

function H.perf_stop(args)
	local p = A.perf[args.id]
	if not p then A.fail("not_found", "unknown measurement (realm recreated?)") end
	A.perf[args.id] = nil
	local hooks = hook.GetTable()[p.event] or {}
	local restored = 0
	for _, w in ipairs(p.wrapped) do
		if hooks[w.hid] == w.w then
			hook.Add(p.event, w.hid, w.orig)
			restored = restored + 1
		end
	end
	if p.gmw and GAMEMODE and GAMEMODE[p.event] == p.gmw.w then GAMEMODE[p.event] = p.gmw.orig end
	table.sort(p.stats, function(a, b) return a.total_ms > b.total_ms end)
	local dur = SysTime() - p.started
	for _, s in ipairs(p.stats) do s.avg_ms = s.calls > 0 and s.total_ms / s.calls or 0 end
	return { event = p.event, duration_s = dur, functions = J.array(p.stats), restored = restored }
end
