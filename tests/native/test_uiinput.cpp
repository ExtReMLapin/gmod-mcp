// ui_click / ui_key, the pure part (src/uiinput.cpp): coordinate conversion, the messages to post, the refusal decision.
#include <limits>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "check.h"
#include "platform/platform.h"
#include "uiinput.h"

using namespace uiinput;

static Geometry geom(int cw, int ch, int bw, int bh, int ox = 0, int oy = 0) {
    Geometry g;
    g.client_w = cw;
    g.client_h = ch;
    g.backbuffer_w = bw;
    g.backbuffer_h = bh;
    g.origin_x = ox;
    g.origin_y = oy;
    return g;
}
static PointRequest shot(double x, double y) {
    PointRequest p;
    p.x = x;
    p.y = y;
    return p;
}
static long long mk_lparam(int x, int y) { return ((long long)(y & 0xFFFF) << 16) | (x & 0xFFFF); }
static bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

static void test_coordinates() {
    // The screenshot is the backbuffer; the client area has the same size: the point is posted as it is.
    Converted c = to_client(shot(100, 50), geom(1280, 720, 1280, 720));
    CHECK(c.ok && c.client.x == 100 && c.client.y == 50 && c.scale_x == 1 && c.scale_y == 1);
    // Corners of the image.
    c = to_client(shot(0, 0), geom(1280, 720, 1280, 720));
    CHECK(c.ok && c.client.x == 0 && c.client.y == 0);
    c = to_client(shot(1279, 719), geom(1280, 720, 1280, 720));
    CHECK(c.ok && c.client.x == 1279 && c.client.y == 719);
    // One pixel past the image: out of bounds, with the size in the message.
    c = to_client(shot(1280, 10), geom(1280, 720, 1280, 720));
    CHECK(!c.ok && c.code == "out_of_bounds" && contains(c.message, "1280x720"));
    c = to_client(shot(10, 720), geom(1280, 720, 1280, 720));
    CHECK(!c.ok && c.code == "out_of_bounds");
    c = to_client(shot(-1, 10), geom(1280, 720, 1280, 720));
    CHECK(!c.ok && c.code == "out_of_bounds");

    // DPI scaling: the client area (150 %) is larger than the backbuffer the engine renders: the point is scaled.
    c = to_client(shot(640, 360), geom(1920, 1080, 1280, 720));
    CHECK(c.ok && c.client.x == 960 && c.client.y == 540 && c.scale_x == 1.5 && c.scale_y == 1.5);
    c = to_client(shot(1279, 719), geom(1920, 1080, 1280, 720));
    CHECK(c.ok && c.client.x == 1919 && c.client.y == 1079);
    // The reverse: a client area smaller than the backbuffer.
    c = to_client(shot(1000, 500), geom(640, 360, 1280, 720));
    CHECK(c.ok && c.client.x == 500 && c.client.y == 250);

    // The caller read the point on an image of another size (a screenshot with w/h): scaled to the client area.
    PointRequest p = shot(320, 180);
    p.has_shot_size = true;
    p.shot_w = 640;
    p.shot_h = 360;
    c = to_client(p, geom(1280, 720, 1280, 720));
    CHECK(c.ok && c.client.x == 641 && c.client.y == 361 && c.scale_x == 2);
    p.x = 640;  // outside the 640-wide image although inside the window
    c = to_client(p, geom(1280, 720, 1280, 720));
    CHECK(!c.ok && c.code == "out_of_bounds" && contains(c.message, "640x360"));
    p.x = 639;
    p.y = 359;
    c = to_client(p, geom(1280, 720, 1280, 720));
    CHECK(c.ok && c.client.x == 1279 && c.client.y == 719);
    // Same size as the client area: identity, whatever the backbuffer is.
    p.shot_w = 1280;
    p.shot_h = 720;
    p.x = 100;
    p.y = 50;
    c = to_client(p, geom(1280, 720, 1920, 1080));
    CHECK(c.ok && c.client.x == 100 && c.client.y == 50);
    // A nonsensical image size.
    p.shot_w = 0;
    c = to_client(p, geom(1280, 720, 1280, 720));
    CHECK(!c.ok && c.code == "invalid_params");

    // coords "window": client-area pixels, no scaling, whatever the backbuffer is.
    PointRequest w = shot(100, 50);
    w.window_space = true;
    c = to_client(w, geom(1920, 1080, 1280, 720));
    CHECK(c.ok && c.client.x == 100 && c.client.y == 50 && c.scale_x == 1);
    w.x = 1919;
    w.y = 1079;
    c = to_client(w, geom(1920, 1080, 1280, 720));
    CHECK(c.ok && c.client.x == 1919 && c.client.y == 1079);
    w.x = 1920;
    c = to_client(w, geom(1920, 1080, 1280, 720));
    CHECK(!c.ok && c.code == "out_of_bounds" && contains(c.message, "1920x1080"));

    // Unknown backbuffer: the client area stands for it.
    c = to_client(shot(100, 50), geom(1280, 720, 0, 0));
    CHECK(c.ok && c.client.x == 100 && c.client.y == 50);
    // No client area (minimized window).
    c = to_client(shot(1, 1), geom(0, 0, 1280, 720));
    CHECK(!c.ok && c.code == "window_unavailable");
    // NaN and infinities.
    c = to_client(shot(std::numeric_limits<double>::quiet_NaN(), 1), geom(1280, 720, 1280, 720));
    CHECK(!c.ok && c.code == "invalid_params");
    c = to_client(shot(1, std::numeric_limits<double>::infinity()), geom(1280, 720, 1280, 720));
    CHECK(!c.ok && c.code == "invalid_params");
    // A fraction is the pixel it falls in.
    c = to_client(shot(10.9, 5.2), geom(1280, 720, 1280, 720));
    CHECK(c.ok && c.client.x == 10 && c.client.y == 5);
}

static void test_mouse_steps() {
    Geometry g = geom(1280, 720, 1280, 720);
    ClickRequest r;
    r.point = shot(100, 50);
    ClickPlan p = plan_click(r, g);
    CHECK(p.ok && p.steps.size() == 3);
    if (p.ok && p.steps.size() == 3) {
        CHECK(p.steps[0].msgs.size() == 1 && p.steps[0].msgs[0].msg == kWmMouseMove && p.steps[0].msgs[0].lparam == mk_lparam(100, 50));
        CHECK(p.steps[1].msgs.size() == 1 && p.steps[1].msgs[0].msg == kWmLButtonDown && p.steps[1].msgs[0].wparam == kMkLButton &&
              p.steps[1].msgs[0].lparam == mk_lparam(100, 50));
        CHECK(p.steps[2].msgs.size() == 1 && p.steps[2].msgs[0].msg == kWmLButtonUp && p.steps[2].msgs[0].wparam == 0);
    }
    CHECK(p.at.client.x == 100 && p.at.client.y == 50);

    r.button = "right";
    r.has_button = true;
    p = plan_click(r, g);
    CHECK(p.ok && p.steps.size() == 3 && p.steps[1].msgs[0].msg == kWmRButtonDown && p.steps[1].msgs[0].wparam == kMkRButton &&
          p.steps[2].msgs[0].msg == kWmRButtonUp);
    r.button = "middle";
    p = plan_click(r, g);
    CHECK(p.ok && p.steps.size() == 3 && p.steps[1].msgs[0].msg == kWmMButtonDown && p.steps[1].msgs[0].wparam == kMkMButton &&
          p.steps[2].msgs[0].msg == kWmMButtonUp);
    r.button = "back";
    p = plan_click(r, g);
    CHECK(!p.ok && p.code == "invalid_params" && contains(p.message, "back"));

    // Double click: press, release, double-click, release (what Windows sends).
    r.button = "left";
    r.dbl = true;
    p = plan_click(r, g);
    CHECK(p.ok && p.steps.size() == 5);
    if (p.ok && p.steps.size() == 5) {
        CHECK(p.steps[1].msgs[0].msg == kWmLButtonDown && p.steps[2].msgs[0].msg == kWmLButtonUp &&
              p.steps[3].msgs[0].msg == kWmLButtonDblClk && p.steps[3].msgs[0].wparam == kMkLButton && p.steps[4].msgs[0].msg == kWmLButtonUp);
    }
    r.button = "right";
    p = plan_click(r, g);
    CHECK(p.ok && p.steps.size() == 5 && p.steps[3].msgs[0].msg == kWmRButtonDblClk);

    // A point outside the window is refused with the conversion's code, nothing built.
    ClickRequest out;
    out.point = shot(5000, 5);
    p = plan_click(out, g);
    CHECK(!p.ok && p.code == "out_of_bounds" && p.steps.empty());

    // Wheel: move, then one wheel message; the delta in the high word, the position in SCREEN coordinates.
    ClickRequest wr;
    wr.point = shot(10, 20);
    wr.has_wheel = true;
    wr.wheel = 3;
    p = plan_click(wr, geom(1280, 720, 1280, 720, 100, 200));
    CHECK(p.ok && p.steps.size() == 2);
    if (p.ok && p.steps.size() == 2) {
        CHECK(p.steps[0].msgs[0].msg == kWmMouseMove && p.steps[0].msgs[0].lparam == mk_lparam(10, 20));
        CHECK(p.steps[1].msgs[0].msg == kWmMouseWheel && (p.steps[1].msgs[0].wparam >> 16) == 360 && (p.steps[1].msgs[0].wparam & 0xFFFF) == 0);
        CHECK(p.steps[1].msgs[0].lparam == mk_lparam(110, 220));
    }
    wr.wheel = -2;
    p = plan_click(wr, geom(1280, 720, 1280, 720));
    CHECK(p.ok && p.steps.size() == 2 && (p.steps[1].msgs[0].wparam >> 16) == 0xFF10);
    // A client area on a monitor left of the primary one: negative screen coordinates, 16-bit two's complement.
    wr.wheel = 1;
    wr.point = shot(100, 20);
    p = plan_click(wr, geom(1280, 720, 1280, 720, -1920, 0));
    CHECK(p.ok && p.steps.size() == 2 && p.steps[1].msgs[0].lparam == mk_lparam(-1820, 20) && (p.steps[1].msgs[0].lparam & 0xFFFF) == 0xF8E4);
    // The wheel does not click: a button, a double click, no notch or too many notches are refused.
    ClickRequest bad = wr;
    bad.has_button = true;
    p = plan_click(bad, g);
    CHECK(!p.ok && p.code == "invalid_params" && contains(p.message, "wheel"));
    bad = wr;
    bad.dbl = true;
    CHECK(plan_click(bad, g).code == "invalid_params");
    bad = wr;
    bad.wheel = 0;
    CHECK(plan_click(bad, g).code == "invalid_params");
    bad.wheel = 101;
    CHECK(plan_click(bad, g).code == "invalid_params");
    bad.wheel = -101;
    CHECK(plan_click(bad, g).code == "invalid_params");

    // parse_button
    Button b;
    CHECK(parse_button("left", b) && b == Button::Left && parse_button("right", b) && b == Button::Right &&
          parse_button("middle", b) && b == Button::Middle && !parse_button("", b) && !parse_button("Left ", b));
}

// FO-5: with the game in front the move is posted again, a frame apart, before the press; in the background the plan is unchanged.
static void test_foreground_settle() {
    CHECK(settle_moves(false) == 0 && settle_moves(true) == kForegroundSettleMoves && kForegroundSettleMoves >= 1);
    Geometry g = geom(1280, 720, 1280, 720);
    ClickRequest r;
    r.point = shot(100, 50);
    ClickPlan back = plan_click(r, g);
    r.settle_moves = settle_moves(true);
    ClickPlan front = plan_click(r, g);
    CHECK(back.ok && front.ok && back.steps.size() == 3 && front.steps.size() == (size_t)(3 + kForegroundSettleMoves));
    for (int i = 0; front.ok && i <= kForegroundSettleMoves; ++i)  // every step before the press is the same move, one per frame
        CHECK(front.steps[i].msgs.size() == 1 && front.steps[i].msgs[0].msg == kWmMouseMove && front.steps[i].msgs[0].lparam == mk_lparam(100, 50));
    if (front.ok) {
        const size_t d = (size_t)kForegroundSettleMoves + 1;
        CHECK(front.steps[d].msgs[0].msg == kWmLButtonDown && front.steps[d + 1].msgs[0].msg == kWmLButtonUp);
    }
    r.has_wheel = true;  // the wheel is a move then one message: also gets the settle moves
    r.wheel = 2;
    ClickPlan wheel = plan_click(r, g);
    CHECK(wheel.ok && wheel.steps.size() == (size_t)(2 + kForegroundSettleMoves) && wheel.steps.back().msgs[0].msg == kWmMouseWheel);
}

static void test_keys() {
    KeyInfo k;
    CHECK(lookup_key("enter", k) && k.vk == 0x0D && k.ch == 13 && !k.extended && k.name == "enter");
    CHECK(lookup_key("RETURN", k) && k.vk == 0x0D && k.name == "enter");
    CHECK(lookup_key("Esc", k) && k.vk == 0x1B && k.ch == 27 && k.name == "escape");
    CHECK(lookup_key("escape", k) && k.vk == 0x1B);
    CHECK(lookup_key("tab", k) && k.vk == 0x09 && k.ch == 9);
    CHECK(lookup_key("backspace", k) && k.vk == 0x08 && k.ch == 8);
    CHECK(lookup_key("space", k) && k.vk == 0x20 && k.ch == ' ');
    CHECK(lookup_key("a", k) && k.vk == 0x41 && k.ch == 'a' && k.name == "a");
    CHECK(lookup_key("Z", k) && k.vk == 0x5A && k.ch == 'z');
    CHECK(lookup_key("5", k) && k.vk == 0x35 && k.ch == '5');
    CHECK(lookup_key("f1", k) && k.vk == 0x70 && k.ch == 0 && !k.extended);
    CHECK(lookup_key("F12", k) && k.vk == 0x7B);
    CHECK(!lookup_key("f13", k) && !lookup_key("f0", k));
    for (const char *n : {"up", "down", "left", "right", "home", "end", "pageup", "pagedown", "insert", "delete"}) {
        bool ok = lookup_key(n, k) && k.extended && k.ch == 0;
        if (!ok) std::printf("key %s\n", n);
        CHECK(ok);
    }
    CHECK(lookup_key("left", k) && k.vk == 0x25 && lookup_key("up", k) && k.vk == 0x26 && lookup_key("right", k) && k.vk == 0x27 &&
          lookup_key("down", k) && k.vk == 0x28);
    CHECK(lookup_key("pgup", k) && k.vk == 0x21 && lookup_key("pgdn", k) && k.vk == 0x22 && lookup_key("del", k) && k.vk == 0x2E);
    CHECK(!lookup_key("", k) && !lookup_key("ctrl", k) && !lookup_key("ab", k) && !lookup_key("enter ", k));

    // A press is key down (+ char), then key up one step later; the scan code comes from the platform.
    ScanFn scan = [](unsigned vk) { return vk == 0x0D ? 0x1Cu : 0x4Bu; };
    lookup_key("enter", k);
    std::vector<Step> s = key_steps(k, scan);
    CHECK(s.size() == 2);
    if (s.size() == 2) {
        CHECK(s[0].msgs.size() == 2 && s[0].msgs[0].msg == kWmKeyDown && s[0].msgs[0].wparam == 0x0D && s[0].msgs[0].lparam == (1 | (0x1C << 16)));
        CHECK(s[0].msgs[1].msg == kWmChar && s[0].msgs[1].wparam == 13);
        CHECK(s[1].msgs.size() == 1 && s[1].msgs[0].msg == kWmKeyUp && s[1].msgs[0].wparam == 0x0D &&
              s[1].msgs[0].lparam == (1 | (0x1C << 16) | (1LL << 30) | (1LL << 31)));
    }
    // An extended key: bit 24, and no WM_CHAR.
    lookup_key("left", k);
    s = key_steps(k, scan);
    CHECK(s.size() == 2 && s[0].msgs.size() == 1 && s[0].msgs[0].msg == kWmKeyDown && (s[0].msgs[0].lparam & (1 << 24)) &&
          ((s[0].msgs[0].lparam >> 16) & 0xFF) == 0x4B && (s[1].msgs[0].lparam & (1 << 24)));
    // A key that types a character (letter, digit, space) is one WM_CHAR and no key message: the game's message loop turns a posted
    // WM_KEYDOWN into its own WM_CHAR (real keyboard layout), so key down + char typed "aa", a space twice and "5(" on AZERTY (live,
    // 0.3.7 verification). Same message as `text` sends for that character.
    for (const char *n : {"a", "z", "5", "0", "space"}) {
        lookup_key(n, k);
        s = key_steps(k, scan);
        bool ok = s.size() == 1 && s[0].msgs.size() == 1 && s[0].msgs[0].msg == kWmChar && s[0].msgs[0].wparam == k.ch;
        if (!ok) std::printf("char key %s: %zu steps\n", n, s.size());
        CHECK(ok);
    }
    lookup_key("a", k);
    s = key_steps(k);
    CHECK(s.size() == 1 && s[0].msgs.size() == 1 && s[0].msgs[0].msg == kWmChar && s[0].msgs[0].wparam == 'a');
    CHECK(text_steps(u"a").size() == 1 && text_steps(u"a")[0].msgs[0].msg == s[0].msgs[0].msg && text_steps(u"a")[0].msgs[0].wparam == s[0].msgs[0].wparam);
    // The control keys keep their key messages (and the WM_CHAR they need); backspace, tab, enter were not doubled live.
    for (const char *n : {"enter", "tab", "backspace", "escape"}) {
        lookup_key(n, k);
        s = key_steps(k, scan);
        bool ok = s.size() == 2 && s[0].msgs.size() == 2 && s[0].msgs[0].msg == kWmKeyDown && s[0].msgs[1].msg == kWmChar &&
                  s[1].msgs.size() == 1 && s[1].msgs[0].msg == kWmKeyUp;
        if (!ok) std::printf("control key %s\n", n);
        CHECK(ok);
    }
    // Non-printing keys without a character: key down, key up.
    for (const char *n : {"f5", "up", "delete", "home"}) {
        lookup_key(n, k);
        s = key_steps(k, scan);
        CHECK(s.size() == 2 && s[0].msgs.size() == 1 && s[0].msgs[0].msg == kWmKeyDown && s[1].msgs[0].msg == kWmKeyUp);
    }
}

static void test_text() {
    std::u16string u;
    std::string err;
    // Accents: one unit each.
    CHECK(text_to_utf16("h\xC3\xA9llo", u, err) && u.size() == 5 && u[1] == 0xE9);
    CHECK(text_to_utf16("\xE2\x82\xAC", u, err) && u.size() == 1 && u[0] == 0x20AC);
    // Outside the BMP: a surrogate pair (U+1F600).
    CHECK(text_to_utf16("\xF0\x9F\x98\x80", u, err) && u.size() == 2 && u[0] == 0xD83D && u[1] == 0xDE00);
    CHECK(text_to_utf16("", u, err) && u.empty());
    CHECK(text_to_utf16("hello world", u, err) && u.size() == 11 && u[5] == ' ');
    // Invalid UTF-8: stray byte, overlong form, encoded surrogate, truncated sequence, beyond U+10FFFF.
    CHECK(!text_to_utf16("\xFF", u, err) && contains(err, "UTF-8"));
    CHECK(!text_to_utf16("\xC0\x80", u, err));
    CHECK(!text_to_utf16("\xED\xA0\x80", u, err));
    CHECK(!text_to_utf16("\xE2\x82", u, err));
    CHECK(!text_to_utf16("\xF4\x90\x80\x80", u, err));
    // Control characters are keys, not text.
    CHECK(!text_to_utf16("a\nb", u, err) && contains(err, "key"));
    CHECK(!text_to_utf16(std::string("a\0b", 3), u, err));
    CHECK(!text_to_utf16("a\tb", u, err));
    CHECK(!text_to_utf16("\x7f", u, err));
    // Length cap (characters, not bytes).
    CHECK(text_to_utf16(std::string(256, 'x'), u, err) && u.size() == 256);
    CHECK(!text_to_utf16(std::string(257, 'x'), u, err) && contains(err, "256"));
    std::string accents;
    for (int i = 0; i < 256; ++i) accents += "\xC3\xA9";  // 512 bytes, 256 characters
    CHECK(text_to_utf16(accents, u, err) && u.size() == 256);

    // 20 units: steps of 8, one WM_CHAR each with the unit as wParam.
    std::u16string t = u"abcdefghijklmnopqrst";
    std::vector<Step> s = text_steps(t);
    CHECK(s.size() == 3 && s[0].msgs.size() == 8 && s[1].msgs.size() == 8 && s[2].msgs.size() == 4);
    if (s.size() == 3) {
        CHECK(s[0].msgs[0].msg == kWmChar && s[0].msgs[0].wparam == 'a' && s[0].msgs[0].lparam == 1);
        CHECK(s[2].msgs[3].wparam == 't');
    }
    CHECK(text_steps(u"").empty());
}

// Live verification of 0.3.7: the engine narrows every WM_CHAR through the system ANSI code page with best-fit
// (CP1252: e-acute stays, l-stroke becomes l, alpha a, Cyrillic and CJK ?). The characters that would change are found per code
// point with an injected "does it survive" function (the Win32 one is plat::ui_ansi_keeps).
static void test_ansi_check() {
    std::u16string u;
    std::string err;
    KeepsFn latin1 = [](unsigned cp) { return cp < 0x100; };
    CHECK(text_to_utf16("h\xC3\xA9llo w\xC3\xB6rld", u, err) && altered_chars(u, latin1).empty());
    // Greek alpha and l-stroke would be altered; each listed once, in order of appearance.
    CHECK(text_to_utf16("\xC5\x82 and \xCE\xB1 and \xC5\x82", u, err));
    CHECK(altered_chars(u, latin1) == (std::vector<unsigned>{0x142, 0x3B1}));
    // A surrogate pair is one character (U+1F600), not two lone surrogates.
    CHECK(text_to_utf16("ok \xF0\x9F\x98\x80", u, err) && altered_chars(u, latin1) == (std::vector<unsigned>{0x1F600}));
    // ASCII is never asked, whatever the function says (a code page always has it).
    int asked = 0;
    KeepsFn none = [&](unsigned) { ++asked; return false; };
    CHECK(text_to_utf16("hello, world 123 !?", u, err) && altered_chars(u, none).empty() && asked == 0);
    CHECK(altered_chars(std::u16string(), none).empty());
    // The refusal names the characters (glyph and code point), the code page, the way out and the override.
    Refusal r = not_representable({0x142, 0x3B1}, 1252);
    CHECK(r.code == "text_not_representable");
    for (const char *w : {"\xC5\x82 (U+0142)", "\xCE\xB1 (U+03B1)", "1252", "allow_lossy", "lua_exec", "SetText", "SetValue"})
        if (!contains(r.message, w)) {
            std::printf("missing \"%s\" in: %s\n", w, r.message.c_str());
            CHECK(false);
        }
    // A long list is cut, with the count left out said.
    std::vector<unsigned> many;
    for (unsigned c = 0x400; c < 0x400 + 40; ++c) many.push_back(c);
    r = not_representable(many, 1252);
    CHECK(contains(r.message, "24 more") && !contains(r.message, "U+0420"));
    CHECK(utf8_of(0x41) == "A" && utf8_of(0xE9) == "\xC3\xA9" && utf8_of(0x65E5) == "\xE6\x97\xA5" && utf8_of(0x1F600) == "\xF0\x9F\x98\x80");

#ifdef _WIN32
    // The Win32 side: ASCII always passes; on a CP1252 machine the examples above hold (the test is skipped on other code pages).
    CHECK(plat::ui_ansi_code_page() != 0 && plat::ui_ansi_keeps('a') && plat::ui_ansi_keeps(' '));
    if (plat::ui_ansi_code_page() == 1252) {
        CHECK(plat::ui_ansi_keeps(0xE9) && plat::ui_ansi_keeps(0x20AC));  // e-acute, euro
        CHECK(!plat::ui_ansi_keeps(0x142) && !plat::ui_ansi_keeps(0x3B1) && !plat::ui_ansi_keeps(0x42F) && !plat::ui_ansi_keeps(0x65E5) &&
              !plat::ui_ansi_keeps(0x1F600));
    }
#else
    CHECK(plat::ui_ansi_code_page() == 0 && plat::ui_ansi_keeps(0x65E5));  // no check off Windows: ui_key is unsupported there anyway
#endif
}

static void test_describe() {
    Geometry g = geom(1280, 720, 1280, 720);
    ClickRequest r;
    r.point = shot(1, 1);
    std::vector<std::string> d = describe(plan_click(r, g).steps);
    CHECK(d == (std::vector<std::string>{"WM_MOUSEMOVE", "WM_LBUTTONDOWN", "WM_LBUTTONUP"}));
    r.dbl = true;
    d = describe(plan_click(r, g).steps);
    CHECK(d == (std::vector<std::string>{"WM_MOUSEMOVE", "WM_LBUTTONDOWN", "WM_LBUTTONUP", "WM_LBUTTONDBLCLK", "WM_LBUTTONUP"}));
    ClickRequest w;
    w.point = shot(1, 1);
    w.has_wheel = true;
    w.wheel = 3;
    d = describe(plan_click(w, g).steps);
    CHECK(d == (std::vector<std::string>{"WM_MOUSEMOVE", "WM_MOUSEWHEEL +3"}));
    w.wheel = -2;
    d = describe(plan_click(w, g).steps);
    CHECK(d == (std::vector<std::string>{"WM_MOUSEMOVE", "WM_MOUSEWHEEL -2"}));
    KeyInfo k;
    lookup_key("enter", k);
    d = describe(key_steps(k));
    CHECK(d == (std::vector<std::string>{"WM_KEYDOWN 0x0D", "WM_CHAR U+000D", "WM_KEYUP 0x0D"}));
    d = describe(text_steps(u"hello"));
    CHECK(d == (std::vector<std::string>{"WM_CHAR x5"}));
    d = describe(text_steps(u"abcdefghij"));  // across two steps: still one run
    CHECK(d == (std::vector<std::string>{"WM_CHAR x10"}));
    d = describe(text_steps(u"h"));
    CHECK(d == (std::vector<std::string>{"WM_CHAR U+0068"}));
}

static Facts good() {
    Facts f;
    f.test_client = true;
    f.cursor_visible = true;
    return f;
}

static void test_decide() {
    CHECK(decide(good()).code.empty());

    Facts f = good();
    f.client_mode = false;
    Refusal r = decide(f);
    CHECK(r.code == "unsupported" && contains(r.message, "srcds"));
    f = good();
    f.windows = false;
    CHECK(decide(f).code == "unsupported" && contains(decide(f).message, "Windows"));

    // The user's own game: refused unless asked for.
    f = good();
    f.test_client = false;
    r = decide(f);
    CHECK(r.code == "not_a_test_client" && contains(r.message, "instances_launch") && contains(r.message, "allow_user_game"));
    f.allow_user_game = true;
    CHECK(decide(f).code.empty());

    // The network lock applies when a client realm exists; at the main menu (no client realm) there is nothing to lock.
    f = good();
    f.client_realm_present = true;
    f.client_allowed = false;
    f.lock_reason = "connected to a server that is not on this machine (1.2.3.4:27015)";
    r = decide(f);
    CHECK(r.code == "client_locked" && contains(r.message, "1.2.3.4:27015"));
    f.client_realm_present = false;
    CHECK(decide(f).code.empty());
    f.client_realm_present = true;
    f.client_allowed = true;
    CHECK(decide(f).code.empty());

    f = good();
    f.loading = true;
    r = decide(f);
    CHECK(r.code == "loading" && contains(r.message, "map_loaded"));

    f = good();
    f.window_found = false;
    r = decide(f);
    CHECK(r.code == "window_unavailable" && contains(r.message, "not found"));
    f = good();
    f.window_minimized = true;
    r = decide(f);
    CHECK(r.code == "window_unavailable" && contains(r.message, "minimized"));

    f = good();
    f.cursor_visible = false;
    r = decide(f);
    CHECK(r.code == "cursor_hidden" && contains(r.message, "cursor"));

    // Order: unsupported, not_a_test_client, client_locked, loading, window_unavailable, cursor_hidden.
    Facts all;  // everything wrong at once
    all.client_mode = false;
    all.client_realm_present = true;
    all.client_allowed = false;
    all.loading = true;
    all.window_found = false;
    CHECK(decide(all).code == "unsupported");
    all.client_mode = true;
    CHECK(decide(all).code == "not_a_test_client");
    all.test_client = true;
    CHECK(decide(all).code == "client_locked");
    all.client_allowed = true;
    CHECK(decide(all).code == "loading");
    all.loading = false;
    CHECK(decide(all).code == "window_unavailable");
    all.window_found = true;
    CHECK(decide(all).code == "cursor_hidden");
    all.cursor_visible = true;
    CHECK(decide(all).code.empty());
}

// The Win32 side, as far as it can be checked without the game: input is never posted to a window of another process.
static void test_platform() {
    std::string err;
#ifdef _WIN32
    CHECK(plat::ui_supported());
    CHECK(!plat::ui_post(0x1, uiinput::kWmMouseMove, 0, 0, err) && !err.empty());  // not a window
    err.clear();
    CHECK(!plat::ui_post((uint64_t)(uintptr_t)GetDesktopWindow(), uiinput::kWmMouseMove, 0, 0, err) && err.find("gone") != std::string::npos);  // another process's
    plat::UiWindow w;
    plat::ui_window(w);  // a test process has no visible window of its own: nothing found, nothing invented
    CHECK(!w.found && w.client_w == 0 && w.hwnd == 0);
    CHECK(plat::ui_scan_code(0x0D) == 0x1C);  // VK_RETURN
#else
    CHECK(!plat::ui_supported());
    CHECK(!plat::ui_post(1, uiinput::kWmMouseMove, 0, 0, err) && !err.empty());
    plat::UiWindow w;
    CHECK(!plat::ui_window(w) && !w.found);
#endif
}

void test_uiinput() {
    test_platform();
    test_coordinates();
    test_mouse_steps();
    test_foreground_settle();
    test_keys();
    test_text();
    test_ansi_check();
    test_describe();
    test_decide();
}
