-- gmod-mcp: test runner using the GLuaTest format (groupName, cases, expect).
local A = __mcpagent
local J = A.json
local H = A.handlers

local EXPECT_FAIL = {}

local function fail_expect(msg, level)
	error(setmetatable({ message = msg }, EXPECT_FAIL), level or 3)
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
			if not cond then fail_expect("expected " .. fmt(subject) .. (negate and " not " or " ") .. desc, 4) end
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

local function line_of(msg)
	local l = string.match(tostring(msg), ":(%d+):")
	return l and tonumber(l) or J.null
end

local function run_case(group, case, state, budget)
	local r = { name = case.name or "?", status = "pass" }
	if case.skip or case.when == false then
		r.status = "skip"
		return r
	end
	local t0 = SysTime()
	local function step(f, label)
		if not f then return true end
		local ok, _, err, tb = A.run_budget(f, case.timeout or budget, state)
		if ok then return true end
		if getmetatable(err) == EXPECT_FAIL then
			r.status = "fail"
			r.message = err.message
		else
			r.status = "error"
			r.message = (label and (label .. ": ") or "") .. tostring(err)
			r.traceback = tb
		end
		r.line = line_of(r.traceback or r.message)
		return false
	end
	if step(group.beforeEach, "beforeEach") then
		step(case.func, nil)
		step(group.afterEach, "afterEach")
	end
	r.duration_ms = (SysTime() - t0) * 1000
	return r
end

function H.tests_run(args)
	local results = J.array({})
	local totals = { pass = 0, fail = 0, error = 0, skip = 0 }
	local budget = args.budget or 10
	local old_expect = expect
	expect = make_expect
	for _, f in ipairs(args.files or {}) do
		local fr = { file = f.name, cases = J.array({}) }
		local fn, cerr = A.compile(f.source, f.chunkname or ("@" .. f.name))
		if not fn then
			fr.error = "syntax: " .. tostring(cerr)
			totals.error = totals.error + 1
		else
			local ok, res, err, tb = A.run_budget(fn, budget)
			local group = ok and res[1] or nil
			if not ok then
				fr.error = "load: " .. tostring(err)
				fr.traceback = tb
				totals.error = totals.error + 1
			elseif type(group) ~= "table" or type(group.cases) ~= "table" then
				fr.error = "invalid format: the file must return { groupName = ..., cases = { ... } }"
				totals.error = totals.error + 1
			else
				fr.group = group.groupName or f.name
				local state = {}
				local setup_ok, setup_err = true, nil
				if group.beforeAll then
					local ok2, _, e2, tb2 = A.run_budget(group.beforeAll, budget, state)
					if not ok2 then setup_ok, setup_err = false, tostring(e2) .. (tb2 and ("\n" .. tb2) or "") end
				end
				for i, case in ipairs(group.cases) do
					local r
					if not setup_ok then
						r = { name = case.name or ("case " .. i), status = "error", message = "beforeAll: " .. setup_err }
					else
						r = run_case(group, case, state, budget)
					end
					totals[r.status] = totals[r.status] + 1
					fr.cases[#fr.cases + 1] = r
				end
				if group.afterAll and setup_ok then
					local ok3, _, e3 = A.run_budget(group.afterAll, budget, state)
					if not ok3 then fr.afterAll_error = tostring(e3) end
				end
				if not setup_ok then fr.setup_error = setup_err end
			end
		end
		results[#results + 1] = fr
	end
	expect = old_expect
	return { files = results, totals = totals, ok = totals.fail == 0 and totals.error == 0 }
end
