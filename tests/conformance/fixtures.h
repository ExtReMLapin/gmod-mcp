// Fixtures of the official MCP conformance suite (@modelcontextprotocol/conformance): the tools, prompts and resources
// its scenarios call by name. They exist ONLY in the gmod-mcp-conformance-host executable (tests/conformance/): never in
// src/, the module DLL, the bridge or any release package.
#pragma once

namespace conformance {

// Registers the fixture tools (mcp::register_tool) and installs the request hooks (mcp::set_request_hooks) that serve
// the fixture tool results, prompts and resources. Call once, after tools::register_all().
void register_fixtures();

}  // namespace conformance
