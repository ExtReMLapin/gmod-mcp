// Declared outputSchema of every tool: the shape of the structuredContent of a SUCCESS result. An isError result is
// {"error": {code, message, details}} and is NOT validated: the MCP SDK bundled in Claude Code 2.1.328 checks structuredContent
// against the schema only when !isError (and rejects a success result that has none): read in the binary, see
// docs/TESTS.md "Hint eval" notes.
//
// Rules, because a client that validates (the MCP SDK in Claude Code does) FAILS the whole call when a result does not
// match its schema:
//   - `required` only for fields that every success path of the tool sets (including a `fields` projection, a result
//     relayed from another instance, and the bridge's own answer while the game is closed: no `answered_by` there);
//   - numbers are `number`, never `integer` (a Lua or C++ double that is whole may be written 3.0), and EVERY number
//     accepts null (`N`): a non-finite value (NaN, infinity, a coordinate that overflows a double) is written as null;
//   - a value that can be NaN (written as null), nil-in-a-nullable-place, or that is a serialized Lua value (an empty
//     table is written `{}`) is `nullable` or untyped;
//   - every object accepts additional properties.
// tests/schema/results.json holds representative results of every tool; coretests validates each against its schema
// (test_output_schemas), so a field whose type drifts from the code fails CI.
#include "output_schemas.h"

#include <initializer_list>
#include <map>
#include <string>
#include <utility>

namespace mcp {

namespace {

using json = nlohmann::json;
using Props = std::initializer_list<std::pair<const char *, json>>;

json T(const char *t) { return json{{"type", t}}; }
json nullable(const char *t) { return json{{"type", json::array({t, "null"})}}; }
const json S = T("string"), B = T("boolean"), Obj = T("object");
// Every number accepts null: the game can hold a number JSON cannot carry (NaN, infinity, an overflowing coordinate or
// "1e999"), it is written as null, and a client that validates results fails the whole call on a null where the schema
// says `number`. There is no bare `number` anywhere (test_output_schemas_nonfinite walks every schema).
const json N = nullable("number");
const json ANY = json::object();  // any JSON value (a serialized Lua value)
const json NS = nullable("string"), NN = N;

// next_call (next_call.h): the call a hint suggests, as data.
const json NextCall = json{{"type", "object"},
                           {"properties", {{"tool", T("string")}, {"arguments", T("object")}, {"after_s", N}}},
                           {"required", json::array({"tool"})}};

json arr(json items) { return json{{"type", "array"}, {"items", std::move(items)}}; }
const json NUMS = arr(N), STRS = arr(S), ANYS = T("array");

// An object with these properties; `required` lists the ones every success path sets.
json obj(Props props, std::initializer_list<const char *> required = {}) {
    json p = json::object();
    for (const auto &kv : props) p[kv.first] = kv.second;
    // Hint fields any tool may add (see "Reading results" in the guide) and what the bridge adds.
    for (const char *h : {"note", "warning", "next", "truncated_note", "version_warning", "answered_by"})
        if (!p.contains(h)) p[h] = S;
    if (!p.contains("warnings")) p["warnings"] = ANYS;  // Issue #15: the list of warnings the game or the bridge adds
    if (!p.contains("next_call")) p["next_call"] = NextCall;
    json o{{"type", "object"}, {"properties", std::move(p)}, {"additionalProperties", true}};
    if (required.size()) {
        json r = json::array();
        for (const char *k : required) r.push_back(k);
        o["required"] = std::move(r);
    }
    return o;
}

// An entity summary (ent_summary in lua/60_world.lua).
json entity() {
    return obj({{"id", N}, {"class", S}, {"model", S}, {"name", S}, {"nick", S}, {"pos", NUMS}, {"ang", NUMS}, {"distance", N}});
}
// The fields of a trace surface (surface_of in lua/60_world.lua).
json surface() {
    return obj({{"mat_type", N}, {"mat_name", NS}, {"surfaceprop", S}, {"surfaceprop_id", N}, {"texture", S}, {"entity", ANY}});
}
// A line of the console buffer.
json console_line() { return obj({{"seq", N}, {"t", N}, {"level", S}, {"text", S}}); }
// What lua_exec and lua_job return for the values.
json values_props_note() { return S; }

// status.layout_check (src/layout.h): one verdict per interface the module calls into by fixed slot or offset.
json layout_check() {
    auto entry = [](Props extra) {
        json p = json::object();
        p["ok"] = B;
        p["state"] = S;  // verified | pending (checked before its first use) | refused | not_applicable
        p["reason"] = S;
        for (const auto &kv : extra) p[kv.first] = kv.second;
        return json{{"type", "object"}, {"properties", p}, {"additionalProperties", true}, {"required", json::array({"ok", "state"})}};
    };
    return obj({{"ifilesystem", entry({{"first_slot", N}, {"lua_shared_agrees", B}})},
                {"iluashared", entry({{"get_lua_interface", B}, {"mount_lua", B}, {"mount_lua_add", B}, {"unmount_lua", B}, {"empty_cache", B},
                                      {"file_system_mark_slot", N}, {"file_system_remove_slot", N}})},
                {"ivengineclient", entry({{"client_cmd", B}, {"get_net_channel_info", B}})},
                {"inetchannelinfo", entry({{"get_address", B}})},
                {"lua_offsets", entry({{"state_offset", N}, {"luabase_offset", N}, {"backref_ok", N}, {"backref_mismatch", N}, {"top_shape", nullable("boolean")}})}});
}

json build(const std::string &tool) {
    static const std::map<std::string, json> m = [] {
        std::map<std::string, json> s;
        // --- status
        s["status"] = obj({{"instance", S}, {"mode", S}, {"agent_version", S}, {"platform", S}, {"pid", N}, {"port", N},
                           {"coordinator", B}, {"uptime_s", N}, {"frames", N}, {"last_tick_age_s", N},
                           {"main_thread_stalled", B}, {"perf", Obj}, {"realms", Obj}, {"client_lock", Obj}, {"layout_check", layout_check()},
                           {"console_capture", S}, {"tmp_dir", S}, {"game_dir", S}, {"menu", Obj}, {"server", Obj}, {"client", Obj},
                           {"host_error", Obj}, {"map", NS}, {"gamemode", ANY}, {"maxplayers", ANY}, {"loading", B}, {"loading_stuck", B}, {"loading_stuck_note", S},
                           {"fps_max", ANY}, {"host_timescale", ANY}, {"projected", STRS},
                           {"lua_mount", Obj}});
        // --- Lua execution
        json exec = obj({{"values", ANYS}, {"n", N}, {"output", S}, {"duration_ms", N}, {"chunk_bytes", N}, {"chunkname", S},
                         {"budget_s", N}, {"generation", N}, {"truncated", N}, {"projected", STRS}, {"fields_note", S}, {"file", S},
                         {"blocked_console_commands", STRS}},
                        {"values", "n"});
        s["lua_exec"] = exec;
        s["lua_exec_file"] = exec;
        // Two shapes: {path} runs one file; {mounted_changed} (issue #9) runs the changed files of the mounted folder.
        s["lua_reload"] = obj({{"ok", B}, {"file", S}, {"chunkname", S}, {"realms", Obj}, {"other_matches", STRS},
                               {"mode", S}, {"path", S}, {"reloaded", ANYS}, {"skipped", ANYS}, {"realms_without_mount", ANYS}},
                              {"ok"});
        s["lua_check"] = obj({{"ok", B}, {"realm", S}, {"checked", N}, {"errors", ANYS}, {"unknown_globals", ANYS},
                              {"globals_written", ANYS}, {"truncated", B}, {"notes", STRS}, {"path", S}},
                             {"ok", "realm", "checked", "errors"});
        // Where a function's code came from (Issue #9): lua_inspect of a function and lua_function_source carry the same fields.
        const Props prov = {{"from_mount", B}, {"mount_has_file", B}, {"disk", S}, {"provenance", S}, {"provenance_note", S}, {"loaded_from", S}};
        auto with_prov = [&](Props base) {
            json o = obj(base, {});
            for (const auto &kv : prov) o["properties"][kv.first] = kv.second;
            return o;
        };
        s["lua_inspect"] = with_prov({{"type", S}, {"value", ANY}, {"count", N}, {"keys", ANYS}, {"truncated", B}, {"metatable", Obj},
                                      {"source", S}, {"linedefined", N}, {"lastlinedefined", N}});
        s["lua_inspect"]["required"] = json::array({"type"});
        s["lua_function_source"] = with_prov({{"source", S}, {"linedefined", N}, {"lastlinedefined", N}, {"file", S}, {"file_age_s", N},
                                              {"matches_disk", nullable("boolean")}, {"line_on_disk", N}});
        s["lua_function_source"]["required"] = json::array({"source", "linedefined", "lastlinedefined", "matches_disk"});
        s["lua_file_write"] = obj({{"path", S}, {"bytes", N}, {"exec_path", S}}, {"path", "bytes"});
        s["lua_file_read"] = obj({{"path", S}, {"content", S}}, {"path", "content"});
        s["lua_file_list"] = obj({{"dir", S}, {"files", STRS}}, {"dir", "files"});
        s["lua_file_delete"] = obj({{"deleted", S}}, {"deleted"});
        s["lua_mount"] = obj({{"lua_mount", NS}, {"base_lua_mounted", ANY}, {"mount_hook", B}, {"hook_calls", N}, {"search_paths", Obj},
                              {"previous", NS}, {"cache_emptied", B}, {"hook_path_ids", Obj}, {"removed", Obj}, {"fs_first_slot", N},
                              {"engine_serves_folder", nullable("boolean")}, {"engine_serves", Obj}, {"folder_missing", B}});
        // --- jobs
        s["lua_job"] = obj({{"job_id", S}, {"state", S}, {"realm", S}, {"generation", N}, {"concurrent_jobs", ANYS}, {"values", ANYS},
                            {"n", N}, {"reports", ANYS}, {"output", S}, {"frames", N}, {"resumes", N}, {"cpu_ms", N}, {"elapsed_s", N},
                            {"truncated", N}, {"reports_dropped", N}, {"reports_truncated", N}, {"projected", STRS}, {"fields_note", S}, {"blocked_console_commands", STRS}},
                           {"job_id"});
        s["lua_job_status"] = obj({{"job_id", S}, {"state", S}, {"frames", N}, {"elapsed_s", N}, {"jobs", ANYS}, {"generation", N},
                                   {"values", ANYS}, {"n", N}, {"reports", ANYS}, {"output", S}, {"resumes", N}, {"cpu_ms", N},
                                   {"truncated", N}, {"reports_count", N}, {"progress", ANY}, {"projected", STRS}, {"fields_note", S}, {"blocked_console_commands", STRS}});
        s["lua_job_cancel"] = obj({{"job_id", S}, {"cancelled", B}}, {"job_id", "cancelled"});
        // --- session
        s["map_list"] = obj({{"maps", STRS}, {"count", N}, {"truncated", B}, {"filter", S}}, {"maps", "count", "truncated"});
        s["map_load"] = obj({{"map", S}, {"gamemode", ANY}, {"maxplayers", ANY}, {"requested_map", S}, {"duration_s", N},
                             {"server_generation", N}, {"client_generation", N}, {"lua_mount", Obj}, {"lua_cache_emptied", B}},
                            {"requested_map", "duration_s"});
        s["disconnect"] = obj({{"disconnected", B}, {"was_in_game", B}, {"duration_s", N}, {"realms", Obj}},
                              {"disconnected", "was_in_game", "duration_s", "realms"});
        s["game_quit"] = obj({{"quitting", B}}, {"quitting"});
        s["game_terminate"] = obj({{"terminating", B}, {"pid", N}, {"main_thread_stalled", B}, {"last_tick_age_s", N}}, {"terminating"});
        s["wait_for"] = obj({{"satisfied", B}, {"elapsed_s", N}, {"polls", N}, {"value", ANY}}, {"satisfied", "elapsed_s", "polls"});
        s["wait_frames"] = obj({{"frames", N}, {"elapsed_s", N}}, {"frames", "elapsed_s"});
        // --- console
        s["console_exec"] = obj({{"cmd", S}, {"realm", S}, {"via", S}, {"statements", ANYS}, {"blocked_in_lua", B}, {"left_game_first", B}, {"connect_result", Obj},
                                 {"output", arr(console_line())}, {"next_cursor", N}, {"output_note", S}, {"convars", Obj}},
                                {"cmd", "realm", "via", "output", "next_cursor"});
        s["console_wait"] = obj({{"matched", N}, {"lines", arr(console_line())}, {"next_cursor", N}, {"waited_s", N}, {"lost", N}},
                                {"matched", "lines", "next_cursor", "waited_s"});
        s["console_read"] = obj({{"lines", arr(console_line())}, {"next_cursor", N}, {"lost", N}, {"oldest", N}, {"capture", S}},
                                {"lines", "next_cursor"});
        s["console_buffer"] = obj({{"capacity", N}, {"cursor", N}}, {"capacity", "cursor"});
        // `errors` in the default output, `groups` + `groups_total` with group_by (issue #11): neither is required.
        s["errors_since"] = obj({{"errors", ANYS}, {"groups", ANYS}, {"groups_total", N}, {"next_cursor", N}, {"lost", N}, {"map_load", N}, {"map", ANY}},
                                {"next_cursor"});
        s["errors_clear"] = obj({{"cleared", B}, {"next_cursor", N}}, {"cleared", "next_cursor"});
        json convar = obj({{"name", S}, {"flags", N}, {"flag_names", STRS}, {"help", S}, {"default", S}, {"value", NS}, {"number", N},
                           {"protected", B}, {"min", N}, {"max", N}, {"previous", NS}, {"via", S}},
                          {"name", "flags", "flag_names"});
        s["convar_get"] = convar;
        s["convar_set"] = convar;
        s["concommand_list"] = obj({{"lua_commands", STRS}, {"realm", S}, {"engine_commands", STRS}, {"engine_convars", STRS}},
                                   {"lua_commands", "realm"});
        // --- world
        s["ents_find"] = obj({{"entities", arr(entity())}, {"count", N}, {"truncated", B}}, {"entities", "count", "truncated"});
        s["ent_info"] = obj({{"id", N}, {"class", S}, {"model", S}, {"name", S}, {"nick", S}, {"pos", NUMS}, {"ang", NUMS}, {"valid", B},
                             {"velocity", NUMS}, {"health", N}, {"max_health", N}, {"solid", N}, {"movetype", N}, {"collision_group", N},
                             {"obb", Obj}, {"parent", NN}, {"owner", NN}, {"nw_vars", ANY}, {"table", ANY}, {"keyvalues", ANY}, {"door", Obj}},
                            {"id", "class", "valid"});
        s["ent_spawn"] = entity();
        s["ent_remove"] = obj({{"removed", N}}, {"removed"});
        s["ent_fire"] = obj({{"fired", S}, {"id", N}, {"param", S}, {"delay", N}}, {"fired", "id"});
        json player = obj({{"id", N}, {"class", S}, {"model", S}, {"nick", S}, {"pos", NUMS}, {"ang", NUMS}, {"userid", N}, {"steamid", S},
                           {"bot", B}, {"alive", B}, {"health", N}, {"eye_pos", NUMS}, {"eye_ang", NUMS}, {"noclip", B}, {"god", B}});
        s["players_list"] = obj({{"players", arr(player)}, {"count", N}, {"maxplayers", N}}, {"players", "count"});
        s["player_set"] = player;
        s["bots_add"] = obj({{"added", arr(entity())}, {"count", N}}, {"added", "count"});
        json tr = surface();
        tr["properties"]["hit"] = B;
        tr["properties"]["hit_world"] = B;
        tr["properties"]["hit_sky"] = B;
        tr["properties"]["hit_nodraw"] = B;
        tr["properties"]["pos"] = NUMS;
        tr["properties"]["normal"] = NUMS;
        tr["properties"]["fraction"] = N;
        tr["properties"]["distance"] = N;
        tr["properties"]["start_solid"] = B;
        tr["properties"]["all_solid"] = B;
        tr["properties"]["fraction_left_solid"] = N;
        tr["properties"]["hitgroup"] = N;
        tr["properties"]["physics_bone"] = N;
        for (const char *h : {"note", "warning", "version_warning", "answered_by"}) tr["properties"][h] = S;
        tr["properties"]["warnings"] = ANYS;
        tr["properties"]["next_call"] = NextCall;
        tr["required"] = json::array({"hit", "pos", "fraction", "distance"});
        s["trace"] = tr;
        s["trace_through"] = obj({{"surfaces", ANYS}, {"count", N}, {"total_thickness", N}, {"distance", N}, {"truncated", B}},
                                 {"surfaces", "count", "total_thickness", "distance", "truncated"});
        json shot = obj({{"format", S}, {"w", N}, {"h", N}, {"screen_w", N}, {"screen_h", N}, {"bytes", N}, {"method", S}, {"camera_restored", B},
                         {"source", S}, {"capture_ms", N}, {"file", S}, {"file_deleted", B}, {"ignored", STRS}, {"note", S}, {"warning", S}},
                        {"bytes"});
        s["screenshot"] = shot;
        s["screenshot_from"] = shot;
        // --- UI input (ui_click, ui_key): what was posted, where, and the window facts the point was converted with
        s["ui_click"] = obj({{"sent", STRS}, {"at", obj({{"x", N}, {"y", N}}, {"x", "y"})}, {"coords", S}, {"button", S}, {"wheel", N},
                             {"scale", Obj}, {"frames_waited", N}, {"window", Obj}, {"screen", Obj}, {"cursor_via", S}, {"screenshot", Obj},
                             {"screenshot_note", S}, {"screenshot_error", ANY}},
                            {"sent", "at", "frames_waited"});
        s["ui_key"] = obj({{"sent", STRS}, {"key", S}, {"text_units", N}, {"lossy_characters", STRS}, {"ansi_code_page", N}, {"frames_waited", N}, {"window", Obj}, {"screen", Obj},
                           {"cursor_via", S}, {"screenshot", Obj}, {"screenshot_note", S}, {"screenshot_error", ANY}},
                          {"sent", "frames_waited"});
        s["debug_draw"] = obj({{"added", N}, {"active", N}, {"expire_in_s", N}}, {"added", "active"});
        s["debug_clear"] = obj({{"cleared", N}}, {"cleared"});
        s["sound_emit"] = obj({{"emitted", S}, {"pos", NUMS}, {"ent", N}, {"level", N}, {"pitch", N}, {"volume", N}, {"dsp", N}}, {"emitted"});
        json trace = obj({{"event", S}, {"calls", ANYS}, {"recorded", N}, {"total_calls", N}, {"capped", B}, {"duration_s", N},
                          {"tracer_removed", B}},
                         {"event", "calls", "recorded", "total_calls"});
        s["sound_trace"] = trace;
        s["hook_trace"] = trace;
        s["perf_snapshot"] = obj({{"frame", obj({{"fps", NN}, {"frametime_avg_ms", NN}, {"frametime_max_ms", NN}, {"samples", N}})},
                                  {"last_tick_age_s", N}, {"realms", Obj}},
                                 {"frame", "last_tick_age_s", "realms"});
        // --- documentation
        s["api_search"] = obj({{"pattern", S}, {"results", ANYS}, {"count", N}, {"realms_searched", Obj}, {"wiki_index", S}},
                              {"results", "count"});
        s["wiki_search"] = obj({{"query", S}, {"results", ANYS}, {"total_matches", N}, {"cache", S}}, {"query", "results", "total_matches"});
        s["wiki_page"] = obj({{"name", S}, {"format", S}, {"url", S}, {"cache", S}, {"content", S}, {"page", ANY}}, {"name"});
        // --- events, reports, hooks, performance, tests
        s["events_wait"] = obj({{"events", ANYS}, {"next_cursor", N}, {"lost", N}, {"timed_out", B}}, {"events", "next_cursor"});
        s["reports_read"] = obj({{"channel", S}, {"items", ANYS}, {"next_cursor", N}, {"lost", N}, {"timed_out", B}}, {"items", "next_cursor"});
        s["reports_channels"] = obj({{"channels", ANY}}, {"channels"});
        s["reports_clear"] = obj({{"cleared", ANY}}, {"cleared"});
        s["hook_list"] = obj({{"hooks", Obj}, {"count", N}, {"event", S}, {"known", B}, {"gamemode_function", ANY}}, {"hooks", "count"});
        s["perf_hook_cost"] = obj({{"event", S}, {"duration_s", N}, {"functions", ANYS}, {"restored", ANY}}, {"event", "functions"});
        s["tests_run"] = obj({{"files", ANYS}, {"totals", Obj}, {"ok", B}}, {"files", "totals", "ok"});
        // --- instances
        // silent_s: seconds since the instance's last heartbeat, on every entry (0 for this game), as in the instance_hung error.
        s["instances_list"] = obj({{"instances", arr(obj({{"state", S}, {"heartbeat_age_s", N}, {"silent_s", N}, {"stalled_s", N}, {"fps_low_s", N}, {"loading", B}}))},
                                   {"coordinator", B}},
                                  {"instances"});
        return s;
    }();
    auto it = m.find(tool);
    return it == m.end() ? json() : it->second;
}

}  // namespace

json output_schema_for(const std::string &tool) { return build(tool); }

}  // namespace mcp
