// MCP wire constants shared by the module and the bridge: protocol versions, _meta keys, JSON-RPC error codes.
// One place, so the two sides cannot drift apart.
#pragma once

namespace mcpproto {

// Protocol versions. kModern is the stateless era (per-request _meta, server/discover); the legacy ones use the
// initialize handshake. kLegacy[0] is the one offered when a legacy client asks for an unknown version.
inline constexpr const char *kModern = "2026-07-28";
inline constexpr const char *kLegacy[] = {"2025-11-25", "2025-06-18", "2025-03-26"};

// _meta keys (2026-07-28 base protocol).
inline constexpr const char *kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
inline constexpr const char *kMetaClientInfo = "io.modelcontextprotocol/clientInfo";
inline constexpr const char *kMetaClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
inline constexpr const char *kMetaServerInfo = "io.modelcontextprotocol/serverInfo";
inline constexpr const char *kMetaSubscriptionId = "io.modelcontextprotocol/subscriptionId";

// JSON-RPC error codes: the JSON-RPC 2.0 ones, then those MCP defines.
namespace err {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kResourceNotFoundLegacy = -32002;  // before 2026-07-28 (now kInvalidParams)
inline constexpr int kHeaderMismatch = -32020;
inline constexpr int kMissingRequiredClientCapability = -32021;
inline constexpr int kUnsupportedProtocolVersion = -32022;
}  // namespace err

}  // namespace mcpproto
