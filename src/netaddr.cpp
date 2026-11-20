#include "netaddr.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "layout.h"
#include "platform/platform.h"

namespace netaddr {

using CreateInterfaceFn = void *(*)(const char *, int *);

// Vtable indices (Source SDK 2013, cdll_int.h / inetchannelinfo.h), verified in client.dll x64 (see docs/REVERSE.md):
// IVEngineClient::GetNetChannelInfo = slot 72 (offset 0x240); INetChannelInfo::GetAddress = slot 1; IVEngineClient::ClientCmd = slot 7.
// None of them is called before the code behind it was recognized (layout.h, docs/REVERSE.md "Slot checks"): an engine build that moves a
// slot turns off the feature that needs it, with a reason, instead of calling another function.
static constexpr int kGetNetChannelInfo = 72;
static constexpr int kGetAddress = 1;
static constexpr int kClientCmd = 7;

static std::mutex g_mu;  // the checks below (memo)
static void *g_cached_engine = nullptr;  // engine interface: lives as long as the process
static void *g_checked_engine = nullptr;  // the engine object the verdicts below are for
static layout::Check g_client_cmd, g_get_nci;
static std::map<void *, layout::Check> g_get_address;  // INetChannelInfo vtable -> verdict on its GetAddress (slot 1)

void set_engine_for_tests(void *engine) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cached_engine = engine;
    g_checked_engine = nullptr;
    g_get_address.clear();
}

static void *engine_client() {
    std::lock_guard<std::mutex> lk(g_mu);
    void *&cached = g_cached_engine;
    if (cached) return cached;
    void *mod = plat::find_loaded_module(plat::engine_names());
    auto ci = reinterpret_cast<CreateInterfaceFn>(plat::find_symbol(mod, "CreateInterface"));
    if (!ci) return nullptr;
    int rc = 0;
    cached = ci("VEngineClient015", &rc);
    return cached;
}

// The verdicts on the slots this module calls on the engine object, read once per object and published for `status`.
static void check_engine(void *eng) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_checked_engine == eng) return;
    g_checked_engine = eng;
    if (!layout::kChecked) {
        g_client_cmd.ok = g_get_nci.ok = true;
        return;
    }
    g_client_cmd = layout::client_cmd_shape(layout::slot_code(eng, kClientCmd, 0x60));
    g_get_nci = layout::get_net_channel_info_shape(layout::slot_code(eng, kGetNetChannelInfo, 16));
    std::string why;
    if (!g_client_cmd.ok) why += "slot 7 (ClientCmd) " + g_client_cmd.why;
    if (!g_get_nci.ok) why += (why.empty() ? "" : "; ") + std::string("slot 72 (GetNetChannelInfo) ") + g_get_nci.why;
    layout::publish("ivengineclient", why.empty() ? layout::State::Verified : layout::State::Refused, why,
                    nlohmann::json{{"client_cmd", g_client_cmd.ok}, {"get_net_channel_info", g_get_nci.ok}});
}

void verify_layout(bool client_mode) {
    if (!layout::kChecked) return;
    if (!client_mode) {
        layout::publish("ivengineclient", layout::State::NotApplicable, "dedicated server: no client engine interface");
        layout::publish("inetchannelinfo", layout::State::NotApplicable, "dedicated server: no client engine interface");
        return;
    }
    void *eng = engine_client();
    if (!eng) {
        layout::publish("ivengineclient", layout::State::Refused, "VEngineClient015 not found in the engine");
        return;
    }
    check_engine(eng);
}

// Validates "a.b.c.d:port" or "loopback" (optionally followed by ":port").
static bool well_formed(const std::string &s) {
    if (s == "loopback" || s.rfind("loopback:", 0) == 0) return true;
    int dots = 0, colons = 0, digits = 0;
    for (char c : s) {
        if (c == '.') dots++;
        else if (c == ':') colons++;
        else if (c >= '0' && c <= '9') digits++;
        else return false;
    }
    return dots == 3 && colons <= 1 && digits >= 4;
}

// The verdict on GetAddress (slot 1) of this net channel's class, checked the first time that class is seen and kept.
static layout::Check check_get_address(void *nvt, void *nci) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_get_address.find(nvt);
    if (it != g_get_address.end()) return it->second;
    layout::Check c;
    if (!layout::kChecked) c.ok = true;
    else c = layout::get_address_shape(layout::slot_code(nci, kGetAddress, 16));
    g_get_address[nvt] = c;
    if (layout::kChecked)
        layout::publish("inetchannelinfo", c.ok ? layout::State::Verified : layout::State::Refused, c.ok ? "" : "slot 1 (GetAddress) " + c.why,
                        nlohmann::json{{"get_address", c.ok}});
    return c;
}

Result current_server_address() {
    Result r;
    r.method = "IVEngineClient015::GetNetChannelInfo()->GetAddress()";
    void *eng = engine_client();
    if (!eng) {
        r.error = "VEngineClient015 not found";
        return r;
    }
    check_engine(eng);
    if (!g_get_nci.ok) {
        r.error = "IVEngineClient::GetNetChannelInfo (slot 72) is not recognized in this game build (layout check: " + g_get_nci.why + ")";
        return r;
    }
    void **vt = nullptr;
    if (!plat::safe_read(eng, &vt, sizeof vt) || !vt) {
        r.error = "engine vtable unreadable";
        return r;
    }
    void *fn = nullptr;
    if (!plat::safe_read(&vt[kGetNetChannelInfo], &fn, sizeof fn) || !fn) {
        r.error = "GetNetChannelInfo unreadable";
        return r;
    }
    using GetNCI = void *(*)(void *);
    void *nci = reinterpret_cast<GetNCI>(fn)(eng);
    if (!nci) {
        r.error = "no net channel (not connected)";
        return r;
    }
    void **nvt = nullptr;
    void *gfn = nullptr;
    if (!plat::safe_read(nci, &nvt, sizeof nvt) || !nvt || !plat::safe_read(&nvt[kGetAddress], &gfn, sizeof gfn) || !gfn) {
        r.error = "INetChannelInfo unreadable";
        return r;
    }
    // The object GetNetChannelInfo returned is a net channel only if its GetAddress is what it is believed to be.
    layout::Check ga = check_get_address(nvt, nci);
    if (!ga.ok) {
        r.error = "INetChannelInfo::GetAddress (slot 1) is not recognized in this game build (layout check: " + ga.why + ")";
        return r;
    }
    using GetAddr = const char *(*)(void *);
    const char *a = reinterpret_cast<GetAddr>(gfn)(nci);
    char buf[64] = {0};
    bool terminated = false;
    if (!a || !plat::safe_read(a, buf, 1)) {
        r.error = "address unreadable";
        return r;
    }
    for (size_t i = 0; i < sizeof buf - 1; ++i) {
        if (!plat::safe_read(a + i, &buf[i], 1)) break;
        if (buf[i] == 0) {
            terminated = true;
            break;
        }
    }
    buf[sizeof buf - 1] = 0;
    std::string s = buf;
    // Short printable ASCII, ending: what GetAddress returns. Anything else: the slot is not what it is believed to be; never asked again.
    if (!terminated || !layout::address_text_plausible(s)) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            layout::Check bad;
            bad.why = "it returned text that is not an address";
            g_get_address[nvt] = bad;
        }
        layout::publish("inetchannelinfo", layout::State::Refused, "slot 1 (GetAddress) returned text that is not an address",
                        nlohmann::json{{"get_address", false}});
        r.error = "INetChannelInfo::GetAddress (slot 1) returned text that is not an address (layout check)";
        return r;
    }
    if (!well_formed(s)) {
        r.error = "unexpected address \"" + s + "\" (engine offsets changed?)";
        return r;
    }
    if (s == "0.0.0.0:0") {
        r.error = "null address";
        return r;
    }
    r.ok = true;
    r.address = s;
    return r;
}

bool client_cmd(const std::string &cmd, std::string &err) {
    // IVEngineClient::ClientCmd = slot 7 (offset 0x38), called by client.dll's RunConsoleCommand / Player:ConCommand
    // AFTER their Lua blacklist (see docs/REVERSE.md). Main thread only.
    if (cmd.empty() || cmd.size() > 1000 || cmd.find('\n') != std::string::npos || cmd.find('\r') != std::string::npos) {
        err = "command empty, too long or multi-line";
        return false;
    }
    void *eng = engine_client();
    void **vt = nullptr;
    void *fn = nullptr;
    if (!eng || !plat::safe_read(eng, &vt, sizeof vt) || !vt || !plat::safe_read(&vt[kClientCmd], &fn, sizeof fn) || !fn) {
        err = "VEngineClient015::ClientCmd unavailable";
        return false;
    }
    check_engine(eng);
    if (!g_client_cmd.ok) {
        err = "IVEngineClient::ClientCmd (slot 7) is not recognized in this game build (layout check: " + g_client_cmd.why +
              "): the command was not sent";
        return false;
    }
    using ClientCmdFn = void (*)(void *, const char *);
    std::string line = cmd + "\n";
    reinterpret_cast<ClientCmdFn>(fn)(eng, line.c_str());
    return true;
}

static std::string host_of(const std::string &address) {
    std::string host = address;
    auto colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) host = host.substr(0, colon);
    return host;
}

bool is_this_machine(const std::string &address, std::string *why) {
    if (is_loopback(address)) {
        if (why) *why = "loopback";
        return true;
    }
    // Interface addresses, cached 10 s (this runs every few frames).
    static std::mutex mu;
    static std::vector<std::string> cache;
    static std::chrono::steady_clock::time_point stamp{};
    std::vector<std::string> ips;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto now = std::chrono::steady_clock::now();
        if (stamp.time_since_epoch().count() == 0 || now - stamp > std::chrono::seconds(10)) {
            cache = plat::local_ipv4_addresses();
            stamp = now;
        }
        ips = cache;
    }
    std::string host = host_of(address);
    for (const auto &ip : ips)
        if (ip == host) {
            if (why) *why = "address of this machine's network interface " + ip;
            return true;
        }
    if (why) *why = "not an address of this machine";
    return false;
}

bool is_loopback(const std::string &address) {
    std::string host = address;
    if (host == "loopback" || host.rfind("loopback:", 0) == 0) return true;
    auto colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) host = host.substr(0, colon);
    if (host == "localhost") return true;
    // 127.0.0.0/8 only (strict dotted IPv4).
    if (host.rfind("127.", 0) == 0) {
        int dots = 0;
        for (char c : host) {
            if (c == '.') dots++;
            else if (c < '0' || c > '9') return false;
        }
        return dots == 3;
    }
    return false;
}

}  // namespace netaddr
