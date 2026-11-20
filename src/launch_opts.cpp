#include "launch_opts.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "cmdline.h"
#include "util.h"

namespace launchopts {

using nlohmann::json;

std::string fps_error(const json &v, const std::string &name) {
    bool integer = v.is_number_integer() || (v.is_number_float() && std::floor(v.get<double>()) == v.get<double>());
    if (!integer) return name + ": expected an integer (0 = no cap, or 10 to 1000), got " + v.dump(-1, ' ', false, json::error_handler_t::replace);
    double d = v.get<double>();
    if (d != 0 && (d < 10 || d > 1000)) return name + ": " + v.dump() + " is out of range: use 0 (no cap) or a value from 10 to 1000";
    return "";
}

static bool plain_name(const std::string &n) {
    if (n.empty() || n.size() > 64) return false;
    for (unsigned char c : n)
        if (!(std::isalnum(c) || c == '_' || c == '.')) return false;
    return true;
}

namespace {

enum class Why { Quit, Connect, Map };

// Console commands that must not run from a launch option: they end the client, connect it to a server or load a map.
const char *why_text(Why w) {
    switch (w) {
        case Why::Quit: return "it would end the client: close clients with instances_close or game_quit";
        case Why::Connect: return "gmod-mcp never connects a client to a server";
        case Why::Map: return "load the map with the map option (it goes through map_load, one load at a time)";
    }
    return "";
}

bool refused_command(const std::string &name, Why &why) {
    static const struct { const char *name; Why why; } kTable[] = {
        {"quit", Why::Quit}, {"exit", Why::Quit},
        {"connect", Why::Connect}, {"retry", Why::Connect}, {"redirect", Why::Connect}, {"connect_lobby", Why::Connect},
        {"map", Why::Map}, {"changelevel", Why::Map}, {"changelevel2", Why::Map}, {"load", Why::Map},
        {"host_workshop_map", Why::Map}, {"map_background", Why::Map},
    };
    for (const auto &e : kTable)
        if (name == e.name) {
            why = e.why;
            return true;
        }
    return false;
}

// Options (without their - or + prefix) gmod-mcp controls itself, with what to say.
const char *refused_option(const std::string &name) {
    if (name.compare(0, 3, "mcp") == 0) return "gmod-mcp sets its own options itself (-mcp*)";
    static const struct { const char *name; const char *why; } kTable[] = {
        {"multirun", "gmod-mcp sets it itself"},
        {"w", "gmod-mcp sets the window size itself (640x360: a narrower client crashes)"},
        {"h", "gmod-mcp sets the window size itself (640x360: a narrower client crashes)"},
        {"width", "gmod-mcp sets the window size itself (640x360: a narrower client crashes)"},
        {"height", "gmod-mcp sets the window size itself (640x360: a narrower client crashes)"},
        {"windowed", "gmod-mcp starts its clients in a small window"},
        {"window", "gmod-mcp starts its clients in a small window"},
        {"sw", "gmod-mcp starts its clients in a small window"},
        {"startwindowed", "gmod-mcp starts its clients in a small window"},
        {"fullscreen", "gmod-mcp starts its clients in a small window"},
        {"full", "gmod-mcp starts its clients in a small window"},
        {"dxlevel", "gmod-mcp sets -dxlevel 95 so the engine leaves the user's video settings alone"},
        {"condebug", "every client would write the same console.log, shared with the user's own game"},
        {"game", "gmod-mcp starts the game itself"},
    };
    for (const auto &e : kTable)
        if (name == e.name) return e.why;
    return nullptr;
}

// A command-line token the engine reads as one argument and no shell sees: printable ASCII without quote or ';'.
bool plain_token(const std::string &t) {
    if (t.empty() || t.size() > kMaxItemLength) return false;
    for (unsigned char c : t)
        if (c < 0x21 || c > 0x7E || c == '"' || c == ';') return false;
    return true;
}

std::string dquote(const std::string &s) { return "\"" + s + "\""; }

}  // namespace

std::string check_commands(const json &list, std::vector<std::string> &statements) {
    statements.clear();
    if (!list.is_array()) return "commands: expected an array of console commands, e.g. [\"fps_max 300\"]";
    if (list.size() > kMaxItems) return "commands: at most " + std::to_string(kMaxItems) + " commands";
    std::vector<std::string> out;
    for (const auto &c : list) {
        if (!c.is_string()) return "commands: every element must be a string";
        std::string line = c.get<std::string>();
        if (line.size() > 256) return "commands: a command is longer than 256 characters";
        for (unsigned char ch : line)
            if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') return "commands: control characters are not allowed";
        auto sts = cmdline::split_statements(line);
        if (sts.empty()) return "commands: empty command";
        for (const auto &st : sts) {
            std::string name = cmdline::command_name(st);
            Why why;
            if (refused_command(name, why)) return "commands: " + dquote(name) + " is refused: " + why_text(why);
            out.push_back(st);
        }
    }
    statements = std::move(out);
    return "";
}

std::string check_launch_args(const json &list, std::vector<std::string> &tokens) {
    tokens.clear();
    if (!list.is_array()) return "launch_args: expected an array of command-line tokens, e.g. [\"-noborder\"]";
    if (list.size() > kMaxItems) return "launch_args: at most " + std::to_string(kMaxItems) + " elements";
    std::vector<std::string> out;
    for (const auto &a : list) {
        if (!a.is_string()) return "launch_args: every element must be a string";
        std::string t = a.get<std::string>();
        if (!plain_token(t))
            return "launch_args: " + dquote(t.size() > 40 ? t.substr(0, 40) + "..." : t) +
                   " is not accepted: one token per element, printable ASCII without spaces, quotes or ';' (a command and its value are two elements: [\"+cl_showfps\", \"1\"])";
        if (t[0] == '-' || t[0] == '+') {
            std::string name = util::to_lower(t.substr(1));
            Why why;
            if (refused_command(name, why)) return "launch_args: " + dquote(t) + " is refused: " + why_text(why);
            if (const char *r = refused_option(name)) return "launch_args: " + dquote(t) + " is refused: " + r;
        }
        out.push_back(t);
    }
    tokens = std::move(out);
    return "";
}

std::vector<std::string> restore_names(const std::vector<std::string> &statements, const std::vector<std::string> &tokens) {
    std::vector<std::string> out;
    auto add = [&](std::string n) {
        n = util::to_lower(n);
        if (plain_name(n) && out.size() < kMaxRestoreNames && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    };
    for (const auto &st : statements) {
        auto parts = util::split_args(st);
        if (parts.empty()) continue;
        std::string first = util::to_lower(parts[0]);
        bool by_arg = first == "toggle" || first == "incrementvar" || first == "decrementvar" || first == "multvar";
        if (by_arg && parts.size() > 1) add(parts[1]);
        else if (!by_arg) add(parts[0]);
    }
    for (const auto &t : tokens)
        if (t.size() > 1 && t[0] == '+') add(t.substr(1));
    return out;
}

std::string join_list(const std::vector<std::string> &names) {
    std::string s;
    for (size_t i = 0; i < names.size(); ++i) s += (i ? "," : "") + names[i];
    return s;
}

std::vector<std::string> parse_restore_list(const std::string &csv) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= csv.size()) {
        size_t end = csv.find(',', pos);
        if (end == std::string::npos) end = csv.size();
        std::string n = util::to_lower(csv.substr(pos, end - pos));
        if (plain_name(n) && out.size() < kMaxRestoreNames && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
        pos = end + 1;
    }
    return out;
}

std::string encode_owner(const std::string &owner) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : owner) {
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string decode_owner(const std::string &token) {
    auto hexv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < token.size(); ++i) {
        unsigned char c = (unsigned char)token[i];
        if (c == '%') {
            if (i + 2 >= token.size()) return "";  // truncated escape
            int h = hexv(token[i + 1]), l = hexv(token[i + 2]);
            if (h < 0 || l < 0) return "";
            c = (unsigned char)(h * 16 + l);
            i += 2;
            if (c < 0x20 || c == 0x7F) return "";
        } else if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return "";
        }
        if ((c & 0xC0) != 0x80) ++chars;  // not a UTF-8 continuation byte: one more character
        out += (char)c;
    }
    return chars > kMaxOwnerChars ? std::string() : out;
}

unsigned long long parse_return_focus(const std::string &token) {
    if (token.empty() || token.size() > 10 || token[0] == '0') return 0;
    unsigned long long v = 0;
    for (unsigned char c : token) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    return v > kMaxWindowHandle ? 0 : v;
}

std::string format_return_focus(unsigned long long hwnd) {
    if (hwnd == 0 || hwnd > kMaxWindowHandle) return "";
    return std::to_string(hwnd);
}

unsigned long long pick_return_target(unsigned long long foreground, bool foreground_is_ours, unsigned long long remembered) {
    if (foreground == 0 || foreground_is_ours) return remembered;
    return foreground;
}

FocusAction decide_return_focus(const FocusFacts &f, int returns, double elapsed_s) {
    if (!f.test_client || f.target == 0) return FocusAction::Stop;
    if (returns >= kMaxFocusReturns || elapsed_s > kFocusWatchSeconds) return FocusAction::Stop;
    if (!f.target_exists) return FocusAction::Stop;
    if (!f.target_usable) return FocusAction::Wait;  // minimized or hidden for now: the user may bring it back
    if (!f.game_window_found || !f.game_is_foreground) return FocusAction::Wait;
    return FocusAction::Return;
}

}  // namespace launchopts
