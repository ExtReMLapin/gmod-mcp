// How to tell an answer of a gmod-mcp server from the answer of a foreign program that holds the same port.
// Shared by the module (coordinator registration) and the bridge. Header only.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace gmcp {

// Every response of the module carries this header (value: the module version).
inline constexpr const char *kSignatureHeader = "X-Gmod-Mcp";

// True when an HTTP answer came from a gmod-mcp server. `has_signature` is the presence of kSignatureHeader. Modules up to
// 0.3.6 send no header, so the body is checked too: a JSON-RPC message, or the module's own rejection
// ({"error": {"code": "<string code>", ...}}, e.g. "unauthorized" for a missing or wrong token).
inline bool is_gmod_mcp_reply(bool has_signature, const std::string &body) {
    if (has_signature) return true;
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) return false;
    if (j.contains("jsonrpc") && j["jsonrpc"].is_string()) return true;
    return j.contains("error") && j["error"].is_object() && j["error"].contains("code") && j["error"]["code"].is_string();
}

}  // namespace gmcp
