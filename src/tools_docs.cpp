// Tools: api_search (runtime introspection), wiki_search, wiki_page (engine HTTP, whitelist, local cache).
#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

#include "agent.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "tools.h"
#include "util.h"

namespace tools {

static const char *kWikiBase = "https://wiki.facepunch.com/gmod/";  // only outbound destination (hard-coded whitelist)
static const int64_t kPagelistTtl = 24 * 3600;
static const int64_t kPageTtl = 7 * 24 * 3600;

static int host_realm() { return agent::config().mode == agent::Mode::Client ? realms::MENU : realms::SERVER; }
static std::string cache_dir() { return agent::config().data_dir + "/wiki"; }
static int64_t now_s() { return (int64_t)std::time(nullptr); }

// Engine HTTP GET (http_get + polling). Returns {"ok":true,"code":..,"body":..} or an error.
static json http_get(const std::string &url, double timeout_s) {
    // Test switch: simulate a network outage (cache fallback scenarios) without touching the firewall.
    const char *off = std::getenv("MCPAGENT_WIKI_OFFLINE");
    if (off && std::string(off) == "1") return agent::err("network_error", "network disabled by MCPAGENT_WIKI_OFFLINE=1");
    json r = agent::rt(host_realm(), "http_get", json{{"url", url}}, 5.0);
    if (!r.value("ok", false)) return r;
    int id = r["result"]["id"];
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        plat::sleep_ms(50);
        json p = agent::rt(host_realm(), "http_poll", json{{"id", id}}, 5.0);
        if (!p.value("ok", false)) return p;
        if (p["result"].value("done", false)) {
            if (p["result"].contains("error")) return agent::err("network_error", p["result"]["error"].get<std::string>());
            std::string body;
            const std::string b64 = p.value("blob_b64", "");
            body = util::base64_decode(b64);
            return json{{"ok", true}, {"code", p["result"].value("code", 0)}, {"body", body}};
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeout_s)
            return agent::err("timeout", "no HTTP response within " + std::to_string((int)timeout_s) + " s");
    }
}

// Cache file: first line = unix timestamp, rest = body.
static bool cache_read(const std::string &path, std::string &body, int64_t &age) {
    std::string raw;
    if (!plat::read_file(path, raw)) return false;
    auto nl = raw.find('\n');
    if (nl == std::string::npos) return false;
    age = now_s() - std::atoll(raw.substr(0, nl).c_str());
    body = raw.substr(nl + 1);
    return true;
}

static void cache_write(const std::string &path, const std::string &body) {
    plat::make_dirs(cache_dir());
    plat::write_file(path, std::to_string(now_s()) + "\n" + body);
}

// Fetch with cache: fresh cache -> cache; otherwise network; network failure -> stale cache if any.
static json cached_get(const std::string &url, const std::string &path, int64_t ttl, bool force) {
    std::string body;
    int64_t age = 0;
    bool have = cache_read(path, body, age);
    if (have && age < ttl && !force) return json{{"ok", true}, {"body", body}, {"cache", "hit"}, {"age_s", age}};
    json r = http_get(url, 20.0);
    if (r.value("ok", false) && r.value("code", 0) == 200) {
        cache_write(path, r["body"].get<std::string>());
        return json{{"ok", true}, {"body", r["body"]}, {"cache", have ? "refreshed" : "miss"}, {"age_s", 0}};
    }
    if (have) return json{{"ok", true}, {"body", body}, {"cache", "stale"}, {"age_s", age}, {"network_error", r.value("error", json())}};
    if (r.value("ok", false)) return agent::err("http_error", "HTTP " + std::to_string(r.value("code", 0)) + " for " + url);
    return r;
}

static std::mutex g_pl_mu;
static std::vector<std::string> g_pagelist;
static int64_t g_pagelist_loaded = 0;

static json load_pagelist(bool force, std::string &cache_state) {
    json r = cached_get(std::string(kWikiBase) + "~pagelist?format=json", cache_dir() + "/pagelist.json", kPagelistTtl, force);
    if (!r.value("ok", false)) return r;
    cache_state = r.value("cache", "");
    try {
        json arr = json::parse(r["body"].get<std::string>());
        std::vector<std::string> pages;
        for (const auto &e : arr)
            if (e.is_object() && e.contains("address") && !e["address"].get<std::string>().empty()) pages.push_back(e["address"]);
        std::lock_guard<std::mutex> lk(g_pl_mu);
        g_pagelist = std::move(pages);
        g_pagelist_loaded = now_s();
    } catch (const std::exception &ex) {
        return agent::err("bad_pagelist", std::string("unreadable page list: ") + ex.what());
    }
    return json{{"ok", true}};
}

// Page list from cache only (never hits the network): used to annotate api_search results.
std::set<std::string> pagelist_from_cache() {
    {
        std::lock_guard<std::mutex> lk(g_pl_mu);
        if (!g_pagelist.empty()) return std::set<std::string>(g_pagelist.begin(), g_pagelist.end());
    }
    std::string body;
    int64_t age;
    std::set<std::string> out;
    if (!cache_read(cache_dir() + "/pagelist.json", body, age)) return out;
    try {
        for (const auto &e : json::parse(body))
            if (e.contains("address")) out.insert(e["address"].get<std::string>());
    } catch (...) {
    }
    return out;
}

static int score(const std::string &page, const std::string &q) {
    std::string p = util::to_lower(page), s = util::to_lower(q);
    if (p == s) return 100;
    // Match the part after the separator too (e.g. "EmitSound" -> "Entity:EmitSound").
    auto sep = p.find_last_of(".:");
    std::string tail = sep == std::string::npos ? p : p.substr(sep + 1);
    if (tail == s) return 95;
    if (p.rfind(s, 0) == 0) return 80;
    if (tail.rfind(s, 0) == 0) return 75;
    if (p.find(s) != std::string::npos) return 60;
    // Approximate: all characters of the query in order (subsequence).
    size_t i = 0;
    for (char c : p)
        if (i < s.size() && c == s[i]) ++i;
    if (i == s.size()) return 30 + (int)(20.0 * (double)s.size() / (double)p.size());
    return 0;
}

static json wiki_search_tool(const json &a) {
    std::string q = a["query"];
    std::string state;
    json l = load_pagelist(a.value("refresh", false), state);
    if (!l.value("ok", false)) return l;
    std::vector<std::pair<int, std::string>> hits;
    {
        std::lock_guard<std::mutex> lk(g_pl_mu);
        for (const auto &p : g_pagelist) {
            int sc = score(p, q);
            if (sc > 0) hits.emplace_back(sc, p);
        }
    }
    std::sort(hits.begin(), hits.end(), [](const auto &x, const auto &y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
    size_t limit = a.value("limit", 20);
    json res = json::array();
    for (size_t i = 0; i < hits.size() && i < limit; ++i)
        res.push_back(json{{"page", hits[i].second}, {"score", hits[i].first}, {"url", std::string(kWikiBase) + hits[i].second}});
    return ok(json{{"query", q}, {"results", res}, {"total_matches", hits.size()}, {"cache", state}});
}

static std::string url_encode_page(const std::string &s) {
    std::string out;
    static const char *hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '.' || c == ':' || c == '_' || c == '-' || c == '~') out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

json wiki_page_tool(const json &a) {
    std::string name = a["name"];
    std::string fmt = a.value("format", "text");
    std::string url = std::string(kWikiBase) + url_encode_page(name) + "?format=" + fmt;
    std::string path = cache_dir() + "/page_" + util::hex(reinterpret_cast<const unsigned char *>(name.data()), name.size()) + "." + fmt;
    json r = cached_get(url, path, kPageTtl, a.value("refresh", false));
    if (!r.value("ok", false)) return r;
    std::string body = r["body"];
    if (body.find("<title>Page Not Found</title>") != std::string::npos) {
        plat::remove_file(path);
        return agent::err("not_found", "wiki page not found: " + name + " (try wiki_search)");
    }
    json out{{"name", name}, {"format", fmt}, {"url", std::string(kWikiBase) + url_encode_page(name)}, {"cache", r["cache"]}};
    if (fmt == "json") {
        try {
            out["page"] = json::parse(body);
        } catch (...) {
            out["content"] = body;
        }
    } else {
        out["content"] = body;
    }
    return ok(out);
}

// api_search: merges the runtime index of every available realm.
static json api_search_tool(const json &a) {
    std::vector<int> targets;
    if (a.contains("realm")) {
        int r = realms::parse(a["realm"]);
        if (r < 0) return agent::err("invalid_params", "unknown realm");
        targets.push_back(r);
    } else {
        for (int r = 0; r < realms::COUNT; ++r) targets.push_back(r);
    }
    auto pages = pagelist_from_cache();
    // Class prefixes of "Class:Method" wiki pages, probed with FindMetaTable in each realm.
    std::set<std::string> meta;
    for (const auto &p : pages) {
        auto c = p.find(':');
        if (c != std::string::npos && c > 0 && p.find('/') == std::string::npos) meta.insert(p.substr(0, c));
    }
    std::map<std::string, json> merged;
    json realms_status = json::object();
    for (int r : targets) {
        json res = agent::rt(r, "api_search", json{{"pattern", a["pattern"]}, {"limit", a.value("limit", 500)}, {"meta_names", meta}}, 15.0);
        if (!res.value("ok", false)) {
            realms_status[realms::name(r)] = res["error"].value("code", "error");
            continue;
        }
        realms_status[realms::name(r)] = "ok";
        for (const auto &m : res["result"]["matches"]) {
            std::string n = m["name"];
            auto &e = merged[n];
            if (e.is_null()) e = json{{"name", n}, {"kind", m["kind"]}, {"realms", json::array()}};
            e["realms"].push_back(realms::name(r));
        }
    }
    json out = json::array();
    for (auto &kv : merged) {
        json e = kv.second;
        std::string page = e["kind"] == "global" ? "Global." + kv.first : kv.first;
        if (pages.empty()) e["wiki_page"] = nullptr;
        else e["wiki_page"] = pages.count(page) ? json(std::string(kWikiBase) + page) : json(nullptr);
        out.push_back(e);
    }
    json r{{"pattern", a["pattern"]}, {"results", out}, {"count", out.size()}, {"realms_searched", realms_status},
           {"wiki_index", pages.empty() ? "not cached yet (call wiki_search once to enable wiki_page links)" : "cached"}};
    return ok(r);
}

void register_docs() {
    json RO = {{"readOnlyHint", true}};
    mcp::register_tool({"api_search", "Search the live Lua API",
                        "Runtime introspection of what really exists in each realm of the installed game: global functions, library "
                        "functions (util.TraceLine) and metatable methods (Entity:EmitSound). Each result lists the realms where it exists "
                        "and the matching wiki page if any. Source of truth for realms (the wiki can be wrong). Pattern: case-insensitive "
                        "substring, or glob with *.",
                        json{{"properties", {{"pattern", {{"type", "string"}, {"minLength", 1}}},
                                             {"realm", realm_prop("Restrict to one realm (default: all available).")},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Per realm, default 500."}}}}},
                             {"required", {"pattern"}}},
                        RO, api_search_tool});
    json open_world = {{"readOnlyHint", true}, {"openWorldHint", true}};
    mcp::register_tool({"wiki_search", "Search the GMod wiki",
                        "Searches page names of https://wiki.facepunch.com/gmod/ (exact, prefix, substring, approximate). The page list is "
                        "cached locally for 24 h; when offline the stale cache is used.",
                        json{{"properties", {{"query", {{"type", "string"}, {"minLength", 1}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"description", "Default 20."}}},
                                             {"refresh", {{"type", "boolean"}, {"description", "Ignore the cache."}}}}},
                             {"required", {"query"}}},
                        open_world, wiki_search_tool});
    mcp::register_tool({"wiki_page", "Read a GMod wiki page",
                        "Raw wiki markup of a page (?format=text, compact) or its JSON form (?format=json). Names like Entity:EmitSound, "
                        "GM:EntityEmitSound, util.TraceLine, Global.EmitSound. Cached 7 days; stale cache used when offline.",
                        json{{"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 200}}},
                                             {"format", {{"type", "string"}, {"enum", {"text", "json"}}, {"description", "Default text."}}},
                                             {"refresh", {{"type", "boolean"}}}}},
                             {"required", {"name"}}},
                        open_world, wiki_page_tool});
}

}  // namespace tools
