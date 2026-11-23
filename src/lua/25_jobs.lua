-- gmod-mcp: lua_job runtime. A job is a coroutine resumed by the module once per frame (job_tick), so long work
-- spreads over many frames without freezing the game, and reports its progress while it runs.
local A = __mcpagent
local J = A.json
local H = A.handlers

A.jobs = A.jobs or {}
local jobs = A.jobs
A.job_seq = A.job_seq or 0

local MAX_REPORTS = 1000
local MAX_REPORT_BYTES = 33554432  -- strings kept in one job's reports (each item is already bounded)
local MAX_OUTPUT = 262144

-- Drops finished jobs nobody collected (5 min; async jobs 1 h). Called from every entry point, not only from the
-- per-frame tick: the module stops ticking once no job runs, and the results would stay in memory forever.
local function prune(now)
	now = now or SysTime()
	for id, job in pairs(jobs) do
		if job.state ~= "running" and now - (job.finished or now) > (job.async and 3600 or 300) then jobs[id] = nil end
	end
end

-- API available to the script as `job` (global inside the chunk, also passed as the chunk's first argument).
local Job = {}
Job.__index = Job

-- job:progress(done, total, message) / job:progress(message). total is optional (unknown).
function Job:progress(done, total, message)
	if type(done) == "string" and total == nil and message == nil then message, done = done, nil end
	local seq = (self._progress and self._progress.seq or 0) + 1
	self._progress = { done = tonumber(done), total = tonumber(total), message = message ~= nil and tostring(message) or nil, seq = seq }
end

-- job:report(value [, channel]): intermediate result, returned with the final result and readable live with
-- reports_read (default channel "job:<id>").
function Job:report(value, channel)
	local item, cut, bytes = A.serialize_report(value)
	if cut then self._reports_truncated = self._reports_truncated + 1 end
	if #self._reports < MAX_REPORTS and self._reports_bytes + bytes <= MAX_REPORT_BYTES then
		self._reports[#self._reports + 1] = item
		self._reports_bytes = self._reports_bytes + bytes
	else
		self._reports_dropped = self._reports_dropped + 1
	end
	A.send_report(channel or ("job:" .. self.id), item)
end

-- Yielding helpers: the job resumes on a later frame.
function Job:sleep(seconds) coroutine.yield("sleep", tonumber(seconds) or 0) end
function Job:frames(n) coroutine.yield("frames", math.max(1, math.floor(tonumber(n) or 1))) end
function Job:yield() coroutine.yield("frames", 1) end
-- Cheap to call in tight loops: yields only once the current frame's slice (slice_ms) is used up.
function Job:step()
	if SysTime() - self._resume_t0 >= self.slice then coroutine.yield("frames", 1) end
end
-- Waits until cond() returns true (checked once per frame). Returns false on timeout.
function Job:wait(cond, timeout)
	local t0 = SysTime()
	while not cond() do
		if timeout and SysTime() - t0 > timeout then return false end
		coroutine.yield("frames", 1)
	end
	return true
end
function Job:elapsed() return SysTime() - self.started end
-- Issue #31: real seconds between the previous resume and this one (since the start on the first resume). FrameTime() is
-- the engine's tick step on the server, not the interval between two resumes of a job.
function Job:dt() return self._dt or 0 end

function H.job_start(args)
	if type(args.code) ~= "string" then A.fail("invalid_params", "code: expected a string") end
	local chunk = args.chunkname or ("=mcpagent:job:" .. A.realm)
	local fn, cerr = A.compile(args.code, chunk)
	if not fn then A.fail("lua_syntax_error", tostring(cerr), { chunkname = chunk, line = (A.compile_error_line(cerr)) }) end
	prune()
	A.job_seq = A.job_seq + 1
	-- The sequence restarts at 0 in every new Lua state (map change): the generation keeps ids unique across states.
	local id = A.realm .. "-g" .. A.generation .. "-" .. A.job_seq
	local job = setmetatable({
		id = id, args = args.args or {}, realm = A.realm, async = args.async == true,
		slice = (tonumber(args.slice_ms) or 8) / 1000,
		-- No resume limit unless asked: a resume that never yields freezes the game (the caller notices).
		limit = args.jit ~= true and tonumber(args.resume_limit_s) or nil,
		hook_guard = args.hook_guard, depth = tonumber(args.depth), items = tonumber(args.limit), maxstr = tonumber(args.max_string),
		started = SysTime(), _frames = 0, _resumes = 0, _cpu = 0, _blocked = {}, _blocked_seen = {}, _inc_failed = {}, _inc_seen = {},
		wake_t = 0, wake_frame = 0, state = "running",
		_reports = {}, _reports_dropped = 0, _reports_truncated = 0, _reports_bytes = 0, _output = {}, _output_size = 0,
	}, Job)
	-- `job` and `args` (= job.args) are visible as globals inside the chunk. Other global reads go to _G; global writes
	-- go to _G too, unless fresh_env: they then stay in this job's own table (no leak into the next job).
	local env = { job = job, args = job.args }
	setfenv(fn, setmetatable(env, { __index = _G, __newindex = args.fresh_env and env or _G }))
	-- Other jobs still running in this realm: two runs on one instance mix their timings and state.
	local others = J.array({})
	for oid, o in pairs(jobs) do
		if o.state == "running" then others[#others + 1] = { id = oid, elapsed_s = SysTime() - o.started, chunkname = o.chunkname } end
	end
	job.chunkname = chunk
	-- With a resume limit, the script's own body runs interpreted so the count hook can stop its loops; the functions
	-- it defines always run compiled.
	if job.limit and jit and jit.off then pcall(jit.off, fn) end
	job.co = coroutine.create(function() return fn(job) end)
	jobs[id] = job
	return { id = id, generation = A.generation, other_jobs = #others > 0 and others or nil }
end

local function pack(...) return { n = select("#", ...), ... } end

local function add_output(job, s)
	if s == "" or job._output_size >= MAX_OUTPUT then return end
	job._output[#job._output + 1] = s
	job._output_size = job._output_size + #s
end

-- Issue #30: job:call(realm, code [, opts]) runs a chunk in another realm of this instance and returns its values.
-- The module crosses over (native.call_realm, on the same main thread, so nothing waits and no frame passes): the chunk
-- runs like lua_exec in that realm, with a budget (default 10 s, opts.budget) so a loop there cannot freeze the game.
-- opts: budget, depth, limit, max_string (as lua_exec). What the chunk printed is added to the job's output as "[realm] ...".
-- A failure (realm_unavailable, client_locked, lua_error...) raises a table {code, message, details}: catch it with pcall.
local REALMS = { client = true, server = true, menu = true }

-- What the other realm serialized, turned back into what the job can use: a Vector, Angle or Color is a real one again (the
-- serializer sends {__type = "Vector", x, y, z}: pos:Distance(...) or ply:SetPos(pos) on that table would fail) and the numbers
-- JSON cannot hold (nan, inf) are numbers again. Entities, functions and the like stay in their {__type = ...} form.
local function revive(v)
	if type(v) ~= "table" or v == J.null then return v end
	local t = v.__type
	if t == "Vector" and Vector and type(v.x) == "number" and type(v.y) == "number" and type(v.z) == "number" then return Vector(v.x, v.y, v.z) end
	if t == "Angle" and Angle and type(v.p) == "number" and type(v.y) == "number" and type(v.r) == "number" then return Angle(v.p, v.y, v.r) end
	if t == "Color" and Color and type(v.r) == "number" and type(v.g) == "number" and type(v.b) == "number" then return Color(v.r, v.g, v.b, tonumber(v.a) or 255) end
	if t == "number" then
		if v.value == "nan" then return 0 / 0 elseif v.value == "inf" then return math.huge elseif v.value == "-inf" then return -math.huge end
	end
	for k, x in pairs(v) do
		if type(x) == "table" then v[k] = revive(x) end  -- assigning to an existing key while traversing is allowed
	end
	return v
end

function Job:call(realm, code, opts)
	if type(realm) ~= "string" or not REALMS[realm] then A.fail("invalid_params", "job:call: realm must be \"client\", \"server\" or \"menu\"") end
	if realm == A.realm then A.fail("invalid_params", "job:call: this job already runs in the " .. realm .. " realm: run the code directly") end
	if type(code) ~= "string" then A.fail("invalid_params", "job:call: code must be a string") end
	if opts ~= nil and type(opts) ~= "table" then A.fail("invalid_params", "job:call: opts must be a table") end
	local native = A.native
	if type(native.call_realm) ~= "function" then A.fail("unsupported", "job:call needs a newer module (this one cannot cross realms)") end
	opts = opts or {}
	local o = { budget = tonumber(opts.budget) or 10, depth = tonumber(opts.depth), limit = tonumber(opts.limit), max_string = tonumber(opts.max_string) }
	local res = J.decode(native.call_realm(realm, code, J.encode(o), A.realm))
	local result = res.result
	-- A chunk that fails reports what it printed in the error's details, not in a result.
	local printed = type(result) == "table" and result.output
	if not printed and type(res.error) == "table" and type(res.error.details) == "table" then printed = res.error.details.output end
	if type(printed) == "string" and printed ~= "" then
		if printed:sub(-1) ~= "\n" then printed = printed .. "\n" end
		add_output(self, "[" .. realm .. "] " .. printed)
	end
	if not res.ok then
		local e = res.error or {}
		A.fail(e.code or "internal", "job:call(" .. realm .. "): " .. tostring(e.message), e.details)
	end
	-- What the other realm noted while the chunk ran (a RunConsoleCommand the engine blocked, an include() that loaded nothing)
	-- belongs to the job's own result, like what it printed.
	for _, c in ipairs(type(result.blocked_console_commands) == "table" and result.blocked_console_commands or {}) do
		if not self._blocked_seen[c] then self._blocked_seen[c] = true self._blocked[#self._blocked + 1] = c end
	end
	for _, p in ipairs(type(result.include_failed) == "table" and result.include_failed or {}) do
		if not self._inc_seen[p] then self._inc_seen[p] = true self._inc_failed[#self._inc_failed + 1] = p end
	end
	local values, n = result.values or {}, result.n or 0
	local unpacked = {}
	for i = 1, n do
		local v = values[i]
		if v == J.null then v = nil end
		unpacked[i] = revive(v)
	end
	return unpack(unpacked, 1, n)
end

local function resume(job)
	local t0 = SysTime()
	job._dt = t0 - (job._resume_t0 or job.started)
	local deadline = t0 + (job.limit or 0)
	local oldf, oldmask, oldcount = debug.gethook()
	if job.limit then
		debug.sethook(function()
			if SysTime() > deadline then
				debug.sethook()
				error("lua_job: a single resume ran longer than " .. job.limit .. " s without yielding (call job:step() or job:yield() inside long loops)", 2)
			end
		end, "", 1000)
	end
	job._resume_t0 = t0
	A.capture_begin()
	A.guard_begin(job.hook_guard)  -- Issue #15: only while this job runs, so another script's hooks are never wrapped
	local res = pack(coroutine.resume(job.co))
	A.guard_end()
	local out, blocked, failed_includes = A.capture_end()
	add_output(job, out)
	for _, p in ipairs(failed_includes or {}) do  -- issue #19: include() calls that failed, over all the resumes
		if not job._inc_seen[p] and #job._inc_failed < 20 then job._inc_seen[p] = true job._inc_failed[#job._inc_failed + 1] = p end
	end
	for _, c in ipairs(blocked or {}) do  -- issue #11: commands the engine blocked from Lua, over all the resumes
		if not job._blocked_seen[c] then job._blocked_seen[c] = true job._blocked[#job._blocked + 1] = c end
	end
	debug.sethook(oldf, oldmask or "", oldcount or 0)
	if not oldf then debug.sethook() end
	job._resumes = job._resumes + 1
	job._cpu = job._cpu + (SysTime() - t0)
	if not res[1] then
		job.state = "failed"
		job.error = A.is_fail(res[2]) and res[2] or { code = "lua_error", message = tostring(res[2]) }
		job.traceback = A.trim_traceback(debug.traceback(job.co, tostring(res[2])))
		job.finished = SysTime()
	elseif coroutine.status(job.co) == "dead" then
		job.state = "done"
		local r = { n = res.n - 1 }
		for i = 2, res.n do r[i - 1] = res[i] end
		local o = A.serialize_values(r, { depth = job.depth, limit = job.items, maxstr = job.maxstr }, {})
		job.values, job.n, job.truncated, job.truncated_note = o.values, o.n, o.truncated, o.truncated_note
		job.finished = SysTime()
	else
		local kind, amount = res[2], res[3]
		if kind == "sleep" then job.wake_t = SysTime() + (amount or 0)
		elseif kind == "frames" then job.wake_frame = job._frames + (amount or 1)
		else job.wake_frame = job._frames + 1 end  -- plain coroutine.yield(): next frame
	end
end

-- Called by the module once per frame while this realm has jobs. Returns the number of jobs still running.
function H.job_tick()
	local now, active = SysTime(), 0
	for id, job in pairs(jobs) do
		if job.state == "running" then
			job._frames = job._frames + 1
			if job._cancel then
				job.state, job.finished = "cancelled", now
			elseif now >= job.wake_t and job._frames >= job.wake_frame then
				resume(job)
			end
			if job.state == "running" then active = active + 1 end
		end
	end
	prune(now)  -- results never collected
	return { active = active }
end

-- State and progress. Reports are drained from `report_cursor` on. The finished job is forgotten once its final
-- state has been read.
function H.job_poll(args)
	prune()
	local job = jobs[args.id]
	if not job then A.fail("not_found", "unknown job " .. tostring(args.id) .. " (realm reloaded or result already collected)") end
	if args.peek and job.state ~= "running" then
		return { id = job.id, state = job.state, frames = job._frames, elapsed_s = (job.finished or SysTime()) - job.started, progress = job._progress }
	end
	local out = {
		id = job.id, state = job.state, frames = job._frames, resumes = job._resumes,
		cpu_ms = job._cpu * 1000, elapsed_s = (job.finished or SysTime()) - job.started,
		progress = job._progress, reports_count = #job._reports, reports_dropped = job._reports_dropped, reports_truncated = job._reports_truncated,
		chunkname = job.chunkname, async = job.async, generation = A.generation,
	}
	if job.state ~= "running" then
		out.reports = J.array(job._reports)
		out.output = table.concat(job._output)
		if job.state == "done" then
			out.values, out.n, out.truncated, out.truncated_note = job.values, job.n, job.truncated, job.truncated_note
			A.apply_capture_fields(out, job._blocked, job._inc_failed)
		end
		if job.state == "failed" then out.error, out.traceback = job.error, job.traceback end
		jobs[args.id] = nil
	end
	return out
end

function H.job_list(args)
	prune()
	local out = J.array({})
	for id, job in pairs(jobs) do
		out[#out + 1] = { id = id, state = job.state, async = job.async, chunkname = job.chunkname, frames = job._frames,
			elapsed_s = (job.finished or SysTime()) - job.started, progress = job._progress }
	end
	return { jobs = out, generation = A.generation }
end

function H.job_cancel(args)
	local job = jobs[args.id]
	if job and job.state == "running" then job._cancel = true end
	return { cancelled = job ~= nil }
end
