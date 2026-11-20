// Tools: status, lua_exec, lua_exec_file, lua_reload, lua_check, lua_inspect, lua_file_*.
#include <algorithm>
#include <filesystem>
#include <map>

#include "agent.h"
#include "console.h"
#include "coordinator.h"
#include "jobs.h"
#include "ljdump.h"
#include "mcp.h"
#include "lua_mount.h"
#include "platform/platform.h"
#include "realms.h"
#include "tools.h"
#include "util.h"

namespace tools {

json ok(json result) { return json{{"ok", true}, {"result", std::move(result)}}; }

json realm_prop(const std::string &desc, bool with_menu) {
    json e = with_menu ? json::array({"menu", "client", "server"}) : json::array({"client", "server"});
    return json{{"type", "string"}, {"enum", e}, {"description", desc}};
}

int realm_of(const json &args, const char *key, int def) {
    if (!args.contains(key)) return def;
    return realms::parse(args[key].get<std::string>());
}

json rt_realm(const json &args, const char *key, int def, const std::string &tool, const json &payload, double timeout_s) {
    int r = realm_of(args, key, def);
    if (r < 0) return agent::err("invalid_params", std::string(key) + ": unknown realm");
    return agent::rt(r, tool, payload, timeout_s);
}

static bool safe_rel(const std::string &name) {
    if (name.empty() || name.size() > 200) return false;
    if (name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos) return false;
    for (char c : name)
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '/')) return false;
    std::string s = "/" + name + "/";
    if (s.find("/../") != std::string::npos || s.find("/./") != std::string::npos || s.find("//") != std::string::npos) return false;
    // Windows reserved device names (con.lua, nul, com1...) in any path component: writing to them can block.
    static const char *reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                     "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        std::string comp = util::to_lower(name.substr(start, end - start));
        std::string stem = comp.substr(0, comp.find('.'));
        for (const char *r : reserved)
            if (stem == r) return false;
        if (!comp.empty() && (comp.back() == '.' || comp.back() == ' ')) return false;
        start = end + 1;
    }
    return true;
}

// A path as shown to the agent: the platform's separators, "." / ".." / doubled separators resolved.
static std::string native_path(const std::string &p) {
    return std::filesystem::u8path(p).lexically_normal().make_preferred().u8string();
}

std::string tmp_path(const std::string &name, std::string &err) {
    if (!safe_rel(name)) {
        err = "invalid name: relative to mcpagent_tmp, characters [A-Za-z0-9_.-/], no '..'";
        return "";
    }
    return native_path(agent::config().tmp_dir + "/" + name);
}

static std::string normalize(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

// Chunk name for a file, as the engine names it: relative to the game folder (e.g. @lua/mcpagent_tmp/x.lua), or, for
// a file of the mounted Lua folder (lua_mount), relative to that folder's root (@lua/my_addon/x.lua: the engine
// names it through the MOD search path, where the root is listed).
static std::string chunk_for(const std::string &abs) {
    std::string na = normalize(abs), game = normalize(agent::config().game_dir) + "/";
    std::string ov = lua_mount::current();
    if (!ov.empty()) {
        ov = normalize(ov) + "/";
        if (util::to_lower(na).rfind(util::to_lower(ov), 0) == 0) return "@" + na.substr(ov.size());
    }
    if (util::to_lower(na).rfind(util::to_lower(game), 0) == 0) return "@" + na.substr(game.size());
    return "@" + na;
}

// Where a relative path may be, in this order: the temporary directory (lua/mcpagent_tmp/), the mounted Lua folder
// when one is set (lua_mount: <root>/<path>, <root>/lua/<path>, <root>/gamemodes/<path>, ahead of the game like the
// engine does), the game folder (garrysmod/<path>), then where include() looks (lua/, gamemodes/, addons/*/lua/,
// addons/*/gamemodes/).
static std::vector<std::string> lua_candidates(const std::string &rel, bool use_mount = true) {
    namespace fs = std::filesystem;
    const auto &c = agent::config();
    std::vector<std::string> out{c.tmp_dir + "/" + rel};
    std::string ov = use_mount ? lua_mount::current() : std::string();
    if (!ov.empty()) {
        out.push_back(ov + "/" + rel);
        out.push_back(ov + "/lua/" + rel);
        out.push_back(ov + "/gamemodes/" + rel);
    }
    for (const auto &g : {c.game_dir + "/" + rel, c.game_dir + "/lua/" + rel, c.game_dir + "/gamemodes/" + rel}) out.push_back(g);
    std::error_code ec;
    std::vector<std::string> addons;
    for (const auto &e : fs::directory_iterator(fs::u8path(c.game_dir + "/addons"), ec))
        if (e.is_directory(ec)) addons.push_back(e.path().filename().u8string());
    std::sort(addons.begin(), addons.end());
    for (const auto &a : addons) {
        out.push_back(c.game_dir + "/addons/" + a + "/lua/" + rel);
        out.push_back(c.game_dir + "/addons/" + a + "/gamemodes/" + rel);
    }
    for (auto &o : out) o = native_path(o);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Resolves a Lua path given by the agent: absolute, or relative (see lua_candidates). With allow_dir, a folder matches
// too. On failure, err lists every place that was tried.
bool resolve_lua_path(const std::string &path_in, bool allow_dir, std::string &abs, std::string &err, bool use_mount) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto fits = [&](const std::string &p) {
        fs::path fp = fs::u8path(p);
        return fs::is_regular_file(fp, ec) || (allow_dir && fs::is_directory(fp, ec));
    };
    if (fs::u8path(path_in).is_absolute()) {
        abs = native_path(path_in);
        if (fits(abs)) return true;
        err = std::string(allow_dir ? "file or folder" : "file") + " not found: " + abs;
        return false;
    }
    std::string rel = normalize(path_in);
    while (rel.size() > 2 && rel.compare(0, 2, "./") == 0) rel.erase(0, 2);
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    if (!safe_rel(rel)) {
        err = "invalid path: absolute, or relative with characters [A-Za-z0-9_.-/] and no '..'";
        return false;
    }
    std::vector<std::string> tried = lua_candidates(rel, use_mount);
    for (const auto &c : tried)
        if (fits(c)) {
            abs = c;
            return true;
        }
    err = std::string(allow_dir ? "file or folder" : "file") + " '" + rel + "' not found; tried:";
    for (const auto &c : tried) err += "\n  " + c;
    return false;
}

bool resolve_lua_file(const std::string &path, std::string &abs, std::string &chunkname, std::string &err) {
    if (!resolve_lua_path(path, false, abs, err)) return false;
    chunkname = chunk_for(abs);
    return true;
}

// `fields`: keeps only these keys of each returned table (e.g. a test's verdict out of a large result).
json project_values(json r, const json &args) {
    if (!args.contains("fields") || !r.value("ok", false) || !r.contains("result")) return r;
    json &res = r["result"];
    if (!res.contains("values") || !res["values"].is_array()) return r;
    // Issue #11: say so when there was nothing to project (e.g. util.TableToJSON returns a string).
    size_t projected_tables = 0;
    std::string seen;
    for (auto &v : res["values"]) {
        if (!v.is_object()) {
            const char *kind = v.is_string() ? "string" : v.is_array() ? "array" : v.is_null() ? "nil" : "number or boolean";
            if (seen.find(kind) == std::string::npos) seen += (seen.empty() ? "" : ", ") + std::string(kind);
            continue;
        }
        ++projected_tables;
        json kept = json::object();
        for (const auto &f : args["fields"])
            if (f.is_string() && v.contains(f.get<std::string>())) kept[f.get<std::string>()] = v[f.get<std::string>()];
        v = std::move(kept);
    }
    res["projected"] = args["fields"];
    if (projected_tables == 0)
        res["fields_note"] = std::string("`fields` projected nothing: none of the returned values is a table with named keys (") +
                             (seen.empty() ? "no value was returned" : "got: " + seen) +
                             "), so the result came back whole. `fields` only keeps keys of returned tables: return the table itself, "
                             "or if the value is a JSON string (util.TableToJSON) return util.JSONToTable(it) instead, "
                             "or drop `fields`.";
    return r;
}

json project_keys(json r, const json &args) {
    if (!args.contains("fields") || !r.value("ok", false) || !r.contains("result") || !r["result"].is_object()) return r;
    json kept = json::object();
    for (const auto &f : args["fields"])
        if (f.is_string() && r["result"].contains(f.get<std::string>())) kept[f.get<std::string>()] = r["result"][f.get<std::string>()];
    kept["projected"] = args["fields"];
    r["result"] = std::move(kept);
    return r;
}

static json status_tool(const json &args) {
    const auto &c = agent::config();
    double since = jobs::seconds_since_last_frame();
    bool stalled = since > 5.0;
    json realms_j = json::object();
    for (int r = 0; r < realms::COUNT; ++r) {
        if (c.mode == agent::Mode::Dedicated && r != realms::SERVER) continue;
        realms_j[realms::name(r)] = json{{"available", agent::realm_present(r)},
                                         {"runtime", agent::realm_injected(r)},
                                         {"generation", agent::generation(r)}};
    }
    json s{{"instance", c.instance_id},
           {"mode", agent::mode_name()},
           {"agent_version", c.version},
           {"platform", plat::platform_tag()},
           {"pid", plat::process_id()},
           {"port", c.port},
           {"coordinator", coordinator::is_coordinator()},
           {"coordinator_link", coordinator::link_state()},
           {"uptime_s", console::uptime()},
           {"frames", jobs::frame_count()},
           {"last_tick_age_s", since},
           {"main_thread_stalled", stalled},
           {"perf", jobs::frame_stats()},
           {"realms", realms_j},
           {"client_lock", agent::lock_status()},
           {"console_capture", console::method()},
           {"tmp_dir", c.tmp_dir},
           {"game_dir", c.game_dir},
           {"pending_jobs", jobs::pending()}};
    if (stalled) {
        s["warning"] = "the main thread has not run for " + std::to_string((int)since) +
                       " s (long load, freeze or infinite loop). game_terminate remains available.";
        return ok(s);
    }
    double to = args.value("timeout_s", 3.0);
    int host = c.mode == agent::Mode::Client ? realms::MENU : realms::SERVER;
    json h = agent::rt(host, "status", json::object(), to);
    if (h.value("ok", false)) s[c.mode == agent::Mode::Client ? "menu" : "server"] = h["result"];
    else s["host_error"] = h["error"];
    if (c.mode == agent::Mode::Client) {
        if (agent::realm_present(realms::SERVER)) {
            json sv = agent::rt(realms::SERVER, "status", json::object(), to);
            if (sv.value("ok", false)) s["server"] = sv["result"];
        }
        if (agent::realm_present(realms::CLIENT) && agent::lock_status().value("client_allowed", false)) {
            json cl = agent::rt(realms::CLIENT, "status", json::object(), to);
            if (cl.value("ok", false)) s["client"] = cl["result"];
        }
    }
    // Mounted folder (Issue #9): what changed in it since the realms loaded it.
    json lm = mount_state(mount_realms());
    if (!lm.is_null()) s["lua_mount"] = lm;
    // Convenience summary.
    json src = s.contains("server") ? s["server"] : (s.contains("client") ? s["client"] : json());
    if (!src.is_null()) {
        s["map"] = src.value("map", "");
        s["gamemode"] = src.value("gamemode", "");
        s["maxplayers"] = src.value("maxplayers", 0);
    } else {
        s["map"] = nullptr;
        if (s.contains("menu")) {
            s["gamemode"] = s["menu"].value("gamemode", json());
            s["maxplayers"] = s["menu"].value("maxplayers", json());
        }
    }
    if (s.contains("menu")) {
        s["gmod_version"] = s["menu"].value("version", json());
        s["gmod_version_str"] = s["menu"].value("version_str", json());
        s["branch"] = s["menu"].value("branch", json());
        s["loading"] = s["menu"].value("loading", false);
    }
    // Frame cap and time scale (a light test client runs at 20 fps: per-frame behaviour differs from a normal game).
    json hostj = s.contains("menu") ? s["menu"] : (s.contains("server") ? s["server"] : json());
    if (!hostj.is_null()) {
        s["fps_max"] = hostj.value("fps_max", json());
        s["host_timescale"] = hostj.value("host_timescale", json());
    }
    return project_keys(ok(s), args);
}

static json exec_payload(const json &args, const std::string &code, const std::string &chunk) {
    json p{{"code", code}, {"chunkname", args.value("chunkname", chunk)}};
    for (const char *k : {"budget", "jit", "depth", "limit", "max_string"})
        if (args.contains(k)) p[k] = args[k];
    return p;
}

// How long the caller waits for the game. There is no budget inside the game by default: an infinite loop freezes it,
// and this wait (timeout_s, default 60 s, or budget + 10 s) is how the caller finds out.
static double exec_timeout(const json &args) {
    if (args.contains("timeout_s")) return args["timeout_s"].get<double>();
    bool off = args.contains("jit") && args["jit"].is_boolean() && args["jit"].get<bool>();
    if (args.contains("budget") && !off) return args["budget"].get<double>() + 10.0;
    return 60.0;
}

static json lua_exec_tool(const json &args) {
    return project_values(rt_realm(args, "realm", realms::MENU, "lua_exec", exec_payload(args, args["code"], "=mcpagent:lua_exec"), exec_timeout(args)), args);
}

static json lua_exec_file_tool(const json &args) {
    std::string abs, chunk, e;
    if (!resolve_lua_file(args["path"], abs, chunk, e)) return agent::err("not_found", e);
    std::string code;
    if (!plat::read_file(abs, code)) return agent::err("io_error", "cannot read: " + abs);
    json r = rt_realm(args, "realm", realms::MENU, "lua_exec", exec_payload(args, code, chunk), exec_timeout(args));
    if (r.value("ok", false)) r["result"]["file"] = abs;
    return project_values(std::move(r), args);
}

// Finds a Lua file the way include() names it ("autorun/x.lua", "mygm/gamemode/init.lua"), or a path relative to the
// game folder ("lua/...", "addons/<name>/lua/..."), or an absolute path. Every match is returned, game folder first.
std::vector<std::string> find_game_lua(const std::string &path, std::string &err) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    if (fs::u8path(path).is_absolute()) {
        if (fs::is_regular_file(fs::u8path(path), ec)) out.push_back(path);
        else err = "file not found: " + path;
        return out;
    }
    std::string rel = normalize(path);
    if (!safe_rel(rel)) {
        err = "invalid path: relative, characters [A-Za-z0-9_.-/], no '..'";
        return out;
    }
    const std::string game = agent::config().game_dir;
    auto add = [&](const std::string &p) {
        if (fs::is_regular_file(fs::u8path(p), ec) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    std::string low = util::to_lower(rel);
    // The mounted Lua folder first, like the engine.
    std::string ov = lua_mount::current();
    if (!ov.empty()) {
        if (low.rfind("lua/", 0) == 0 || low.rfind("gamemodes/", 0) == 0) add(ov + "/" + rel);
        add(ov + "/lua/" + rel);
        add(ov + "/gamemodes/" + rel);
    }
    if (low.rfind("lua/", 0) == 0 || low.rfind("addons/", 0) == 0 || low.rfind("gamemodes/", 0) == 0) add(game + "/" + rel);
    add(game + "/lua/" + rel);
    add(game + "/gamemodes/" + rel);
    std::vector<std::string> addons;
    for (const auto &e : fs::directory_iterator(fs::u8path(game + "/addons"), ec))
        if (e.is_directory(ec)) addons.push_back(e.path().filename().u8string());
    std::sort(addons.begin(), addons.end());
    for (const auto &a : addons) {
        add(game + "/addons/" + a + "/lua/" + rel);
        add(game + "/addons/" + a + "/gamemodes/" + rel);
    }
    if (out.empty()) err = "no file '" + rel + "' under lua/, gamemodes/ or addons/*/lua|gamemodes/, nor relative to the game folder";
    return out;
}

// Realms a file runs in, from GMod's naming conventions (folders server/ client/, prefixes sv_ cl_).
static std::vector<int> default_realms(const std::string &chunk) {
    std::string l = util::to_lower(chunk);
    std::string file = l.substr(l.find_last_of('/') + 1);
    if (l.find("/server/") != std::string::npos || file.rfind("sv_", 0) == 0 || file == "init.lua") return {realms::SERVER};
    if (l.find("/client/") != std::string::npos || file.rfind("cl_", 0) == 0 || file == "cl_init.lua") return {realms::CLIENT};
    if (l.find("lua/menu/") != std::string::npos) return {realms::MENU};
    return {realms::SERVER, realms::CLIENT};
}

// ---- Mounted folder: provenance, changes since load, reload (Issue #9) ----

// Where a function came from. `source` is debug.getinfo's, as the engine names the file: "@lua/pba/x.lua" for a file
// of the mounted folder (named from its root) AND for the same Lua path inside a .gma or the game's lua/ folder, so the
// name alone cannot tell. The module knows what the engine was told: if the mounted folder was put in front of this
// realm's Lua path right before the realm loaded (recorded at that moment, with the folder's files), or the file was
// run again from it with lua_reload, the realm's copy came from the mount.
json source_provenance(int realm, const std::string &source) {
    json out{{"from_mount", false}};
    if (source.empty() || source[0] != '@') {
        out["provenance"] = "no_file";
        return out;
    }
    std::string rel = source.substr(1), root = lua_mount::current();
    std::error_code ec;
    bool has = false;
    std::string mfile;
    if (!root.empty() && !std::filesystem::u8path(rel).is_absolute()) {
        mfile = native_path(root + "/" + rel);
        has = std::filesystem::is_regular_file(std::filesystem::u8path(mfile), ec);
    }
    out["mount_has_file"] = has;
    bool reloaded = false;
    if (!mfile.empty() && lua_mount::loaded_from_mount(realm, rel, &reloaded)) {
        out["from_mount"] = true;
        out["disk"] = mfile;
        out["provenance"] = reloaded ? "reloaded_from_mount" : "mount_in_place_at_load";
        if (!has) out["provenance_note"] = "the file was in the mounted folder when this realm loaded it and has been deleted since";
        return out;
    }
    std::string abs, e;
    if (resolve_lua_path(rel, false, abs, e, false)) {
        out["disk"] = abs;
        out["provenance"] = "installed";
    } else {
        out["provenance"] = "not_on_disk";  // inside a .gma, or only on the server that sent it
    }
    if (has)
        out["provenance_note"] = "the mounted folder has a file of this name, but this realm did not load it from there (the folder was "
                      "mounted after the realm loaded, or the file was added to it since): map_load, or lua_reload the file";
    return out;
}

static const char *realm_name_of(int r) { return realms::name(r); }

// Realms with a Lua state and the module's runtime in it where mounted files matter (client only if it is ours to touch).
std::vector<int> mount_realms() {
    std::vector<int> rs;
    for (int r : {realms::SERVER, realms::CLIENT}) {
        if (!agent::realm_present(r) || !agent::realm_injected(r)) continue;
        if (r == realms::CLIENT && !agent::lock_status().value("client_allowed", false)) continue;
        rs.push_back(r);
    }
    return rs;
}

static std::function<json(int, const std::vector<lua_mount::Probe> &)> g_mount_probe;

void set_mount_probe_for_tests(std::function<json(int, const std::vector<lua_mount::Probe> &)> fn) { g_mount_probe = std::move(fn); }

// Asks one realm's file system about a few files of the mounted folder (Issue #9): answers [{exists, time?}] or null.
static json ask_engine(int realm, const std::vector<lua_mount::Probe> &probes) {
    if (g_mount_probe) return g_mount_probe(realm, probes);
    if (!agent::realm_present(realm) || !agent::realm_injected(realm)) return nullptr;
    json files = json::array();
    for (const auto &p : probes) files.push_back({{"name", p.name}});
    json r = agent::rt(realm, "mount_probe", json{{"files", files}}, 5.0);
    if (!r.value("ok", false) || !r["result"].is_object() || !r["result"].contains("answers")) return nullptr;
    return r["result"]["answers"];
}

// {engine_serves: {realm: bool}, engine_serves_folder: true|false|null}: what the realms' file systems say about the folder.
json mount_engine_serves(const std::vector<int> &rs) {
    json serves = json::object();
    bool any_true = false, any_false = false;
    for (int r : rs) {
        auto probes = lua_mount::pick_probes(lua_mount::probe_sample(r));
        if (probes.empty()) continue;
        json v = lua_mount::probes_verdict(probes, ask_engine(r, probes));
        if (v.is_null()) continue;
        serves[realm_name_of(r)] = v;
        (v.get<bool>() ? any_true : any_false) = true;
    }
    return json{{"engine_serves", serves}, {"engine_serves_folder", any_false ? json(false) : any_true ? json(true) : json(nullptr)}};
}

// status.lua_mount and the map_load check. null when no folder is mounted.
json mount_state(const std::vector<int> &rs) {
    std::string root = lua_mount::current();
    if (root.empty()) return nullptr;
    json st{{"path", root}};
    json with = json::object(), without = json::array();
    std::map<std::string, json> files;
    // Issue #9: a folder renamed or deleted since it was mounted serves nothing; every file would only show up as "deleted".
    bool missing = !lua_mount::folder_present(root);
    if (missing) st["folder_missing"] = true;
    for (int r : rs) {
        bool w = lua_mount::realm_loaded_with_mount(r);
        with[realm_name_of(r)] = w;
        if (!w) without.push_back(realm_name_of(r));
        if (missing) continue;
        for (const auto &c : lua_mount::changed_files(r)) {
            auto runs_in = default_realms(c.rel);
            if (std::find(runs_in.begin(), runs_in.end(), r) == runs_in.end()) continue;
            auto it = files.find(c.rel);
            if (it == files.end())
                it = files.emplace(c.rel, json{{"file", c.rel}, {"disk", native_path(root + "/" + c.rel)}, {"reason", c.reason},
                                               {"realms", json::array()}}).first;
            it->second["realms"].push_back(realm_name_of(r));
        }
    }
    json stale = json::array();
    for (auto &kv : files) stale.push_back(std::move(kv.second));
    st["loaded_with_mount"] = with;
    st["realms_without_mount"] = without;
    st["stale_files"] = stale;
    // Issue #9: the engine, not the module's bookkeeping, has the last word: does each realm's file system serve the folder?
    if (!missing) {
        json es = mount_engine_serves(rs);
        st["engine_serves"] = es["engine_serves"];
        st["engine_serves_folder"] = es["engine_serves_folder"];
    }
    std::string w = mount_warning(st);
    if (!w.empty()) st["warning"] = w;
    return st;
}

std::string mount_warning(const json &st) {
    if (!st.is_object()) return "";
    std::string out;
    if (st.value("folder_missing", false)) {
        // The rest (stale files, realms that predate the mount) is moot while there is nothing to serve.
        return "mount_folder_missing: the mounted folder " + st.value("path", std::string("?")) +
               " (or its lua/ folder) does not exist any more (renamed, moved or deleted), so the game serves nothing from it and runs the "
               "installed code. Restore it, or mount the right folder with lua_mount {path}, or remove it with lua_mount {path: null}; then map_load.";
    }
    if (st.contains("engine_serves_folder") && st["engine_serves_folder"] == json(false)) {
        out = "The game's file system does not serve this folder's files";
        std::string which;
        if (st.contains("engine_serves") && st["engine_serves"].is_object())
            for (auto it = st["engine_serves"].begin(); it != st["engine_serves"].end(); ++it)
                if (it.value() == json(false)) which += (which.empty() ? "" : "/") + it.key();
        if (!which.empty()) out += " (the " + which + " realm finds none of the files it was asked for)";
        out += ": the game runs the installed code, whatever the module reports as mounted. The path is probably one the engine cannot "
               "read (UNC, junction, over-long or non-ASCII path, a folder moved or deleted). Remove it with lua_mount {path: null}, "
               "mount a plain local path, then map_load.";
    }
    const json &stale = st.contains("stale_files") ? st["stale_files"] : json::array();
    if (stale.is_array() && !stale.empty()) {
        if (!out.empty()) out += " ";
        out += std::to_string(stale.size()) + " file(s) of the mounted folder changed since the game loaded them, so the game still runs the old code: ";
        size_t n = 0;
        for (const auto &f : stale) {
            if (n++) out += ", ";
            if (n > 8) {
                out += "...";
                break;
            }
            out += f.value("file", "");
        }
        out += ". lua_reload {mounted_changed: true} runs them again (or map_load).";
    }
    if (st.contains("realms_without_mount") && st["realms_without_mount"].is_array() && !st["realms_without_mount"].empty()) {
        if (!out.empty()) out += " ";
        out += "The ";
        size_t n = 0;
        for (const auto &r : st["realms_without_mount"]) out += (n++ ? "/" : "") + r.get<std::string>();
        out += " realm loaded its Lua before this folder was mounted, so it runs the installed addon's files: map_load.";
    }
    return out;
}

// What lua_reload {mounted_changed} runs, from a mount_state: {reload: [{file, disk, realms, reason}], skipped: [{file, why}]}.
// Files that were deleted cannot run; scripted classes (entities, weapons, effects: the engine builds their ENT/SWEP/EFFECT
// table and registers them) are not plain chunks, so running them again is left to an explicit lua_reload {path}.
json plan_mount_reload(const json &st) {
    json plan{{"reload", json::array()}, {"skipped", json::array()}};
    if (!st.is_object() || !st.contains("stale_files")) return plan;
    auto scripted = [](const std::string &rel) {
        std::string l = util::to_lower(rel);
        for (const char *p : {"lua/entities/", "lua/weapons/", "lua/effects/"})
            if (l.rfind(p, 0) == 0) return true;
        return l.find("/entities/") != std::string::npos || l.find("/weapons/") != std::string::npos || l.find("/effects/") != std::string::npos;
    };
    for (const auto &f : st["stale_files"]) {
        std::string rel = f.value("file", "");
        if (f.value("reason", "") == "deleted")
            plan["skipped"].push_back({{"file", rel}, {"why", "deleted from the mounted folder: the game keeps the old code until map_load"}});
        else if (scripted(rel))
            plan["skipped"].push_back({{"file", rel}, {"why", "scripted class file (entity, weapon, effect): the engine builds and registers it, running it again as a plain "
                                                              "chunk would not update the class; map_load, or lua_reload {path} to force it"}});
        else
            plan["reload"].push_back({{"file", rel}, {"disk", f.value("disk", "")}, {"realms", f["realms"]}, {"reason", f.value("reason", "")}});
    }
    return plan;
}

// Runs one file in the given realms (a reload). Files of the mounted folder are recorded as loaded from it, with the file
// time seen BEFORE the read, so an edit during the reload is still reported afterwards.
static json run_file_in_realms(const std::string &abs, const json &args, const std::vector<int> &rs, bool &all_ok, std::string &chunk) {
    long long mt = lua_mount::mtime_of(abs);
    std::string code;
    if (!plat::read_file(abs, code)) {
        all_ok = false;
        return json{{"error", {{"code", "io_error"}, {"message", "cannot read: " + abs}}}};
    }
    chunk = chunk_for(abs);
    std::string rel = lua_mount::rel_in_mount(abs);
    json per = json::object();
    for (int r : rs) {
        json res = agent::rt(r, "lua_exec", exec_payload(args, code, chunk), exec_timeout(args));
        bool rok = res.value("ok", false);
        all_ok = all_ok && rok;
        if (rok && !rel.empty()) lua_mount::note_reloaded(r, lua_mount::current(), rel, mt);
        per[realms::name(r)] = rok ? res["result"] : json{{"error", res.value("error", json())}};
    }
    return per;
}

static json reload_mounted_changed(const json &args) {
    std::vector<int> present = mount_realms();
    json st = mount_state(present);
    if (st.is_null()) return agent::err("no_mount", "no folder is mounted (lua_mount {path} first): there is nothing to reload from it");
    if (st.value("folder_missing", false)) return agent::err("mount_folder_missing", mount_warning(st));
    json plan = plan_mount_reload(st);
    json reloaded = json::array();
    bool all_ok = true;
    for (const auto &it : plan["reload"]) {
        std::vector<int> rs;
        for (const auto &n : it["realms"]) rs.push_back(realms::parse(n.get<std::string>()));
        std::string chunk;
        json per = run_file_in_realms(it["disk"].get<std::string>(), args, rs, all_ok, chunk);
        reloaded.push_back({{"file", it["file"]}, {"chunkname", chunk}, {"reason", it["reason"]}, {"realms", per}});
    }
    json out{{"ok", all_ok}, {"mode", "mounted_changed"}, {"path", st["path"]}, {"reloaded", reloaded}, {"skipped", plan["skipped"]},
             {"realms_without_mount", st["realms_without_mount"]}};
    if (reloaded.empty() && plan["skipped"].empty()) out["note"] = "no mounted file changed since the realms loaded them: nothing to reload";
    if (!st["realms_without_mount"].empty())
        out["warning"] = "a realm loaded its Lua before the folder was mounted: files run here from the folder, but the rest of its code is the installed copy: map_load";
    return ok(out);
}

static json lua_reload_tool(const json &args) {
    bool changed = args.value("mounted_changed", false), has_path = args.contains("path");
    if (changed && has_path) return agent::err("invalid_params", "pass either path or mounted_changed, not both");
    if (!changed && !has_path)
        return agent::err("invalid_params", "pass path (one file) or mounted_changed: true (every changed file of the mounted folder)");
    if (changed) return reload_mounted_changed(args);
    std::string e;
    auto found = find_game_lua(args["path"], e);
    if (found.empty()) return agent::err("not_found", e);
    const std::string &abs = found.front();
    std::string probe;
    if (!plat::read_file(abs, probe)) return agent::err("io_error", "cannot read: " + abs);
    std::string chunk = chunk_for(abs);
    std::vector<int> rs;
    if (args.contains("realms")) {
        for (const auto &n : args["realms"]) rs.push_back(realms::parse(n.get<std::string>()));
    } else {
        rs = default_realms(chunk);
    }
    bool all_ok = true;
    json per = run_file_in_realms(abs, args, rs, all_ok, chunk);
    json out{{"ok", all_ok}, {"file", native_path(abs)}, {"chunkname", chunk}, {"realms", per}};
    if (found.size() > 1) {
        out["other_matches"] = json::array();
        for (size_t i = 1; i < found.size(); ++i) out["other_matches"].push_back(found[i]);
        out["warning"] = "several files match this path: the first one was run (pass a longer or absolute path to pick another)";
    }
    return ok(out);
}

static json lua_check_tool(const json &args) {
    constexpr size_t kMaxFiles = 2000, kMaxBytes = 8u << 20;
    bool has_code = args.contains("code"), has_path = args.contains("path");
    if (has_code == has_path) return agent::err("invalid_params", "pass exactly one of code or path");
    json items = json::array();
    std::string abs;
    if (has_code) {
        items.push_back(json{{"file", "code"}, {"chunk", "=mcpagent:lua_check"}, {"code", args["code"]}});
    } else {
        std::string e;
        if (!resolve_lua_path(args["path"], true, abs, e)) return agent::err("not_found", e);
        std::error_code ec;
        auto fp = std::filesystem::u8path(abs);
        std::vector<std::string> files;
        if (std::filesystem::is_directory(fp, ec)) {
            std::string base = normalize(abs);
            while (base.size() > 1 && base.back() == '/') base.pop_back();
            for (const auto &rel : plat::list_files(abs, true)) {
                std::string l = util::to_lower(rel);
                if (l.size() > 4 && l.compare(l.size() - 4, 4, ".lua") == 0) files.push_back(base + "/" + rel);
            }
            std::sort(files.begin(), files.end());
            if (files.empty()) return agent::err("not_found", "no .lua file under: " + abs);
        } else if (std::filesystem::is_regular_file(fp, ec)) {
            files.push_back(abs);
        } else {
            return agent::err("not_found", "file not found: " + abs);
        }
        if (files.size() > kMaxFiles)
            return agent::err("invalid_params", std::to_string(files.size()) + " .lua files, the limit is " + std::to_string(kMaxFiles) +
                                                    ": check a subfolder");
        size_t total = 0;
        for (const auto &f : files) {
            std::string code;
            if (!plat::read_file(f, code)) return agent::err("io_error", "cannot read: " + f);
            if ((total += code.size()) > kMaxBytes) return agent::err("invalid_params", "more than 8 MB of Lua: check a subfolder");
            std::string chunk = chunk_for(f);
            items.push_back(json{{"file", chunk.substr(1)}, {"chunk", chunk}, {"code", std::move(code)}});
        }
    }
    json p{{"items", std::move(items)}, {"globals", args.value("globals", true)}, {"limit", args.value("limit", 200)}};
    json r = rt_realm(args, "realm", realms::MENU, "lua_check", p, 30.0);
    if (has_path && r.value("ok", false)) r["result"]["path"] = abs;
    return r;
}

static json lua_inspect_tool(const json &args) {
    json p{{"expr", args["expr"]}, {"depth", args.value("depth", 1)}, {"limit", args.value("limit", 100)}};
    json r = rt_realm(args, "realm", realms::MENU, "lua_inspect", p);
    // A function: where its code came from (Issue #9). The runtime gave `source`; the module knows about the mount.
    if (r.value("ok", false) && r["result"].is_object() && r["result"].value("type", "") == "function" && r["result"].contains("source")) {
        json pv = source_provenance(realm_of(args, "realm", realms::MENU), r["result"]["source"].get<std::string>());
        for (auto it = pv.begin(); it != pv.end(); ++it) r["result"][it.key()] = it.value();
    }
    return r;
}

static std::string decode_dump(const json &r) {
    std::string d = r.value("dump", "");
    if (r.value("encoding", "") == "base64") return util::base64_decode(d);
    std::string out;
    for (size_t i = 0; i + 1 < d.size(); i += 2) out += (char)std::stoi(d.substr(i, 2), nullptr, 16);
    return out;
}

// Is the function running in the game the code of the file on disk? Compares the function's bytecode with the file
// compiled again from disk, in the same realm (same parser), debug info aside.
static json lua_function_source_tool(const json &args) {
    int realm = realm_of(args, "realm", realms::MENU);
    if (realm < 0) return agent::err("invalid_params", "unknown realm");
    json a = agent::rt(realm, "function_dump", json{{"expr", args["expr"]}});
    if (!a.value("ok", false)) return a;
    const json &fi = a["result"];
    std::string src = fi.value("source", "");
    json out{{"source", src}, {"linedefined", fi.value("linedefined", 0)}, {"lastlinedefined", fi.value("lastlinedefined", 0)}};
    if (src.empty() || src[0] != '@') {
        out["matches_disk"] = nullptr;
        out["note"] = "defined by code that has no file (lua_exec, RunString, CompileString): nothing to compare";
        return ok(out);
    }
    json pv = source_provenance(realm, src);  // Issue #9: which copy this realm really ran
    for (auto it = pv.begin(); it != pv.end(); ++it) out[it.key()] = it.value();
    std::string abs, e;
    if (!pv.contains("disk")) {
        out["matches_disk"] = nullptr;
        out["note"] = "the file the realm ran is not on disk (inside a .gma, or only on the server that sent it)";
        return ok(out);
    }
    abs = pv["disk"].get<std::string>();
    {
        std::error_code ec0;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(abs), ec0)) {
            out["matches_disk"] = nullptr;
            out["note"] = "the file the realm ran no longer exists on disk: " + abs;
            return ok(out);
        }
    }
    out["file"] = abs;
    std::error_code ec;
    auto mt = std::filesystem::last_write_time(std::filesystem::u8path(abs), ec);
    if (!ec) out["file_age_s"] = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - mt).count();
    if (fi.contains("dump_error")) return agent::err("dump_failed", "string.dump failed: " + fi.value("dump_error", ""), out);
    std::string code;
    if (!plat::read_file(abs, code)) return agent::err("io_error", "cannot read: " + abs);
    json d = agent::rt(realm, "chunk_dump", json{{"code", code}, {"chunkname", src}});
    if (!d.value("ok", false)) {
        if (d["error"].value("code", "") == "lua_syntax_error") {
            out["matches_disk"] = false;
            out["note"] = "the file on disk no longer compiles: " + d["error"].value("message", "");
            return ok(out);
        }
        return d;
    }
    std::vector<ljdump::Proto> live, disk;
    if (!ljdump::parse(decode_dump(fi), live, e) || !ljdump::parse(decode_dump(d["result"]), disk, e))
        return agent::err("dump_unreadable", e, out);
    auto m = ljdump::find(live, disk);
    out["matches_disk"] = m.found;
    if (!m.found) {
        out["note"] = "the running function is NOT the code in the file: the file was edited after it was loaded, or the "
                      "function came from another copy. lua_reload the file (or reload the map) to run the current code.";
    } else {
        out["line_on_disk"] = m.disk_line;
        if (m.disk_line != (uint32_t)fi.value("linedefined", 0))
            out["note"] = "same code, but lines were added or removed above it: it now starts at line " + std::to_string(m.disk_line);
    }
    return ok(out);
}

static json file_write(const json &args) {
    std::string e, p = tmp_path(args["name"], e);
    if (p.empty()) return agent::err("invalid_params", e);
    std::filesystem::path fp = std::filesystem::u8path(p);
    plat::make_dirs(fp.parent_path().u8string());
    std::string content = args["content"];
    if (!plat::write_file(p, content)) return agent::err("io_error", "cannot write: " + p);
    return ok(json{{"path", p}, {"bytes", content.size()}, {"exec_path", args["name"]}});
}

static json file_read(const json &args) {
    std::string e, p = tmp_path(args["name"], e);
    if (p.empty()) return agent::err("invalid_params", e);
    std::string content;
    if (!plat::read_file(p, content)) return agent::err("not_found", "file not found: " + p);
    return ok(json{{"path", p}, {"content", content}});
}

static json file_list(const json &) {
    auto files = plat::list_files(agent::config().tmp_dir, true);
    std::sort(files.begin(), files.end());
    return ok(json{{"dir", agent::config().tmp_dir}, {"files", files}});
}

static json file_delete(const json &args) {
    std::string e, p = tmp_path(args["name"], e);
    if (p.empty()) return agent::err("invalid_params", e);
    if (!plat::remove_file(p)) return agent::err("not_found", "file not found: " + p);
    return ok(json{{"deleted", p}});
}

void register_core() {
    json RO = {{"readOnlyHint", true}};
    json realm_exec = realm_prop("Lua state to run in. Default: menu (client mode), which has no game libraries (ents, "
                                 "util.TraceLine, players...): pass 'server' or 'client' for game code; they exist only once a map "
                                 "is loaded (map_load). On srcds only 'server' exists.");

    mcp::register_tool({"status", "Instance status",
                        "GMod version, branch, platform, map, gamemode, maxplayers, available realms (and injection generation), "
                        "client lock, fps/frametime (perf), fps_max (the frame cap: 20 on a light test client) and host_timescale, "
                        "loading (a map load in progress), uptime, age of the main thread's last tick (main_thread_stalled if > 5 s), "
                        "Lua temporary directory (tmp_dir, absolute path), and with a mounted Lua folder (lua_mount) `lua_mount`: {path, "
                        "loaded_with_mount, realms_without_mount, stale_files: [{file, disk, reason, realms}], warning?} (the folder's files that "
                        "changed since each realm loaded them). Call this first. Works even if the game is frozen. "
                        "Result: summary fields at the top (instance, mode, map, gamemode, maxplayers, fps_max, loading, "
                        "main_thread_stalled...) and one object per realm (menu, server, client) with its own details; `fields` "
                        "keeps only some top-level keys. Example: {\"fields\": [\"map\", \"realms\", \"perf\"]}.",
                        json{{"properties", {{"timeout_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 30},
                                                            {"description", "Max wait for Lua info (default 3)."}}},
                                             {"fields", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 50},
                                                         {"description", "Keep only these top-level keys, e.g. [\"map\", \"perf\", \"fps_max\"]."}}}}}},
                        RO, status_tool});

    json exec_props = {{"realm", realm_exec},
                       {"budget", {{"type", "number"}, {"minimum", 0.05}, {"maximum", 600},
                                   {"description", "Optional time limit in seconds, best effort (none by default). It stops loops "
                                                   "written in this code itself (error budget_exceeded), not a loop inside a "
                                                   "compiled function it calls. Without it, an infinite loop freezes the game: the "
                                                   "call fails with main_thread_timeout after timeout_s, status shows "
                                                   "main_thread_stalled, and game_terminate ends the game."}}},
                       {"timeout_s", {{"type", "number"}, {"minimum", 1}, {"maximum", 3600},
                                      {"description", "How long to wait for the game's answer (default 60, or budget + 10). Past "
                                                      "it the call fails with main_thread_timeout; the code may still be running."}}},
                       {"jit", {{"type", "boolean"},
                                {"description", "Obsolete. gmod-mcp no longer turns the LuaJIT compiler off, neither for this code "
                                                "nor for the functions it defines (hooks and timers run at real speed, timings "
                                                "are real). jit: true now only ignores `budget`."}}},
                       {"chunkname", {{"type", "string"}, {"maxLength", 260},
                                      {"description", "Chunk name: the source shown by debug.getinfo, errors and tracebacks, e.g. "
                                                      "'@addons/my_addon/lua/my_addon/core.lua' for code that identifies its "
                                                      "caller by file. Default: '=mcpagent:lua_exec', or the file's path."}}},
                       {"depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 200},
                                  {"description", "Cut tables deeper than this (default: no cut)."}}},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000000},
                                  {"description", "Cut tables longer than this (default: no cut)."}}},
                       {"max_string", {{"type", "integer"}, {"minimum", 16}, {"maximum", 1073741824},
                                       {"description", "Cut strings longer than this (default: no cut)."}}}};
    exec_props["fields"] = {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 50},
                            {"description", "Keep only these keys of each returned table (e.g. [\"pass\", \"fails\"]): the rest "
                                            "of a large result is dropped before it reaches you. Only returned tables are projected: when none is "
                                            "(a JSON string, a number), the result says so in fields_note."}};
    json exec_code = exec_props;
    exec_code["code"] = {{"type", "string"}, {"description", "Lua code. Use `return` to return values (several allowed)."}};
    mcp::register_tool({"lua_exec", "Execute Lua",
                        "Executes Lua code in a realm under xpcall + debug.traceback, at full speed (the LuaJIT compiler is "
                        "left on; no time limit unless `budget`). Returns all serialized return values (values, n), captured "
                        "print/Msg/MsgN/MsgC (output), duration_ms, chunk_bytes, chunkname and the realm's generation (it "
                        "changes on every map load: compare it with map_load's result to know which state ran the code). "
                        "Values come back whole: nothing is cut unless you pass depth, limit or max_string, and then the "
                        "result says truncated: <count>. A value too big to send (over a million values or 64 MiB of "
                        "strings) fails with result_too_large; a whole _G is about 4.5 MB: return only what you need. "
                        "Non-JSON types: {__type: Vector|Angle|Color|Entity|function|cycle|...}. Example: {\"realm\": \"server\", "
                        "\"code\": \"return #player.GetAll(), game.GetMap()\"} -> {values: [1, \"gm_construct\"], n: 2, output: \"\", "
                        "duration_ms, chunk_bytes, chunkname, generation}. "
                        "Errors: lua_syntax_error, lua_error (with traceback), budget_exceeded, main_thread_timeout, "
                        "result_too_large, realm_unavailable, client_locked. "
                        "Code that is already in a file: lua_exec_file (or lua_job {path}), not its content pasted here. "
                        "Some convars cannot be set from Lua (RunConsoleCommand is refused for protected ones, e.g. "
                        "volume_sfx, fps_max, silently): when the code calls RunConsoleCommand with such a command the result lists it "
                        "in blocked_console_commands with a warning (only RunConsoleCommand calls made by this code while it runs "
                        "are seen; a command refused for another reason, e.g. a cheat convar, is not): use convar_set or "
                        "console_exec, then read the value back.",
                        json{{"properties", exec_code}, {"required", {"code"}}}, json{{"destructiveHint", true}}, lua_exec_tool});

    json exec_file = exec_props;
    exec_file["path"] = {{"type", "string"},
                         {"description", "Absolute path on the game's disk (e.g. a file in your git worktree), or relative: looked up in "
                                         "lua/mcpagent_tmp/, then the mounted Lua folder if any (lua_mount), then the game folder ('addons/x/lua/y.lua'), then lua/, gamemodes/ and "
                                         "addons/*/lua/ (include() style)."}};
    mcp::register_tool({"lua_exec_file", "Execute a Lua file",
                        "Like lua_exec but reads the code from a file on the game's disk (with a mounted Lua folder, see "
                        "lua_mount, a relative path finds its files first, like the engine). The chunk is named after the real path "
                        "(e.g. @lua/mcpagent_tmp/test.lua) for actionable errors and tracebacks, or `chunkname` (e.g. the "
                        "addon-relative name of a file from your git worktree). Prefer it to pasting a file into lua_exec's "
                        "code: it saves the tokens of the file on every call. With `fields`, only the test's verdict comes back. "
                        "Example: {\"realm\": \"server\", \"path\": \"tests/check_doors.lua\", \"fields\": [\"pass\", \"fails\"]} "
                        "(relative: lua/mcpagent_tmp/tests/check_doors.lua first).",
                        json{{"properties", exec_file}, {"required", {"path"}}}, json{{"destructiveHint", true}}, lua_exec_file_tool});

    json reload_props = exec_props;
    reload_props.erase("realm");
    reload_props["path"] = {{"type", "string"},
                            {"description", "The file as include() names it (e.g. 'autorun/my_addon.lua', 'my_addon/sh_core.lua'), "
                                            "a path relative to the game folder ('addons/my_addon/lua/...'), or an absolute path."}};
    reload_props["mounted_changed"] = {{"type", "boolean"},
                                       {"description", "Instead of `path`: reload every file of the mounted Lua folder (lua_mount) that changed since each realm "
                                                       "loaded it (status.lua_mount.stale_files), each in the realms it runs in. Not with `path`."}};
    reload_props["realms"] = {{"type", "array"}, {"minItems", 1}, {"uniqueItems", true},
                              {"items", {{"type", "string"}, {"enum", json::array({"menu", "client", "server"})}}},
                              {"description", "Realms to run it in. Default from the name: server/ folder, sv_ prefix or init.lua -> "
                                              "server; client/ folder or cl_ prefix -> client; lua/menu/ -> menu; otherwise "
                                              "server then client (shared file)."}};
    mcp::register_tool({"lua_reload", "Reload a Lua file from disk",
                        "Reads an addon or gamemode file from the disk again and runs it in its realms, to test an edit without "
                        "reloading the map, in every realm it belongs to in one call. On the machine that hosts the game (singleplayer "
                        "or listen server) include() from lua_exec also reads the edited file, in the server and the client realm "
                        "(checked live); a client connected to a separate server (e.g. a local srcds) only has the copies that "
                        "server sent, so there lua_reload is the way to run an edit on the client. Looks under "
                        "the mounted Lua folder if any (lua_mount), then lua/, gamemodes/, addons/*/lua|gamemodes/: the engine's order, "
                        "so the copy the game would load (other matches are listed). Errors name the real file. "
                        "Running a file again redefines its functions and hooks; what it built at load time (entities, timers, "
                        "net strings) is not reset. Returns {ok, file, chunkname, realms: {<realm>: lua_exec result or {error}}, "
                        "other_matches?}. Example: {\"path\": \"my_addon/sh_core.lua\"} (shared file: runs in server then client). "
                        "With a mounted folder (lua_mount) there is no autorefresh: `mounted_changed: true` (instead of `path`) runs again every "
                        "file of that folder whose time differs from what each realm loaded (modified, added; the list is status.lua_mount.stale_files), "
                        "in alphabetical order and in the realms each file belongs to, and returns {ok, mode, path, reloaded: [{file, chunkname, "
                        "reason, realms: {<realm>: lua_exec result or {error}}}], skipped: [{file, why}] (deleted files, entity/weapon/effect "
                        "files: map_load or lua_reload {path}), realms_without_mount, note?, warning?}; error no_mount if no folder is mounted. "
                        "Limits: a reload re-runs the file, it does not undo what the old copy did (hooks and timers it added under another name, "
                        "net strings, entities), and it does not reload a realm that loaded before the folder was mounted (map_load).",
                        json{{"properties", reload_props}}, json{{"destructiveHint", true}}, lua_reload_tool});

    mcp::register_tool(
        {"lua_check", "Check Lua without running it",
         "Compiles Lua with the game's own parser (GLua syntax: !=, &&, ||, !, //, /* */, continue) and runs nothing. "
         "`code`, or `path`: a file or a whole folder (every .lua under it, e.g. an addon), absolute or relative to the "
         "game folder or lua/mcpagent_tmp/. Returns ok, errors [{file, line, message}] (one per file: the parser stops at the first error), and from "
         "the bytecode: unknown_globals (globals read but neither defined in the checked files nor present in the realm: "
         "typos, missing includes, wrong realm) and globals_written (every global assignment, e.g. a forgotten `local`). "
         "Unknown globals depend on the realm: pick the one the code runs in ('client'/'server' need a map; 'menu' lacks "
         "most game functions). ENT, SWEP, GM, EFFECT and TOOL are never reported. Result: {ok, realm, checked (files), errors: "
         "[{file, line, message}], unknown_globals: [{name, file, line}], globals_written: [{name, file, line}], "
         "truncated? (lists cut at limit), notes?}. Errors: "
         "realm_unavailable, not_found. Example: {\"realm\": \"server\", \"path\": \"addons/my_addon/lua\"}.",
         json{{"properties",
               {{"realm", realm_prop("Lua state whose parser and globals are used: 'menu' (default, always present), 'client' or 'server'.")},
                {"code", {{"type", "string"}, {"description", "Lua code to check (instead of path)."}}},
                {"path", {{"type", "string"}, {"description", "File or folder to check (instead of code): absolute, or relative (looked up in lua/mcpagent_tmp/, the mounted Lua folder if any, the game folder "
                                                           "e.g. 'addons/my_addon/lua', then lua/, gamemodes/, addons/*/lua/)."}}},
                {"globals", {{"type", "boolean"}, {"description", "Also list unknown and written globals (default true)."}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Max entries per list (default 200)."}}}}}},
         RO, lua_check_tool});

    mcp::register_tool({"lua_inspect", "Inspect a Lua value",
                        "Evaluates an expression in a realm (it runs: keep it side-effect free; 2 s time limit) and describes the "
                        "value. Result: {type, value (serialized to `depth`; 0 = summary only), and for a table: count, keys: [{key, "
                        "key_type, type}] up to `limit`, truncated; metatable?: {type, name (MetaName), keys, methods (functions of "
                        "__index)}}; for a function also {source, linedefined, lastlinedefined, from_mount, disk, mount_has_file, provenance} "
                        "(see lua_function_source). Use it to discover an API or a structure; use lua_exec to compute something. Example: "
                        "{\"realm\": \"server\", \"expr\": \"FindMetaTable(\\\"Player\\\")\", \"depth\": 0, \"limit\": 300}.",
                        json{{"properties",
                              {{"realm", realm_exec},
                               {"expr", {{"type", "string"}, {"description", "Lua expression, e.g. 'hook.GetTable()' or 'FindMetaTable(\"Entity\")'."}}},
                               {"depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 8}, {"description", "Depth (default 1)."}}},
                               {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Max number of keys (default 100)."}}}}},
                             {"required", {"expr"}}},
                        RO, lua_inspect_tool});

    mcp::register_tool({"lua_function_source", "Is this function the code on disk?",
                        "Tells where a function was defined and whether the game runs the current code of that file: its "
                        "bytecode is compared with the file compiled again from disk, in the same realm. Use it before "
                        "trusting a test result after an edit (a map that was not reloaded, a file the client got from the "
                        "server, another copy of the addon). Pick the realm the function lives in (default menu: addon code is "
                        "usually in 'server' or 'client'). The file is found like the engine does (mounted Lua folder first, then "
                        "lua/, gamemodes/, addons). Result: {source (\"@<path>\" as the engine names it), linedefined, "
                        "lastlinedefined, file/disk (absolute; the copy this realm ran), from_mount (true: it came from the mounted Lua folder, "
                        "recorded when the folder was put in front of the realm's Lua path; false: the installed copy), mount_has_file, "
                        "provenance (mount_in_place_at_load | reloaded_from_mount | installed | not_on_disk | no_file), file_age_s, matches_disk: true (same code) | false (edited since it was "
                        "loaded, or another copy) | null (no file: code from lua_exec/RunString, or only inside a .gma), "
                        "line_on_disk (where it starts now), note}. Example: {\"realm\": \"server\", \"expr\": \"MyAddon.Update\"}.",
                        json{{"properties",
                              {{"realm", realm_exec},
                               {"expr", {{"type", "string"}, {"description", "Lua expression giving the function, e.g. 'MyAddon.Update' or 'hook.GetTable().Think.MyHook'."}}}}},
                             {"required", {"expr"}}},
                        RO, lua_function_source_tool});

    json name_prop = {{"type", "string"}, {"description", "Name relative to the lua/mcpagent_tmp/ directory (e.g. 'scan.lua', 'lib/util.lua')."}};
    mcp::register_tool({"lua_file_write", "Write a temporary Lua file",
                        "Writes (or overwrites) a file in the game's lua/mcpagent_tmp/ folder, creating subfolders. Useful when you do "
                        "not share the game's disk; if you do, write there directly. Run it with lua_exec_file {path: name}; from "
                        "game code it is include(\"mcpagent_tmp/<name>\"). Result: {path (absolute), bytes, exec_path}.",
                        json{{"properties", {{"name", name_prop}, {"content", {{"type", "string"}, {"description", "File content."}}}}},
                             {"required", {"name", "content"}}},
                        json{{"destructiveHint", false}, {"idempotentHint", true}}, file_write});
    mcp::register_tool({"lua_file_read", "Read a temporary Lua file",
                        "Reads a file of the game's lua/mcpagent_tmp/ folder. Result: {path, content}. Error not_found.",
                        json{{"properties", {{"name", name_prop}}}, {"required", {"name"}}}, RO, file_read});
    mcp::register_tool({"lua_file_list", "List temporary Lua files",
                        "Lists every file under the game's lua/mcpagent_tmp/ folder, subfolders included. Result: {dir (absolute), "
                        "files: [paths relative to dir, sorted]}.", json::object(), RO, file_list});
    mcp::register_tool({"lua_file_delete", "Delete a temporary Lua file",
                        "Deletes one file of the game's lua/mcpagent_tmp/ folder (code it defined keeps running). Result: {deleted}. "
                        "Error not_found.",
                        json{{"properties", {{"name", name_prop}}}, {"required", {"name"}}}, json{{"destructiveHint", true}}, file_delete});
}

void register_all() {
    static bool done = false;
    if (done) return;
    done = true;
    register_core();
    register_session();
    register_console();
    register_world();
    register_render();
    register_docs();
    register_reports();
    register_lua_tools();
    register_jobs();
    register_instances();
    register_lua_mount();
    // Long-running tools executable as MCP tasks (declared after all tools are registered).
    for (const char *n : {"events_wait", "reports_read"}) mcp::set_task_support(n, "optional");
}

}  // namespace tools
