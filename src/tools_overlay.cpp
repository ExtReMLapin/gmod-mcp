// Tool: lua_overlay (per-instance Lua overlay: a folder searched before every addon, in this game process only).
#include "agent.h"
#include "jobs.h"
#include "mcp.h"
#include "overlay.h"
#include "tools.h"

namespace tools {

static json lua_overlay_tool(const json &args) {
    bool set = args.contains("path");
    std::string root;
    // The path is checked first, on any platform, before touching the game's main thread.
    if (set && args["path"].is_string() && !args["path"].get<std::string>().empty()) {
        std::string e;
        if (!overlay::normalize_root(args["path"].get<std::string>(), root, e)) return agent::err("invalid_params", e);
    }
    std::string why;
    if (!overlay::supported(&why)) return agent::err("unsupported_platform", why);
    bool empty_cache = args.value("empty_cache", true);
    json r = jobs::run_on_main([=] { return set ? overlay::set(root, empty_cache) : overlay::info(); }, 10.0);
    if (jobs::is_timeout(r))
        return agent::err("main_thread_timeout", "the main thread did not respond in time (map loading, freeze or infinite loop?)");
    if (r.is_object() && r.contains("__error")) return agent::err("internal", r["__error"].get<std::string>());
    return r;
}

void register_overlay() {
    mcp::register_tool(
        {"lua_overlay", "Load Lua from another folder first",
         "Per-instance code overlay: makes THIS game process look for Lua files in a folder of your choice (e.g. the git "
         "worktree of an addon) before every addon, so include(), autorun files and AddCSLuaFile get your branch's "
         "files instead of the installed addon's, at every map load, without touching the shared addons/ folder or other "
         "instances. `path`: the addon's root (the folder that contains lua/) or its lua/ folder, absolute. "
         "IMPORTANT: it applies to files loaded from now on; code already running is not re-run, so call map_load "
         "right after (then check with lua_function_source or debug.getinfo that the function comes from your file). "
         "Only Lua is overlaid (not materials or sounds). The result says base_lua_mounted: false if the game's own lua/ went missing (then remove the overlay). path: null removes the overlay "
         "(from the next map load). Without `path`: the current overlay and the engine's Lua search paths (lsv = "
         "server, lcl = client). A test client can also start with one: instances_launch {overlay}. empty_cache "
         "(default true) empties the engine's Lua file cache so an older copy with the same name and time is not "
         "served. Windows game clients only for now (unsupported_platform on Linux; not on a dedicated server).",
         json{{"properties",
               {{"path", {{"type", json::array({"string", "null"})},
                          {"description", "Overlay folder (absolute), or null / \"\" to remove it. Omit to read the current state."}}},
                {"empty_cache", {{"type", "boolean"}, {"description", "Empty the engine's Lua file cache after the change (default true)."}}}}}},
         json{{"destructiveHint", false}, {"idempotentHint", true}}, lua_overlay_tool});
}

}  // namespace tools
