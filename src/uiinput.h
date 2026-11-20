// UI input (ui_click, ui_key): the pure part. Coordinate conversion, the window messages to post, and the decision to
// refuse a call. No Win32 and no game access here (native unit tests cover it); the Win32 side is plat::ui_* in
// platform_win.cpp and the tools are in tools_ui.cpp.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace uiinput {

// Win32 message numbers and flags (winuser.h). Prefixed k: windows.h defines WM_* as macros. platform_win.cpp
// static_asserts that they equal the real ones.
constexpr unsigned kWmKeyDown = 0x0100, kWmKeyUp = 0x0101, kWmChar = 0x0102;
constexpr unsigned kWmMouseMove = 0x0200, kWmLButtonDown = 0x0201, kWmLButtonUp = 0x0202, kWmLButtonDblClk = 0x0203,
                   kWmRButtonDown = 0x0204, kWmRButtonUp = 0x0205, kWmRButtonDblClk = 0x0206, kWmMButtonDown = 0x0207,
                   kWmMButtonUp = 0x0208, kWmMButtonDblClk = 0x0209, kWmMouseWheel = 0x020A;
constexpr unsigned kMkLButton = 0x0001, kMkRButton = 0x0002, kMkMButton = 0x0010;
constexpr int kWheelDelta = 120;  // WHEEL_DELTA: one notch

struct Msg {
    unsigned msg = 0;
    unsigned long long wparam = 0;
    long long lparam = 0;
};
// Messages posted together; the tool waits a frame between two steps so that the engine processes the cursor move, the press
// and the release on different frames, as it would with a real mouse.
struct Step {
    std::vector<Msg> msgs;
};

// --- Coordinates ---

// Pixel sizes. client: GetClientRect of the game window (title bar and borders excluded). backbuffer: what the engine renders
// (ScrW/ScrH), which is the size of the `screenshot` image without w/h. origin: the client area's top-left in screen coordinates.
struct Geometry {
    int client_w = 0, client_h = 0;
    int backbuffer_w = 0, backbuffer_h = 0;
    int origin_x = 0, origin_y = 0;
};
struct Point {
    int x = 0, y = 0;
};
struct PointRequest {
    double x = 0, y = 0;
    bool window_space = false;    // coords "window": client-area pixels; otherwise coords "screenshot"
    bool has_shot_size = false;   // screenshot_size given: the size of the image the caller read the point on
    int shot_w = 0, shot_h = 0;
};
struct Converted {
    bool ok = false;
    Point client;                  // client-area pixels (what is posted)
    double scale_x = 1, scale_y = 1;  // client pixels per source pixel
    std::string code, message;     // when !ok: out_of_bounds, window_unavailable or invalid_params
};
Converted to_client(const PointRequest &req, const Geometry &g);

// --- Mouse ---

enum class Button { Left, Right, Middle };
bool parse_button(const std::string &name, Button &out);  // "left", "right", "middle"

struct ClickRequest {
    PointRequest point;
    std::string button = "left";
    bool has_button = false;  // the caller named a button (refused together with wheel)
    bool dbl = false;
    bool has_wheel = false;
    int wheel = 0;            // notches, positive = away from the user (up)
};
struct ClickPlan {
    bool ok = false;
    std::string code, message;
    Converted at;
    std::vector<Step> steps;
};
// Validates the request, converts the point and builds the messages: move, then press/release (double: press, release,
// double-click, release), or move then one wheel message (wheel is in screen coordinates, hence g.origin).
ClickPlan plan_click(const ClickRequest &req, const Geometry &g);

// --- Keyboard ---

struct KeyInfo {
    unsigned vk = 0;
    bool extended = false;  // extended-key flag (arrows, home, end...)
    unsigned ch = 0;        // the character the key types (letters, digits, space) or the WM_CHAR of enter 13, tab 9, backspace 8, escape 27; 0 = none
    std::string name;       // canonical name
};
// Case-insensitive; aliases (return, esc, pgup, pgdn...). False for an unknown name.
bool lookup_key(const std::string &name, KeyInfo &out);
// Scan code of a virtual key (MapVirtualKey on Windows); null = 0.
using ScanFn = std::function<unsigned(unsigned vk)>;
// A key that types a character (ch >= 0x20: letters, digits, space) is one WM_CHAR, nothing else: a posted WM_KEYDOWN would make the
// game's own message loop (TranslateMessage) add a second character, from the real keyboard layout. Any other key: key down (+ the
// WM_CHAR of enter, tab, backspace, escape), then key up one step later.
std::vector<Step> key_steps(const KeyInfo &k, const ScanFn &scan = nullptr);

constexpr size_t kMaxTextChars = 256;
// UTF-8 to UTF-16 (strict). False with `err` for invalid UTF-8, a control character (use `key` for enter, tab, backspace...)
// or more than kMaxTextChars characters.
bool text_to_utf16(const std::string &utf8, std::u16string &out, std::string &err);
// One WM_CHAR per UTF-16 unit (a surrogate pair is two, as Windows delivers it), a few per step.
std::vector<Step> text_steps(const std::u16string &text);

// Names of what was posted, for the result: ["WM_MOUSEMOVE", "WM_LBUTTONDOWN", "WM_CHAR x5", ...].
std::vector<std::string> describe(const std::vector<Step> &steps);

// --- Refusals ---

struct Facts {
    bool client_mode = true;   // false: srcds
    bool windows = true;       // false: no window messages on this platform
    bool test_client = false;  // started by instances_launch (-mcp_test_client)
    bool allow_user_game = false;
    bool client_realm_present = false;
    bool client_allowed = true;  // the network lock (agent::client_allowed)
    std::string lock_reason;
    bool loading = false;
    bool window_found = true;
    bool window_minimized = false;
    bool cursor_visible = false;
};
struct Refusal {
    std::string code;  // "" = nothing refused
    std::string message;
};
// The first refusal, in this order: unsupported, not_a_test_client, client_locked, loading, window_unavailable, cursor_hidden.
Refusal decide(const Facts &f);

}  // namespace uiinput
