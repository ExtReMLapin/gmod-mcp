// Fixtures of the official MCP conformance suite. See fixtures.h: test-only, compiled into gmod-mcp-conformance-host only.
//
// The tools are registered in the module's own tool registry (so tools/list, stream selection and the Tasks store are the
// module's), and the request hooks (mcp::set_request_hooks) serve their results: the suite checks raw MCP content
// (text, image, audio, embedded resources, input_required results) which the module's tools never return.
// The behaviour follows the suite's reference fixture server (examples/servers/typescript/everything-server.ts of
// modelcontextprotocol/conformance) and the scenario sources at the pinned version.
#include "fixtures.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "agent.h"
#include "mcp.h"
#include "mcp_protocol.h"
#include "platform/platform.h"
#include "reqctx.h"
#include "tasks.h"
#include "util.h"

namespace conformance {

using json = nlohmann::json;
namespace perr = mcpproto::err;

// 1x1 red PNG and a minimal WAV (the suite's own samples).
static const char *kImageB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";
static const char *kAudioB64 = "UklGRiYAAABXQVZFZm10IBAAAAABAAEAQB8AAAB9AAACABAAZGF0YQIAAAA=";

// ---------------------------------------------------------------------------------------------------------------------
// HMAC-SHA256 (signed requestState of the tampered-state scenario).
// ---------------------------------------------------------------------------------------------------------------------
namespace {

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t len = 0;
    unsigned char buf[64];
    size_t n = 0;

    static uint32_t rotr(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }

    void block(const unsigned char *p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }

    void update(const void *data, size_t size) {
        const unsigned char *p = static_cast<const unsigned char *>(data);
        len += size;
        while (size > 0) {
            size_t k = std::min(size, sizeof buf - n);
            std::memcpy(buf + n, p, k);
            n += k, p += k, size -= k;
            if (n == sizeof buf) block(buf), n = 0;
        }
    }

    std::string finish() {
        uint64_t bits = len * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (n != 56) update(&zero, 1);
        unsigned char l[8];
        for (int i = 0; i < 8; ++i) l[i] = (unsigned char)(bits >> (56 - 8 * i));
        update(l, 8);
        std::string out(32, '\0');
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = (char)(h[i] >> (24 - 8 * j));
        return out;
    }
};

std::string hmac_sha256_hex(const std::string &key, const std::string &data) {
    unsigned char k[64] = {0};
    std::memcpy(k, key.data(), std::min<size_t>(key.size(), 64));  // keys here are shorter than a block
    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) ipad[i] = k[i] ^ 0x36, opad[i] = k[i] ^ 0x5c;
    Sha256 in;
    in.update(ipad, 64);
    in.update(data.data(), data.size());
    std::string inner = in.finish();
    Sha256 out;
    out.update(opad, 64);
    out.update(inner.data(), inner.size());
    std::string d = out.finish();
    return util::hex(reinterpret_cast<const unsigned char *>(d.data()), d.size());
}

std::string random_hex(size_t bytes) {
    std::string raw(bytes, '\0');
    plat::random_bytes(reinterpret_cast<unsigned char *>(&raw[0]), bytes);
    return util::hex(reinterpret_cast<const unsigned char *>(raw.data()), bytes);
}

const std::string &state_secret() {
    static const std::string s = "conformance-mrtr-secret-" + random_hex(16);
    return s;
}

// requestState protected against tampering: {"data": "<json>", "hmac": "<hex>"}.
std::string sign_state(const json &payload) {
    std::string data = payload.dump();
    return json{{"data", data}, {"hmac", hmac_sha256_hex(state_secret(), data)}}.dump();
}

bool verify_state(const std::string &raw, json &out) {
    try {
        json j = json::parse(raw);
        std::string data = j.at("data").get<std::string>();
        if (j.at("hmac").get<std::string>() != hmac_sha256_hex(state_secret(), data)) return false;
        out = json::parse(data);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Result builders
// ---------------------------------------------------------------------------------------------------------------------
static json text_item(const std::string &t) { return json{{"type", "text"}, {"text", t}}; }

static json content_result(json content, bool is_error = false) {
    json r{{"content", std::move(content)}};
    if (is_error) r["isError"] = true;
    return r;
}

static json text_result(const std::string &t, bool is_error = false) { return content_result(json::array({text_item(t)}), is_error); }

static json elicit_request(const std::string &message, const json &properties, const json &required) {
    return json{{"method", "elicitation/create"},
                {"params", {{"message", message}, {"requestedSchema", {{"type", "object"}, {"properties", properties}, {"required", required}}}}}};
}

static json sampling_request(const std::string &text, int max_tokens) {
    return json{{"method", "sampling/createMessage"},
                {"params", {{"messages", json::array({json{{"role", "user"}, {"content", text_item(text)}}})}, {"maxTokens", max_tokens}}}};
}

static json input_required(json requests, const std::string &state = "") {
    json r{{"resultType", "input_required"}, {"inputRequests", std::move(requests)}};
    if (!state.empty()) r["requestState"] = state;
    return r;
}

// Text of a field of an elicitation answer (inputResponses[key] = {action, content:{field}}).
static std::string answer_text(const json &responses, const std::string &key, const std::string &field) {
    if (!responses.is_object() || !responses.contains(key)) return "unknown";
    const json &c = responses[key].value("content", json::object());
    return c.is_object() && c.contains(field) && c[field].is_string() ? c[field].get<std::string>() : "unknown";
}

static bool has_response(const json &responses, const std::string &key) {
    return responses.is_object() && responses.contains(key) && !responses[key].is_null();
}

// One request to a fixture tool, prompt or resource.
struct Call {
    const json &params;
    const json &args;
    const json &caps;  // capabilities the client declared in this request (2026-07-28)
    bool modern;
    const mcp::Emit *emit;
    json &result;
    json &error;
};

static void fail(Call &c, int code, const std::string &msg, int http_status = 0, json data = nullptr) {
    c.error = json{{"code", code}, {"message", msg}};
    if (!data.is_null()) c.error["data"] = std::move(data);
    if (http_status) c.error["httpStatus"] = http_status;
}

static bool tasks_extension(const Call &c) {
    return c.modern && c.caps.is_object() && c.caps.contains("extensions") && c.caps["extensions"].is_object() &&
           c.caps["extensions"].contains(tasks::kExtensionId);
}

// Starts the module's own task (tasks::create_tool_task) for a fixture tool, or runs it in place for a client that did
// not declare the Tasks extension.
static void run_as_task(Call &c, const std::string &name, const json &args, bool required = false) {
    if (!tasks_extension(c)) {
        // A tool whose task support is `required` cannot serve a client without the extension (-32021, SEP-2663).
        if (required && c.modern)
            return fail(c, perr::kMissingRequiredClientCapability, name + " needs the client capability extensions." + tasks::kExtensionId, 400,
                        json{{"requiredCapabilities", {{"extensions", {{tasks::kExtensionId, json::object()}}}}}});
        c.result = mcp::call_tool(name, args);
        return;
    }
    json r = tasks::create_tool_task(name, args, json::object(), tasks::Era::Modern);
    if (r.is_object() && r.contains("__rpc_error")) fail(c, r["__rpc_error"]["code"].get<int>(), r["__rpc_error"]["message"].get<std::string>());
    else c.result = std::move(r);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------------------------------------------------
using ToolFn = std::function<void(Call &)>;
static std::map<std::string, ToolFn> g_tool_fns;

static const json kEmptySchema = {{"type", "object"}, {"properties", json::object()}};

static void add_tool(const std::string &name, const std::string &description, json schema, ToolFn fn,
                     mcp::Handler handler = nullptr, const std::string &task_support = "") {
    mcp::Tool t;
    t.name = name;
    t.title = name;
    t.description = description;
    t.input_schema = std::move(schema);
    t.local_only = true;  // no multi-instance `instance` argument on a test fixture
    t.handler = handler ? handler : [name](const json &) { return agent::err("internal", "answered by the host's request hooks: " + name); };
    mcp::register_tool(std::move(t));
    if (!task_support.empty()) mcp::set_task_support(name, task_support);
    g_tool_fns[name] = std::move(fn);
}

static ToolFn fixed(json content, bool is_error = false) {
    return [content = std::move(content), is_error](Call &c) { c.result = content_result(content, is_error); };
}

// MRTR (SEP-2322) fixtures; `elicit_only` tools wait for inputResponses[key] and answer with `done`.
static void register_mrtr() {
    add_tool("test_input_required_result_elicitation", "MRTR: returns InputRequiredResult with an elicitation request", kEmptySchema, [](Call &c) {
        json resp = c.params.value("inputResponses", json::object());
        if (has_response(resp, "user_name")) {
            c.result = text_result("Hello, " + answer_text(resp, "user_name", "name") + "!");
            return;
        }
        c.result = input_required(json{{"user_name", elicit_request("What is your name?", json{{"name", {{"type", "string"}}}}, json::array({"name"}))}});
    });
    add_tool("test_input_required_result_sampling", "MRTR: returns InputRequiredResult with a sampling request", kEmptySchema, [](Call &c) {
        json resp = c.params.value("inputResponses", json::object());
        if (has_response(resp, "sample_request")) {
            const json &content = resp["sample_request"].value("content", json::object());
            std::string t = content.is_object() && content.contains("text") && content["text"].is_string() ? content["text"].get<std::string>() : "no response";
            c.result = text_result("Sampling result: " + t);
            return;
        }
        c.result = input_required(json{{"sample_request", sampling_request("What is the capital of France?", 100)}});
    });
    add_tool("test_input_required_result_list_roots", "MRTR: returns InputRequiredResult with a roots/list request", kEmptySchema, [](Call &c) {
        json resp = c.params.value("inputResponses", json::object());
        if (has_response(resp, "roots_request")) {
            const json &roots = resp["roots_request"].value("roots", json::array());
            c.result = text_result("Found " + std::to_string(roots.is_array() ? roots.size() : 0) + " root(s)");
            return;
        }
        c.result = input_required(json{{"roots_request", json{{"method", "roots/list"}, {"params", json::object()}}}});
    });
    add_tool("test_input_required_result_request_state", "MRTR: returns InputRequiredResult with requestState", kEmptySchema, [](Call &c) {
        json resp = c.params.value("inputResponses", json::object());
        std::string state = c.params.value("requestState", "");
        if (!state.empty() && has_response(resp, "confirm")) {
            try {
                json st = json::parse(state);
                const json &content = resp["confirm"].value("content", json::object());
                if (st.value("kind", "") == "request-state" && content.is_object() && content.value("ok", false) == true) {
                    c.result = text_result("state-ok: requestState validated");
                    return;
                }
            } catch (...) {
            }
        }
        c.result = input_required(json{{"confirm", elicit_request("Please confirm", json{{"ok", {{"type", "boolean"}}}}, json::array({"ok"}))}},
                                  json{{"kind", "request-state"}, {"nonce", random_hex(8)}}.dump());
    });
    add_tool("test_input_required_result_multiple_inputs", "MRTR: returns InputRequiredResult with several input requests", kEmptySchema, [](Call &c) {
        json resp = c.params.value("inputResponses", json::object());
        std::string state = c.params.value("requestState", "");
        if (!state.empty() && has_response(resp, "user_name") && has_response(resp, "greeting") && has_response(resp, "client_roots")) {
            try {
                if (json::parse(state).value("kind", "") == "multiple-inputs") {
                    const json &gc = resp["greeting"].value("content", json::object());
                    std::string greeting = gc.is_object() && gc.contains("text") && gc["text"].is_string() ? gc["text"].get<std::string>() : "Hello there!";
                    const json &roots = resp["client_roots"].value("roots", json::array());
                    c.result = text_result("Name: " + answer_text(resp, "user_name", "name") + "; Greeting: " + greeting +
                                           "; Roots: " + std::to_string(roots.is_array() ? roots.size() : 0));
                    return;
                }
            } catch (...) {
            }
        }
        c.result = input_required(json{{"user_name", elicit_request("What is your name?", json{{"name", {{"type", "string"}}}}, json::array({"name"}))},
                                       {"greeting", sampling_request("Generate a greeting", 50)},
                                       {"client_roots", json{{"method", "roots/list"}, {"params", json::object()}}}},
                                  json{{"kind", "multiple-inputs"}, {"nonce", random_hex(8)}}.dump());
    });
    add_tool("test_input_required_result_multi_round", "MRTR: multi-round InputRequiredResult workflow", kEmptySchema, [](Call &c) {
        json resp = c.params.value("inputResponses", json::object());
        std::string state = c.params.value("requestState", "");
        auto step1 = [&] {
            c.result = input_required(json{{"step1", elicit_request("Step 1: What is your name?", json{{"name", {{"type", "string"}}}}, json::array({"name"}))}},
                                      json{{"round", 1}, {"nonce", random_hex(8)}}.dump());
        };
        if (state.empty()) return step1();
        json st;
        try {
            st = json::parse(state);
        } catch (...) {
            return step1();
        }
        if (st.value("round", 0) == 1 && has_response(resp, "step1")) {
            c.result = input_required(json{{"step2", elicit_request("Step 2: What is your favorite color?", json{{"color", {{"type", "string"}}}}, json::array({"color"}))}},
                                      json{{"round", 2}, {"name", answer_text(resp, "step1", "name")}, {"nonce", random_hex(8)}}.dump());
            return;
        }
        if (st.value("round", 0) == 2 && has_response(resp, "step2")) {
            std::string who = st.contains("name") && st["name"].is_string() ? st["name"].get<std::string>() : "friend";
            c.result = text_result("Multi-round complete for " + who + " who likes " + answer_text(resp, "step2", "color"));
            return;
        }
        step1();
    });
    add_tool("test_input_required_result_tampered_state", "MRTR: HMAC-signed requestState integrity test", kEmptySchema, [](Call &c) {
        json resp = c.params.value("inputResponses", json::object());
        std::string state = c.params.value("requestState", "");
        if (!state.empty()) {
            json v;
            if (!verify_state(state, v)) return fail(c, perr::kInvalidParams, "requestState integrity check failed");
            if (v.value("kind", "") == "tamper-test" && has_response(resp, "confirm")) {
                c.result = text_result("integrity-ok: state verified");
                return;
            }
        }
        c.result = input_required(json{{"confirm", elicit_request("Please confirm", json{{"ok", {{"type", "boolean"}}}}, json::array({"ok"}))}},
                                  sign_state(json{{"kind", "tamper-test"}, {"nonce", random_hex(8)}}));
    });
    add_tool("test_input_required_result_capabilities", "MRTR: respects the client capabilities in inputRequests", kEmptySchema, [](Call &c) {
        json resp = c.params.value("inputResponses", json::object());
        json requests = json::object();
        if (c.caps.is_object() && c.caps.contains("elicitation"))
            requests["elicit_input"] = elicit_request("Elicitation input", json{{"value", {{"type", "string"}}}}, json::array({"value"}));
        if (c.caps.is_object() && c.caps.contains("sampling")) requests["sample_input"] = sampling_request("Sample request", 50);
        if (resp.is_object() && !resp.empty()) {
            std::string keys;
            for (auto it = resp.begin(); it != resp.end(); ++it) keys += (keys.empty() ? "" : ",") + it.key();
            c.result = text_result("capabilities-ok: received " + keys);
            return;
        }
        if (requests.empty()) {
            c.result = text_result("No supported capabilities declared");
            return;
        }
        c.result = input_required(requests, sign_state(json{{"kind", "capabilities-test"}, {"nonce", random_hex(8)}}));
    });
}

// Server-initiated requests (sampling/elicitation on the tool call's stream) belong to the 2025-11-25 era: this module
// does not send requests to the client (the 2026-07-28 replacement is input_required, above). The fixtures exist and
// say so; the scenarios that need the request itself are listed in expected-failures.json.
static ToolFn no_server_requests(const std::string &what) {
    return [what](Call &c) { c.result = text_result(what + " not supported: this server never sends requests to the client", true); };
}

static void register_tasks_fixtures() {
    // slow_compute: sleeps `seconds`, then a result; settles to cancelled when tasks/cancel arrives while it runs.
    json slow_schema = {{"type", "object"},
                        {"properties", {{"seconds", {{"type", "number"}}}, {"label", {{"type", "string"}}}}}};
    add_tool(
        "slow_compute", "Task-supporting tool: sleeps `seconds` seconds, then returns a result.", slow_schema,
        [](Call &c) { run_as_task(c, "slow_compute", c.args); },
        [](const json &a) {
            double seconds = a.value("seconds", 1.0);
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((int64_t)(seconds * 1000));
            while (std::chrono::steady_clock::now() < end) {
                if (reqctx::cancelled()) return agent::err("cancelled", "cancelled");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return json{{"ok", true}, {"result", {{"message", "Computed " + a.value("label", std::string("job")) + " after " + std::to_string(seconds) + " seconds"}}}};
        },
        "optional");
    add_tool(
        "failing_job", "Task-required tool (task support `required`): always returns a tool execution error after about one second.", kEmptySchema,
        [](Call &c) { run_as_task(c, "failing_job", c.args, true); },
        [](const json &) {
            for (int i = 0; i < 20 && !reqctx::cancelled(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return agent::err("job_failed", "failing_job always fails");
        },
        "optional");
    add_tool(
        "protocol_error_job", "Task-supporting tool: fails with a protocol-level error.", kEmptySchema,
        [](Call &c) { run_as_task(c, "protocol_error_job", c.args); },
        [](const json &) -> json { throw std::runtime_error("protocol_error_job: internal failure"); }, "optional");
    json file_schema = {{"type", "object"}, {"properties", {{"filename", {{"type", "string"}}}}}};
    add_tool(
        "confirm_delete", "Task-supporting tool: asks for a confirmation before deleting `filename`.", file_schema,
        [](Call &c) { run_as_task(c, "confirm_delete", c.args); },
        [](const json &a) { return json{{"ok", true}, {"result", {{"message", "deleted " + a.value("filename", std::string("file"))}}}}; }, "optional");
    add_tool(
        "multi_input", "Task-supporting tool: asks for two inputs at once.", file_schema,
        [](Call &c) { run_as_task(c, "multi_input", c.args); },
        [](const json &a) { return json{{"ok", true}, {"result", {{"message", "multi_input done"}}}}; }, "optional");
    json greet_schema = {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}};
    add_tool("greet", "Sync-only tool: returns `Hello, {name}!`.", greet_schema,
             [](Call &c) { c.result = text_result("Hello, " + c.args.value("name", std::string("World")) + "!"); });
    // MRTR round, then a task on the final round (SEP-2322 + SEP-2663).
    add_tool(
        "test_tool_with_task", "Task-supporting tool: gathers `user_name` through an input request, then runs as a task.", kEmptySchema,
        [](Call &c) {
            json resp = c.params.value("inputResponses", json::object());
            if (!has_response(resp, "user_name")) {
                c.result = input_required(json{{"user_name", elicit_request("What is your name?", json{{"name", {{"type", "string"}}}}, json::array({"name"}))}});
                return;
            }
            run_as_task(c, "test_tool_with_task", json{{"user_name", answer_text(resp, "user_name", "name")}});
        },
        [](const json &a) {
            return json{{"ok", true}, {"result", {{"message", "Hello, " + a.value("user_name", std::string("friend")) + "!"}}}};
        },
        "required");
}

static json json_schema_2020_12() {
    return json::parse(R"({
      "$schema": "https://json-schema.org/draft/2020-12/schema",
      "type": "object",
      "$defs": {"address": {"$anchor": "addressDef", "type": "object",
                            "properties": {"street": {"type": "string"}, "city": {"type": "string"}}}},
      "properties": {"name": {"type": "string"}, "address": {"$ref": "#/$defs/address"},
                     "contactMethod": {"type": "string", "enum": ["phone", "email"]},
                     "phone": {"type": "string"}, "email": {"type": "string"}},
      "allOf": [{"anyOf": [{"required": ["phone"]}, {"required": ["email"]}]}],
      "if": {"properties": {"contactMethod": {"const": "phone"}}, "required": ["contactMethod"]},
      "then": {"required": ["phone"]},
      "else": {"required": ["email"]},
      "additionalProperties": false})");
}

static void register_tools() {
    add_tool("test_simple_text", "Tests simple text content response", kEmptySchema, fixed(json::array({text_item("This is a simple text response for testing.")})));
    add_tool("test_image_content", "Tests image content response", kEmptySchema,
             fixed(json::array({json{{"type", "image"}, {"data", kImageB64}, {"mimeType", "image/png"}}})));
    add_tool("test_audio_content", "Tests audio content response", kEmptySchema,
             fixed(json::array({json{{"type", "audio"}, {"data", kAudioB64}, {"mimeType", "audio/wav"}}})));
    add_tool("test_embedded_resource", "Tests embedded resource content response", kEmptySchema,
             fixed(json::array({json{{"type", "resource"},
                                     {"resource", {{"uri", "test://embedded-resource"}, {"mimeType", "text/plain"}, {"text", "This is an embedded resource content."}}}}})));
    add_tool("test_multiple_content_types", "Tests response with multiple content types (text, image, resource)", kEmptySchema,
             fixed(json::array({text_item("Multiple content types test:"), json{{"type", "image"}, {"data", kImageB64}, {"mimeType", "image/png"}},
                                json{{"type", "resource"},
                                     {"resource", {{"uri", "test://mixed-content-resource"},
                                                   {"mimeType", "application/json"},
                                                   {"text", json{{"test", "data"}, {"value", 123}}.dump()}}}}})));
    add_tool("test_error_handling", "Tests error response handling", kEmptySchema,
             fixed(json::array({text_item("This tool intentionally returns an error for testing")}), true));

    // The scenario sets the log level first (logging/setLevel, not offered by this server), so it cannot pass; the
    // fixture still exists and logs on the response stream when there is one.
    add_tool("test_tool_with_logging", "Tests tool that emits log messages during execution", kEmptySchema, [](Call &c) {
        for (const char *m : {"Tool execution started", "Tool processing data", "Tool execution completed"}) {
            if (c.emit) (*c.emit)(json{{"jsonrpc", "2.0"}, {"method", "notifications/message"}, {"params", {{"level", "info"}, {"data", m}}}});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        c.result = text_result("Tool with logging executed successfully");
    });
    // Progress notifications through the module's own reqctx::progress (SSE when the request carries a progressToken).
    add_tool("test_tool_with_progress", "Tests tool that reports progress notifications", kEmptySchema, [](Call &c) {
        std::string token = c.params.contains("_meta") && c.params["_meta"].contains("progressToken") ? c.params["_meta"]["progressToken"].dump() : "0";
        if (token.size() >= 2 && token.front() == '"') token = token.substr(1, token.size() - 2);
        for (int p : {0, 50, 100}) {
            reqctx::progress(p, 100, "Completed step " + std::to_string(p) + " of 100");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        c.result = text_result(token);
    });
    add_tool("test_reconnection", "Tests SSE stream disconnection and client reconnection (SEP-1699)", kEmptySchema, [](Call &c) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        c.result = text_result("Reconnection test completed successfully. If you received this, the client properly reconnected after stream closure.");
    });

    add_tool("test_sampling", "Tests server-initiated sampling (LLM completion request)",
             json{{"type", "object"}, {"properties", {{"prompt", {{"type", "string"}, {"description", "The prompt to send to the LLM"}}}}}},
             no_server_requests("Sampling"));
    add_tool("test_elicitation", "Tests server-initiated elicitation (user input request)",
             json{{"type", "object"}, {"properties", {{"message", {{"type", "string"}, {"description", "The message to show the user"}}}}}},
             no_server_requests("Elicitation"));
    add_tool("test_elicitation_sep1034_defaults", "Tests elicitation with default values per SEP-1034", kEmptySchema, no_server_requests("Elicitation"));
    add_tool("test_elicitation_sep1330_enums", "Tests elicitation with enum schema improvements per SEP-1330", kEmptySchema, no_server_requests("Elicitation"));

    add_tool("json_schema_2020_12_tool", "Tool with JSON Schema 2020-12 features for conformance testing (SEP-1613)", json_schema_2020_12(), [](Call &c) {
        c.result = text_result("JSON Schema 2020-12 tool called with: " + c.args.dump());
    });

    // Stateless (2026-07-28) diagnostics.
    add_tool("test_missing_capability", "Test tool requiring the sampling capability", kEmptySchema, [](Call &c) {
        if (c.modern && !(c.caps.is_object() && c.caps.contains("sampling")))
            return fail(c, perr::kMissingRequiredClientCapability, "MissingRequiredClientCapabilityError", 400, json{{"requiredCapabilities", {{"sampling", json::object()}}}});
        c.result = text_result("Success");
    });
    add_tool("test_streaming_elicitation", "Diagnostic tool validating response progress streams", kEmptySchema,
             [](Call &c) { c.result = text_result("Streaming complete"); });
    add_tool("test_logging_tool", "Diagnostic logging validator tool", kEmptySchema, [](Call &c) {
        // No notifications/message unless the request set a log level (it never does here): nothing to log.
        c.result = text_result("Logging evaluated");
    });
    add_tool("test_trigger_tool_change", "Diagnostic hook: mutates the tool list", kEmptySchema, [](Call &c) { c.result = text_result("Mutation triggered"); });
    add_tool("test_trigger_prompt_change", "Diagnostic hook: mutates the prompt list", kEmptySchema, [](Call &c) { c.result = text_result("Mutation triggered"); });

    register_mrtr();
    register_tasks_fixtures();
}

// ---------------------------------------------------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------------------------------------------------
static json prompt_list() {
    return json::array(
        {json{{"name", "test_simple_prompt"}, {"title", "Simple Test Prompt"}, {"description", "A simple prompt without arguments"}},
         json{{"name", "test_prompt_with_arguments"},
              {"title", "Prompt With Arguments"},
              {"description", "A prompt with required arguments"},
              {"arguments", json::array({json{{"name", "arg1"}, {"description", "First test argument"}, {"required", true}},
                                         json{{"name", "arg2"}, {"description", "Second test argument"}, {"required", true}}})}},
         json{{"name", "test_prompt_with_embedded_resource"},
              {"title", "Prompt With Embedded Resource"},
              {"description", "A prompt that includes an embedded resource"},
              {"arguments", json::array({json{{"name", "resourceUri"}, {"description", "URI of the resource to embed"}, {"required", true}}})}},
         json{{"name", "test_prompt_with_image"}, {"title", "Prompt With Image"}, {"description", "A prompt that includes image content"}},
         json{{"name", "test_input_required_result_prompt"}, {"title", "MRTR prompt"}, {"description", "MRTR: prompt that requires elicitation input"}}});
}

static json user_message(json content) { return json{{"role", "user"}, {"content", std::move(content)}}; }

static bool get_prompt(Call &c) {
    std::string name = c.params.value("name", "");
    json args = c.params.contains("arguments") && c.params["arguments"].is_object() ? c.params["arguments"] : json::object();
    auto arg = [&](const char *k) { return args.contains(k) && args[k].is_string() ? args[k].get<std::string>() : std::string(); };
    if (name == "test_simple_prompt") {
        c.result = json{{"messages", json::array({user_message(text_item("This is a simple prompt for testing."))})}};
    } else if (name == "test_prompt_with_arguments") {
        for (const char *k : {"arg1", "arg2"})
            if (!args.contains(k)) return fail(c, perr::kInvalidParams, std::string("missing required argument: ") + k), true;
        c.result = json{{"messages", json::array({user_message(text_item("Prompt with arguments: arg1='" + arg("arg1") + "', arg2='" + arg("arg2") + "'"))})}};
    } else if (name == "test_prompt_with_embedded_resource") {
        if (!args.contains("resourceUri")) return fail(c, perr::kInvalidParams, "missing required argument: resourceUri"), true;
        c.result = json{{"messages",
                         json::array({user_message(json{{"type", "resource"},
                                                        {"resource", {{"uri", arg("resourceUri")}, {"mimeType", "text/plain"}, {"text", "Embedded resource content for testing."}}}}),
                                      user_message(text_item("Please process the embedded resource above."))})}};
    } else if (name == "test_prompt_with_image") {
        c.result = json{{"messages", json::array({user_message(json{{"type", "image"}, {"data", kImageB64}, {"mimeType", "image/png"}}),
                                                  user_message(text_item("Please analyze the image above."))})}};
    } else if (name == "test_input_required_result_prompt") {
        json resp = c.params.value("inputResponses", json::object());
        if (has_response(resp, "user_context")) {
            c.result = json{{"messages", json::array({user_message(text_item("Prompt with context: " + answer_text(resp, "user_context", "context")))})}};
            return true;
        }
        c.result = input_required(json{{"user_context", elicit_request("What context should the prompt use?", json{{"context", {{"type", "string"}}}}, json::array({"context"}))}});
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------------------------------------------------
static json resource_list() {
    return json::array({json{{"uri", "test://static-text"}, {"name", "static-text"}, {"title", "Static Text Resource"},
                             {"description", "A static text resource for testing"}, {"mimeType", "text/plain"}},
                        json{{"uri", "test://static-binary"}, {"name", "static-binary"}, {"title", "Static Binary Resource"},
                             {"description", "A static binary resource (image) for testing"}, {"mimeType", "image/png"}},
                        json{{"uri", "test://watched-resource"}, {"name", "watched-resource"}, {"title", "Watched Resource"},
                             {"description", "A resource that auto-updates every 3 seconds"}, {"mimeType", "text/plain"}}});
}

static json template_list() {
    return json::array({json{{"uriTemplate", "test://template/{id}/data"}, {"name", "template"}, {"title", "Resource Template"},
                             {"description", "A resource template with parameter substitution"}, {"mimeType", "application/json"}}});
}

static bool read_resource(Call &c) {
    std::string uri = c.params.value("uri", "");
    json content;
    if (uri == "test://static-text") {
        content = json{{"uri", uri}, {"mimeType", "text/plain"}, {"text", "This is the content of the static text resource."}};
    } else if (uri == "test://static-binary") {
        content = json{{"uri", uri}, {"mimeType", "image/png"}, {"blob", kImageB64}};
    } else if (uri == "test://watched-resource") {
        content = json{{"uri", uri}, {"mimeType", "text/plain"}, {"text", "Watched resource content"}};
    } else {
        const std::string pre = "test://template/", suf = "/data";
        if (uri.size() <= pre.size() + suf.size() || uri.compare(0, pre.size(), pre) != 0 || uri.compare(uri.size() - suf.size(), suf.size(), suf) != 0) return false;
        std::string id = uri.substr(pre.size(), uri.size() - pre.size() - suf.size());
        if (id.empty() || id.find('/') != std::string::npos) return false;
        content = json{{"uri", uri},
                       {"mimeType", "application/json"},
                       {"text", json{{"id", id}, {"templateTest", true}, {"data", "Data for ID: " + id}}.dump()}};
    }
    c.result = json{{"contents", json::array({content})}, {"ttlMs", 0}, {"cacheScope", "private"}};
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
void register_fixtures() {
    register_tools();
    mcp::RequestHooks hooks;
    hooks.intercept = [](const std::string &method, const json &params, const json &caps, bool modern, const mcp::Emit *emit, json &result, json &error) {
        json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
        Call c{params, args, caps, modern, emit, result, error};
        if (method == "tools/call") {
            auto it = g_tool_fns.find(params.value("name", ""));
            if (it == g_tool_fns.end()) return false;
            // The module's own argument validation (same schema check as a registry call).
            const mcp::Tool *t = mcp::find_tool(it->first);
            if (t && t->validator) {
                try {
                    t->validator->validate(args);
                } catch (const std::exception &e) {
                    fail(c, perr::kInvalidParams, std::string("invalid arguments: ") + e.what());
                    return true;
                }
            }
            it->second(c);
            return true;
        }
        if (method == "prompts/get") return get_prompt(c);
        if (method == "resources/read") return read_resource(c);
        return false;
    };
    hooks.list_extra = [](const std::string &method) -> json {
        if (method == "prompts/list") return prompt_list();
        if (method == "resources/list") return resource_list();
        if (method == "resources/templates/list") return template_list();
        return nullptr;
    };
    mcp::set_request_hooks(std::move(hooks));
}

}  // namespace conformance
