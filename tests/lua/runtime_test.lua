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
-- Convars: GetConVar (GMod's cached wrapper) must never be used by the runtime: in the menu state its cache keeps a
-- ConVar whose engine object an addon re-creates at every map load, and the next call crashed the game (0.3.2).
bit = bit or { band = function(a, b) return 0 end }
local fake_cv = { GetFlags = function() return 0 end, GetName = function() return "x" end, GetHelpText = function() return "" end,
	GetDefault = function() return "0" end, GetString = function() return "1" end, GetFloat = function() return 1 end }
fake_cv.__index = fake_cv
function ConVarExists(n) return true end
function GetConVar(n) error("cached GetConVar used (crash risk)") end
function GetConVar_Internal(n) return setmetatable({}, fake_cv) end
function RunConsoleCommand() end
game = game or { ConsoleCommand = function() end }
for _, name in ipairs({ "00_json.lua", "10_core.lua", "20_lua.lua", "22_check.lua", "25_jobs.lua", "30_session.lua" }) do
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
-- Live verification of 0.3.7: code with a "\\u0041" escape (LuaJIT 2.1 only knows "\\u{41}") came back as an empty result instead of a syntax error.
-- Every way the compiler can say "no" must come back as lua_syntax_error with the message: the plain error, the escape, and a compiler that
-- answers with something that is not a function (no message, or the message as the first value, as CompileString does).
r = call("lua_exec", { code = 'return "\\u0041"' })
check(not r.ok and r.error.code == "lua_syntax_error" and r.error.message:find("escape", 1, true), "an invalid unicode escape is a syntax error")
check(r.error and r.error.details and r.error.details.line == 1, "its line is reported")
r = call("lua_exec", { code = 'return "\\u{41}"' })
check(r.ok and r.result.values[1] == "A", "\\u{41} is fine")
do
	local old = __mcpagent_native.compile
	__mcpagent_native.compile = function() return nil end
	r = call("lua_exec", { code = "return 1" })
	check(not r.ok and r.error.code == "lua_syntax_error" and r.error.message ~= "nil" and #r.error.message > 10, "compiler answering nothing: a syntax error with a message")
	__mcpagent_native.compile = function() return "bad:1: boom" end
	r = call("lua_exec", { code = "return 1" })
	check(not r.ok and r.error.code == "lua_syntax_error" and r.error.message == "bad:1: boom", "compiler answering the message first: a syntax error")
	__mcpagent_native.compile = function() return false, "bad:2: nope" end
	r = call("lua_exec", { code = "return 1" })
	check(not r.ok and r.error.code == "lua_syntax_error" and r.error.message == "bad:2: nope" and r.error.details.line == 2, "false + message")
	__mcpagent_native.compile = old
end
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

r = call("convar_get", { name = "pba_x" })
check(r.ok and r.result.number == 1, "convar_get uses the uncached GetConVar_Internal" .. (r.ok and "" or (" " .. tostring(r.error and r.error.message))))
r = call("convar_set", { name = "pba_x", value = 2 })
check(r.ok, "convar_set uses the uncached GetConVar_Internal" .. (r.ok and "" or (" " .. tostring(r.error and r.error.message))))
local realm0 = A.realm A.realm = "menu"
r = call("status", {})
A.realm = realm0
check(r.ok and r.result.fps_max == 1, "status reads fps_max without the cached GetConVar" .. (r.ok and "" or (" " .. tostring(r.error and r.error.message))))

-- Job ids carry the realm's generation: a new Lua state restarts job_seq at 0, so "server-1" of map A and of map B
-- must never be the same id (lua_job_status {id} answered the wrong map's job and consumed its result).
local gen_a = call("job_start", { code = "return 1", async = true })
check(gen_a.ok and gen_a.result.id:match("^server%-g7%-%d+$") ~= nil, "job id carries the generation: " .. tostring(gen_a.ok and gen_a.result.id))

-- Pruning must not depend on the tick (the module stops ticking when nothing runs): finished jobs left uncollected
-- are dropped by age when a job is started, polled or listed.
local function age_all()
	for _, j in pairs(A.jobs) do
		if j.state ~= "running" then j.finished = SysTime() - 10000 end
	end
end
local function count_jobs() local n = 0 for _ in pairs(A.jobs) do n = n + 1 end return n end
for _ = 1, 50 do H.job_tick() end
age_all()
local before = count_jobs()
call("job_start", { code = "return 9", async = true })
check(before > 0 and count_jobs() == 1, "job_start prunes old finished jobs (" .. before .. " -> " .. count_jobs() .. ")")
for _ = 1, 5 do H.job_tick() end
age_all()
local ping = call("job_start", { code = "job:sleep(100)" })
age_all()
call("job_poll", { id = ping.result.id, peek = true })
check(count_jobs() == 1, "job_poll prunes old finished jobs (" .. count_jobs() .. ")")
call("job_cancel", { id = ping.result.id })
H.job_tick()
age_all()
local lst2 = call("job_list", {})
check(lst2.ok and #lst2.result.jobs == 0 and count_jobs() == 0, "job_list prunes old finished jobs (" .. count_jobs() .. ")")

-- Reports are bounded per item: depth 8, 5000 entries, 1 MiB strings; a cut is flagged. The count cap stays.
local rj = run_job({ code = [[
	job:report(string.rep("z", 2 * 1048576))
	local deep = {} local t = deep for i = 1, 30 do t.n = {} t = t.n end
	job:report(deep)
	local wide = {} for i = 1, 20000 do wide[i] = i end
	job:report(wide)
	job:report("small")
	return 1]] })
check(rj.ok and #rj.result.reports == 4, "reports kept")
local rep1, rep2, rep3, rep4 = rj.result.reports[1], rj.result.reports[2], rj.result.reports[3], rj.result.reports[4]
check(type(rep1) == "table" and rep1.truncated == true and #rep1.value == 1048576, "report string cut at 1 MiB")
local d, depth = rep2, 0
while type(d) == "table" and d.n do d = d.n depth = depth + 1 end
check(depth <= 9 and type(d) == "table" and d.truncated == true, "report depth bounded (reached " .. depth .. ")")
check(#rep3 <= 5001 and rep3[#rep3].__type == "truncated", "report array bounded to 5000 entries")
check(rep4 == "small", "small report untouched")
check(rj.result.reports_truncated == 3, "poll says how many reports were cut: " .. tostring(rj.result.reports_truncated))
-- A table the serializer refuses (too many nodes) must not break the job: the report is replaced by a marker.
local rk = run_job({ code = [[
	local big = {} for i = 1, 200 do local row = {} for j = 1, 200 do row[j] = { j } end big[i] = row end
	job:report(big)
	return 1]] })
check(rk.ok and rk.result.state == "done" and type(rk.result.reports[1]) == "table" and rk.result.reports_truncated == 1,
	"an oversized report is replaced by a marker, the job survives")
-- Count cap still holds.
local rc = run_job({ code = "for i = 1, 1100 do job:report(i) end return 1" })
check(rc.ok and #rc.result.reports == 1000 and rc.result.reports_dropped == 100, "report count cap")

-- Test client (launched by the bridge): the archived convars its command line changed (fps_max...) are given back to the
-- user's values, read from cfg/config.cfg at that moment, before a clean quit; a convar without a line there gets its default.
do
	local cfg_text = table.concat({
		"unbindall", 'bind "a" "+attack"', '// fps_max "1"', 'name "someone"', 'fps_max "240"', "FPS_MAX_NOFOCUS 60",
		'snd_mute_losefocus "1"', 'fps_max "144"', "" }, "\r\n")
	local m = A.parse_config_values(cfg_text)
	check(m.fps_max == "144", "config.cfg: the last line of a convar wins")
	check(m.fps_max_nofocus == "60" and m.snd_mute_losefocus == "1", "config.cfg: names are case-insensitive, unquoted values read")
	check(m.name == "someone" and m["//"] == nil and m.bind == nil, "config.cfg: comments and binds are not convars")
	check(next(A.parse_config_values("")) == nil and next(A.parse_config_values(nil)) == nil, "config.cfg: empty or missing file")

	local state = { fps_max = { "20", "300" }, fps_max_nofocus = { "20", "60" }, engine_no_focus_sleep = { "0", "50" },
		snd_mute_losefocus = { "0", "1" } }
	local function get(n) return state[n] and { current = state[n][1], default = state[n][2] } end
	local names = { "fps_max", "fps_max_nofocus", "engine_no_focus_sleep", "snd_mute_losefocus" }
	local plan = A.plan_convar_restore(names, 'fps_max "240"\nsnd_mute_losefocus "1"\nengine_no_focus_sleep "bad;x"\n', get)
	local got = {}
	for _, c in ipairs(plan) do got[c.name] = c.value end
	check(got.fps_max == "240" and got.snd_mute_losefocus == "1", "plan: the user's value from config.cfg")
	check(got.fps_max_nofocus == "60", "plan: no line in config.cfg: the convar's default")
	check(got.engine_no_focus_sleep == "50", "plan: an unsafe value in config.cfg is ignored, the default is used")
	check(#plan == 4, "plan: four changes")
	plan = A.plan_convar_restore({ "fps_max", "nope" }, 'fps_max "20"', get)
	check(#plan == 0, "plan: nothing to do when the convar already has the user's value; unknown convars skipped")

	-- Wiring: game_quit and console_exec quit/exit on a test client; nothing changes on a user's own game.
	local log, files = {}, { ["cfg/config.cfg"] = 'fps_max "240"\nsnd_mute_losefocus "1"\n' }
	local old_rcc, old_gci, old_cmd = RunConsoleCommand, GetConVar_Internal, game.ConsoleCommand
	RunConsoleCommand = function(...) log[#log + 1] = table.concat({ ... }, " ") end
	game.ConsoleCommand = function(c) log[#log + 1] = "GAME:" .. c end
	__mcpagent_native.user_config = function() return files["cfg/config.cfg"] end
	GetConVar_Internal = function(n)
		local st = state[n]
		if not st then return nil end
		return { GetString = function() return st[1] end, GetDefault = function() return st[2] end, GetName = function() return n end }
	end
	local q = call("quit", {})
	check(q.ok and #log == 1 and log[1] == "GAME:quit\n", "user's own game: quit only: " .. table.concat(log, "|"))
	log = {}
	__mcpagent_native.test_client = true
	q = call("quit", {})
	local joined = table.concat(log, "|")
	check(q.ok and q.result.restored == 4, "test client: quit reports the restored convars")
	check(log[#log] == "GAME:quit\n" and #log == 5, "test client: four restores, then quit: " .. joined)
	check(joined:find("fps_max 240", 1, true) and joined:find("fps_max_nofocus 60", 1, true)
		and joined:find("engine_no_focus_sleep 50", 1, true) and joined:find("snd_mute_losefocus 1", 1, true), "test client: the user's values: " .. joined)
	log = {}
	check(call("console_exec", { cmd = "quit" }).ok and log[#log] == "GAME:quit\n" and #log == 5, "console_exec quit restores first: " .. table.concat(log, "|"))
	log = {}
	call("console_exec", { cmd = "exit" })
	check(#log == 5 and log[#log] == "GAME:exit\n", "console_exec exit restores first")
	log = {}
	local rc = call("restore_convars", {})
	check(rc.ok and rc.result.restored == 4 and #log == 4, "restore_convars (window close) restores without quitting: " .. table.concat(log, "|"))
	-- The main menu's Quit button (RunGameUICommand "quit") on a test client.
	log = {}
	local old_gui, old_eng = RunGameUICommand, __mcpagent_native.engine_cmd
	RunGameUICommand = function(c) log[#log + 1] = "GUI:" .. tostring(c) end
	__mcpagent_native.engine_cmd = function(l) log[#log + 1] = "ENG:" .. l return true end
	check(A.wrap_menu_quit() and not A.wrap_menu_quit(), "menu quit: wrapped once")
	RunGameUICommand("quit")
	check(#log == 5 and log[1] == 'ENG:fps_max "240"' and log[5] == "ENG:quit", "menu quit button: restores, then quits through the console path: " .. table.concat(log, "|"))
	log = {}
	RunGameUICommand("openserverbrowser")
	check(#log == 1 and log[1] == "GUI:openserverbrowser", "other gameui commands pass through")
	RunGameUICommand, __mcpagent_native.engine_cmd, A.quit_wrapped = old_gui, old_eng, nil
	log = {}
	call("console_exec", { cmd = "echo hi" })
	check(#log == 1 and log[1] == "GAME:echo hi\n", "other commands do not restore")
	__mcpagent_native.test_client = nil
	__mcpagent_native.user_config = nil
	RunConsoleCommand, GetConVar_Internal, game.ConsoleCommand = old_rcc, old_gci, old_cmd
end

-- Linux finding LF-1: on srcds game.ConsoleCommand("quit") is blocked from server Lua (the engine's own blacklist), so
-- game_quit answered quitting:true and nothing quit. When the engine says `quit` is blocked, the runtime asks the module to
-- end the process (native.request_quit), and answers an error when there is no way to quit: never "requested" for nothing.
do
	local log = {}
	local old_cmd, old_blocked, old_req = game.ConsoleCommand, IsConCommandBlocked, __mcpagent_native.request_quit
	game.ConsoleCommand = function(c) log[#log + 1] = "GAME:" .. c end
	IsConCommandBlocked = function(c) return c == "quit" or c == "exit" end
	__mcpagent_native.request_quit = nil
	local q = call("quit", {})
	check(not q.ok and q.error.code == "blocked" and #log == 0, "quit blocked, no native way: an error, nothing sent: " .. J.encode(q))
	__mcpagent_native.request_quit = function(ms) log[#log + 1] = "SIG:" .. tostring(ms) return true end
	q = call("quit", {})
	check(q.ok and q.result.requested == true and q.result.via == "signal" and #log == 1 and log[1]:find("^SIG:") ~= nil,
		"quit blocked: the module ends the process: " .. J.encode(q) .. table.concat(log, "|"))
	log = {}
	__mcpagent_native.request_quit = function() log[#log + 1] = "SIG" return false end
	q = call("quit", {})
	check(not q.ok and q.error.code == "blocked" and #log == 1, "the module cannot quit: an error, not requested:true: " .. J.encode(q))
	log = {}
	IsConCommandBlocked = function() return false end
	q = call("quit", {})
	check(q.ok and q.result.via == "server" and #log == 1 and log[1] == "GAME:quit\n", "quit not blocked: console command as before")
	game.ConsoleCommand, IsConCommandBlocked, __mcpagent_native.request_quit = old_cmd, old_blocked, old_req
end

-- Issue #10: instances_launch {commands, launch_args, fps_max} can change any archived convar, not only the four the launch
-- itself sets. The bridge lists the names on the command line (-mcp_restore_convars a,b); the module hands the list to the
-- runtime as native.restore_convars and the restore covers them too.
do
	local base = A.test_client_convars
	__mcpagent_native.restore_convars = nil
	check(#A.test_client_convar_names() == #base, "Issue #10: no extra list: only the built-in convars")
	__mcpagent_native.restore_convars = "cl_showfps,FPS_MAX,bad name,cl_drawhud;x,mat_vsync,,CL_SHOWFPS"
	local names = A.test_client_convar_names()
	local seen = {}
	for _, n in ipairs(names) do seen[n] = (seen[n] or 0) + 1 end
	check(#names == #base + 2 and seen.cl_showfps == 1 and seen.mat_vsync == 1 and seen.fps_max == 1,
		"Issue #10: extra names are added once, lower case, the built-in ones stay: " .. table.concat(names, ","))
	check(not seen["bad name"] and not seen["cl_drawhud;x"], "Issue #10: a name that is not a plain convar name is dropped")
	check(#A.test_client_convars == #base, "Issue #10: the built-in list is not modified by the extras")

	-- Wiring: a convar listed by the bridge goes back to the user's value; one that is not archived is left alone.
	local log, files = {}, { ["cfg/config.cfg"] = 'cl_showfps "0"\nmat_vsync "1"\n' }
	local old_rcc, old_gci, old_cmd = RunConsoleCommand, GetConVar_Internal, game.ConsoleCommand
	local state = { cl_showfps = { "3", "0", 128 }, mat_vsync = { "1", "0", 128 }, sv_cheats = { "1", "0", 0 }, cl_drawhud = { "0", "1", 0 } }
	RunConsoleCommand = function(...) log[#log + 1] = table.concat({ ... }, " ") end
	game.ConsoleCommand = function(c) log[#log + 1] = "GAME:" .. c end
	__mcpagent_native.user_config = function() return files["cfg/config.cfg"] end
	GetConVar_Internal = function(n)
		local st = state[n]
		if not st then return nil end
		return { GetString = function() return st[1] end, GetDefault = function() return st[2] end, GetName = function() return n end,
			GetFlags = function() return st[3] end }
	end
	__mcpagent_native.test_client = true
	__mcpagent_native.restore_convars = "cl_showfps,mat_vsync,sv_cheats"
	local rc = call("restore_convars", {})
	local joined = table.concat(log, "|")
	check(rc.ok and rc.result.restored == 1 and log[1] == "cl_showfps 0", "Issue #10: the listed archived convar gets the user's value back: " .. joined)
	check(not joined:find("sv_cheats", 1, true) and not joined:find("mat_vsync", 1, true),
		"Issue #10: a convar that is not archived, or already at the user's value, is not touched: " .. joined)
	__mcpagent_native.test_client, __mcpagent_native.restore_convars, __mcpagent_native.user_config = nil, nil, nil
	RunConsoleCommand, GetConVar_Internal, game.ConsoleCommand = old_rcc, old_gci, old_cmd
end

-- Issue #9: lua_inspect of a function gives where it was defined (source, linedefined, lastlinedefined), so the C side
-- can add the real disk path and whether the file came from the mounted folder. A table or a C function gets no source.
do
	local chunk = assert(loadstring("local function f()\n  return 1\nend\nreturn f", "@lua/pba/sv_relay.lua"))
	__issue9_fn = chunk()
	local ri = call("lua_inspect", { expr = "__issue9_fn" })
	check(ri.ok and ri.result.type == "function", "Issue #9: inspect of a function: type")
	check(ri.ok and ri.result.source == "@lua/pba/sv_relay.lua" and ri.result.linedefined == 1 and ri.result.lastlinedefined == 3,
		"Issue #9: inspect of a function: source and lines")
	ri = call("lua_inspect", { expr = "print" })
	check(ri.ok and ri.result.source == nil, "Issue #9: a C function has no source")
	ri = call("lua_inspect", { expr = "{}" })
	check(ri.ok and ri.result.source == nil, "Issue #9: a table has no source")
	__issue9_fn = nil
end

-- A second Lua state (new map): reload the runtime with another generation. Its ids differ from the first state's.
local function new_state(generation)
	__mcpagent = {}
	__mcpagent_native.generation = generation
	for _, name in ipairs({ "00_json.lua", "10_core.lua", "20_lua.lua", "22_check.lua", "25_jobs.lua", "30_session.lua" }) do
		assert(loadfile(dir .. "/" .. name))()
	end
	A, H, J = __mcpagent, __mcpagent.handlers, __mcpagent.json
end
local old_id = gen_a.result.id
new_state(8)
local gen_b = call("job_start", { code = "return 2", async = true })
check(gen_b.ok and gen_b.result.id:match("^server%-g8%-%d+$") ~= nil, "new state: id carries the new generation: " .. tostring(gen_b.result and gen_b.result.id))
check(gen_b.result.id ~= old_id, "same sequence number, different state: different ids")
local stale = call("job_poll", { id = old_id })
check(not stale.ok and stale.error.code == "not_found", "the new state does not know the old state's id")

-- Issue #11: RunConsoleCommand of a command the engine blocks from Lua does nothing without a word; lua_exec / lua_job
-- now report it (blocked_console_commands + warning), only for the commands that really are blocked.
local ran = {}
local real_rcc = function(cmd, ...) ran[#ran + 1] = cmd end
RunConsoleCommand = real_rcc
IsConCommandBlocked = function(c) return c == "fps_max" or c == "volume_sfx" end
r = call("lua_exec", { code = "RunConsoleCommand('fps_max', '30') RunConsoleCommand('sv_gravity', '500') RunConsoleCommand('fps_max', '40') return 1" })
check(r.ok and type(r.result.blocked_console_commands) == "table" and #r.result.blocked_console_commands == 1
	and r.result.blocked_console_commands[1] == "fps_max", "lua_exec reports the blocked command once")
check(r.ok and type(r.result.warning) == "string" and r.result.warning:find("fps_max", 1, true) and r.result.warning:find("console_exec", 1, true),
	"lua_exec warning names the command and the tool that works")
check(#ran == 3 and ran[2] == "sv_gravity", "the calls still reach RunConsoleCommand")
check(RunConsoleCommand == real_rcc, "RunConsoleCommand is put back after the call")
r = call("lua_exec", { code = "RunConsoleCommand('sv_gravity', '500') return 1" })
check(r.ok and r.result.blocked_console_commands == nil and r.result.warning == nil, "no field when nothing was blocked")
r = call("lua_exec", { code = "RunConsoleCommand('volume_sfx', '0') error('boom')" })
check(not r.ok and RunConsoleCommand == real_rcc, "RunConsoleCommand is put back after an error")
r = run_job({ code = "RunConsoleCommand('fps_max', '30') job:frames(1) RunConsoleCommand('volume_sfx', '1') RunConsoleCommand('fps_max', '30') return 1" })
check(r.ok and r.result.state == "done" and type(r.result.blocked_console_commands) == "table" and #r.result.blocked_console_commands == 2
	and type(r.result.warning) == "string" and r.result.warning:find("volume_sfx", 1, true), "lua_job reports the blocked commands of all its resumes")
check(RunConsoleCommand == real_rcc, "RunConsoleCommand is put back between a job's resumes")
r = run_job({ code = "RunConsoleCommand('sv_gravity', '1') return 1" })
check(r.ok and r.result.blocked_console_commands == nil, "lua_job: no field when nothing was blocked")
IsConCommandBlocked = nil
r = call("lua_exec", { code = "RunConsoleCommand('fps_max', '30') return 1" })
check(r.ok and r.result.blocked_console_commands == nil, "without IsConCommandBlocked (older game) nothing is claimed")

-- Issue #3 (live fuzz): debug_draw had no "arrow" primitive. A.arrow_segments(from, to) is the shaft plus two head wings
-- as plain {x,y,z} pairs (the render hook turns them into lines): the shaft ends at the tip, the wings start at the tip
-- and lie behind it, a zero-length arrow has no head, and a vertical arrow still gets wings.
do
	local segs = A.arrow_segments({ x = 0, y = 0, z = 0 }, { x = 100, y = 0, z = 0 })
	check(type(segs) == "table" and #segs == 3, "arrow: shaft and two wings")
	local s0 = segs[1]
	check(s0.a.x == 0 and s0.b.x == 100 and s0.b.y == 0, "arrow: the shaft goes from the start to the tip")
	local w1, w2 = segs[2], segs[3]
	check(w1.a.x == 100 and w2.a.x == 100 and w1.b.x < 100 and w2.b.x < 100, "arrow: wings start at the tip and lie behind it")
	check(w1.b.y == -w2.b.y and w1.b.y ~= 0, "arrow: wings are symmetric around the shaft")
	local v = A.arrow_segments({ x = 0, y = 0, z = 0 }, { x = 0, y = 0, z = 100 })
	check(#v == 3 and (v[2].b.x ~= 0 or v[2].b.y ~= 0), "arrow: a vertical arrow still has a head")
	local z = A.arrow_segments({ x = 5, y = 5, z = 5 }, { x = 5, y = 5, z = 5 })
	check(#z == 1, "arrow: zero length has no head")
	local nan = A.arrow_segments({ x = 0, y = 0, z = 0 }, { x = 0 / 0, y = 0, z = 0 })
	check(#nan == 1, "arrow: a non-finite tip draws the shaft only")
end

-- Issue #3 (live fuzz): a clientside-only entity (ClientsideModel) has EntIndex -1 in ents_find, and ent_info refuses ids
-- below 0, so the agent had no handle: ents_find now says so in `note`. A non-finite number is written as JSON null.
do
	local fake_ents = {
		{ EntIndex = 5, class = "prop_physics" },
		{ EntIndex = -1, class = "class C_PhysPropClientside" },
	}
	local function mk(d)
		local e = { d = d }
		function e:EntIndex() return d.EntIndex end
		function e:GetClass() return d.class end
		function e:GetPos() return { x = 1, y = 2, z = 3 } end
		function e:GetAngles() return { p = 0, y = 0, r = 0 } end
		function e:GetModel() return "models/props_c17/oildrum001.mdl" end
		function e:IsPlayer() return false end
		function e:IsValid() return true end
		return e
	end
	IsValid = function(e) return e ~= nil end
	local list = {}
	for i, d in ipairs(fake_ents) do list[i] = mk(d) end
	ents = { GetAll = function() return list end }
	local f = assert(loadfile(dir .. "/60_world.lua"))
	f()
	r = call("ents_find", {})
	check(r.ok and r.result.count == 2 and r.result.entities[2].id == -1, "ents_find: a clientside entity has id -1")
	check(type(r.result.note) == "string" and r.result.note:find("clientside", 1, true) and r.result.note:find("-1", 1, true)
		and r.result.note:find("lua_exec", 1, true), "ents_find: id -1 is explained in a note (no ent_info handle)")
	r = call("ents_find", { class = "prop_physics" })
	check(r.ok and r.result.count == 1 and r.result.note == nil, "ents_find: no note when every entity has an index")
	-- A NaN or infinite number is written as null (the output schemas accept null for every number).
	local enc = J.encode({ pos = J.array({ 0 / 0, 1, math.huge, -math.huge }) })
	check(enc:find("null", 1, true) and not enc:find("nan", 1, true) and not enc:find("inf", 1, true), "non-finite numbers are written as null: " .. enc)
end
-- Issue #9 (chaos testing 2026-11-20): mount_probe asks the realm's own file system whether it finds files of the mounted
-- folder (file.Exists and file.Time with "LUA", which see the mounted folder): the module cannot know it from the folder alone.
file = { Exists = function(n, p) return p == "LUA" and n == "pba/a.lua" end,
	Time = function(n, p) if p == "LUA" and n == "pba/a.lua" then return 1234 end return 0 end }
r = call("mount_probe", { files = { { name = "pba/a.lua" }, { name = "zz/none.lua" } } })
check(r.ok and #r.result.answers == 2 and r.result.answers[1].exists == true and r.result.answers[1].time == 1234
	and r.result.answers[2].exists == false and r.result.answers[2].time == nil, "mount_probe answers exists and time per file, in order")
r = call("mount_probe", { files = "pba/a.lua" })
check(not r.ok and r.error.code == "invalid_params", "mount_probe: files must be a list")
r = call("mount_probe", { files = { { name = 5 } } })
check(not r.ok and r.error.code == "invalid_params", "mount_probe: a file name must be a string")
local many = {}
for i = 1, 40 do many[i] = { name = "x" .. i .. ".lua" } end
r = call("mount_probe", { files = many })
check(not r.ok and r.error.code == "invalid_params", "mount_probe: at most a few files (it runs on the main thread)")
file = nil
r = call("mount_probe", { files = { { name = "pba/a.lua" } } })
check(not r.ok, "mount_probe without a file library is an error, not a guess")

-- ui_click / ui_key: the state the DLL reads before it posts input (menu: game UI, loading, backbuffer size; client: cursor).
do
	local realm_before = A.realm
	local old = { gui = gui, vgui = vgui, ScrW = ScrW, ScrH = ScrH, IsInLoading = IsInLoading, IsInGame = IsInGame }
	ScrW, ScrH = function() return 1280 end, function() return 720 end
	gui = { IsGameUIVisible = function() return true end }
	vgui = { CursorVisible = function() return false end }
	IsInLoading, IsInGame = function() return false end, function() return true end
	A.realm = "menu"
	r = call("ui_state", {})
	check(r.ok and r.result.realm == "menu" and r.result.game_ui == true and r.result.loading == false and r.result.in_game == true
		and r.result.scr_w == 1280 and r.result.scr_h == 720 and r.result.cursor == nil, "ui_state (menu): game UI, loading, backbuffer size")
	gui = { IsGameUIVisible = function() return nil end }
	r = call("ui_state", {})
	check(r.ok and r.result.game_ui == false, "ui_state (menu): a nil from IsGameUIVisible is false")
	IsInLoading = function() return true end
	check(call("ui_state", {}).result.loading == true, "ui_state (menu): loading screen")
	A.realm = "client"
	r = call("ui_state", {})
	check(r.ok and r.result.cursor == false and r.result.game_ui == nil and r.result.scr_w == 1280, "ui_state (client): cursor hidden")
	vgui = { CursorVisible = function() return true end }
	check(call("ui_state", {}).result.cursor == true, "ui_state (client): cursor visible (a Derma popup is open)")
	-- An older build without these functions: no field, no error.
	gui, vgui, ScrW, ScrH, IsInLoading, IsInGame = nil, nil, nil, nil, nil, nil
	r = call("ui_state", {})
	check(r.ok and r.result.cursor == nil and r.result.scr_w == nil, "ui_state (client): missing APIs give no field")
	A.realm = "menu"
	r = call("ui_state", {})
	check(r.ok and r.result.game_ui == nil and r.result.loading == false and r.result.in_game == false, "ui_state (menu): missing APIs give no field")
	A.realm = realm_before
	gui, vgui, ScrW, ScrH, IsInLoading, IsInGame = old.gui, old.vgui, old.ScrW, old.ScrH, old.IsInLoading, old.IsInGame
end

-- Issue #23: convar_get says whether the convar is archived (FCVAR_ARCHIVE, 128) instead of leaving the agent to decode `flags`
-- (ConVar:IsFlagSet(FCVAR_ARCHIVE) is not reliable from Lua). A convar the game's config.cfg lists is archived too, even when its
-- flags lack the bit (a convar made by CreateClientConVar(..., true): live, GetFlags had no 128 and it was in config.cfg).
do
	local old_flags, old_name, old_cfg = fake_cv.GetFlags, fake_cv.GetName, A.native.user_config
	fake_cv.GetName = function() return "pba_arch" end
	fake_cv.GetFlags = function() return 128 + 8 end
	r = call("convar_get", { name = "pba_arch" })
	check(r.ok and r.result.archived == true and r.result.archived_from == "flags" and r.result.flags == 136, "convar_get: FCVAR_ARCHIVE in the flags -> archived true")
	fake_cv.GetFlags = function() return 268763144 end
	r = call("convar_get", { name = "pba_arch" })
	check(r.ok and r.result.archived == false and r.result.archived_from == nil and r.result.in_config_cfg == nil, "convar_get: no archive bit, no config.cfg -> archived false")
	A.native.user_config = function() return 'pba_other "1"\npba_arch "5"\n' end
	r = call("convar_get", { name = "pba_arch" })
	check(r.ok and r.result.archived == true and r.result.archived_from == "config.cfg" and r.result.in_config_cfg == true,
		"convar_get: no archive bit but listed in config.cfg -> archived true, from config.cfg")
	A.native.user_config = function() return 'pba_other "1"\n' end
	r = call("convar_get", { name = "pba_arch" })
	check(r.ok and r.result.archived == false and r.result.in_config_cfg == false, "convar_get: config.cfg readable and without the convar -> archived false, in_config_cfg false")
	A.native.user_config = function() error("boom") end
	r = call("convar_get", { name = "pba_arch" })
	check(r.ok and r.result.archived == false, "convar_get: an unreadable config.cfg does not break the call")
	fake_cv.GetFlags, fake_cv.GetName, A.native.user_config = old_flags, old_name, old_cfg
end

-- Issue #18 item 2: hook_list takes `name`, a case-insensitive part of the hook identifier, so "is my hook gone from Think?" does not
-- need the whole list. Events with no match are left out and `count` counts the matches.
do
	local f1, f2 = function() end, function() end
	hook = { GetTable = function() return { Think = { ["pba_tick"] = f1, ["Other_Tick"] = f2, ["x"] = f2 }, Tick = { ["PBA_other"] = f1 } } end }
	assert(loadfile(dir .. "/40_hooks.lua"))()
	r = call("hook_list", {})
	check(r.ok and r.result.count == 4, "hook_list: without a filter every hook")
	r = call("hook_list", { name = "pba" })
	check(r.ok and r.result.count == 2 and #r.result.hooks.Think == 1 and r.result.hooks.Think[1].id == "pba_tick"
		and #r.result.hooks.Tick == 1 and r.result.hooks.Tick[1].id == "PBA_other", "hook_list name: case-insensitive part of the id, in every event")
	r = call("hook_list", { name = "pba", event = "Think" })
	check(r.ok and r.result.count == 1 and r.result.hooks.Tick == nil and r.result.event == "Think", "hook_list name + event")
	r = call("hook_list", { name = "gone", event = "Think" })
	check(r.ok and r.result.count == 0 and next(r.result.hooks) == nil and r.result.known == true, "hook_list name: no match is an empty list, not an error")
	r = call("hook_list", { name = "a.b" })
	check(r.ok and r.result.count == 0, "hook_list name is plain text, not a pattern")
	hook = nil
end

-- Issue #18 item 4: ent_info tells what is knowable about the entity reaching the client: the server realm says whether the entity is
-- always sent and which players' PVS it is in; the client realm says whether it is dormant. Fields are absent when the API is.
do
	local function ent(extra)
		local e = { }
		function e:EntIndex() return 12 end
		function e:GetClass() return "npc_grenade_frag" end
		function e:GetPos() return { x = 0, y = 0, z = 0 } end
		function e:GetAngles() return { p = 0, y = 0, r = 0 } end
		function e:GetModel() return "" end
		function e:IsPlayer() return false end
		function e:GetVelocity() return { x = 0, y = 0, z = 0 } end
		function e:Health() return 0 end
		function e:GetMaxHealth() return 0 end
		function e:GetSolid() return 0 end
		function e:GetMoveType() return 0 end
		function e:GetCollisionGroup() return 0 end
		function e:OBBMins() return { x = 0, y = 0, z = 0 } end
		function e:OBBMaxs() return { x = 0, y = 0, z = 0 } end
		function e:GetParent() return nil end
		function e:GetOwner() return nil end
		function e:GetNWVarTable() return {} end
		function e:GetTable() return {} end
		function e:GetKeyValues() return {} end
		function e:GetNoDraw() return false end
		for k, v in pairs(extra or {}) do e[k] = v end
		return e
	end
	local realm0, server0, sv_ent = A.realm, SERVER, nil
	local players = { { id = 1 }, { id = 2 } }
	for _, p in ipairs(players) do function p:EntIndex() return p.id end function p:IsValid() return true end end
	player = { GetAll = function() return players end }
	Entity = function(i) if i == 12 then return sv_ent end end
	IsValid = function(e) return e ~= nil end
	game = game or {}
	game.GetWorld = game.GetWorld or function() return {} end
	assert(loadfile(dir .. "/60_world.lua"))()
	SERVER = true
	A.realm = "server"
	sv_ent = ent({ TestPVS = function(self, p) return p.id == 2 end, IsEFlagSet = function(self, f) return false end })
	EFL_FORCE_CHECK_TRANSMIT = 1
	r = call("ent_info", { id = 12 })
	check(r.ok and r.result.transmit and r.result.transmit.in_pvs_of[1] == 2 and #r.result.transmit.in_pvs_of == 1
		and r.result.transmit.always_sent == false and type(r.result.transmit.note) == "string" and r.result.transmit.note:find("client", 1, true),
		"ent_info (server): transmit lists the players whose PVS holds the entity, and points at the client realm")
	sv_ent = ent({ TestPVS = function() return false end, IsEFlagSet = function(self, f) return f == 1 end })
	r = call("ent_info", { id = 12 })
	check(r.ok and r.result.transmit.always_sent == true and #r.result.transmit.in_pvs_of == 0, "ent_info (server): FORCE_CHECK_TRANSMIT -> always_sent")
	sv_ent = ent({})
	r = call("ent_info", { id = 12 })
	check(r.ok and r.result.transmit and r.result.transmit.in_pvs_of == nil and r.result.transmit.always_sent == nil, "ent_info (server): no TestPVS / IsEFlagSet -> only the note")
	SERVER = nil
	A.realm = "client"
	sv_ent = ent({ IsDormant = function() return true end })
	r = call("ent_info", { id = 12 })
	check(r.ok and r.result.transmit and r.result.transmit.dormant == true and r.result.transmit.on_client == true
		and r.result.transmit.note:find("dormant", 1, true), "ent_info (client): dormant is reported and the entity exists on the client")
	sv_ent = ent({ IsDormant = function() return false end })
	r = call("ent_info", { id = 12 })
	check(r.ok and r.result.transmit.dormant == false, "ent_info (client): not dormant")
	SERVER, A.realm = server0, realm0
	EFL_FORCE_CHECK_TRANSMIT, player, Entity = nil, nil, nil
end

-- Issue #11 (comment): lua_check does not report the test runner's own globals (`expect`) in a file marked as a test file
-- (`test_file`, set by the tool from the path), but still reports every other unknown global there; a normal file still gets `expect`.
do
	local code = "local a = expect(1)\nlocal b = prnit(2)\nreturn a, b"
	r = call("lua_check", { items = { { file = "t.lua", chunk = "@t.lua", code = code, test_file = true } } })
	local names = {}
	for _, u in ipairs(r.result.unknown_globals or {}) do names[u.name] = true end
	check(r.ok and names.prnit and not names.expect, "lua_check: expect is known in a test file, a typo is still reported")
	check(type(r.result.notes) == "table" and tostring(r.result.notes[1]):find("expect", 1, true), "lua_check: a note says which test globals were not reported")
	r = call("lua_check", { items = { { file = "t.lua", chunk = "@t.lua", code = code } } })
	names = {}
	for _, u in ipairs(r.result.unknown_globals or {}) do names[u.name] = true end
	check(r.ok and names.prnit and names.expect and r.result.notes == nil, "lua_check: expect stays unknown in a normal file")
end

print(string.format("runtime_test: %d passed, %d failed", passes, fails))
os.exit(fails == 0 and 0 or 1)
