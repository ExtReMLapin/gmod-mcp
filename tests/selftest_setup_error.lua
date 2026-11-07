-- Scenario TESTS-03: error in beforeAll -> every case errors, with the setup message.
return {
	groupName = "gmod-mcp: self-test (setup error)",
	beforeAll = function() error("setup failed") end,
	cases = {
		{ name = "a", func = function() expect(1).to.equal(1) end },
		{ name = "b", func = function() expect(2).to.equal(2) end },
	},
}
