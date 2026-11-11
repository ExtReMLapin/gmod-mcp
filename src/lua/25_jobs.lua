-- gmod-mcp: lua_job runtime. A job is a coroutine resumed by the module once per frame (job_tick), so long work
-- spreads over many frames without freezing the game, and reports its progress while it runs.
local A = __mcpagent
local J = A.json
local H = A.handlers

A.jobs = A.jobs or {}
local jobs = A.jobs
A.job_seq = A.job_seq or 0

local MAX_REPORTS = 1000
local MAX_OUTPUT = 262144

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
	if #self._reports < MAX_REPORTS then
		self._reports[#self._reports + 1] = A.serialize(value, { depth = 6, limit = 2000 })
	else
		self._reports_dropped = self._reports_dropped + 1
	end
	mcpagent.report(channel or ("job:" .. self.id), value)
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

function H.job_start(args)
	if type(args.code) ~= "string" then A.fail("invalid_params", "code: expected a string") end
	local chunk = args.chunkname or ("=mcpagent:job:" .. A.realm)
	local fn, cerr = A.compile(args.code, chunk)
	if not fn then A.fail("lua_syntax_error", tostring(cerr), { chunkname = chunk, line = (A.compile_error_line(cerr)) }) end
	A.job_seq = A.job_seq + 1
	local id = A.realm .. "-" .. A.job_seq
	local job = setmetatable({
		id = id, args = args.args or {}, realm = A.realm,
		slice = (tonumber(args.slice_ms) or 8) / 1000, limit = tonumber(args.resume_limit_s) or 2,
		started = SysTime(), _frames = 0, _resumes = 0, _cpu = 0,
		wake_t = 0, wake_frame = 0, state = "running",
		_reports = {}, _reports_dropped = 0, _output = {}, _output_size = 0,
	}, Job)
	-- `job` is visible as a global inside the chunk; other global reads and writes go to _G.
	setfenv(fn, setmetatable({ job = job }, { __index = _G, __newindex = _G }))
	-- Keep the count hook firing inside the script's loops, unless the caller wants the compiler on (measurements).
	job.jit = args.jit == true
	if not job.jit and jit and jit.off then pcall(jit.off, fn, true) end
	job.co = coroutine.create(function() return fn(job) end)
	jobs[id] = job
	return { id = id }
end

local function pack(...) return { n = select("#", ...), ... } end

local function add_output(job, s)
	if s == "" or job._output_size >= MAX_OUTPUT then return end
	job._output[#job._output + 1] = s
	job._output_size = job._output_size + #s
end

local function resume(job)
	local t0 = SysTime()
	local deadline = t0 + job.limit
	local oldf, oldmask, oldcount = debug.gethook()
	if not job.jit then
		debug.sethook(function()
			if SysTime() > deadline then
				debug.sethook()
				error("lua_job: a single resume ran longer than " .. job.limit .. " s without yielding (call job:step() or job:yield() inside long loops)", 2)
			end
		end, "", 1000)
	end
	job._resume_t0 = t0
	A.capture_begin()
	local res = pack(coroutine.resume(job.co))
	add_output(job, A.capture_end())
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
		local values = J.array({})
		for i = 2, res.n do values[i - 1] = A.serialize(res[i], { depth = 6, limit = 2000 }) end
		job.values, job.n = values, res.n - 1
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
		elseif now - (job.finished or now) > 300 then
			jobs[id] = nil  -- result never collected
		end
	end
	return { active = active }
end

-- State and progress. Reports are drained from `report_cursor` on. The finished job is forgotten once its final
-- state has been read.
function H.job_poll(args)
	local job = jobs[args.id]
	if not job then A.fail("not_found", "unknown job " .. tostring(args.id) .. " (realm reloaded or result already collected)") end
	local out = {
		id = job.id, state = job.state, frames = job._frames, resumes = job._resumes,
		cpu_ms = job._cpu * 1000, elapsed_s = (job.finished or SysTime()) - job.started,
		progress = job._progress, reports_count = #job._reports, reports_dropped = job._reports_dropped,
	}
	if job.state ~= "running" then
		out.reports = J.array(job._reports)
		out.output = table.concat(job._output)
		if job.state == "done" then out.values, out.n = job.values, job.n end
		if job.state == "failed" then out.error, out.traceback = job.error, job.traceback end
		jobs[args.id] = nil
	end
	return out
end

function H.job_cancel(args)
	local job = jobs[args.id]
	if job and job.state == "running" then job._cancel = true end
	return { cancelled = job ~= nil }
end
