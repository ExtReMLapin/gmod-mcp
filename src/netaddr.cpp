#include "netaddr.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <cstring>

#include "platform/platform.h"

namespace netaddr {

using CreateInterfaceFn = void *(*)(const char *, int *);

// Vtable indices (Source SDK 2013, cdll_int.h / inetchannelinfo.h), verified in client.dll x64 (see docs/REVERSE.md):
// IVEngineClient::GetNetChannelInfo = slot 72 (offset 0x240); INetChannelInfo::GetAddress = slot 1.
static constexpr int kGetNetChannelInfo = 72;
static constexpr int kGetAddress = 1;

static void *engine_client() {
    static void *cached = nullptr;  // engine interface: lives as long as the process
    if (cached) return cached;
    void *mod = plat::find_loaded_module(plat::engine_names());
    auto ci = reinterpret_cast<CreateInterfaceFn>(plat::find_symbol(mod, "CreateInterface"));
    if (!ci) return nullptr;
    int rc = 0;
    cached = ci("VEngineClient015", &rc);
    return cached;
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

Result current_server_address() {
    Result r;
    r.method = "IVEngineClient015::GetNetChannelInfo()->GetAddress()";
    void *eng = engine_client();
    if (!eng) {
        r.error = "VEngineClient015 not found";
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
    using GetAddr = const char *(*)(void *);
    const char *a = reinterpret_cast<GetAddr>(gfn)(nci);
    char buf[64] = {0};
    if (!a || !plat::safe_read(a, buf, 1)) {
        r.error = "address unreadable";
        return r;
    }
    for (size_t i = 0; i < sizeof buf - 1; ++i) {
        if (!plat::safe_read(a + i, &buf[i], 1)) break;
        if (buf[i] == 0) break;
    }
    buf[sizeof buf - 1] = 0;
    std::string s = buf;
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
    static constexpr int kClientCmd = 7;
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
