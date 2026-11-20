#include "uiinput.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace uiinput {

namespace {

std::string size_text(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

// MAKELPARAM: the low word is x, the high word y, each two's complement (a position on a monitor left of or above the
// primary one is negative).
long long pack(int x, int y) { return ((long long)(unsigned short)y << 16) | (unsigned short)x; }

Msg make(unsigned msg, unsigned long long wparam, long long lparam) {
    Msg m;
    m.msg = msg;
    m.wparam = wparam;
    m.lparam = lparam;
    return m;
}

Step one(Msg m) {
    Step s;
    s.msgs.push_back(m);
    return s;
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string hex(unsigned v, int width, const char *prefix) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%0*X", prefix, width, v);
    return buf;
}

}  // namespace

Converted to_client(const PointRequest &req, const Geometry &g) {
    Converted c;
    if (!std::isfinite(req.x) || !std::isfinite(req.y)) {
        c.code = "invalid_params";
        c.message = "x and y must be finite numbers (pixels)";
        return c;
    }
    if (g.client_w <= 0 || g.client_h <= 0) {
        c.code = "window_unavailable";
        c.message = "the game window has no client area (minimized?)";
        return c;
    }
    // The space the point is given in: the screenshot image (its size is the backbuffer's unless the caller says another),
    // or the client area itself.
    int sw, sh;
    const char *what;
    if (req.window_space) {
        sw = g.client_w;
        sh = g.client_h;
        what = "the window's client area";
    } else if (req.has_shot_size) {
        if (req.shot_w <= 0 || req.shot_h <= 0) {
            c.code = "invalid_params";
            c.message = "screenshot_size must be the width and height in pixels of the image you read the point on";
            return c;
        }
        sw = req.shot_w;
        sh = req.shot_h;
        what = "the screenshot (screenshot_size)";
    } else {
        sw = g.backbuffer_w > 0 ? g.backbuffer_w : g.client_w;
        sh = g.backbuffer_h > 0 ? g.backbuffer_h : g.client_h;
        what = "the screenshot";
    }
    double fx = std::floor(req.x), fy = std::floor(req.y);
    if (fx < 0 || fy < 0 || fx >= sw || fy >= sh) {
        c.code = "out_of_bounds";
        c.message = "the point (" + std::to_string((long long)req.x) + ", " + std::to_string((long long)req.y) + ") is outside " + what + " (" +
                    size_text(sw, sh) + " pixels, origin at the top-left)";
        return c;
    }
    int px = (int)fx, py = (int)fy;
    c.scale_x = (double)g.client_w / sw;
    c.scale_y = (double)g.client_h / sh;
    if (req.window_space) {
        c.client = Point{px, py};
    } else {
        // The centre of the source pixel, scaled to the client area.
        int cx = (int)std::floor((px + 0.5) * g.client_w / sw);
        int cy = (int)std::floor((py + 0.5) * g.client_h / sh);
        if (cx > g.client_w - 1) cx = g.client_w - 1;
        if (cy > g.client_h - 1) cy = g.client_h - 1;
        c.client = Point{cx, cy};
    }
    c.ok = true;
    return c;
}

bool parse_button(const std::string &name, Button &out) {
    if (name == "left") out = Button::Left;
    else if (name == "right") out = Button::Right;
    else if (name == "middle") out = Button::Middle;
    else return false;
    return true;
}

ClickPlan plan_click(const ClickRequest &req, const Geometry &g) {
    ClickPlan p;
    auto bad = [&](const std::string &msg) {
        p.code = "invalid_params";
        p.message = msg;
        return p;
    };
    Button button = Button::Left;
    if (req.has_wheel) {
        if (req.has_button || req.dbl) return bad("wheel scrolls without clicking: leave button and double out when you pass wheel");
        if (req.wheel == 0 || req.wheel > 100 || req.wheel < -100)
            return bad("wheel is a number of notches, from -100 to -1 (down) or 1 to 100 (up)");
    } else if (!parse_button(req.button, button)) {
        return bad("unknown button \"" + req.button + "\" (left, right or middle)");
    }
    p.at = to_client(req.point, g);
    if (!p.at.ok) {
        p.code = p.at.code;
        p.message = p.at.message;
        return p;
    }
    const Point at = p.at.client;
    p.steps.push_back(one(make(kWmMouseMove, 0, pack(at.x, at.y))));
    if (req.has_wheel) {
        unsigned long long delta = (unsigned long long)(unsigned short)(short)(req.wheel * kWheelDelta);
        p.steps.push_back(one(make(kWmMouseWheel, delta << 16, pack(g.origin_x + at.x, g.origin_y + at.y))));
        p.ok = true;
        return p;
    }
    unsigned down, up, dbl, flag;
    switch (button) {
        case Button::Right: down = kWmRButtonDown, up = kWmRButtonUp, dbl = kWmRButtonDblClk, flag = kMkRButton; break;
        case Button::Middle: down = kWmMButtonDown, up = kWmMButtonUp, dbl = kWmMButtonDblClk, flag = kMkMButton; break;
        default: down = kWmLButtonDown, up = kWmLButtonUp, dbl = kWmLButtonDblClk, flag = kMkLButton; break;
    }
    long long lp = pack(at.x, at.y);
    p.steps.push_back(one(make(down, flag, lp)));
    p.steps.push_back(one(make(up, 0, lp)));
    if (req.dbl) {  // what Windows sends for a double click: down, up, double-click (a second down), up
        p.steps.push_back(one(make(dbl, flag, lp)));
        p.steps.push_back(one(make(up, 0, lp)));
    }
    p.ok = true;
    return p;
}

bool lookup_key(const std::string &name_in, KeyInfo &out) {
    std::string n = lower(name_in);
    KeyInfo k;
    k.name = n;
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') {
        k.vk = 0x41 + (n[0] - 'a');
        k.ch = (unsigned)n[0];
    } else if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') {
        k.vk = 0x30 + (n[0] - '0');
        k.ch = (unsigned)n[0];
    } else if (n.size() >= 2 && n[0] == 'f' && n.find_first_not_of("0123456789", 1) == std::string::npos && n.size() <= 3) {
        int f = std::atoi(n.c_str() + 1);
        if (f < 1 || f > 12 || (n.size() == 3 && n[1] == '0')) return false;
        k.vk = 0x70 + (f - 1);
    } else {
        struct Named {
            const char *name, *alias;
            unsigned vk;
            bool ext;
            unsigned ch;
        };
        static const Named table[] = {
            {"enter", "return", 0x0D, false, 13},   {"escape", "esc", 0x1B, false, 27},   {"tab", nullptr, 0x09, false, 9},
            {"backspace", nullptr, 0x08, false, 8}, {"space", nullptr, 0x20, false, 32},   {"up", nullptr, 0x26, true, 0},
            {"down", nullptr, 0x28, true, 0},       {"left", nullptr, 0x25, true, 0},      {"right", nullptr, 0x27, true, 0},
            {"home", nullptr, 0x24, true, 0},       {"end", nullptr, 0x23, true, 0},       {"pageup", "pgup", 0x21, true, 0},
            {"pagedown", "pgdn", 0x22, true, 0},    {"insert", "ins", 0x2D, true, 0},      {"delete", "del", 0x2E, true, 0},
        };
        bool found = false;
        for (const Named &e : table)
            if (n == e.name || (e.alias && n == e.alias)) {
                k.name = e.name;
                k.vk = e.vk;
                k.extended = e.ext;
                k.ch = e.ch;
                found = true;
                break;
            }
        if (!found) return false;
    }
    out = k;
    return true;
}

std::vector<Step> key_steps(const KeyInfo &k, const ScanFn &scan) {
    // A key that types a character (letter, digit, space): only the WM_CHAR, like `text`. The game's message loop runs
    // TranslateMessage on a posted WM_KEYDOWN and makes its own WM_CHAR from the real keyboard layout, next to ours: the character
    // came twice ("aa"), and on AZERTY the digit key typed "5(" (live, 0.3.7). The control keys (enter, tab, backspace, escape) are
    // not doubled: the engine reads them from the key messages.
    if (k.ch >= 0x20) return {one(make(kWmChar, k.ch, 1))};
    long long scancode = scan ? (long long)(scan(k.vk) & 0xFF) : 0;
    long long base = 1 | (scancode << 16) | (k.extended ? (1LL << 24) : 0);
    std::vector<Step> out;
    Step down;
    down.msgs.push_back(make(kWmKeyDown, k.vk, base));
    if (k.ch) down.msgs.push_back(make(kWmChar, k.ch, base));
    out.push_back(down);
    out.push_back(one(make(kWmKeyUp, k.vk, base | (1LL << 30) | (1LL << 31))));  // previous state down, transition up
    return out;
}

bool text_to_utf16(const std::string &s, std::u16string &out, std::string &err) {
    out.clear();
    size_t chars = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char b = (unsigned char)s[i];
        unsigned cp;
        size_t len;
        if (b < 0x80) cp = b, len = 1;
        else if ((b & 0xE0) == 0xC0) cp = b & 0x1F, len = 2;
        else if ((b & 0xF0) == 0xE0) cp = b & 0x0F, len = 3;
        else if ((b & 0xF8) == 0xF0) cp = b & 0x07, len = 4;
        else {
            err = "text is not valid UTF-8";
            return false;
        }
        if (i + len > s.size()) {
            err = "text is not valid UTF-8 (truncated sequence)";
            return false;
        }
        for (size_t j = 1; j < len; ++j) {
            unsigned char c = (unsigned char)s[i + j];
            if ((c & 0xC0) != 0x80) {
                err = "text is not valid UTF-8";
                return false;
            }
            cp = (cp << 6) | (c & 0x3F);
        }
        static const unsigned min_for_len[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < min_for_len[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            err = "text is not valid UTF-8 (overlong form, surrogate or beyond U+10FFFF)";
            return false;
        }
        if (cp < 0x20 || cp == 0x7F) {
            err = "text contains the control character " + hex(cp, 4, "U+") + ": use `key` for enter, tab, backspace and escape";
            return false;
        }
        if (++chars > kMaxTextChars) {
            err = "text is longer than " + std::to_string(kMaxTextChars) + " characters: send it in several calls";
            return false;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back((char16_t)(0xD800 + (cp >> 10)));
            out.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back((char16_t)cp);
        }
        i += len;
    }
    return true;
}

std::string utf8_of(unsigned cp) {
    std::string s;
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

std::vector<unsigned> altered_chars(const std::u16string &text, const KeepsFn &keeps) {
    std::vector<unsigned> out;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned cp = text[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) continue;
        if (std::find(out.begin(), out.end(), cp) != out.end()) continue;
        if (!keeps || !keeps(cp)) out.push_back(cp);
    }
    return out;
}

Refusal not_representable(const std::vector<unsigned> &cps, unsigned code_page) {
    constexpr size_t kListed = 16;
    std::string list;
    for (size_t i = 0; i < cps.size() && i < kListed; ++i) list += (i ? ", " : "") + utf8_of(cps[i]) + " (" + hex(cps[i], 4, "U+") + ")";
    if (cps.size() > kListed) list += " and " + std::to_string(cps.size() - kListed) + " more";
    std::string msg =
        "the game cannot receive these characters unchanged: " + list + ". The engine narrows every typed character through this machine's "
        "ANSI code page (" + std::to_string(code_page) + ") with best-fit substitutions, so each would arrive as another character or as '?' "
        "(WM_UNICHAR, IME characters and VK_PACKET are ignored, there is no other way in). Set the text from Lua instead: lua_exec in the "
        "realm that owns the panel (menu for the main menu's dialogs, client for Derma in game), e.g. entry:SetText(\"...\") on a DTextEntry "
        "(DTextEntry:SetValue does nothing while the entry has the keyboard focus, which a ui_click gives it; call entry:OnValueChange(text) "
        "afterwards if the page listens to it). Or pass allow_lossy: true to type the text anyway, altered.";
    return Refusal{"text_not_representable", msg};
}

std::vector<Step> text_steps(const std::u16string &text) {
    constexpr size_t kPerStep = 8;
    std::vector<Step> out;
    for (size_t i = 0; i < text.size(); i += kPerStep) {
        Step s;
        for (size_t j = i; j < text.size() && j < i + kPerStep; ++j) s.msgs.push_back(make(kWmChar, (unsigned)text[j], 1));
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<std::string> describe(const std::vector<Step> &steps) {
    std::vector<std::string> out;
    size_t run = 0;
    unsigned run_first = 0;
    auto flush = [&] {
        if (!run) return;
        out.push_back(run == 1 ? "WM_CHAR " + hex(run_first, 4, "U+") : "WM_CHAR x" + std::to_string(run));
        run = 0;
    };
    for (const Step &s : steps)
        for (const Msg &m : s.msgs) {
            if (m.msg == kWmChar) {
                if (!run) run_first = (unsigned)m.wparam;
                ++run;
                continue;
            }
            flush();
            switch (m.msg) {
                case kWmMouseMove: out.push_back("WM_MOUSEMOVE"); break;
                case kWmLButtonDown: out.push_back("WM_LBUTTONDOWN"); break;
                case kWmLButtonUp: out.push_back("WM_LBUTTONUP"); break;
                case kWmLButtonDblClk: out.push_back("WM_LBUTTONDBLCLK"); break;
                case kWmRButtonDown: out.push_back("WM_RBUTTONDOWN"); break;
                case kWmRButtonUp: out.push_back("WM_RBUTTONUP"); break;
                case kWmRButtonDblClk: out.push_back("WM_RBUTTONDBLCLK"); break;
                case kWmMButtonDown: out.push_back("WM_MBUTTONDOWN"); break;
                case kWmMButtonUp: out.push_back("WM_MBUTTONUP"); break;
                case kWmMButtonDblClk: out.push_back("WM_MBUTTONDBLCLK"); break;
                case kWmMouseWheel: {
                    int notches = (int)(short)(unsigned short)(m.wparam >> 16) / kWheelDelta;
                    out.push_back(std::string("WM_MOUSEWHEEL ") + (notches > 0 ? "+" : "") + std::to_string(notches));
                    break;
                }
                case kWmKeyDown: out.push_back("WM_KEYDOWN " + hex((unsigned)m.wparam, 2, "0x")); break;
                case kWmKeyUp: out.push_back("WM_KEYUP " + hex((unsigned)m.wparam, 2, "0x")); break;
                default: out.push_back("WM_" + hex(m.msg, 4, "0x")); break;
            }
        }
    flush();
    return out;
}

Refusal decide(const Facts &f) {
    auto no = [](const char *code, const std::string &msg) { return Refusal{code, msg}; };
    if (!f.client_mode)
        return no("unsupported", "ui_click and ui_key drive a game client's window: this is a dedicated server (srcds), it has none");
    if (!f.windows) return no("unsupported", "ui_click and ui_key post window messages and only work on Windows");
    if (!f.test_client && !f.allow_user_game)
        return no("not_a_test_client",
                  "this game was not started by instances_launch (no -mcp_test_client): input is not posted into a game someone may be "
                  "playing. Start a test client with instances_launch and call this on it (pass its `instance`), or pass "
                  "allow_user_game: true to accept that the input goes to this game");
    if (f.client_realm_present && !f.client_allowed)
        return no("client_locked", f.lock_reason.empty() ? "the client realm is locked (connected to a server that is not on this machine)" : f.lock_reason);
    if (f.loading)
        return no("loading", "a map is loading (the screen is changing): wait for the map_loaded event (events_wait), then retry");
    if (!f.window_found)
        return no("window_unavailable", "the game's main window was not found (no visible top-level window of this process)");
    if (f.window_minimized)
        return no("window_unavailable", "the game window is minimized: restore it (the module never changes the window state), then retry");
    if (!f.cursor_visible)
        return no("cursor_hidden",
                  "the mouse cursor is not visible: the game has the mouse (no menu, dialog or popup is open), so a click would "
                  "be game input. Open a menu first, e.g. console_exec {cmd: \"gameui_activate\"} for the game menu");
    return Refusal{};
}

}  // namespace uiinput
