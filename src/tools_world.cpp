// Tools: world (entities, players, bots, traces), rendering (screenshots, debug draw), sound, performance.
#include <chrono>

#include "agent.h"
#include "jobs.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "tasks.h"
#include "tools.h"

namespace tools {

using Clock = std::chrono::steady_clock;

static json vec_schema(const std::string &desc) {
    return json{{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", desc}};
}
// Shared wording: realms of the world tools. A map must be loaded (the server and client realms exist only then).
static const char *VEC = "[x, y, z] in Hammer units";
static const char *kNeedsMap = " Needs a loaded map (map_load): the server and client realms do not exist at the main menu.";

static json pass(const json &a, std::initializer_list<const char *> keys) {
    json p = json::object();
    for (const char *k : keys)
        if (a.contains(k)) p[k] = a[k];
    return p;
}

// Screenshot: request in a render hook, then poll until the frame has been captured.
static json capture(int realm, const json &req) {
    json r = agent::rt(realm, "capture_request", req, 5.0);
    if (!r.value("ok", false)) return r;
    auto t0 = Clock::now();
    for (;;) {
        plat::sleep_ms(15);
        json p = agent::rt(realm, "capture_poll", json::object(), 5.0);
        if (!p.value("ok", false)) return p;
        if (p["result"].value("done", false)) {
            json res = p["result"];
            std::string fmt = res.value("format", "jpeg");
            json out = json{{"ok", true}, {"result", res}};
            out["image"] = json{{"data", p.value("blob_b64", "")}, {"mimeType", fmt == "png" ? "image/png" : "image/jpeg"}};
            out["result"].erase("done");
            return out;
        }
        if (std::chrono::duration<double>(Clock::now() - t0).count() > 10.0)
            return agent::err("timeout", "the frame was not rendered within 10 s (game minimized or frozen?)");
    }
}

static json screenshot_tool(const json &a) {
    if (agent::config().mode != agent::Mode::Client) return agent::err("unsupported", "no renderer in srcds mode");
    // The menu state has no render.Capture: at the main menu, screenshots are not available (defined behavior).
    if (!agent::realm_present(realms::CLIENT))
        return agent::err("realm_unavailable", "screenshots need the client realm: load a map first (the main menu cannot be captured)");
    return capture(realms::CLIENT, pass(a, {"w", "h", "format", "quality"}));
}

static json screenshot_from_tool(const json &a) {
    if (!agent::realm_present(realms::CLIENT)) return agent::err("realm_unavailable", "screenshot_from needs a loaded map (client realm)");
    bool custom_size = a.contains("w") || a.contains("h");
    if (custom_size) {
        // Custom size: render the requested view into a render target (no CalcView needed).
        json req = pass(a, {"w", "h", "format", "quality", "fov"});
        req["origin"] = a["pos"];
        req["angles"] = a["ang"];
        return capture(realms::CLIENT, req);
    }
    // Screen size: temporary CalcView hook, wait a few frames, capture, then always remove the hook.
    json s = agent::rt(realms::CLIENT, "view_set", pass(a, {"pos", "ang", "fov"}));
    if (!s.value("ok", false)) return s;
    json req = pass(a, {"format", "quality"});
    req["wait_frames"] = 3;
    json r = capture(realms::CLIENT, req);
    json c = agent::rt(realms::CLIENT, "view_clear", json::object());
    if (r.value("ok", false)) {
        r["result"]["method"] = "calcview";
        r["result"]["camera_restored"] = c.value("ok", false) && c["result"].value("removed", false);
    }
    return r;
}

static json sound_trace_tool(const json &a) {
    int r = realm_of(a, "realm", realms::SERVER);
    if (r < 0) return agent::err("invalid_params", "unknown realm");
    int max = a.value("max_calls", 100);
    return trace_hook(r, "sound_trace_start", json{{"max_calls", max}}, a.value("duration_s", 5.0), max);
}

static json perf_snapshot_tool(const json &) {
    json out{{"frame", jobs::frame_stats()}, {"last_tick_age_s", jobs::seconds_since_last_frame()}};
    json realms_j = json::object();
    for (int r = 0; r < realms::COUNT; ++r) {
        if (!agent::realm_present(r)) continue;
        if (r == realms::CLIENT && !agent::lock_status().value("client_allowed", false)) continue;
        json p = agent::rt(r, "perf", json::object(), 3.0);
        if (p.value("ok", false)) realms_j[realms::name(r)] = p["result"];
    }
    out["realms"] = realms_j;
    return ok(out);
}

void register_world() {
    json RO = {{"readOnlyHint", true}};
    json realm_sv = realm_prop("Realm (default server).", false);

    mcp::register_tool({"ents_find", "Find entities",
                        std::string("Lists entities, filtered by class, model and/or distance (all filters combine; none = every "
                        "entity). class and model are case-insensitive globs where * matches anything (\"prop_*\", "
                        "\"*door*\", \"models/props_c17/*\"). near + radius keep the entities whose position is within radius "
                        "of near (ents.FindInSphere), sorted nearest first, each with `distance`. Result: {entities: [{id, class, "
                        "model, name (targetname, if any), nick (players), pos [x,y,z], ang [p,y,r], distance?}], count (all "
                        "matches), truncated (count > limit)}. id is the entity index: pass it to ent_info, ent_fire, ent_remove. "
                        "Server realm by default; the client realm only sees networked entities (and no targetnames). Example: "
                        "{\"class\": \"func_door*\", \"near\": [0, 0, 64], \"radius\": 1024, \"limit\": 10}.") + kNeedsMap,
                        json{{"properties", {{"class", {{"type", "string"}, {"description", "Class glob, case-insensitive, e.g. prop_physics or prop_*."}}},
                                             {"model", {{"type", "string"}, {"description", "Model path glob, case-insensitive, e.g. *chair*."}}},
                                             {"near", vec_schema("Center of the search sphere, " + std::string(VEC) + ".")},
                                             {"radius", {{"type", "number"}, {"minimum", 0}, {"description", "Sphere radius in Hammer units, used with near (default 512)."}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Max entities returned (default 100); count still counts all."}}},
                                             {"realm", realm_sv}}}},
                        RO, [](const json &a) {
                            return rt_realm(a, "realm", realms::SERVER, "ents_find", pass(a, {"class", "model", "near", "radius", "limit"}));
                        }});
    mcp::register_tool({"ent_info", "Entity details",
                        std::string("Details of one entity by index (id 0 = the world). Result: id, class, model, name, nick, pos, "
                        "ang, valid, velocity, health, max_health, solid, movetype, collision_group (raw engine enums), obb "
                        "{mins, maxs} (local bounds), parent and owner (entity ids or null), nw_vars (SetNW* values), table (the "
                        "entity's Lua table, cut at depth/limit), and in the server realm keyvalues (map/engine keyvalues) and, "
                        "for classes containing \"door\", door {m_eDoorState, m_toggle_state, state: closed|opening|open|"
                        "closing|ajar}. Error not_found for an invalid id. Example: {\"id\": 57}.") + kNeedsMap,
                        json{{"properties", {{"id", {{"type", "integer"}, {"minimum", 0}, {"description", "Entity index (from ents_find / players_list)."}}},
                                             {"realm", realm_sv},
                                             {"depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4}, {"description", "Depth of the Lua `table` field (default 1)."}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}, {"description", "Max keys per table in `table` (default 100)."}}}}},
                             {"required", {"id"}}},
                        RO, [](const json &a) { return rt_realm(a, "realm", realms::SERVER, "ent_info", pass(a, {"id", "depth", "limit"})); }});
    mcp::register_tool({"ent_spawn", "Spawn an entity",
                        std::string("Creates an entity in the server realm, in this order: ents.Create(class), SetModel, SetPos, "
                        "SetAngles, SetKeyValue for each keyvalue (values sent as strings), Spawn, Activate, then freeze. It stays "
                        "until removed or the map changes. Result: the entity summary {id, class, model, name, pos, ang}. Error "
                        "invalid_class if the class does not exist. Example: {\"class\": \"prop_physics\", \"model\": "
                        "\"models/props_c17/oildrum001.mdl\", \"pos\": [0, 0, 100], \"freeze\": true}.") + kNeedsMap,
                        json{{"properties", {{"class", {{"type", "string"}, {"minLength", 1}, {"description", "Entity class, e.g. prop_physics, npc_citizen."}}},
                                             {"pos", vec_schema("Position, " + std::string(VEC) + " (default: the origin).")},
                                             {"ang", vec_schema("Angles [pitch, yaw, roll] in degrees.")},
                                             {"model", {{"type", "string"}, {"description", "Model path, e.g. models/props_c17/oildrum001.mdl."}}},
                                             {"keyvalues", {{"type", "object"}, {"description", "Map keyvalues set before Spawn, e.g. {\"targetname\": \"test\"}."}}},
                                             {"freeze", {{"type", "boolean"}, {"description", "Disable physics motion after spawning (default false)."}}}}},
                             {"required", {"class"}}},
                        json{{"destructiveHint", false}},
                        [](const json &a) { return agent::rt(realms::SERVER, "ent_spawn", pass(a, {"class", "pos", "ang", "model", "keyvalues", "freeze"})); }});
    mcp::register_tool({"ent_remove", "Remove an entity",
                        std::string("Removes an entity by index in the server realm (Entity:Remove, effective at once). Players and the "
                        "world are refused (forbidden); an invalid id gives not_found. Result: {removed: id}.") + kNeedsMap,
                        json{{"properties", {{"id", {{"type", "integer"}, {"minimum", 1}, {"description", "Entity index."}}}}}, {"required", {"id"}}},
                        json{{"destructiveHint", true}}, [](const json &a) { return agent::rt(realms::SERVER, "ent_remove", pass(a, {"id"})); }});
    mcp::register_tool({"ent_fire", "Fire an entity input",
                        std::string("Sends a map I/O input to an entity in the server realm (Entity:Fire(input, param, delay)), e.g. "
                        "Open, Close, Toggle, Lock on a door, or Kill. Returns at once ({fired, id, param, delay}): the effect "
                        "happens on a later frame (after `delay` seconds), so check it with ent_info or wait_for. An input the "
                        "entity does not know raises no error here. Example: {\"id\": 57, \"input\": \"Open\"}.") + kNeedsMap,
                        json{{"properties", {{"id", {{"type", "integer"}, {"minimum", 0}, {"description", "Entity index."}}},
                                             {"input", {{"type", "string"}, {"minLength", 1}, {"description", "Input name, e.g. Open."}}},
                                             {"param", {{"type", "string"}, {"description", "Input parameter (default empty)."}}},
                                             {"delay", {{"type", "number"}, {"minimum", 0}, {"description", "Delay in seconds (default 0)."}}}}},
                             {"required", {"id", "input"}}},
                        json{{"destructiveHint", true}}, [](const json &a) { return agent::rt(realms::SERVER, "ent_fire", pass(a, {"id", "input", "param", "delay"})); }});
    mcp::register_tool({"players_list", "List players",
                        std::string("Every player (bots included). Result: {players: [{id (entity index, for player_set), class, "
                        "model, nick, pos, ang, userid, steamid, bot, alive, health, eye_pos, eye_ang, noclip, god (server realm "
                        "only)}], count, maxplayers}.") + kNeedsMap,
                        json{{"properties", {{"realm", realm_sv}}}}, RO,
                        [](const json &a) { return rt_realm(a, "realm", realms::SERVER, "players_list", json::object()); }});
    mcp::register_tool({"player_set", "Move/configure a player",
                        std::string("Changes a player in the server realm: pos teleports it (velocity reset to zero), ang sets its eye "
                        "angles, noclip switches MOVETYPE_NOCLIP/WALK, god toggles god mode. Only the given fields change. Default "
                        "player: the first one (the local player in singleplayer). Result: {id, class, model, nick, pos, ang, "
                        "eye_ang, noclip, god} read right after. The client sees it on a later frame: check with "
                        "lua_exec {realm: \"client\", code: \"return LocalPlayer():GetPos()\"}. Example: {\"pos\": [0, 0, 64], "
                        "\"ang\": [0, 90, 0], \"noclip\": true}.") + kNeedsMap,
                        json{{"properties", {{"id", {{"type", "integer"}, {"minimum", 1}, {"description", "Player entity index (players_list `id`, not userid). Default: the first player."}}},
                                             {"pos", vec_schema("New position, " + std::string(VEC) + ".")},
                                             {"ang", vec_schema("Eye angles [pitch, yaw, roll] in degrees.")},
                                             {"noclip", {{"type", "boolean"}, {"description", "true = noclip, false = walk."}}},
                                             {"god", {{"type", "boolean"}, {"description", "true = god mode on."}}}}}},
                        json{{"destructiveHint", false}, {"idempotentHint", true}},
                        [](const json &a) { return agent::rt(realms::SERVER, "player_set", pass(a, {"id", "pos", "ang", "noclip", "god"})); }});
    mcp::register_tool({"bots_add", "Add bots",
                        std::string("Adds n bots in the server realm (player.CreateNextBot), named <name>_<k>. Bots need free player "
                        "slots: in singleplayer (map_load maxplayers 1) there are none, so load the map with e.g. maxplayers: 4 first; "
                        "otherwise error no_slots. Result: {added: [entity summaries], count}.") + kNeedsMap,
                        json{{"properties", {{"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 127}, {"description", "Number of bots."}}},
                                             {"name", {{"type", "string"}, {"description", "Name prefix (default mcpbot)."}}}}},
                             {"required", {"n"}}},
                        json{{"destructiveHint", false}}, [](const json &a) { return agent::rt(realms::SERVER, "bots_add", pass(a, {"n", "name"})); }});
    json trace_props = {{"start", vec_schema("Start, " + std::string(VEC) + ".")},
                        {"end", vec_schema("End, " + std::string(VEC) + ".")},
                        {"mask", {{"type", {"string", "integer"}},
                                  {"description", "Content mask: MASK_SOLID (default), MASK_SHOT, MASK_SHOT_HULL, MASK_SOLID_BRUSHONLY, MASK_PLAYERSOLID, "
                                                  "MASK_PLAYERSOLID_BRUSHONLY, MASK_NPCSOLID, MASK_WATER, MASK_VISIBLE, MASK_OPAQUE, MASK_ALL, or the number."}}},
                        {"filter", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Entity indexes the trace ignores (e.g. the player)."}}},
                        {"realm", realm_sv}};
    mcp::register_tool({"trace", "Trace a line",
                        std::string("One util.TraceLine from start to end. Result: hit, hit_world, hit_sky, hit_nodraw, pos (hit position, "
                        "or end if nothing was hit), normal, fraction (0..1 along the segment), distance (to the hit, or the whole "
                        "length), mat_type + mat_name (MAT_* constant), surfaceprop (name) + surfaceprop_id, texture, entity "
                        "(summary, {id: 0, class: worldspawn} for the world, or null), start_solid, all_solid, fraction_left_solid, "
                        "hitgroup, physics_bone. Example: {\"start\": [0, 0, 64], \"end\": [0, 0, -1000]}. For everything a "
                        "segment crosses (walls, doors, thickness), use trace_through.") + kNeedsMap,
                        json{{"properties", trace_props}, {"required", {"start", "end"}}}, RO,
                        [](const json &a) { return rt_realm(a, "realm", realms::SERVER, "trace", pass(a, {"start", "end", "mask", "filter"})); }});
    json tt_props = trace_props;
    tt_props["max_surfaces"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"description", "Max solids listed (default 32); truncated says if more remained."}};
    mcp::register_tool({"trace_through", "Surfaces crossed by a segment",
                        std::string("Lists every solid the segment start->end goes through, in order, by repeated traces (FractionLeftSolid "
                        "gives where each solid is left). Result: {surfaces: [{entry, entry_normal, entry_surface (same surface "
                        "fields as trace: mat_name, surfaceprop, texture, entity), distance_from_start, exit, exit_normal, "
                        "exit_surface, thickness (units), reaches_end_inside?}], count, total_thickness, distance (segment "
                        "length), truncated}. A start point inside a solid gives a first entry with entry_surface null. Made for "
                        "sound propagation through walls and doors. Example: {\"start\": [0, 0, 64], \"end\": [800, 0, 64]}.") + kNeedsMap,
                        json{{"properties", tt_props}, {"required", {"start", "end"}}}, RO,
                        [](const json &a) {
                            return rt_realm(a, "realm", realms::SERVER, "trace_through", pass(a, {"start", "end", "mask", "filter", "max_surfaces"}));
                        }});
}

void register_render() {
    json realm_sv = realm_prop("Realm (default server).", false);
    json img ={{"format", {{"type", "string"}, {"enum", {"jpeg", "png"}}, {"description", "Default jpeg."}}},
                {"quality", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}, {"description", "JPEG quality (default 90)."}}},
                {"w", {{"type", "integer"}, {"minimum", 16}, {"maximum", 3840}, {"description", "Width in pixels (default: the game window's)."}}},
                {"h", {{"type", "integer"}, {"minimum", 16}, {"maximum", 2160}, {"description", "Height in pixels (default: the game window's)."}}}};
    mcp::register_tool({"screenshot", "Take a screenshot",
                        "Captures what the local player sees and returns it as an MCP image (plus {format, w, h, screen_w, screen_h, "
                        "method, bytes}). Game client with a loaded map only: error realm_unavailable at the main menu (the menu "
                        "cannot be captured) and unsupported on srcds. Without w/h: the game window as it is, HUD included "
                        "(method backbuffer). With w/h: the player's view rendered again at that size, without HUD or view model "
                        "(method render_target). A minimized or frozen game gives timeout after 10 s. debug_draw primitives are "
                        "visible. Example: {\"w\": 1280, \"h\": 720, \"format\": \"png\"}.",
                        json{{"properties", img}}, json{{"readOnlyHint", true}}, screenshot_tool});
    json from = img;
    from["pos"] = vec_schema("Camera position, " + std::string(VEC) + ".");
    from["ang"] = vec_schema("Camera angles [pitch, yaw, roll] in degrees.");
    from["fov"] = {{"type", "number"}, {"minimum", 1}, {"maximum", 179}, {"description", "Field of view in degrees (default 90)."}};
    mcp::register_tool({"screenshot_from", "Screenshot from a viewpoint",
                        "Captures the scene from an arbitrary camera (pos, ang, fov) and returns it as an MCP image; the player does "
                        "not move. Game client with a loaded map only. Without w/h: a temporary CalcView hook moves the view for 3 "
                        "frames, the window is captured, then the hook is removed (result method calcview, camera_restored: true "
                        "when the player's view is back). With w/h: rendered into an off-screen target of that size (method "
                        "render_target), the view is not touched. The local player's body is drawn (it is not the camera). "
                        "debug_draw primitives are visible. Example: {\"pos\": [0, -300, 200], \"ang\": [30, 90, 0], "
                        "\"w\": 800, \"h\": 600}.",
                        json{{"properties", from}, {"required", {"pos", "ang"}}}, json{{"readOnlyHint", true}}, screenshot_from_tool});
    json prim = {{"type", "object"},
                 {"properties",
                  {{"type", {{"type", "string"}, {"enum", {"line", "box", "sphere", "text"}}}},
                   {"from", vec_schema("line: start point.")},
                   {"to", vec_schema("line: end point.")},
                   {"pos", vec_schema("box: origin of mins/maxs; sphere: center; text: position.")},
                   {"ang", vec_schema("box: rotation [p,y,r] (default 0,0,0).")},
                   {"mins", vec_schema("box: corner relative to pos (default [-8,-8,-8]).")},
                   {"maxs", vec_schema("box: corner relative to pos (default [8,8,8]).")},
                   {"min", vec_schema("box given as a world-space AABB instead of pos/mins/maxs: lower corner.")},
                   {"max", vec_schema("box as a world-space AABB: upper corner.")},
                   {"radius", {{"type", "number"}, {"description", "sphere radius in units (default 8)."}}},
                   {"text", {{"type", "string"}, {"description", "text to draw, facing the camera."}}},
                   {"scale", {{"type", "number"}, {"description", "text scale (default 0.25)."}}},
                   {"color", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 4}, {"description", "[r, g, b] or [r, g, b, a], 0-255 (default red)."}}},
                   {"ignore_z", {{"type", "boolean"}, {"description", "Draw through walls (default true)."}}}}},
                 {"required", {"type"}},
                 {"additionalProperties", false}};
    mcp::register_tool({"debug_draw", "Draw debug primitives",
                        "Draws 3D lines, wireframe boxes, wireframe spheres and text in the game world, in the client realm (a map "
                        "must be loaded), visible on screen and in screenshot/screenshot_from. They add up across calls and expire "
                        "after `duration` seconds of game time (0 = until debug_clear or the next map). Nothing changes in the "
                        "game itself. Result: {added, active, expire_in_s}. Example: {\"primitives\": [{\"type\": \"line\", "
                        "\"from\": [0, 0, 64], \"to\": [500, 0, 64], \"color\": [0, 255, 0]}, {\"type\": \"sphere\", "
                        "\"pos\": [500, 0, 64], \"radius\": 16}, {\"type\": \"box\", \"min\": [-32, -32, 0], \"max\": [32, 32, 72]}, "
                        "{\"type\": \"text\", \"pos\": [0, 0, 100], \"text\": \"spawn\"}], \"duration\": 30}.",
                        json{{"properties", {{"primitives", {{"type", "array"}, {"items", prim}, {"minItems", 1}, {"maxItems", 5000}, {"description", "Shapes to draw (see the example)."}}},
                                             {"duration", {{"type", "number"}, {"minimum", 0}, {"description", "Lifetime in seconds of game time (default 10; 0 = until debug_clear)."}}}}},
                             {"required", {"primitives"}}},
                        json{{"destructiveHint", false}},
                        [](const json &a) { return agent::rt(realms::CLIENT, "debug_draw", pass(a, {"primitives", "duration"})); }});
    mcp::register_tool({"debug_clear", "Clear debug primitives", "Removes every primitive drawn by debug_draw (client realm). Result: {cleared: count}.", json::object(),
                        json{{"destructiveHint", false}, {"idempotentHint", true}},
                        [](const json &) { return agent::rt(realms::CLIENT, "debug_clear", json::object()); }});

    // Sound.
    mcp::register_tool({"sound_emit", "Emit a sound",
                        std::string("Plays a sound once, from a position (global EmitSound) or from an entity (Entity:EmitSound, it "
                        "follows the entity), on CHAN_AUTO. Pass pos or ent. `name` is a soundscript name or a path under sound/ "
                        "(e.g. \"ambient/alarms/klaxon1.wav\"); error not_found if neither exists. Server realm by default "
                        "(networked to the client); realm client plays it only locally. Returns at once: {emitted, pos or ent, "
                        "level, pitch, volume, dsp}. To see what the engine and the hooks received, run sound_trace meanwhile. "
                        "Example: {\"name\": \"ambient/alarms/klaxon1.wav\", \"pos\": [0, 0, 64], \"level\": 90}.") + kNeedsMap,
                        json{{"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"description", "Soundscript name or path relative to sound/."}}},
                                             {"pos", vec_schema("Where to play it, " + std::string(VEC) + " (or ent).")},
                                             {"ent", {{"type", "integer"}, {"minimum", 0}, {"description", "Entity index to play it from (or pos)."}}},
                                             {"level", {{"type", "integer"}, {"minimum", 0}, {"maximum", 511}, {"description", "Sound level in dB, default 75."}}},
                                             {"pitch", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"description", "Default 100."}}},
                                             {"volume", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"description", "Default 1."}}},
                                             {"dsp", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"description", "DSP preset, default 0."}}},
                                             {"realm", realm_sv}}},
                             {"required", {"name"}}},
                        json{{"destructiveHint", false}},
                        [](const json &a) { return rt_realm(a, "realm", realms::SERVER, "sound_emit", pass(a, {"name", "pos", "ent", "level", "pitch", "volume", "dsp"})); }});
    mcp::register_tool({"sound_trace", "Trace emitted sounds",
                        std::string("Records the sounds the game emits through the EntityEmitSound hook during duration_s (or until "
                        "max_calls), then removes its recorder: game behaviour is unchanged. Waits the whole duration (progress "
                        "reported; can run as an MCP task). Result: {event, calls: [{t (s since start), sound: {name, "
                        "original_name, level, pitch, volume, dsp, channel, flags, sound_time, pos, entity}}], recorded, total_calls, "
                        "capped, duration_s, tracer_removed}. Not seen: sounds that a hook running before the recorder suppressed "
                        "(returned a value), and sounds that do not go through the EntityEmitSound hook. "
                        "Example: {\"duration_s\": 3, \"realm\": \"server\"}.") + kNeedsMap,
                        json{{"properties", {{"duration_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 600}, {"description", "Recording length in seconds (default 5)."}}},
                                             {"max_calls", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Stop after this many sounds (default 100)."}}},
                                             {"realm", realm_sv}}}},
                        json{{"readOnlyHint", true}}, sound_trace_tool});
    mcp::set_task_support("sound_trace", "optional");

    // Performance.
    mcp::register_tool({"perf_snapshot", "Performance snapshot",
                        "Quick performance numbers of this game process. Result: {frame: {fps, frametime_avg_ms, frametime_max_ms, "
                        "samples} over the last 256 frames of the main thread, last_tick_age_s (seconds since the last frame: large "
                        "= frozen), realms: {menu|client|server: {lua_memory_kb, entity_count, frametime, tick_interval, "
                        "real_frametime (client)}}} for the realms that exist. Note the frame cap (status fps_max: 20 on a light "
                        "test client). For per-hook costs use perf_hook_cost.",
                        json::object(), json{{"readOnlyHint", true}}, perf_snapshot_tool});
}

}  // namespace tools
