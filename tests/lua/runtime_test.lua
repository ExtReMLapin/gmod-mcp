-- Runtime tests without the game: loads the embedded runtime chunks into plain LuaJIT with a few GMod stubs and checks
-- the serializer, lua_exec and lua_job handlers. Run: luajit tests/lua/runtime_test.lua src/lua
local dir = arg[1] or "src/lua"

-- GMod stubs (only what the tested handlers touch).
SysTime = os.clock
CurTime = os.clock
MENU_DLL, SERVER, CLIENT = nil, true, nil
function CompileString(code, chunk) return loadstring(code, chunk) end
util = {}
Vector = function(x, y, z) return { x = x, y = y, z = z } end
Angle = function(p, y, r) return { p = p, y = y, r = r } end
IsColor = function() return false end

__mcpagent = {}
__mcpagent_native = { realm = "server", mode = "client", version = "test", generation = 7 }
for _, name in ipairs({ "00_json.lua", "10_core.lua", "20_lua.lua", "25_jobs.lua" }) do
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

print(string.format("runtime_test: %d passed, %d failed", passes, fails))
os.exit(fails == 0 and 0 or 1)
