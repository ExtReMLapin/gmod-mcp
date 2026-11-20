// Mounted folder check (pure: shared by the module and the bridge).
#include <algorithm>
#include <filesystem>

#include "lua_mount.h"
#include "util.h"

namespace lua_mount {

namespace fs = std::filesystem;

// A directory path: 247 characters (MAX_PATH 260 minus the 8.3 file name Windows reserves and the terminator).
constexpr size_t kMaxRootChars = 248;

bool same_path(const std::string &a, const std::string &b) {
    auto norm = [](std::string x) {
        for (char &c : x) {
            if (c == '/') c = '\\';
            else if ((unsigned char)c < 0x80) c = (char)std::tolower((unsigned char)c);
        }
        while (!x.empty() && (x.back() == '\\' || x.back() == '.' || x.back() == ' ')) x.pop_back();
        return x;
    };
    std::string na = norm(a), nb = norm(b);
    return !na.empty() && na == nb;
}

std::vector<MountPath> mount_search_paths(const std::string &root) {
    return {{root, "MOD", true},
            {root + "\\lua", "lsv", false},
            {root + "\\gamemodes", "lsv", false},
            {root + "\\lua", "lcl", false},
            {root + "\\gamemodes", "lcl", false}};
}

bool normalize_root(const std::string &in, std::string &root, std::string &err) {
    root.clear();
    if (in.empty()) {
        err = "empty path: pass the folder to mount, or null to unmount it";
        return false;
    }
    // A quote or line break would break the -mcp_lua_mount command line; ';' separates the engine's path lists.
    if (in.find_first_of("\";\r\n") != std::string::npos) {
        err = "invalid character in the path (quote, ';' or line break)";
        return false;
    }
    // The engine's file system does not find files under a non-ASCII path: such a folder is accepted into its search
    // paths but none of its files load (seen live 2026-11-14). Refuse it, with the way out.
    for (unsigned char c : in)
        if (c >= 0x80) {
            err = "the folder path has non-ASCII characters, which the game's file system cannot read (the folder would be "
                  "mounted but none of its files would load). Use an ASCII-only path, e.g. a junction: mklink /J "
                  "C:\\gmod-wt \"<your folder>\", then mount C:\\gmod-wt";
            return false;
        }
    // A UNC share or an extended-length / device path (\\?\D:\x, \\.\D:\x) is accepted into the engine's search paths but none
    // of its files load (seen live 2026-11-20, also for \\localhost\D$\...): refused, with the way out.
    {
        std::string sl = in;
        for (char &c : sl)
            if (c == '/') c = '\\';
        if (sl.rfind("\\\\?\\", 0) == 0 || sl.rfind("\\\\.\\", 0) == 0) {
            err = "extended-length (\\\\?\\) and device (\\\\.\\) paths are not read by the game's file system (the folder would be "
                  "mounted but none of its files would load). Pass the plain drive path, e.g. D:/dev/my-addon-wt";
            return false;
        }
        if (sl.rfind("\\\\", 0) == 0) {
            err = "a UNC path (\\\\server\\share\\...) is not read by the game's file system (the folder would be mounted but none "
                  "of its files would load, even for \\\\localhost\\D$). Use a local drive path, or a junction: mklink /J "
                  "C:\\gmod-wt \"<your folder>\"";
            return false;
        }
    }
    // Windows creates and lists folders only up to MAX_PATH here: such a root cannot hold any file the engine can open.
    if (in.size() >= kMaxRootChars) {
        err = "the folder path is too long (" + std::to_string(in.size()) + " characters, the limit is " + std::to_string(kMaxRootChars - 1) +
              "): the game's file system cannot open files under it (MAX_PATH). Use a shorter path, e.g. a junction: mklink /J "
              "C:\\gmod-wt \"<your folder>\"";
        return false;
    }
    fs::path p = fs::u8path(in);
    if (!p.is_absolute()) {
        err = "the folder to mount must be an absolute path (e.g. D:/dev/my-addon-worktree): " + in;
        return false;
    }
    p = p.lexically_normal();
    while (p.filename().empty() && p.has_parent_path() && p != p.root_path()) p = p.parent_path();  // trailing separator
#ifdef _WIN32
    {
        // Windows resolves "wt." and "wt " to "wt": the stored root must not keep them (comparisons and the engine would disagree).
        std::string s = p.u8string();
        while (s.size() > 3 && (s.back() == '.' || s.back() == ' ')) s.pop_back();
        while (s.size() > 3 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
        p = fs::u8path(s);
    }
#endif
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        err = "folder not found: " + p.make_preferred().u8string();
        return false;
    }
    if (!fs::is_directory(p, ec)) {
        err = "not a folder: " + p.make_preferred().u8string();
        return false;
    }
    if (util::to_lower(p.filename().u8string()) == "lua") p = p.parent_path();
    if (!fs::is_directory(p / "lua", ec)) {
        err = "no lua/ folder in " + p.make_preferred().u8string() +
              ": pass the addon's root (the folder that contains lua/) or its lua/ folder";
        return false;
    }
    root = p.make_preferred().u8string();
    while (root.size() > 3 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    return true;
}

std::string refuse_inside_game(const std::string &root, const std::string &game_dir) {
    if (game_dir.empty()) return "";
    auto norm = [](const std::string &p) {
        std::string s = util::to_lower(fs::u8path(p).lexically_normal().make_preferred().u8string());
        while (s.size() > 3 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
        return s;
    };
    std::string r = norm(root), g = norm(game_dir);
    std::string sep(1, (char)fs::path::preferred_separator);
    bool inside = r == g || r.rfind(g + sep, 0) == 0;
    if (!inside) {
        // A junction or symlink that resolves into the game folder is the game folder too.
        std::error_code ec;
        fs::path cr = fs::weakly_canonical(fs::u8path(root), ec);
        fs::path cg = ec ? fs::path() : fs::weakly_canonical(fs::u8path(game_dir), ec);
        if (!ec && !cr.empty() && !cg.empty()) {
            std::string rr = norm(cr.u8string()), gg = norm(cg.u8string());
            inside = rr == gg || rr.rfind(gg + sep, 0) == 0;
        }
    }
    if (inside)
        return "the folder to mount is inside the game folder (" + game_dir + "), which the engine already searches: mounting "
               "it would only reorder the game's own paths. Pass a folder outside the game, e.g. your git worktree";
    return "";
}

std::string refuse_too_long(const std::string &root, size_t max_path) {
    size_t longest = 0;
    std::string which;
    auto walk = [&](const fs::path &dir, const char *prefix) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string name = it->path().filename().u8string();
            if (name.size() < 4 || util::to_lower(name.substr(name.size() - 4)) != ".lua") continue;
            std::string rel = std::string(prefix) + "/" + it->path().lexically_relative(dir).generic_u8string();
            size_t n = root.size() + 1 + rel.size();
            if (!e2 && n > longest) {
                longest = n;
                which = rel;
            }
        }
    };
    walk(fs::u8path(root) / "lua", "lua");
    walk(fs::u8path(root) / "gamemodes", "gamemodes");
    if (longest <= max_path) return "";
    return "the folder holds a file whose path would be " + std::to_string(longest) + " characters long (" + which + " under " + root +
           "), over the limit of " + std::to_string(max_path) + " (Windows MAX_PATH): the game's file system cannot open it, so " +
           "the folder would be mounted but its files would not load. Use a shorter root path (e.g. a junction: mklink /J " +
           "C:\\gmod-wt \"<your folder>\") or shorten the file's path";
}

std::vector<std::string> shadowed_base_files(const std::string &root, const std::string &game_dir) {
    std::vector<std::string> out;
    if (root.empty() || game_dir.empty()) return out;
    auto walk = [&](const fs::path &mount_dir, const fs::path &game_side, const std::string &prefix) {
        std::error_code ec;
        if (!fs::is_directory(mount_dir, ec) || !fs::is_directory(game_side, ec)) return;
        for (fs::recursive_directory_iterator it(mount_dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string name = it->path().filename().u8string();
            if (name.size() < 4 || util::to_lower(name.substr(name.size() - 4)) != ".lua") continue;
            fs::path sub = it->path().lexically_relative(mount_dir);
            if (e2) continue;
            if (fs::is_regular_file(game_side / sub, e2)) out.push_back(prefix + "/" + sub.generic_u8string());
        }
    };
    walk(fs::u8path(root) / "lua", fs::u8path(game_dir) / "lua", "lua");
    walk(fs::u8path(root) / "gamemodes" / "base", fs::u8path(game_dir) / "gamemodes" / "base", "gamemodes/base");
    std::sort(out.begin(), out.end());
    return out;
}

std::string refuse_mount(const std::string &root, const std::string &game_dir, bool allow_shadow, std::string *code) {
    if (code) *code = "invalid_params";
    std::string e = refuse_inside_game(root, game_dir);
    if (e.empty()) e = refuse_too_long(root);
    if (!e.empty() || allow_shadow) return e;
    auto sh = shadowed_base_files(root, game_dir);
    if (sh.empty()) return "";
    if (code) *code = "shadows_base_lua";
    std::string list;
    for (size_t i = 0; i < sh.size() && i < 8; ++i) list += (i ? ", " : "") + sh[i];
    if (sh.size() > 8) list += ", ...";
    return "the folder holds " + std::to_string(sh.size()) + " file(s) that also exist in the game's own base Lua (" + list +
           "): mounting it would replace the game's own files, and a client started that way hung in its load (map_load timed "
           "out). Move those files out of the mounted folder, or pass allow_shadow: true if you really mean to override the base Lua";
}

}  // namespace lua_mount
