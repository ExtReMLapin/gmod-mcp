-- Scenario TESTS-01: everything passes.
return {
	groupName = "gmod-mcp: self-test (pass)",
	beforeAll = function(state) state.base = 40 end,
	beforeEach = function(state) state.n = (state.n or 0) + 1 end,
	cases = {
		{ name = "equality", func = function(state) expect(state.base + 2).to.equal(42) end },
		{ name = "types", func = function() expect("x").to.beA("string") expect(nil).to.beNil() end },
		{ name = "negation", func = function() expect(1).notTo.equal(2) end },
		{ name = "expected error", func = function() expect(error, "boom").to.errWith("boom") end },
		{ name = "vectors", func = function() expect(Vector(1, 2, 3)).to.aboutEqual(Vector(1, 2, 3.0000001), 0.001) end },
		{ name = "skipped", skip = true, func = function() error("must not run") end },
	},
}
