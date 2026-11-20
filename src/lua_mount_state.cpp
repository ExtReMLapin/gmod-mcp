// What each realm loaded from the mounted folder (Issue #9): load-time snapshots, reload records, changes since. Pure
// bookkeeping over the file system; the engine side (MountLua hook, runtime injection) feeds it. No game needed.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <set>

#include "lua_mount.h"
#include "realms.h"
#include "util.h"

namespace lua_mount {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct Record {
    std::string root;
    Files files;            // the folder when the realm loaded, plus files reloaded since
    bool full = false;      // from a load start (the folder was in front for the whole load), not only reloads
    std::set<std::string> reloaded;  // keys the mount only supplied through lua_reload (not in the load snapshot)
};

std::mutex g_mu;
Record g_pending[realms::COUNT];  // hook seen, runtime not injected yet
Record g_cur[realms::COUNT];
// The last injection of each realm, and whether a load start was waiting for it. The server's MountLua comes before its
// Lua state exists (docs/REVERSE.md), so its start is always pending at injection. The client's order is not verified:
// a start that arrives within a few seconds after an injection that found none belongs to that injection.
using SteadyClock = std::chrono::steady_clock;
SteadyClock::time_point g_injected_at[realms::COUNT];
bool g_injected_without_start[realms::COUNT];
constexpr std::chrono::seconds kLateStartWindow(3);

bool valid_realm(int r) { return r >= 0 && r < realms::COUNT; }

std::string norm(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.size() > 3 && p.back() == '/') p.pop_back();
    return util::to_lower(p);
}

bool same_root(const std::string &a, const std::string &b) { return !a.empty() && norm(a) == norm(b); }

void scan_dir(const fs::path &dir, const std::string &prefix, Files &out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        std::string name = it->path().filename().u8string();
        if (name.size() < 4 || util::to_lower(name.substr(name.size() - 4)) != ".lua") continue;
        std::string rel = fs::relative(it->path(), dir, e2).generic_u8string();
        if (e2) continue;
        rel = prefix + "/" + rel;
        long long mt = mtime_of(it->path().u8string());
        out[util::to_lower(rel)] = FileStamp{rel, mt};
    }
}

}  // namespace

long long mtime_of(const std::string &abs) {
    std::error_code ec;
    auto t = fs::last_write_time(fs::u8path(abs), ec);
    if (ec) return -1;
    return (long long)t.time_since_epoch().count();
}

Files scan_files(const std::string &root) {
    Files out;
    if (root.empty()) return out;
    scan_dir(fs::u8path(root) / "lua", "lua", out);
    scan_dir(fs::u8path(root) / "gamemodes", "gamemodes", out);
    return out;
}

std::string rel_in_mount(const std::string &abs) {
    std::string root = current();
    if (root.empty()) return "";
    std::string a = fs::u8path(abs).lexically_normal().generic_u8string();
    std::string r = fs::u8path(root).lexically_normal().generic_u8string();
    while (r.size() > 3 && r.back() == '/') r.pop_back();
    r += "/";
    if (util::to_lower(a).rfind(util::to_lower(r), 0) != 0) return "";
    return a.substr(r.size());
}

bool folder_present(const std::string &root) {
    if (root.empty()) return false;
    std::error_code ec;
    return fs::is_directory(fs::u8path(root) / "lua", ec);
}

void record_load_start(int realm, const std::string &root) {
    if (!valid_realm(realm)) return;
    if (!folder_present(root)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending[realm] = Record{};  // nothing was in front for this load
        return;
    }
    note_load_start(realm, root, scan_files(root));
}

void note_load_start(int realm, const std::string &root, Files files) {
    if (!valid_realm(realm)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Record r{root, std::move(files), true, {}};
    if (g_injected_without_start[realm] && SteadyClock::now() - g_injected_at[realm] < kLateStartWindow) {
        g_injected_without_start[realm] = false;
        g_cur[realm] = std::move(r);
        return;
    }
    g_pending[realm] = std::move(r);
}

void note_realm_loaded(int realm) {
    if (!valid_realm(realm)) return;
    std::string cur = current();
    std::lock_guard<std::mutex> lk(g_mu);
    Record p = std::move(g_pending[realm]);
    g_pending[realm] = Record{};
    // A load start for a folder that is no longer the mounted one (removed or replaced since) does not count.
    bool usable = p.full && same_root(p.root, cur);
    g_cur[realm] = usable ? std::move(p) : Record{};
    g_injected_at[realm] = SteadyClock::now();
    g_injected_without_start[realm] = !usable;
}

void note_reloaded(int realm, const std::string &root, const std::string &rel, long long mtime) {
    if (!valid_realm(realm) || root.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Record &r = g_cur[realm];
    if (!same_root(r.root, root)) r = Record{root, {}, false, {}};
    std::string key = util::to_lower(rel);
    r.reloaded.insert(key);  // from now on the realm's copy is the one lua_reload ran, in place at load or not
    r.files[key] = FileStamp{rel, mtime};
}

bool realm_loaded_with_mount(int realm) {
    if (!valid_realm(realm)) return false;
    std::string cur = current();
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cur[realm].full && same_root(g_cur[realm].root, cur);
}

bool loaded_from_mount(int realm, const std::string &rel, bool *reloaded) {
    if (reloaded) *reloaded = false;
    if (!valid_realm(realm)) return false;
    std::string cur = current();
    std::lock_guard<std::mutex> lk(g_mu);
    const Record &r = g_cur[realm];
    if (!same_root(r.root, cur)) return false;
    auto it = r.files.find(util::to_lower(rel));
    if (it == r.files.end()) return false;
    if (reloaded) *reloaded = r.reloaded.count(util::to_lower(rel)) > 0;
    return true;
}

std::string loaded_from_other_mount(int realm, const std::string &rel) {
    if (!valid_realm(realm)) return "";
    std::string cur = current();
    std::lock_guard<std::mutex> lk(g_mu);
    const Record &r = g_cur[realm];
    if (r.root.empty() || same_root(r.root, cur) || !r.files.count(util::to_lower(rel))) return "";
    return r.root;
}

std::vector<Change> changed_files(int realm) {
    std::vector<Change> out;
    if (!valid_realm(realm)) return out;
    std::string cur = current();
    Record rec;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        rec = g_cur[realm];
    }
    if (!same_root(rec.root, cur)) return out;
    Files now = scan_files(cur);
    for (const auto &kv : rec.files) {
        auto it = now.find(kv.first);
        if (it == now.end()) out.push_back({kv.second.rel, "deleted"});
        else if (it->second.mtime != kv.second.mtime) out.push_back({it->second.rel, "modified"});
    }
    if (rec.full)
        for (const auto &kv : now)
            if (!rec.files.count(kv.first)) out.push_back({kv.second.rel, "added"});
    std::sort(out.begin(), out.end(), [](const Change &a, const Change &b) { return a.rel < b.rel; });
    return out;
}

std::vector<Probe> pick_probes(const Files &files, size_t n) {
    std::vector<std::string> lua, gm;  // names as file.Exists takes them, in key order
    std::vector<long long> lua_t, gm_t;
    for (const auto &kv : files) {
        const std::string &rel = kv.second.rel;
        if (rel.rfind("lua/", 0) == 0) {
            lua.push_back(rel.substr(4));
            lua_t.push_back(kv.second.mtime);
        } else if (rel.rfind("gamemodes/", 0) == 0) {
            gm.push_back(rel.substr(10));
            gm_t.push_back(kv.second.mtime);
        }
    }
    std::vector<Probe> out;
    auto take = [&](const std::vector<std::string> &names, const std::vector<long long> &times, size_t want) {
        if (names.empty() || want == 0) return;
        want = std::min(want, names.size());
        // spread over the sorted names: first, middle, last, ...
        for (size_t i = 0; i < want; ++i) {
            size_t idx = want == 1 ? 0 : i * (names.size() - 1) / (want - 1);
            out.push_back(Probe{names[idx], unix_seconds_of(times[idx])});
        }
    };
    take(lua, lua_t, n);
    if (out.size() < n) take(gm, gm_t, n - out.size());
    return out;
}

long long unix_seconds_of(long long ticks) {
    if (ticks < 0) return -1;
    // The file system clock has its own epoch: measure the offset against the system clock instead of assuming it.
    using namespace std::chrono;
    auto fnow = fs::file_time_type::clock::now();
    auto snow = system_clock::now();
    auto delta = fs::file_time_type(fs::file_time_type::duration(ticks)) - fnow;
    return duration_cast<seconds>(snow.time_since_epoch()).count() + duration_cast<seconds>(delta).count();
}

json probes_verdict(const std::vector<Probe> &probes, const json &answers) {
    if (probes.empty() || !answers.is_array() || answers.size() != probes.size()) return nullptr;
    bool any = false;
    for (size_t i = 0; i < probes.size(); ++i) {
        const json &a = answers[i];
        if (!a.is_object() || !a.value("exists", false)) continue;
        if (probes[i].unix_time < 0 || !a.contains("time") || !a["time"].is_number()) {
            any = true;
            continue;
        }
        long long t = a["time"].get<long long>(), want = probes[i].unix_time;
        if (t >= want - 2 && t <= want + 2) any = true;
    }
    return any;
}

Files probe_sample(int realm) {
    std::string cur = current();
    if (cur.empty()) return {};
    if (valid_realm(realm)) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_cur[realm].full && same_root(g_cur[realm].root, cur)) return g_cur[realm].files;
    }
    return scan_files(cur);
}

void forget_loads_for_tests() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < realms::COUNT; ++i) {
        g_pending[i] = g_cur[i] = Record{};
        g_injected_without_start[i] = false;
    }
}

}  // namespace lua_mount
