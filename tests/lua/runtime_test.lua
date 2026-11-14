-- Runtime tests without the game: loads the embedded runtime chunks into plain LuaJIT with a few GMod stubs and checks
-- the serializer, lua_exec and lua_job handlers. Run: luajit tests/lua/runtime_test.lua src/lua
local dir = arg[1] or "src/lua"

-- GMod stubs (only what the tested handlers touch).
SysTime = os.clock
CurTime = os.clock
MENU_DLL, SERVER, CLIENT = nil, true, nil
-- Like GMod: with handleError false, a syntax error is returned as a string instead of a function.
function CompileString(code, chunk, handle)
	local f, e = loadstring(code, chunk)
	if f then return f end
	if handle == false then return e end
	error(e)
end
util = {}
Vector = function(x, y, z) return { x = x, y = y, z = z } end
Angle = function(p, y, r) return { p = p, y = y, r = r } end
IsColor = function() return false end

__mcpagent = {}
__mcpagent_native = { realm = "server", mode = "client", version = "test", generation = 7 }
for _, name in ipairs({ "00_json.lua", "10_core.lua", "20_lua.lua", "22_check.lua", "25_jobs.lua" }) do
	local f = assert(loadfile(dir .. "/" .. name))
	f()
end
local A = __mcpagent
local H = A.handlers
local J = A.json

local fails, passes = 0, 0
local function check(cond, what)
	if cond then passes = passes + 1 else fails = fails + 1 print("FAIL " .. what) end
end
local function call(tool, args)
	return J.decode(A.dispatch(J.encode({ tool = tool, args = args })))
end

-- lua_exec: plain values, no cut.
local r = call("lua_exec", { code = "return 1, 'a', { x = 2 }" })
check(r.ok and r.result.n == 3 and r.result.truncated == nil, "plain values are not flagged")
check(r.result.generation == 7 and r.result.chunk_bytes == 24 and r.result.budget_s == nil,
	"exec reports generation and size; no budget by default")

-- A1: nothing is cut by default (SW2: a 150 KB string came back as a stub and gave 340 false findings).
r = call("lua_exec", { code = "return string.rep('x', 200000)" })
check(r.ok and r.result.truncated == nil and #r.result.values[1] == 200000, "long string returned whole")
r = call("lua_exec", { code = "return {{{{{{{{{{1}}}}}}}}}}" })
check(r.ok and r.result.truncated == nil and r.result.values[1][1][1][1][1][1][1][1][1][1][1] == 1, "deep table returned whole")
r = call("lua_exec", { code = "local t = {} for i = 1, 5000 do t[i] = i; t[-i] = i end return t" })
check(r.ok and r.result.truncated == nil, "big table returned whole")
-- Limits asked for explicitly still cut, and say so at the top level.
r = call("lua_exec", { code = "return string.rep('x', 70000)", max_string = 100 })
check(r.ok and r.result.truncated == 1 and r.result.truncated_note ~= nil and r.result.values[1].length == 70000,
	"explicit max_string cuts and flags")
r = call("lua_exec", { code = "return {{{{1}}}}", depth = 2 })
check(r.ok and r.result.truncated == 1, "explicit depth cuts and flags")
r = call("lua_exec", { code = "local t = {} for i = 1, 300 do t[i] = i end return t", limit = 10 })
check(r.ok and r.result.truncated == 1, "explicit limit cuts and flags")
-- The only guard: a value too big to send (e.g. return _G with everything it reaches) fails loudly, fast.
local t0 = os.clock()
r = call("lua_exec", { code = "local t = {} for i = 1, 3000000 do t[i] = i end return t", budget = 30 })
check(not r.ok and r.error.code == "result_too_large" and r.error.message:find("fields", 1, true), "huge result: explicit error")
check(os.clock() - t0 < 10, "huge result: the guard stops early")
-- Cycles are still fine.
r = call("lua_exec", { code = "local t = {} t.self = t return t" })
check(r.ok and r.result.values[1].self.__type == "cycle", "cycle")

-- chunkname.
r = call("lua_exec", { code = "return debug.getinfo(1, 'S').short_src", chunkname = "@addons/pba/lua/pba/x.lua" })
check(r.ok and r.result.values[1] == "addons/pba/lua/pba/x.lua", "chunkname is the source")

-- A4: no budget and no JIT cut by default (an infinite loop freezes the game: the caller notices through its timeout and
-- status). A budget asked for explicitly is best effort: it stops a loop at the chunk's top level and a loop that
-- calls compiled code, not a loop inside a compiled function.
local function spin(n) local s = 0 for i = 1, n do s = s + i % 7 end return s end
for _ = 1, 50 do spin(100000) end
__spin = spin
t0 = os.clock()
r = call("lua_exec", { code = "local x = 0 while true do x = x + 1 end", budget = 0.3 })
check(not r.ok and r.error.code == "budget_exceeded" and os.clock() - t0 < 5, "explicit budget: infinite top-level loop stopped")
r = call("lua_exec", { code = "while true do __spin(100000) end", budget = 0.3 })
check(not r.ok and r.error.code == "budget_exceeded", "loop over compiled code stopped")
-- ...but the functions it defines run compiled, as fast as the addon's own (P2: 44 ms vs 16.7 ms).
call("lua_exec", { code = "function __fast(n) local s = 0 for i = 1, n do s = s + i % 7 end return s end" })
local function plain(n) local s = 0 for i = 1, n do s = s + i % 7 end return s end
local function best(f)
	local b = math.huge
	for _ = 1, 3 do
		local t = os.clock()
		for _ = 1, 10 do f(2000000) end
		b = math.min(b, os.clock() - t)
	end
	return b
end
best(__fast) best(plain)
local ratio = best(__fast) / best(plain)
check(ratio < 1.4, string.format("functions defined by lua_exec run compiled (ratio %.2f)", ratio))
-- With an explicit budget too, the functions the chunk defines run compiled (only its own body is interpreted).
call("lua_exec", { code = "function __fast2(n) local s = 0 for i = 1, n do s = s + i % 7 end return s end", budget = 5 })
best(__fast2)
ratio = best(__fast2) / best(plain)
check(ratio < 1.4, string.format("explicit budget: defined functions still compiled (ratio %.2f)", ratio))
-- Timing measured inside the call itself runs compiled too.
r = call("lua_exec", { code = [[local t = os.clock() for _ = 1, 10 do __fast(2000000) end
	local s = 0 for i = 1, 20000000 do s = s + i % 7 end return os.clock() - t]] })
local inside = r.ok and r.result.values[1]
local t1 = os.clock() for _ = 1, 10 do plain(2000000) end local s0 = 0 for i = 1, 20000000 do s0 = s0 + i % 7 end
local outside = os.clock() - t1
check(inside and inside / outside < 1.4, string.format("code timed inside lua_exec runs compiled (ratio %.2f)", inside and inside / outside or -1))
-- jit: true (old parameter) is still accepted.
r = call("lua_exec", { code = "return 1", jit = true })
check(r.ok, "jit: true still accepted")

-- run_budget keeps its old signature (tests pass extra arguments).
local ok, res = A.run_budget(function(a, b) return a + b end, 1, 2, 3)
check(ok and res[1] == 5, "run_budget passes extra arguments")

-- lua_job: global args, fresh_env, concurrent jobs, truncation.
local function run_job(args)
	local st = call("job_start", args)
	if not st.ok then return st end
	for _ = 1, 1000 do
		H.job_tick()
		local p = call("job_poll", { id = st.result.id, peek = true })
		if p.ok and p.result.state ~= "running" then break end
	end
	local out = call("job_poll", { id = st.result.id })
	out.start = st.result
	return out
end
r = run_job({ code = "return args.n * 2, job.args.n", args = { n = 21 } })
check(r.ok and r.result.state == "done" and r.result.values[1] == 42 and r.result.values[2] == 21, "job sees global args")
check(r.start.generation == 7, "job_start reports the generation")

run_job({ code = "LEAK_A = 1 return true" })
check(rawget(_G, "LEAK_A") == 1, "default env writes globals")
run_job({ code = "LEAK_B = 1 return true", fresh_env = true })
check(rawget(_G, "LEAK_B") == nil, "fresh_env keeps globals local")
r = run_job({ code = "return LEAK_A, type(hook), print ~= nil", fresh_env = true })
check(r.ok and r.result.values[1] == 1 and r.result.values[3] == true, "fresh_env still reads _G")

r = run_job({ code = "return string.rep('y', 200000)" })
check(r.ok and r.result.truncated == nil and #r.result.values[1] == 200000, "job values returned whole")
r = run_job({ code = "return string.rep('y', 70000)", max_string = 100 })
check(r.ok and r.result.truncated == 1, "job explicit max_string flags the cut")
r = run_job({ code = "job:report(string.rep('z', 100000)) return 1" })
check(r.ok and #r.result.reports[1] == 100000, "job reports returned whole")
-- A runaway loop in a job (no job:step) is stopped by an explicit resume_limit_s.
r = run_job({ code = "local x = 0 while true do x = x + 1 end", resume_limit_s = 0.3 })
check(r.ok and r.result.state == "failed", "runaway job stopped")
-- Functions a job defines run compiled.
run_job({ code = "function __jfast(n) local s = 0 for i = 1, n do s = s + i % 7 end return s end return 1" })
best(__jfast)
ratio = best(__jfast) / best(plain)
check(ratio < 1.4, string.format("functions defined by a job run compiled (ratio %.2f)", ratio))

-- Two jobs at once: the second start says the first is running.
local s1 = call("job_start", { code = "job:sleep(10) return 1", chunkname = "=first" })
local s2 = call("job_start", { code = "return 2" })
check(s2.ok and type(s2.result.other_jobs) == "table" and s2.result.other_jobs[1].id == s1.result.id, "concurrent job reported")
local l = call("job_list", {})
check(l.ok and #l.result.jobs >= 2, "job_list lists jobs")
call("job_cancel", { id = s1.result.id })
H.job_tick()
local p = call("job_poll", { id = s1.result.id })
check(p.ok and p.result.state == "cancelled", "cancel stops a sleeping job")

-- Async jobs are kept longer when nobody collects them.
local s3 = call("job_start", { code = "return 3", async = true })
H.job_tick()
A.jobs[s3.result.id].finished = SysTime() - 1000
H.job_tick()
check(A.jobs[s3.result.id] ~= nil, "async result kept after 300 s")

-- ===================== Edge cases =====================
-- Strict UTF-8 check (what nlohmann::json::parse on the C++ side accepts): no overlongs, no surrogates, <= U+10FFFF.
local function strict_utf8(str)
	local i, n = 1, #str
	while i <= n do
		local c = str:byte(i)
		if c < 0x80 then i = i + 1
		else
			local need, lo, hi
			if c >= 0xC2 and c <= 0xDF then need, lo, hi = 1, 0x80, 0xBF
			elseif c == 0xE0 then need, lo, hi = 2, 0xA0, 0xBF
			elseif c == 0xED then need, lo, hi = 2, 0x80, 0x9F
			elseif c >= 0xE1 and c <= 0xEF then need, lo, hi = 2, 0x80, 0xBF
			elseif c == 0xF0 then need, lo, hi = 3, 0x90, 0xBF
			elseif c == 0xF4 then need, lo, hi = 3, 0x80, 0x8F
			elseif c >= 0xF1 and c <= 0xF3 then need, lo, hi = 3, 0x80, 0xBF
			else return false, i end
			if i + need > n then return false, i end
			local c2 = str:byte(i + 1)
			if c2 < lo or c2 > hi then return false, i end
			for k = 2, need do local cc = str:byte(i + k) if cc < 0x80 or cc > 0xBF then return false, i end end
			i = i + need + 1
		end
	end
	return true
end
local function raw(tool, args) return A.dispatch(J.encode({ tool = tool, args = args })) end

-- Binary strings and every malformed UTF-8 shape must give strictly valid JSON (else the C++ side fails to parse
-- the runtime's answer: bad_runtime_output).
for _, case in ipairs({
	{ "nul byte", "a\\0b" }, { "lone 0xFB", "\\251" }, { "lone continuation", "\\128" },
	{ "overlong 2-byte", "\\192\\175" }, { "overlong 3-byte", "\\224\\128\\175" },
	{ "UTF-16 surrogate in UTF-8", "\\237\\160\\128" }, { "above U+10FFFF", "\\244\\144\\128\\128" },
	{ "truncated 3-byte", "x\\226\\130" }, { "valid euro sign", "\\226\\130\\172" }, { "valid emoji", "\\240\\159\\152\\128" },
}) do
	local out = raw("lua_exec", { code = "return '" .. case[2] .. "'" })
	local ok, at = strict_utf8(out)
	check(ok, "strict UTF-8 output for " .. case[1] .. (ok and "" or (" (bad byte at " .. tostring(at) .. ")")))
end
-- Valid UTF-8 comes back unchanged.
r = call("lua_exec", { code = "return '\\226\\130\\172 \\240\\159\\152\\128'" })
check(r.ok and r.result.values[1] == "\226\130\172 \240\159\152\128", "valid UTF-8 unchanged")
-- Numbers.
r = call("lua_exec", { code = "return 0/0, 1/0, -1/0, -0, 2^53, 2^63, 1e308, 5e-324, 0.1" })
local v = r.ok and r.result.values or {}
check(r.ok and v[1].value == "nan" and v[2].value == "inf" and v[3].value == "-inf", "nan/inf/-inf tagged")
check(r.ok and v[4] == 0 and v[5] == 2^53 and v[6] == 2^63 and v[7] == 1e308 and v[8] == 5e-324 and v[9] == 0.1, "number round trip")
-- Odd tables.
r = call("lua_exec", { code = "return { [1] = 'a', [3] = 'c' }, { 'x', k = 'y' }, { [true] = 1, [false] = 2 }, { [{}] = 1 }, { [print] = 1 }, { [1.5] = 1 }, {}" })
v = r.ok and r.result.values or {}
check(r.ok and v[1]["[1]"] == "a" and v[1]["[3]"] == "c", "sparse array as an object")
check(r.ok and v[2]["[1]"] == "x" and v[2].k == "y", "mixed table")
check(r.ok and v[3]["[boolean:true]"] == 1 and v[3]["[boolean:false]"] == 2, "boolean keys")
local tk, fk = 0, 0
for k in pairs(v[4] or {}) do if k:find("^%[table:") then tk = tk + 1 end end
for k in pairs(v[5] or {}) do if k:find("^%[function:") then fk = fk + 1 end end
check(tk == 1 and fk == 1, "table and function keys named by type")
check(r.ok and v[6]["[1.5]"] == 1, "fractional key")
check(r.ok and type(v[7]) == "table" and next(v[7]) == nil, "empty table")
-- Depth beyond the hard cap: cut and flagged, no stack overflow.
r = call("lua_exec", { code = "local t = {} local c = t for i = 1, 5000 do c.n = {} c = c.n end return t" })
check(r.ok and r.result.truncated == 1, "5000-deep table cut at the hard cap, flagged")
-- Metatables that throw while being serialized.
r = call("lua_exec", { code = "return setmetatable({ a = 1 }, { __index = function() error('idx') end, __tostring = function() error('ts') end })" })
check(r.ok and r.result.values[1].a == 1, "throwing __index/__tostring on a table")
r = call("lua_exec", { code = "return setmetatable({}, { __metatable = 'locked' })" })
check(r.ok, "protected metatable")
r = call("lua_exec", { code = "return newproxy and setmetatable and newproxy(true) or 1" })
check(r.ok, "plain userdata")
r = call("lua_exec", { code = "local u = newproxy(true) getmetatable(u).__tostring = function() error('ud ts') end return u" })
check(r.ok and r.result.values[1].__type == "userdata", "userdata whose __tostring throws")
-- Errors of every shape.
r = call("lua_exec", { code = "error(nil)" })
check(not r.ok and r.error.code == "lua_error", "error(nil)")
r = call("lua_exec", { code = "error({ code = 42 })" })
check(not r.ok and r.error.code == "lua_error" and type(r.error.message) == "string", "error(table)")
r = call("lua_exec", { code = "error(setmetatable({}, { __tostring = function() return 'custom' end }))" })
check(not r.ok and r.error.message == "custom", "error object with __tostring")
r = call("lua_exec", { code = "error(setmetatable({}, { __tostring = function() error('ts fails') end }))" })
check(not r.ok and r.error.code == "lua_error", "error object whose __tostring throws")
local out = raw("lua_exec", { code = "error(setmetatable({}, { __tostring = function() error('ts fails') end }))" })
check(pcall(J.decode, out), "dispatch still answers JSON when the error's __tostring throws")
r = call("lua_exec", { code = "error('\\251 bad bytes')" })
check(not r.ok and strict_utf8(raw("lua_exec", { code = "error('\\251 bad bytes')" })), "error message with invalid UTF-8")
r = call("lua_exec", { code = "return" })
check(r.ok and r.result.n == 0, "no return value")
r = call("lua_exec", { code = "return nil, nil" })
check(r.ok and r.result.n == 2, "two nils counted")
r = call("lua_exec", { code = "print('a\\0b\\251') return 1" })
check(r.ok and strict_utf8(raw("lua_exec", { code = "print('a\\0b\\251') return 1" })), "captured output with binary bytes")
-- Syntax errors and odd code.
r = call("lua_exec", { code = "" })
check(r.ok and r.result.n == 0, "empty code")
r = call("lua_exec", { code = "return 1 +" })
check(not r.ok and r.error.code == "lua_syntax_error", "syntax error")
r = call("lua_exec", { code = "\\239\\187\\191return 1" })
check(r.ok or (not r.ok and r.error.code == "lua_syntax_error"), "BOM at the start: clean answer")
r = call("lua_exec", {})
check(not r.ok and r.error.code == "invalid_params", "missing code")
r = call("no_such_tool", {})
check(not r.ok and r.error.code == "unknown_tool", "unknown tool")
check(J.decode(A.dispatch("not json")).ok == false, "bad request payload")

-- Jobs: error after reports, yield inside pcall, nothing returned, cancel twice, unknown id, many jobs.
r = run_job({ code = "job:report('r1') job:frames(1) error('late failure')" })
check(not r.ok or r.result.state == "failed", "job failing after a report")
local st = call("job_start", { code = "job:report('r1') job:frames(1) error('late failure')" })
for _ = 1, 5 do H.job_tick() end
local p2 = call("job_poll", { id = st.result.id })
check(p2.ok and p2.result.state == "failed" and #p2.result.reports == 1 and p2.result.error.message:find("late failure"), "failed job keeps its reports and message")
r = run_job({ code = "local ok, v = pcall(function() job:frames(2) return 7 end) return ok, v" })
check(r.ok and r.result.values[1] == true and r.result.values[2] == 7, "yield inside pcall")
r = run_job({ code = "local ok, e = pcall(function() job:frames(1) error('inner') end) return ok, e ~= nil" })
check(r.ok and r.result.values[1] == false and r.result.values[2] == true, "error caught by pcall across a yield")
r = run_job({ code = "job:frames(1)" })
check(r.ok and r.result.state == "done" and r.result.n == 0, "job returning nothing")
st = call("job_start", { code = "job:sleep(60)" })
local c1, c2 = call("job_cancel", { id = st.result.id }), call("job_cancel", { id = st.result.id })
check(c1.ok and c2.ok and c1.result.cancelled and c2.result.cancelled, "cancel twice")
H.job_tick()
check(call("job_poll", { id = st.result.id }).result.state == "cancelled", "cancelled after a double cancel")
check(call("job_cancel", { id = "server-999999" }).result.cancelled == false, "cancel of an unknown id")
local up = call("job_poll", { id = "server-999999" })
check(not up.ok and up.error.code == "not_found", "poll of an unknown id")
local ids = {}
for i = 1, 100 do ids[i] = call("job_start", { code = "job:frames(" .. (i % 3 + 1) .. ") return " .. i, async = true }).result.id end
for _ = 1, 10 do H.job_tick() end
local good = 0
for i = 1, 100 do
	local p = call("job_poll", { id = ids[i] })
	if p.ok and p.result.state == "done" and p.result.values[1] == i then good = good + 1 end
end
check(good == 100, "100 jobs at once all finish with their own result (" .. good .. ")")
local lst = call("job_list", {})
local still = 0
for _, j in ipairs(lst.result.jobs) do for i = 1, 100 do if j.id == ids[i] then still = still + 1 end end end
check(still == 0, "collected jobs leave job_list")
r = run_job({ code = "job:sleep(0) job:sleep(-5) job:frames(0) job:frames('x') return 1" })
check(r.ok and r.result.state == "done", "odd sleep/frames arguments do not break the job")
r = run_job({ code = "coroutine.yield('weird', 1) return 2" })
check(r.ok and r.result.values[1] == 2, "plain coroutine.yield with odd values")

print(string.format("runtime_test: %d passed, %d failed", passes, fails))
os.exit(fails == 0 and 0 or 1)
