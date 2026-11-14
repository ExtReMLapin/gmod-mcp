#pragma once

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tools {

using json = nlohmann::json;

void register_all();

// Per-domain registration.
void register_core();
void register_session();
void register_console();
void register_world();
void register_render();
void register_docs();
void register_reports();
void register_lua_mount();
void register_lua_tools();
void register_jobs();
void register_instances();

// Shared helpers.
json ok(json result);
json realm_prop(const std::string &desc, bool with_menu = true);
int realm_of(const json &args, const char *key, int def);  // -1 if invalid
// Runtime call with realm resolution and an error message if invalid.
json rt_realm(const json &args, const char *key, int def, const std::string &tool, const json &payload, double timeout_s = 10.0);
// Path inside the Lua temporary directory (escape-proof validation). Returns "" if invalid.
std::string tmp_path(const std::string &name, std::string &err);
// Traces an event (start_tool -> trace_poll -> trace_stop); used by hook_trace and sound_trace.
json trace_hook(int realm, const std::string &start_tool, const json &start_args, double duration_s, int max_calls);
// Path resolution for lua_exec_file/tests_run (relative to tmp_dir or absolute) + chunk name.
bool resolve_lua_file(const std::string &path, std::string &abs, std::string &chunkname, std::string &err);
// Same lookup for a file, or with allow_dir a folder too (lua_check). err lists the places tried.
bool resolve_lua_path(const std::string &path, bool allow_dir, std::string &abs, std::string &err);
// Applies the `fields` argument (lua_exec, lua_exec_file, lua_job): keeps only these keys of each returned table.
json project_values(json result, const json &args);
// `fields` on an object result (status): keeps only these top-level keys.
json project_keys(json result, const json &args);
// lua_reload's lookup: every file matching an include()-style or game-relative path, in the engine's order.
std::vector<std::string> find_game_lua(const std::string &path, std::string &err);

// The `job` protocol of lua_job (tool description, also used by the write-lua-job prompt).
const char *job_protocol();
// Wiki helpers shared with resources and completion.
json wiki_page_tool(const json &args);  // {name, format?, refresh?} -> tool result
std::set<std::string> pagelist_from_cache();  // never hits the network

}  // namespace tools
