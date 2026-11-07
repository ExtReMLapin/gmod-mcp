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
                        "Searches entities by class (glob, e.g. prop_*), model (glob) and/or distance to a point. Returns id, class, model, "
                        "targetname, position, angles (sorted by distance when `near` is set).",
                        json{{"properties", {{"class", {{"type", "string"}}},
                                             {"model", {{"type", "string"}}},
                                             {"near", vec_schema("Center [x,y,z].")},
                                             {"radius", {{"type", "number"}, {"minimum", 0}, {"description", "Default 512."}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Default 100."}}},
                                             {"realm", realm_sv}}}},
                        RO, [](const json &a) {
                            return rt_realm(a, "realm", realms::SERVER, "ents_find", pass(a, {"class", "model", "near", "radius", "limit"}));
                        }});
    mcp::register_tool({"ent_info", "Entity details",
                        "Keyvalues (server), networked vars, Lua table fields, velocity, health, bounds, parent/owner and, for doors, "
                        "their state (m_eDoorState / m_toggle_state).",
                        json{{"properties", {{"id", {{"type", "integer"}, {"minimum", 0}}},
                                             {"realm", realm_sv},
                                             {"depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}}}},
                             {"required", {"id"}}},
                        RO, [](const json &a) { return rt_realm(a, "realm", realms::SERVER, "ent_info", pass(a, {"id", "depth", "limit"})); }});
    mcp::register_tool({"ent_spawn", "Spawn an entity",
                        "ents.Create + keyvalues + Spawn/Activate on the server. Error invalid_class if the class does not exist.",
                        json{{"properties", {{"class", {{"type", "string"}, {"minLength", 1}}},
                                             {"pos", vec_schema("Position.")},
                                             {"ang", vec_schema("Angles [p,y,r].")},
                                             {"model", {{"type", "string"}}},
                                             {"keyvalues", {{"type", "object"}}},
                                             {"freeze", {{"type", "boolean"}, {"description", "Disable physics motion."}}}}},
                             {"required", {"class"}}},
                        json{{"destructiveHint", false}},
                        [](const json &a) { return agent::rt(realms::SERVER, "ent_spawn", pass(a, {"class", "pos", "ang", "model", "keyvalues", "freeze"})); }});
    mcp::register_tool({"ent_remove", "Remove an entity", "Removes an entity (players and the world are refused).",
                        json{{"properties", {{"id", {{"type", "integer"}, {"minimum", 1}}}}}, {"required", {"id"}}},
                        json{{"destructiveHint", true}}, [](const json &a) { return agent::rt(realms::SERVER, "ent_remove", pass(a, {"id"})); }});
    mcp::register_tool({"ent_fire", "Fire an entity input",
                        "Sends a map I/O input (ent:Fire), e.g. Open/Close/Toggle on a door. The effect is asynchronous: check with ent_info "
                        "or wait_for.",
                        json{{"properties", {{"id", {{"type", "integer"}, {"minimum", 0}}},
                                             {"input", {{"type", "string"}, {"minLength", 1}}},
                                             {"param", {{"type", "string"}}},
                                             {"delay", {{"type", "number"}, {"minimum", 0}}}}},
                             {"required", {"id", "input"}}},
                        json{{"destructiveHint", true}}, [](const json &a) { return agent::rt(realms::SERVER, "ent_fire", pass(a, {"id", "input", "param", "delay"})); }});
    mcp::register_tool({"players_list", "List players", "Players with position, eye position/angles, health, noclip, god mode, bot flag.",
                        json{{"properties", {{"realm", realm_sv}}}}, RO,
                        [](const json &a) { return rt_realm(a, "realm", realms::SERVER, "players_list", json::object()); }});
    mcp::register_tool({"player_set", "Move/configure a player",
                        "Sets position, eye angles, noclip and god mode of a player (default: first player). Check client-side with "
                        "lua_exec(client, 'return LocalPlayer():GetPos()') after a frame.",
                        json{{"properties", {{"id", {{"type", "integer"}, {"minimum", 1}}},
                                             {"pos", vec_schema("Position.")},
                                             {"ang", vec_schema("Eye angles [p,y,r].")},
                                             {"noclip", {{"type", "boolean"}}},
                                             {"god", {{"type", "boolean"}}}}}},
                        json{{"destructiveHint", false}, {"idempotentHint", true}},
                        [](const json &a) { return agent::rt(realms::SERVER, "player_set", pass(a, {"id", "pos", "ang", "noclip", "god"})); }});
    mcp::register_tool({"bots_add", "Add bots", "Adds n bots (player.CreateNextBot). Error no_slots when maxplayers is too small (e.g. singleplayer).",
                        json{{"properties", {{"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 127}}}, {"name", {{"type", "string"}}}}},
                             {"required", {"n"}}},
                        json{{"destructiveHint", false}}, [](const json &a) { return agent::rt(realms::SERVER, "bots_add", pass(a, {"n", "name"})); }});
    json trace_props = {{"start", vec_schema("Start [x,y,z].")},
                        {"end", vec_schema("End [x,y,z].")},
                        {"mask", {{"type", {"string", "integer"}}, {"description", "MASK_SOLID (default), MASK_SHOT, MASK_SOLID_BRUSHONLY, MASK_VISIBLE, ... or a number."}}},
                        {"filter", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Entity ids to ignore."}}},
                        {"realm", realm_sv}};
    mcp::register_tool({"trace", "Trace a line",
                        "util.TraceLine: hit, position, normal, fraction, MatType (+ MAT_* name), surfaceprop name, texture, entity, "
                        "startsolid/allsolid, fraction_left_solid.",
                        json{{"properties", trace_props}, {"required", {"start", "end"}}}, RO,
                        [](const json &a) { return rt_realm(a, "realm", realms::SERVER, "trace", pass(a, {"start", "end", "mask", "filter"})); }});
    json tt_props = trace_props;
    tt_props["max_surfaces"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"description", "Default 32."}};
    mcp::register_tool({"trace_through", "Surfaces crossed by a segment",
                        "Lists every solid crossed between start and end: entry and exit points, thickness, entry/exit materials and normals "
                        "(uses FractionLeftSolid). Designed for sound propagation through walls and doors.",
                        json{{"properties", tt_props}, {"required", {"start", "end"}}}, RO,
                        [](const json &a) {
                            return rt_realm(a, "realm", realms::SERVER, "trace_through", pass(a, {"start", "end", "mask", "filter", "max_surfaces"}));
                        }});
}

void register_render() {
    json realm_sv = realm_prop("Realm (default server).", false);
    json img ={{"format", {{"type", "string"}, {"enum", {"jpeg", "png"}}, {"description", "Default jpeg."}}},
                {"quality", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}, {"description", "JPEG quality (default 90)."}}},
                {"w", {{"type", "integer"}, {"minimum", 16}, {"maximum", 3840}}},
                {"h", {{"type", "integer"}, {"minimum", 16}, {"maximum", 2160}}}};
    mcp::register_tool({"screenshot", "Take a screenshot",
                        "Captures the game view (render.Capture in a render hook) and returns it as an MCP image. In game: player view; "
                        "custom w/h renders the player view at that size (no HUD). At the main menu: captures the menu (w/h crop the "
                        "top-left corner).",
                        json{{"properties", img}}, json{{"readOnlyHint", true}}, screenshot_tool});
    json from = img;
    from["pos"] = vec_schema("Camera position.");
    from["ang"] = vec_schema("Camera angles [p,y,r].");
    from["fov"] = {{"type", "number"}, {"minimum", 1}, {"maximum", 179}, {"description", "Default 90."}};
    mcp::register_tool({"screenshot_from", "Screenshot from a viewpoint",
                        "Captures the scene from an arbitrary camera. Screen size: temporary CalcView hook removed afterwards "
                        "(camera_restored). Custom w/h: rendered into a render target. Debug primitives (debug_draw) are visible.",
                        json{{"properties", from}, {"required", {"pos", "ang"}}}, json{{"readOnlyHint", true}}, screenshot_from_tool});
    json prim = {{"type", "object"},
                 {"properties",
                  {{"type", {{"type", "string"}, {"enum", {"line", "box", "sphere", "text"}}}},
                   {"from", vec_schema("line")},
                   {"to", vec_schema("line")},
                   {"pos", vec_schema("box/sphere/text")},
                   {"ang", vec_schema("box")},
                   {"mins", vec_schema("box (local)")},
                   {"maxs", vec_schema("box (local)")},
                   {"min", vec_schema("box (world AABB)")},
                   {"max", vec_schema("box (world AABB)")},
                   {"radius", {{"type", "number"}}},
                   {"text", {{"type", "string"}}},
                   {"scale", {{"type", "number"}}},
                   {"color", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 4}}},
                   {"ignore_z", {{"type", "boolean"}, {"description", "Draw through walls (default true)."}}}}},
                 {"required", {"type"}},
                 {"additionalProperties", false}};
    mcp::register_tool({"debug_draw", "Draw debug primitives",
                        "Draws 3D lines, boxes, spheres and text in the client view (Lua rendering, visible in screenshots) for `duration` "
                        "seconds (0 = until debug_clear).",
                        json{{"properties", {{"primitives", {{"type", "array"}, {"items", prim}, {"minItems", 1}, {"maxItems", 5000}}},
                                             {"duration", {{"type", "number"}, {"minimum", 0}, {"description", "Seconds, default 10."}}}}},
                             {"required", {"primitives"}}},
                        json{{"destructiveHint", false}},
                        [](const json &a) { return agent::rt(realms::CLIENT, "debug_draw", pass(a, {"primitives", "duration"})); }});
    mcp::register_tool({"debug_clear", "Clear debug primitives", "Removes every primitive drawn by debug_draw.", json::object(),
                        json{{"destructiveHint", false}, {"idempotentHint", true}},
                        [](const json &) { return agent::rt(realms::CLIENT, "debug_clear", json::object()); }});

    // Sound.
    mcp::register_tool({"sound_emit", "Emit a sound",
                        "Plays a test sound at a position (EmitSound) or on an entity (Entity:EmitSound) with level (dB), pitch, volume and "
                        "DSP preset. Error not_found if neither a soundscript nor a sound/ file exists.",
                        json{{"properties", {{"name", {{"type", "string"}, {"minLength", 1}}},
                                             {"pos", vec_schema("Position (or ent).")},
                                             {"ent", {{"type", "integer"}, {"minimum", 0}}},
                                             {"level", {{"type", "integer"}, {"minimum", 0}, {"maximum", 511}, {"description", "Sound level in dB, default 75."}}},
                                             {"pitch", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"description", "Default 100."}}},
                                             {"volume", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"description", "Default 1."}}},
                                             {"dsp", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"description", "DSP preset, default 0."}}},
                                             {"realm", realm_sv}}},
                             {"required", {"name"}}},
                        json{{"destructiveHint", false}},
                        [](const json &a) { return rt_realm(a, "realm", realms::SERVER, "sound_emit", pass(a, {"name", "pos", "ent", "level", "pitch", "volume", "dsp"})); }});
    mcp::register_tool({"sound_trace", "Trace emitted sounds",
                        "Records EntityEmitSound calls for duration_s with normalized fields (name, level, pitch, volume, dsp, channel, flags, "
                        "position, entity). Same mechanism as hook_trace.",
                        json{{"properties", {{"duration_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 600}}},
                                             {"max_calls", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
                                             {"realm", realm_sv}}}},
                        json{{"readOnlyHint", true}}, sound_trace_tool});
    mcp::set_task_support("sound_trace", "optional");

    // Performance.
    mcp::register_tool({"perf_snapshot", "Performance snapshot",
                        "FPS and average/max frame time (main thread, last 256 frames), Lua memory per realm, entity count, tick interval.",
                        json::object(), json{{"readOnlyHint", true}}, perf_snapshot_tool});
}

}  // namespace tools
