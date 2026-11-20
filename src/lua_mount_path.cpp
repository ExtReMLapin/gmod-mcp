// Mounted folder check (pure: shared by the module and the bridge).
#include <filesystem>

#include "lua_mount.h"
#include "util.h"

namespace lua_mount {

namespace fs = std::filesystem;

// A directory path: 247 characters (MAX_PATH 260 minus the 8.3 file name Windows reserves and the terminator).
constexpr size_t kMaxRootChars = 248;

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
    if (r == g || r.rfind(g + sep, 0) == 0)
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
            std::string rel = std::string(prefix) + "/" + fs::relative(it->path(), dir, e2).generic_u8string();
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

std::string refuse_mount(const std::string &root, const std::string &game_dir, bool allow_shadow, std::string *code) {
    (void)allow_shadow;
    if (code) *code = "invalid_params";
    std::string e = refuse_inside_game(root, game_dir);
    if (e.empty()) e = refuse_too_long(root);
    return e;
}

}  // namespace lua_mount
