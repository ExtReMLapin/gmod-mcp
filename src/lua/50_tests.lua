-- gmod-mcp: test runner using the GLuaTest format (groupName, cases, expect).
local A = __mcpagent
local J = A.json
local H = A.handlers

-- Expectation failures and skips are raised as plain strings ("file.lua:12: expected 2 to equal 3"), so that
-- assert(ok, err) and error(err) around an expect() keep the message (a table error made assert fail with "string
-- expected, got table"). The runner recognises them by the text, through this registry (reset before every step).
local self_src = debug.getinfo(1, "S").source
local marks = {}

-- "short_src:line:" of the first frame above the runner's own functions (the test's line), and the bare line.
local function caller_pos()
	for level = 2, 20 do
		local info = debug.getinfo(level, "Sl")
		if not info then return nil end
		if info.source ~= self_src and info.what ~= "C" then
			if info.currentline and info.currentline > 0 then return info.short_src .. ":" .. info.currentline .. ":", info.currentline end
			return nil
		end
	end
end

local function raise_mark(kind, msg)
	local pos, line = caller_pos()
	local text = pos and (pos .. " " .. msg) or msg
	marks[text] = { kind = kind, message = msg, line = line }
	error(text, 0)
end

local function fail_expect(msg) raise_mark("fail", msg) end

-- The mark of an error value: exact text first, then a registered text contained in it (a rethrow with a prefix).
local function find_mark(err)
	if type(err) ~= "string" then return nil end
	local m = marks[err]
	if m then return m end
	for text, mk in pairs(marks) do
		if string.find(err, text, 1, true) then return mk end
	end
end

local function fmt(v)
	if type(v) == "string" then return string.format("%q", v) end
	return tostring(v)
end

-- Builds expect(subject, ...) with .to / .notTo / .toNot.
local function make_expect(subject, ...)
	local extra = { n = select("#", ...), ... }
	local function build(negate)
		local m = {}
		local function check(cond, desc)
			if negate then cond = not cond end
			if not cond then fail_expect("expected " .. fmt(subject) .. (negate and " not " or " ") .. desc) end
		end
		function m.equal(x) check(subject == x, "to equal " .. fmt(x)) end
		m.eq = m.equal
		function m.beTrue() check(subject == true, "to be true") end
		function m.beFalse() check(subject == false, "to be false") end
		function m.beNil() check(subject == nil, "to be nil") end
		function m.exist() check(subject ~= nil, "to exist") end
		function m.beA(t) check(type(subject) == t, "to be a " .. t .. " (is " .. type(subject) .. ")") end
		m.beOfType = m.beA
		function m.beValid() check(IsValid(subject), "to be valid") end
		function m.beInvalid() check(not IsValid(subject), "to be invalid") end
		function m.beGreaterThan(x) check(subject > x, "to be greater than " .. fmt(x)) end
		function m.beLessThan(x) check(subject < x, "to be less than " .. fmt(x)) end
		function m.beBetween(a, b) check(subject >= a and subject <= b, "to be between " .. fmt(a) .. " and " .. fmt(b)) end
		function m.aboutEqual(x, tol)
			tol = tol or 1e-5
			local d
			if type(subject) == "Vector" then d = subject:Distance(x) else d = math.abs(subject - x) end
			check(d <= tol, "to be about " .. fmt(x) .. " (+/-" .. tol .. ")")
		end
		function m.haveLength(n) check(#subject == n, "to have length " .. n .. " (has " .. #subject .. ")") end
		function m.contain(v)
			local found = false
			if type(subject) == "string" then found = string.find(subject, v, 1, true) ~= nil
			else for _, x in pairs(subject) do if x == v then found = true break end end end
			check(found, "to contain " .. fmt(v))
		end
		local function call_subject() return pcall(subject, unpack(extra, 1, extra.n)) end
		function m.err()
			local ok = call_subject()
			check(not ok, "to error")
		end
		function m.errWith(msg)
			local ok, e = call_subject()
			local matches = not ok and string.find(tostring(e), msg, 1, true) ~= nil
			check(matches, "to error with " .. fmt(msg) .. " (got " .. fmt(ok and "no error" or e) .. ")")
		end
		function m.succeed()
			local ok, e = call_subject()
			check(ok, "to succeed (error: " .. tostring(e) .. ")")
		end
		return m
	end
	return { to = build(false), notTo = build(true), toNot = build(true) }
end

-- `expect` is callable (expect(x).to...) and has expect.skip(reason): the case stops and shows as skipped, with the reason.
local expect_api = setmetatable({}, { __call = function(_, ...) return make_expect(...) end })
function expect_api.skip(reason) raise_mark("skip", reason and tostring(reason) or "skipped") end

local function line_of(msg)
	local l = string.match(tostring(msg), ":(%d+):")
	return l and tonumber(l) or J.null
end

local MAX_OUTPUT = 4096
local function cut_output(out)
	if #out <= MAX_OUTPUT then return out end
	return string.sub(out, 1, MAX_OUTPUT) .. string.format("\n[mcpagent: output truncated, %d more bytes]\n", #out - MAX_OUTPUT)
end

-- opts: output = "failures" (default) | "all" | "none".
local function run_case(group, case, state, budget, opts)
	local r = { name = case.name or "?", status = "pass" }
	if case.skip or case.when == false then
		r.status = "skip"
		if type(case.skip) == "string" then r.message = case.skip end
		return r
	end
	-- A case that times something opts out of the budget: the count hook makes LuaJIT interpret (timings 3-40x too slow).
	local b = case.timeout
	if b == nil then b = budget end
	if case.timing == true or b == 0 then b = nil end
	local capture = opts.output ~= "none"
	if capture then A.capture_begin() end
	local t0 = SysTime()
	local function step(f, label)
		if not f then return true end
		marks = {}
		local ok, _, err, tb = A.run_budget(f, b, state)
		if ok then return true end
		local mark = find_mark(err)
		if mark and mark.kind == "skip" then
			r.status = "skip"
			r.message = mark.message
			r.line = mark.line
		elseif mark then
			r.status = "fail"
			r.message = mark.message
			r.line = mark.line
		else
			r.status = "error"
			r.message = (label and (label .. ": ") or "") .. A.error_text(err)
			r.traceback = tb
			r.line = line_of(r.traceback or r.message)
		end
		return false
	end
	if step(group.beforeEach, "beforeEach") then
		step(case.func, nil)
		step(group.afterEach, "afterEach")
	end
	r.duration_ms = (SysTime() - t0) * 1000
	if capture then
		local out = A.capture_end()
		if out ~= "" and (opts.output == "all" or r.status == "fail" or r.status == "error") then r.output = cut_output(out) end
	end
	return r
end

local function matches_filter(case, needle)
	return needle == nil or string.find(string.lower(tostring(case.name or "")), needle, 1, true) ~= nil
end

function H.tests_run(args)
	local results = J.array({})
	local totals = { pass = 0, fail = 0, error = 0, skip = 0 }
	local budget = args.budget or 10
	if budget <= 0 then budget = nil end
	local needle = type(args.case) == "string" and args.case ~= "" and string.lower(args.case) or nil
	local opts = { output = args.output or "failures", only_failures = args.only_failures == true }
	local old_expect = expect
	expect = expect_api
	for _, f in ipairs(args.files or {}) do
		local fr = { file = f.name, cases = J.array({}) }
		local counts = { pass = 0, fail = 0, error = 0, skip = 0 }
		local fn, cerr = A.compile(f.source, f.chunkname or ("@" .. f.name))
		if not fn then
			fr.error = "syntax: " .. tostring(cerr)
			totals.error = totals.error + 1
		else
			marks = {}
			local ok, res, err, tb = A.run_budget(fn, budget)
			local group = ok and res[1] or nil
			if not ok then
				fr.error = "load: " .. A.error_text(err)
				fr.traceback = tb
				totals.error = totals.error + 1
			elseif type(group) ~= "table" or type(group.cases) ~= "table" then
				fr.error = "invalid format: the file must return { groupName = ..., cases = { ... } }"
				totals.error = totals.error + 1
			else
				fr.group = group.groupName or f.name
				local state = {}  -- one table per file, handed to every step; NOT reset between cases
				local selected = {}
				for i, case in ipairs(group.cases) do
					if matches_filter(case, needle) then selected[#selected + 1] = { case = case, index = i } end
				end
				if needle then fr.filtered = #group.cases - #selected end
				local setup_ok, setup_err = true, nil
				if group.beforeAll and #selected > 0 then
					marks = {}
					local ok2, _, e2, tb2 = A.run_budget(group.beforeAll, budget, state)
					if not ok2 then setup_ok, setup_err = false, A.error_text(e2) .. (tb2 and ("\n" .. tb2) or "") end
				end
				for _, sel in ipairs(selected) do
					local r
					if not setup_ok then
						r = { name = sel.case.name or ("case " .. sel.index), status = "error", message = "beforeAll: " .. setup_err }
					else
						r = run_case(group, sel.case, state, budget, opts)
					end
					totals[r.status] = totals[r.status] + 1
					counts[r.status] = counts[r.status] + 1
					if not opts.only_failures or r.status == "fail" or r.status == "error" then fr.cases[#fr.cases + 1] = r end
				end
				if group.afterAll and setup_ok and #selected > 0 then
					marks = {}
					local ok3, _, e3 = A.run_budget(group.afterAll, budget, state)
					if not ok3 then fr.afterAll_error = A.error_text(e3) end
				end
				if not setup_ok then fr.setup_error = setup_err end
			end
		end
		fr.totals = counts
		results[#results + 1] = fr
	end
	expect = old_expect
	return { files = results, totals = totals, ok = totals.fail == 0 and totals.error == 0 }
end
