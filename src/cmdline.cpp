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
    static const std::string prefix = "Unknown command \"";
    std::vector<std::string> out;
    for (const auto &l : lines) {
        if (l.compare(0, prefix.size(), prefix) != 0) continue;
        size_t end = l.find('"', prefix.size());
        if (end == std::string::npos) continue;
        out.push_back(util::to_lower(l.substr(prefix.size(), end - prefix.size())));
    }
    return out;
}

bool own_log_line(const std::string &line) { return line.compare(0, 11, "[mcpagent] ") == 0; }

}  // namespace cmdline
