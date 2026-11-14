// Tool: lua_mount (per-instance Lua mount: a folder searched before every addon, in this game process only).
#include "agent.h"
#include "jobs.h"
#include "mcp.h"
#include "lua_mount.h"
#include "tools.h"

namespace tools {

static json lua_mount_tool(const json &args) {
    bool set = args.contains("path");
    std::string root;
    // The path is checked first, on any platform, before touching the game's main thread.
    if (set && args["path"].is_string() && !args["path"].get<std::string>().empty()) {
        std::string e;
        if (!lua_mount::normalize_root(args["path"].get<std::string>(), root, e)) return agent::err("invalid_params", e);
        e = lua_mount::refuse_inside_game(root, agent::config().game_dir);
        if (!e.empty()) return agent::err("invalid_params", e);
    }
    std::string why;
    if (!lua_mount::supported(&why)) return agent::err("unsupported_platform", why);
    bool empty_cache = args.value("empty_cache", true);
    json r = jobs::run_on_main([=] { return set ? lua_mount::set(root, empty_cache) : lua_mount::info(); }, 10.0);
    if (jobs::is_timeout(r))
        return agent::err("main_thread_timeout", "the main thread did not respond in time (map loading, freeze or infinite loop?)");
    if (r.is_object() && r.contains("__error")) return agent::err("internal", r["__error"].get<std::string>());
    return r;
}

void register_lua_mount() {
    mcp::register_tool(
        {"lua_mount", "Load Lua from another folder first",
         "Mounted Lua folder: plugs a folder (e.g. the git worktree of an addon: the folder that contains lua/) into "
         "THIS game process only. Every Lua lookup of this game then looks there first: autorun, include(), "
         "AddCSLuaFile (the client gets the folder's copy), lua_openscript, file.Read/Find with \"LUA\", and gmod-mcp's "
         "own lua_exec_file / lua_job {path} / lua_check / lua_reload / lua_function_source. A file in the folder hides "
         "the installed addon's file of the same Lua path; files only in the game are still loaded. Other games are "
         "not affected. Example: path \"D:/dev/my-addon-wt\" (a worktree of the addon installed as addons/my_addon): "
         "D:/dev/my-addon-wt/lua/autorun/my_addon.lua runs as autorun/my_addon.lua; "
         "D:/dev/my-addon-wt/lua/my_addon/core.lua is what include(\"my_addon/core.lua\") loads, instead of "
         "addons/my_addon/lua/my_addon/core.lua; D:/dev/my-addon-wt/gamemodes/<gm>/... are gamemode files; "
         "materials/, sound/, models/ and everything outside lua/ and gamemodes/ are not mounted. "
         "IT TAKES EFFECT AT THE NEXT MAP LOAD: call map_load right after (code already running stays "
         "until loaded again). No autorefresh: after an edit, lua_reload {path} or map_load; check with "
         "lua_function_source. The engine names the folder's files from the folder's root (debug.getinfo source "
         "\"@lua/my_addon/x.lua\", not \"@addons/my_addon/lua/my_addon/x.lua\"). Only Lua (not materials, sounds, "
         "data/). `path`: the addon's root or its lua/ folder, absolute; null removes it (from the next map load); "
         "omitted: the current folder and the engine's Lua search paths (lsv = server, lcl = client). If the result "
         "says base_lua_mounted: false, remove the folder at once. A test client can start with one: instances_launch "
         "{lua_mount}. empty_cache (default true) empties the engine's Lua file cache. Windows game clients only "
         "(unsupported_platform on Linux; refused on a dedicated server). Full guide: resource mcpagent://guide, "
         "section \"Mounted Lua folder\".",
         json{{"properties",
               {{"path", {{"type", json::array({"string", "null"})},
                          {"description", "Absolute path of the folder to mount: the addon's root, i.e. the folder that contains lua/ "
                                         "(e.g. \"D:/dev/my-addon-wt\"), or that lua/ folder itself (same result). Must be "
                                         "outside the game folder. null or \"\" unmounts it (from the next map load). Omit "
                                         "it to read the current state."}}},
                {"empty_cache", {{"type", "boolean"}, {"description", "Empty the engine's Lua file cache after the change (default true)."}}}}}},
         json{{"destructiveHint", false}, {"idempotentHint", true}}, lua_mount_tool});
}

}  // namespace tools
