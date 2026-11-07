-- Scenario TESTS-02: one assertion failure and one runtime error, next to a pass.
return {
	groupName = "gmod-mcp: self-test (intentional failures)",
	cases = {
		{ name = "passes", func = function() expect(true).to.beTrue() end },
		{ name = "fails", func = function() expect(1 + 1).to.equal(3) end },
		{ name = "error", func = function() local t = nil; return t.field end },
	},
}
