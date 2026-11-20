// Tools: ui_click, ui_key. Click and type into a game client's UI (VGUI2 dialogs, Derma, the menu's HTML) by posting window
// messages to the game's own main window. The pure part (coordinates, messages, refusals) is uiinput.cpp; the Win32 part is
// plat::ui_* in platform_win.cpp.
//
// What the module never does: SetCursorPos, SendInput or SetForegroundWindow. The user's real cursor does not move and
// their focus is not taken: PostMessage puts the messages in the game window's own queue.
#include <algorithm>
#include <chrono>

#include "agent.h"
#include "jobs.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "reqctx.h"
#include "tools.h"
#include "uiinput.h"

namespace tools {

using Clock = std::chrono::steady_clock;

namespace {

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// Waits for n more frames of the main loop. "" when they ran, else why not (timeout, cancelled).
std::string wait_n_frames(uint64_t n, double timeout_s) {
    uint64_t start = jobs::frame_count();
    auto t0 = Clock::now();
    while (jobs::frame_count() < start + n) {
        if (reqctx::cancelled()) return "cancelled";
        if (since(t0) > timeout_s) return "timeout";
        plat::sleep_ms(2);
    }
    return "";
}

// What the call needs from the game, read on the main thread in one go.
struct Gathered {
    json error;  // a failed runtime call (an agent::err result), null when none
    bool client_realm_present = false, client_allowed = true;
    std::string lock_reason;
    bool loading = false;
    bool game_ui = false, client_cursor = false;
    int scr_w = 0, scr_h = 0;
};

Gathered gather() {
    json r = jobs::run_on_main(
        [] {
            json s = json::object();
            s["client_realm_present"] = realms::state(realms::CLIENT) != nullptr;
            std::string why;
            s["client_allowed"] = agent::client_allowed(&why);
            s["lock_reason"] = why;
            s["loading"] = agent::map_loading();
            json m = agent::call_runtime(realms::MENU, "ui_state", json::object());
            if (!m.value("ok", false)) return m;
            s["menu"] = m["result"];
            if (s["client_realm_present"].get<bool>() && s["client_allowed"].get<bool>()) {
                json c = agent::call_runtime(realms::CLIENT, "ui_state", json::object());
                if (!c.value("ok", false)) return c;
                s["client"] = c["result"];
            }
            return json{{"ok", true}, {"state", s}};
        },
        5.0);
    Gathered g;
    if (jobs::is_timeout(r)) {
        g.error = agent::err("main_thread_timeout", "the main thread did not respond in time (map loading, freeze or infinite loop?)",
                             json{{"timeout_s", 5.0}, {"seconds_since_last_frame", jobs::seconds_since_last_frame()}});
        return g;
    }
    if (!r.value("ok", false)) {
        g.error = r;
        return g;
    }
    const json &s = r["state"];
    g.client_realm_present = s.value("client_realm_present", false);
    g.client_allowed = s.value("client_allowed", true);
    g.lock_reason = s.value("lock_reason", "");
    g.loading = s.value("loading", false);
    const json menu = s.value("menu", json::object());
    g.loading = g.loading || menu.value("loading", false);
    g.game_ui = menu.value("game_ui", false);
    g.scr_w = (int)menu.value("scr_w", 0.0);
    g.scr_h = (int)menu.value("scr_h", 0.0);
    if (s.contains("client")) g.client_cursor = s["client"].value("cursor", false);
    return g;
}

struct Prepared {
    json error;  // an agent::err result when the call is refused, null otherwise
    plat::UiWindow window;
    uiinput::Geometry geometry;
    std::string cursor_via;  // game_ui | vgui
    json info;               // window / screen, for the result
};

json window_info(const plat::UiWindow &w, const uiinput::Geometry &g) {
    return json{{"client_w", g.client_w}, {"client_h", g.client_h}, {"dpi", w.dpi}, {"dpi_awareness", w.dpi_awareness}, {"class", w.window_class}};
}

// The refusals that need nothing from the game (srcds, not Windows, not a test client): an error result, or null.
json early_refusal(const json &a) {
    uiinput::Facts f;
    f.client_mode = agent::config().mode == agent::Mode::Client;
    f.windows = plat::ui_supported();
    f.test_client = agent::config().test_client;
    f.allow_user_game = a.value("allow_user_game", false);
    f.cursor_visible = true;  // not known yet: only the policy refusals can come out of this first pass
    uiinput::Refusal early = uiinput::decide(f);
    if (!early.code.empty()) return agent::err(early.code, early.message);
    return nullptr;
}

// Everything that can refuse the call, in uiinput::decide's order; fills `p` when it does not.
void prepare(const json &a, Prepared &p) {
    p.error = early_refusal(a);
    if (!p.error.is_null()) return;
    uiinput::Facts f;
    f.client_mode = agent::config().mode == agent::Mode::Client;
    f.windows = plat::ui_supported();
    f.test_client = agent::config().test_client;
    f.allow_user_game = a.value("allow_user_game", false);
    Gathered g = gather();
    if (!g.error.is_null()) {
        p.error = g.error;
        return;
    }
    f.client_realm_present = g.client_realm_present;
    f.client_allowed = g.client_allowed;
    f.lock_reason = g.lock_reason;
    f.loading = g.loading;
    f.window_found = plat::ui_window(p.window);
    f.window_minimized = p.window.minimized;
    f.cursor_visible = g.game_ui || g.client_cursor;
    p.cursor_via = g.game_ui ? "game_ui" : (g.client_cursor ? "vgui" : "");
    p.geometry.client_w = p.window.client_w;
    p.geometry.client_h = p.window.client_h;
    p.geometry.backbuffer_w = g.scr_w;
    p.geometry.backbuffer_h = g.scr_h;
    p.geometry.origin_x = p.window.origin_x;
    p.geometry.origin_y = p.window.origin_y;
    p.info = json{{"window", window_info(p.window, p.geometry)}, {"screen", {{"w", g.scr_w}, {"h", g.scr_h}}}};
    uiinput::Refusal r = uiinput::decide(f);
    if (r.code.empty()) return;
    json details = nullptr;
    if (r.code == "cursor_hidden")
        details = json{{"game_ui_visible", g.game_ui}, {"vgui_cursor_visible", g.client_cursor}, {"client_realm_present", g.client_realm_present}};
    else if (r.code == "window_unavailable")
        details = p.info;
    p.error = agent::err(r.code, r.message, details);
}

// Posts the steps, a frame apart, then waits `wait_frames` more. On success returns null and fills `sent`/`frames_waited`.
json post_steps(const Prepared &p, const std::vector<uiinput::Step> &steps, int wait_frames, std::vector<std::string> &sent,
                uint64_t &frames_waited) {
    uint64_t f0 = jobs::frame_count();
    std::vector<uiinput::Step> done;
    for (size_t i = 0; i < steps.size(); ++i) {
        if (i > 0) {  // the engine handles the cursor move, the press and the release on different frames, like a real mouse
            std::string why = wait_n_frames(1, 5.0);
            if (!why.empty()) {
                sent = uiinput::describe(done);
                return why == "cancelled" ? agent::err("cancelled", "the call was cancelled part-way", json{{"sent", sent}})
                                          : agent::err("timeout", "the game did not render a frame within 5 s (frozen or minimized?): the input is incomplete",
                                                       json{{"sent", sent}});
            }
        }
        for (const uiinput::Msg &m : steps[i].msgs) {
            std::string e;
            if (!plat::ui_post(p.window.hwnd, m.msg, m.wparam, m.lparam, e)) {
                sent = uiinput::describe(done);
                return agent::err("window_unavailable", e, json{{"sent", sent}});
            }
        }
        done.push_back(steps[i]);
    }
    sent = uiinput::describe(done);
    std::string why = wait_n_frames((uint64_t)wait_frames, 5.0 + wait_frames * 0.5);
    frames_waited = jobs::frame_count() - f0;
    if (why == "cancelled") return agent::err("cancelled", "the call was cancelled after the input was posted", json{{"sent", sent}});
    return nullptr;  // a wait that timed out is not an error: the input was posted
}

// The optional screenshot, after the wait. Same image as the `screenshot` tool (no w/h: the game window as it is).
void attach_screenshot(const json &a, json &out) {
    if (!a.value("screenshot_after", false)) return;
    if (!agent::realm_present(realms::CLIENT)) {
        out["result"]["screenshot_note"] =
            "no screenshot: the screenshot tool needs a loaded map (client realm) and cannot capture the main menu or a dialog without one";
        return;
    }
    json shot = capture_screenshot(realms::CLIENT, json::object());
    if (shot.value("ok", false)) {
        out["image"] = shot["image"];
        out["result"]["screenshot"] = shot["result"];
    } else {
        out["result"]["screenshot_error"] = shot["error"];
    }
}

json finish(const json &a, const Prepared &p, json result, const std::vector<std::string> &sent, uint64_t frames_waited) {
    result["sent"] = sent;
    result["frames_waited"] = frames_waited;
    result["window"] = p.info["window"];
    result["screen"] = p.info["screen"];
    result["cursor_via"] = p.cursor_via;
    json out = ok(std::move(result));
    attach_screenshot(a, out);
    return out;
}

json ui_click_tool(const json &a) {
    Prepared p;
    prepare(a, p);
    if (!p.error.is_null()) return p.error;
    uiinput::ClickRequest r;
    r.point.x = a["x"].get<double>();
    r.point.y = a["y"].get<double>();
    r.point.window_space = a.value("coords", "screenshot") == "window";
    if (a.contains("screenshot_size")) {
        const json &sz = a["screenshot_size"];
        r.point.has_shot_size = true;
        r.point.shot_w = sz[0].get<int>();
        r.point.shot_h = sz[1].get<int>();
    }
    r.has_button = a.contains("button");
    r.button = a.value("button", "left");
    r.dbl = a.value("double", false);
    r.has_wheel = a.contains("wheel");
    r.wheel = a.value("wheel", 0);
    uiinput::ClickPlan plan = uiinput::plan_click(r, p.geometry);
    if (!plan.ok) return agent::err(plan.code, plan.message, p.info);
    std::vector<std::string> sent;
    uint64_t frames = 0;
    json e = post_steps(p, plan.steps, a.value("wait_frames", 5), sent, frames);
    if (!e.is_null()) return e;
    json res = json::object();
    res["at"] = json{{"x", plan.at.client.x}, {"y", plan.at.client.y}};
    res["coords"] = r.point.window_space ? "window" : "screenshot";
    if (plan.at.scale_x != 1 || plan.at.scale_y != 1) res["scale"] = json{{"x", plan.at.scale_x}, {"y", plan.at.scale_y}};
    if (r.has_wheel) res["wheel"] = r.wheel;
    else res["button"] = r.button;
    return finish(a, p, res, sent, frames);
}

json ui_key_tool(const json &a) {
    bool has_key = a.contains("key") && !a["key"].get<std::string>().empty();
    bool has_text = a.contains("text") && !a["text"].get<std::string>().empty();
    if (!has_key && !has_text) return agent::err("invalid_params", "pass `key` (enter, escape, tab, backspace, up, f1...) and/or `text` (what to type)");
    uiinput::KeyInfo key;
    if (has_key && !uiinput::lookup_key(a["key"].get<std::string>(), key))
        return agent::err("invalid_params",
                          "unknown key \"" + a["key"].get<std::string>() +
                              "\": enter, escape, tab, backspace, space, up, down, left, right, home, end, pageup, pagedown, insert, delete, "
                              "f1..f12, a..z, 0..9. Use `text` to type characters");
    std::u16string text;
    if (has_text) {
        std::string e;
        if (!uiinput::text_to_utf16(a["text"].get<std::string>(), text, e)) return agent::err("invalid_params", e);
    }
    // Characters the engine would alter (ANSI code page narrowing): refused, before the game is touched, unless allow_lossy.
    std::vector<unsigned> lossy = uiinput::altered_chars(text, [](unsigned cp) { return plat::ui_ansi_keeps(cp); });
    if (!lossy.empty() && !a.value("allow_lossy", false)) {
        json early = early_refusal(a);  // srcds, not Windows, not a test client come first
        if (!early.is_null()) return early;
        uiinput::Refusal r = uiinput::not_representable(lossy, plat::ui_ansi_code_page());
        json chars = json::array();
        for (unsigned cp : lossy) chars.push_back(uiinput::utf8_of(cp));
        return agent::err(r.code, r.message, json{{"code_page", plat::ui_ansi_code_page()}, {"characters", chars}});
    }
    Prepared p;
    prepare(a, p);
    if (!p.error.is_null()) return p.error;
    // Text first, then the key (type a name, press enter).
    std::vector<uiinput::Step> steps = uiinput::text_steps(text);
    if (has_key) {
        std::vector<uiinput::Step> k = uiinput::key_steps(key, [](unsigned vk) { return plat::ui_scan_code(vk); });
        steps.insert(steps.end(), k.begin(), k.end());
    }
    std::vector<std::string> sent;
    uint64_t frames = 0;
    json e = post_steps(p, steps, a.value("wait_frames", 5), sent, frames);
    if (!e.is_null()) return e;
    json res = json::object();
    if (has_key) res["key"] = key.name;
    if (has_text) res["text_units"] = text.size();
    if (!lossy.empty()) {  // allow_lossy: say which characters the engine changed
        json chars = json::array();
        for (unsigned cp : lossy) chars.push_back(uiinput::utf8_of(cp));
        res["lossy_characters"] = chars;
        res["ansi_code_page"] = plat::ui_ansi_code_page();
    }
    return finish(a, p, res, sent, frames);
}

}  // namespace

void register_ui() {
    const std::string common =
        " Windows game client only (unsupported on srcds). Refusals, each its own error code: unsupported; not_a_test_client (the game "
        "was not started by instances_launch: pass allow_user_game: true to post into the user's own game); client_locked (connected to "
        "a server that is not on this machine); loading (a map is loading: events_wait map_loaded); window_unavailable (window "
        "minimized or not found); cursor_hidden (the mouse is captured by the game view: no menu, dialog or popup is open; open one "
        "first, e.g. console_exec gameui_activate); out_of_bounds (ui_click only: the point is outside the image or window). The "
        "input is posted to the game's own window (PostMessage): the user's real cursor does not move and no window takes the focus.";
    json wait_frames = {{"type", "integer"}, {"minimum", 0}, {"maximum", 600}, {"description", "Frames to wait after the last message so the UI reacts (default 5; a light test client runs at 20 fps)."}};
    json shot_after = {{"type", "boolean"}, {"description", "Also return a screenshot after the wait (same image as `screenshot`; needs a loaded map: the main menu cannot be captured)."}};
    json allow = {{"type", "boolean"}, {"description", "Post into a game that instances_launch did not start (the user's own). Default false."}};

    mcp::register_tool(
        {"ui_click", "Click in the game UI",
         "Clicks (or scrolls the wheel) at a point of the game window: use it for what has no console command, the buttons of VGUI2 "
         "dialogs (\"Disconnected by the server\" > Close), Derma panels and the menu's HTML. Take the coordinates from a `screenshot` "
         "(coords \"screenshot\", the default: pixels of that image, origin top-left; if the image you read has another size than the "
         "game window's, e.g. a screenshot with w/h, pass screenshot_size [w, h] and the point is scaled) or give coords \"window\" "
         "(pixels of the window's client area, title bar excluded). DPI scaling is handled: the result says the window size, DPI and "
         "the scale applied. Posts WM_MOUSEMOVE, then the button down and up (double: down, up, double-click, up) one frame apart, or "
         "WM_MOUSEMOVE and one WM_MOUSEWHEEL (wheel: notches, positive = up). Result: {sent: [messages], at: {x, y} (client pixels), "
         "coords, button | wheel, scale?, frames_waited, window: {client_w, client_h, dpi, dpi_awareness, class}, screen: {w, h}, "
         "cursor_via (game_ui | vgui), screenshot?, screenshot_note?} plus the image with screenshot_after. The cursor is visible when "
         "the game menu (main menu, ESC menu, a dialog on it) is up, gui.IsGameUIVisible(), or a Derma popup, the context or spawn "
         "menu has it, vgui.CursorVisible() in the client realm." + common +
         " Example: {\"x\": 640, \"y\": 400, \"screenshot_after\": true}.",
         json{{"properties",
               {{"x", {{"type", "number"}, {"description", "Pixel column (see coords)."}}},
                {"y", {{"type", "number"}, {"description", "Pixel row (see coords)."}}},
                {"button", {{"type", "string"}, {"enum", {"left", "right", "middle"}}, {"description", "Default left. Not with wheel."}}},
                {"double", {{"type", "boolean"}, {"description", "Double click (down, up, double-click, up). Not with wheel."}}},
                {"wheel", {{"type", "integer"}, {"minimum", -100}, {"maximum", 100}, {"description", "Scroll this many notches instead of clicking (positive = up, negative = down; not 0)."}}},
                {"coords", {{"type", "string"}, {"enum", {"screenshot", "window"}}, {"description", "Space of x and y: \"screenshot\" (default) or \"window\" (client-area pixels)."}}},
                {"screenshot_size", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 1}}}, {"minItems", 2}, {"maxItems", 2},
                                     {"description", "[w, h] of the image x and y were read on, when it is not the game window's size (a screenshot with w/h)."}}},
                {"wait_frames", wait_frames},
                {"screenshot_after", shot_after},
                {"allow_user_game", allow}}},
               {"required", {"x", "y"}}},
         json{{"destructiveHint", false}, {"openWorldHint", false}}, ui_click_tool});

    mcp::register_tool(
        {"ui_key", "Press a key or type text in the game UI",
         "Presses a key and/or types text into the focused part of the game UI (a text entry of a dialog, a Derma TextEntry, an HTML "
         "input): key is a named key (enter, escape, tab, backspace, space, up, down, left, right, home, end, pageup, pagedown, "
         "insert, delete, f1..f12, a letter, a digit or space: those type the character, as WM_CHAR like `text`, they are not pressed as keys), text is typed character by character as WM_CHAR "
         "(up to 256 characters; no control characters: use key). The engine narrows every typed character through the machine's ANSI code page with best-fit "
         "(live, CP1252: accents of western Europe arrive, l-stroke becomes l, Greek a, Cyrillic and CJK ?): a text with such a character is refused "
         "with text_not_representable (before anything is typed; details: code_page, characters) unless allow_lossy: true. For those, set the text from Lua with "
         "lua_exec in the realm that owns the panel (menu for the main menu's dialogs, client for Derma in game): entry:SetText(\"...\") on a "
         "DTextEntry (DTextEntry:SetValue does nothing while the entry has the keyboard focus, which a ui_click gives it; entry:OnValueChange(text) "
         "afterwards if the page listens to it). With both, the text is typed first, then the key is "
         "pressed (text \"hello\" + key enter). Click the field first with ui_click. Result: {sent: [messages], key?, text_units?, "
         "lossy_characters? and ansi_code_page? (allow_lossy), frames_waited, window, screen, cursor_via, screenshot?}." + common +
         " Example: {\"text\": \"my name\", \"key\": \"enter\"}.",
         json{{"properties",
               {{"key", {{"type", "string"}, {"description", "Named key to press (enter, escape, tab, backspace, up, f1...)."}}},
                {"text", {{"type", "string"}, {"description", "Text to type (UTF-8, 256 characters at most). Only characters of the machine's ANSI code page arrive unchanged (accents of western Europe on CP1252); see text_not_representable."}}},
                {"allow_lossy", {{"type", "boolean"}, {"description", "Type the text even if the engine will alter some characters (l-stroke becomes l, Greek a, Cyrillic and CJK ?). Default false: the call is refused with text_not_representable."}}},
                {"wait_frames", wait_frames},
                {"screenshot_after", shot_after},
                {"allow_user_game", allow}}}},
         json{{"destructiveHint", false}, {"openWorldHint", false}}, ui_key_tool});
}

}  // namespace tools
