#include "early_lua.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <random>

namespace earlylua {

namespace fs = std::filesystem;

bool valid_token(const std::string &t) {
    if (t.empty() || t.size() > 40) return false;
    for (unsigned char c : t)
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    return true;
}

std::string new_token() {
    std::random_device rd;
    std::mt19937_64 g((uint64_t(rd()) << 32) ^ rd() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
    static const char *hex = "0123456789abcdef";
    std::string t = "c";
    for (int i = 0; i < 11; ++i) t += hex[g() & 15];
    return t;
}

std::string check_early_lua(const std::string &lua) {
    if (lua.size() > kMaxEarlyLua) return "early_lua: at most " + std::to_string(kMaxEarlyLua) + " bytes (put longer code in a file of your folder and dofile/include it, or use lua_mount)";
    if (lua.find('\0') != std::string::npos) return "early_lua: a NUL character is not allowed";
    return "";
}

// A long-bracket opener/closer pair whose closer does not occur in `text`: [==[ ... ]==].
static std::string bracket_level(const std::string &text) {
    for (int n = 0; n < 32; ++n) {
        std::string eq(n, '=');
        if (text.find("]" + eq + "]") == std::string::npos) return eq;
    }
    return std::string(32, '=');
}

std::string build_shim(const std::string &early_lua) {
    std::string s = "-- Written by gmod-mcp for a test client (instances_launch): runs before the addons' own files. Not part of any addon.\n";
    if (!early_lua.empty()) {
        std::string eq = bracket_level(early_lua);
        s += "do\n\tlocal src = [" + eq + "[\n" + early_lua + "\n]" + eq + "]\n";
        s += "\tlocal f = CompileString(src, \"mcpagent_early_lua\", false)\n";
        s += "\tif type(f) ~= \"function\" then ErrorNoHalt(\"[mcpagent] early_lua: \" .. tostring(f) .. \"\\n\")\n";
        s += "\telse local ok, err = pcall(f) if not ok then ErrorNoHalt(\"[mcpagent] early_lua: \" .. tostring(err) .. \"\\n\") end end\nend\n";
    }
    return s;
}

int sweep_dir(const std::string &base_dir, double max_age_hours, const std::vector<std::string> &keep) {
    std::error_code ec;
    fs::path base = fs::u8path(base_dir);
    if (!fs::is_directory(base, ec)) return 0;
    int n = 0;
    auto now = fs::file_time_type::clock::now();
    std::vector<fs::path> victims;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().u8string();
        if (!valid_token(name) || !it->is_directory(ec)) continue;
        if (std::find(keep.begin(), keep.end(), name) != keep.end()) continue;
        // The newest write inside counts: a folder a client still writes to is not old.
        fs::file_time_type newest = fs::last_write_time(it->path(), ec);
        for (fs::recursive_directory_iterator r(it->path(), ec), e2; !ec && r != e2; r.increment(ec)) {
            auto t = fs::last_write_time(r->path(), ec);
            if (!ec && t > newest) newest = t;
        }
        ec.clear();
        double age_h = std::chrono::duration<double, std::ratio<3600>>(now - newest).count();
        if (age_h >= max_age_hours) victims.push_back(it->path());
    }
    for (const auto &v : victims) {
        fs::remove_all(v, ec);
        if (!ec) ++n;
    }
    return n;
}

}  // namespace earlylua
