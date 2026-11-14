#include "cmdline.h"

#include "util.h"

namespace cmdline {

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split_statements(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    auto flush = [&] {
        std::string t = trim(cur);
        if (!t.empty()) out.push_back(t);
        cur.clear();
    };
    for (char c : line) {
        if (c == '"') inq = !inq;
        if (!inq && (c == ';' || c == '\n' || c == '\r')) {
            flush();
            continue;
        }
        if (inq && (c == '\n' || c == '\r')) {  // the engine ends a quoted string at a line break too
            inq = false;
            flush();
            continue;
        }
        cur += c;
    }
    flush();
    return out;
}

std::string command_name(const std::string &statement) {
    auto parts = util::split_args(statement);
    return parts.empty() ? "" : util::to_lower(parts[0]);
}

std::vector<std::string> unknown_commands(const std::vector<std::string> &lines) {
    // GMod prints `Unknown command: name` (seen in every realm); Source 2013 prints `Unknown command "name"`.
    static const std::string colon = "Unknown command: ", quoted = "Unknown command \"";
    std::vector<std::string> out;
    for (const auto &l : lines) {
        std::string name;
        if (l.compare(0, colon.size(), colon) == 0) {
            name = l.substr(colon.size());
            name = name.substr(0, name.find_first_of(" \t"));
        } else if (l.compare(0, quoted.size(), quoted) == 0) {
            size_t end = l.find('"', quoted.size());
            if (end != std::string::npos) name = l.substr(quoted.size(), end - quoted.size());
        }
        if (!name.empty()) out.push_back(util::to_lower(name));
    }
    return out;
}

bool own_log_line(const std::string &line) { return line.compare(0, 11, "[mcpagent] ") == 0; }

}  // namespace cmdline
