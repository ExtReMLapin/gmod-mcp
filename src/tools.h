#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lua_mount.h"

namespace tools {

using json = nlohmann::json;

void register_all();

// Per-domain registration.
void register_core();
void register_session();
void register_console();
void register_world();
void register_render();
void register_ui();
void register_docs();
void register_reports();
void register_lua_mount();
void register_lua_tools();
void register_jobs();
void register_instances();

// Shared helpers.
// Screenshot of a realm's frame (render hook + poll): {ok, result, image} like the screenshot tool, or an error.
json capture_screenshot(int realm, const json &req);
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
// use_mount false skips the mounted folder (the installed copy of a file).
bool resolve_lua_path(const std::string &path, bool allow_dir, std::string &abs, std::string &err, bool use_mount = true);
// Applies the `fields` argument (lua_exec, lua_exec_file, lua_job): keeps only these keys of each returned table.
json project_values(json result, const json &args);
// `fields` on an object result (status): keeps only these top-level keys.
json project_keys(json result, const json &args);
// lua_reload's lookup: every file matching an include()-style or game-relative path, in the engine's order.
std::vector<std::string> find_game_lua(const std::string &path, std::string &err);
// Mounted folder (Issue #9). Where a function's file came from: {from_mount, mount_has_file?, disk?, provenance, note?}.
json source_provenance(int realm, const std::string &source);
// status.lua_mount: {path, loaded_with_mount, realms_without_mount, stale_files:[{file, disk, reason, realms}], warning?}
// for the given realms; null when no folder is mounted.
json mount_state(const std::vector<int> &realms);
// {engine_serves: {realm: bool}, engine_serves_folder: true|false|null} (Issue #9): whether each realm's file system really finds
// the mounted folder's files (null: nobody could be asked, e.g. before a map is loaded).
json mount_engine_serves(const std::vector<int> &realms);
// One sentence for map_load / status when mounted files changed since they were loaded; "" when all is current.
std::string mount_warning(const json &state);
// {reload:[{file, disk, realms, reason}], skipped:[{file, why}]}: what lua_reload {mounted_changed} would run.
json plan_mount_reload(const json &state);
// What lua_reload {mounted_changed} answers once its files ran: `reloaded` is [{file, chunkname, reason, realms: {<realm>: result or
// {error}}}]. All ran: ok {ok:true, mode, path, reloaded, skipped, realms_without_mount, note?, warning?}. A file failed in a realm
// (`all_ok` false): the error reload_failed (isError at the MCP layer), the same object in its details.
json mounted_reload_result(const json &state, const json &plan, const json &reloaded, bool all_ok);
// Tests only: replaces what asks a realm's file system about the mounted folder's files (answers [{exists, time?}] one per
// probe, or null when it cannot be asked); an empty function restores the real one (the realm's mount_probe).
void set_mount_probe_for_tests(std::function<json(int, const std::vector<lua_mount::Probe> &)> fn);
// For a map_load that timed out: one sentence naming the mounted folder and where to look ("" when no folder is mounted).
std::string mount_timeout_hint();
// The realms where the mounted folder matters now (server, client) that are up with the runtime.
std::vector<int> mount_realms();
// Non-negotiable network rule: "" if the console line may run, else why it is refused (connect/redirect/retry to a
// server that is not this machine, in any of its statements).
std::string forbidden_network_cmd(const std::string &line);

// The `job` protocol of lua_job (tool description, also used by the write-lua-job prompt).
const char *job_protocol();
// lua_job ids: "<realm>-g<generation>-<n>" (e.g. "server-g3-1"); the old "<realm>-<n>" still parses (no generation).
struct JobId {
    int realm = -1;  // -1: not a job id
    bool has_generation = false;
    uint64_t generation = 0;
};
JobId parse_job_id(const std::string &id);
// "" if the job may still exist; otherwise why not: the id was issued by an earlier Lua state of its realm (map change).
std::string job_reset_message(const JobId &id, uint64_t current_generation);
// Wiki helpers shared with resources and completion.
json wiki_page_tool(const json &args);  // {name, format?, refresh?} -> tool result
std::set<std::string> pagelist_from_cache();  // never hits the network
// Realms the wiki GET may run in, in order: the host realm (menu in client mode, server on srcds), then the others that can
// carry HTTP. Issue #3: the menu realm of a game build can lack the HTTP function; wiki_page then tries the next one.
std::vector<int> wiki_http_realms(bool client_mode);

}  // namespace tools
