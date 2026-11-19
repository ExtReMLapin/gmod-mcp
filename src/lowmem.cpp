#include "lowmem.h"

#include <cstdlib>
#include <sstream>

#include "platform/platform.h"

using nlohmann::json;

namespace lowmem {

// "Name:   1234 kB" lines: the value in kB of `name`, or -1.
static long long meminfo_kb(const std::string &text, const char *name) {
    std::istringstream in(text);
    std::string line;
    std::string key = std::string(name) + ":";
    while (std::getline(in, line)) {
        if (line.compare(0, key.size(), key) != 0) continue;
        const char *p = line.c_str() + key.size();
        char *end = nullptr;
        long long v = std::strtoll(p, &end, 10);
        return end == p ? -1 : v;
    }
    return -1;
}

Memory parse_meminfo(const std::string &text) {
    Memory m;
    long long avail = meminfo_kb(text, "MemAvailable"), limit = meminfo_kb(text, "CommitLimit"), used = meminfo_kb(text, "Committed_AS");
    if (avail < 0 || limit < 0 || used < 0) return m;
    m.known = true;
    m.free_ram_mb = (uint64_t)avail / 1024;
    m.free_commit_mb = limit > used ? (uint64_t)(limit - used) / 1024 : 0;
    return m;
}

static bool env_mb(const char *name, uint64_t &out) {
    const char *v = std::getenv(name);
    if (!v || !*v) return false;
    out = (uint64_t)std::strtoull(v, nullptr, 10);
    return true;
}

Memory current() {
    Memory m;
#ifdef _WIN32
    m.known = plat::memory_status(m.free_commit_mb, m.free_ram_mb);
#else
    std::string text;
    if (plat::read_file("/proc/meminfo", text)) m = parse_meminfo(text);
#endif
    uint64_t v = 0;
    if (env_mb("GMOD_MCP_FAKE_FREE_COMMIT_MB", v)) {
        m.free_commit_mb = v;
        m.known = true;
    }
    if (env_mb("GMOD_MCP_FAKE_FREE_RAM_MB", v)) {
        m.free_ram_mb = v;
        m.known = true;
    }
    return m;
}

Verdict check_launch(const Memory &m, int count, int running_clients, bool ignore) {
    Verdict v;
    if (count < 1) count = 1;
    uint64_t need_commit = (uint64_t)count * kPerClientCommitMb + kReserveCommitMb, need_ram = (uint64_t)count * kPerClientRamMb;
    v.details = json{{"free_commit_mb", m.free_commit_mb}, {"free_ram_mb", m.free_ram_mb}, {"running_clients", running_clients},
                     {"per_client_mb", kPerClientCommitMb}, {"count", count}, {"needed_commit_mb", need_commit}, {"needed_ram_mb", need_ram}};
    if (!m.known || ignore) return v;
    bool commit_low = m.free_commit_mb < need_commit, ram_low = m.free_ram_mb < need_ram;
    if (!commit_low && !ram_low) return v;
    v.refuse = true;
    std::ostringstream o;
    o << "not enough memory to start " << count << " client" << (count > 1 ? "s" : "") << ": " << m.free_commit_mb << " MB of commit (virtual memory) and "
      << m.free_ram_mb << " MB of RAM are free, " << running_clients << " client" << (running_clients == 1 ? "" : "s") << " already run" << (running_clients == 1 ? "s" : "")
      << " on this machine (all sessions). A client holds about " << kPerClientCommitMb << " MB of commit and " << kPerClientRamMb
      << " MB of RAM; " << need_commit << " MB of commit (the clients plus " << kReserveCommitMb << " MB for the rest of the machine) and " << need_ram
      << " MB of RAM are needed. A machine out of commit makes every game crash (0xC000041D, no dump): close clients you no longer use "
         "(instances_close), start fewer, or pass ignore_low_memory: true to start anyway. Nothing was started.";
    v.message = o.str();
    return v;
}

json memory_json(const Memory &m) {
    if (!m.known) return nullptr;
    return json{{"free_commit_mb", m.free_commit_mb}, {"free_ram_mb", m.free_ram_mb}};
}

void annotate_crash(const Memory &m, json &details, std::string &message) {
    if (!m.known) return;
    details["memory"] = memory_json(m);
    if (m.free_commit_mb < kLowCommitHintMb)
        message += " Memory is low on this machine (" + std::to_string(m.free_commit_mb) + " MB of commit and " + std::to_string(m.free_ram_mb) +
                   " MB of RAM free now, measured when this was reported, after the game freed its own): running out of memory is a likely cause "
                   "(an exit code 0xC000041D with no dump is typical); close other clients before relaunching.";
}

}  // namespace lowmem
