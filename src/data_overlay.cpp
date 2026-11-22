#include "data_overlay.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace dataoverlay {

namespace fs = std::filesystem;
using nlohmann::json;

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

const char *mode_name(Mode m) { return m == Mode::Overlay ? "overlay" : m == Mode::Audit ? "audit" : "off"; }
Mode parse_mode(const std::string &s) { return s == "overlay" ? Mode::Overlay : s == "audit" ? Mode::Audit : Mode::Off; }

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

std::string build_shim(const std::string &overlay_chunk, Mode mode, const std::string &token, const std::string &early_lua) {
    std::string s = "-- Written by gmod-mcp for a test client (instances_launch): runs before the addons' own files. Not part of any addon.\n";
    if (mode != Mode::Off) {
        s += "__mcpagent_data_cfg = { mode = \"" + std::string(mode_name(mode)) + "\", token = \"" + token + "\" }\n";
        s += "do\n" + overlay_chunk + "\nend\n";
    }
    if (!early_lua.empty()) {
        std::string eq = bracket_level(early_lua);
        s += "do\n\tlocal src = [" + eq + "[\n" + early_lua + "\n]" + eq + "]\n";
        s += "\tlocal f = CompileString(src, \"mcpagent_early_lua\", false)\n";
        s += "\tif type(f) ~= \"function\" then ErrorNoHalt(\"[mcpagent] early_lua: \" .. tostring(f) .. \"\\n\")\n";
        s += "\telse local ok, err = pcall(f) if not ok then ErrorNoHalt(\"[mcpagent] early_lua: \" .. tostring(err) .. \"\\n\") end end\nend\n";
    }
    return s;
}

static bool read_file(const fs::path &p, std::string &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string rel_of(const fs::path &base, const fs::path &p) {
    std::string r = p.lexically_relative(base).generic_u8string();
    return r;
}

std::vector<Change> read_changes(const std::string &data_dir, const std::string &token, Mode mode) {
    std::vector<Change> out;
    if (!valid_token(token) || mode == Mode::Off) return out;
    std::error_code ec;
    fs::path data = fs::u8path(data_dir), root = data / kFolder / token;
    if (!fs::is_directory(root, ec)) return out;
    if (mode == Mode::Overlay) {
        fs::path f = root / "f", w = root / "w";
        if (fs::is_directory(f, ec)) {
            for (fs::recursive_directory_iterator it(f, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                std::string rel = rel_of(f, it->path());
                if (rel.empty()) continue;
                bool real = fs::exists(data / fs::u8path(rel), ec);
                if (it->is_directory(ec)) {
                    // An empty folder the client created; a folder with files is implied by them.
                    if (!real && fs::is_empty(it->path(), ec)) out.push_back({rel, "created_dir", -1});
                    continue;
                }
                long long sz = (long long)fs::file_size(it->path(), ec);
                out.push_back({rel, real ? "modified" : "created", ec ? -1 : sz});
            }
        }
        if (fs::is_directory(w, ec)) {
            for (fs::recursive_directory_iterator it(w, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec)) continue;
                std::string rel = rel_of(w, it->path());
                if (rel.size() <= 4 || rel.compare(rel.size() - 4, 4, ".txt") != 0) continue;
                rel.resize(rel.size() - 4);
                if (fs::exists(data / fs::u8path(rel), ec)) out.push_back({rel, "deleted", -1});
            }
        }
    } else {
        std::string log;
        if (!read_file(root / "audit.txt", log)) return out;
        struct St {
            std::string name;
            int initial = -1;
            bool exists = false, dir = false, wrote = false;
        };
        std::map<std::string, St> by;
        std::istringstream in(log);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == (char)13) line.pop_back();  // tolerate CRLF
            size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos || a != 1 || b != 3) continue;
            char op = line[0];
            if (line[2] != '0' && line[2] != '1') continue;
            int existed = line[2] == '1' ? 1 : 0;
            std::string name = line.substr(b + 1);
            if (name.empty() || (op != 'W' && op != 'D' && op != 'M')) continue;
            St &s = by[lower(name)];
            if (s.name.empty()) s.name = name;
            if (s.initial < 0) s.initial = existed;
            if (op == 'M') s.dir = true;
            s.exists = op != 'D';
            if (op == 'W') s.wrote = true;
        }
        for (auto &kv : by) {
            const St &s = kv.second;
            std::string change;
            if (s.initial == 0 && s.exists) change = s.dir ? "created_dir" : "created";
            else if (s.initial == 1 && !s.exists) change = "deleted";
            else if (s.initial == 1 && s.exists && s.wrote) change = "modified";
            if (change.empty()) continue;
            long long sz = -1;
            if (s.exists && !s.dir) {
                sz = (long long)fs::file_size(data / fs::u8path(s.name), ec);
                if (ec) sz = -1;
            }
            out.push_back({s.name, change, sz});
        }
    }
    std::sort(out.begin(), out.end(), [](const Change &a, const Change &b) { return a.path < b.path; });
    return out;
}

json changes_json(const std::vector<Change> &changes, Mode mode, const std::string &token) {
    json list = json::array();
    for (size_t i = 0; i < changes.size() && i < kMaxChanges; ++i) {
        json e{{"path", changes[i].path}, {"change", changes[i].change}};
        if (changes[i].size >= 0) e["size"] = changes[i].size;
        list.push_back(e);
    }
    json r{{"mode", mode_name(mode)}, {"token", token}, {"changes", list}, {"total", changes.size()}};
    if (changes.size() > kMaxChanges) r["truncated"] = true;
    return r;
}

bool remove_folder(const std::string &data_dir, const std::string &token) {
    if (!valid_token(token)) return false;
    std::error_code ec;
    fs::path p = fs::u8path(data_dir) / kFolder / token;
    if (!fs::exists(p, ec)) return true;
    fs::remove_all(p, ec);
    return !ec && !fs::exists(p, ec);
}

int sweep_old(const std::string &data_dir, double max_age_hours, const std::vector<std::string> &keep) {
    return sweep_dir((fs::u8path(data_dir) / kFolder).u8string(), max_age_hours, keep);
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

}  // namespace dataoverlay
