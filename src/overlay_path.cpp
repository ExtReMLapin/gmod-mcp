// Overlay folder check (pure: shared by the module and the bridge).
#include <filesystem>

#include "overlay.h"
#include "util.h"

namespace overlay {

namespace fs = std::filesystem;

bool normalize_root(const std::string &in, std::string &root, std::string &err) {
    root.clear();
    if (in.empty()) {
        err = "empty path: pass the overlay folder, or null to remove the overlay";
        return false;
    }
    // A quote or line break would break the -mcp_overlay command line; ';' separates the engine's path lists.
    if (in.find_first_of("\";\r\n") != std::string::npos) {
        err = "invalid character in the path (quote, ';' or line break)";
        return false;
    }
    fs::path p = fs::u8path(in);
    if (!p.is_absolute()) {
        err = "the overlay path must be absolute (e.g. D:/dev/my-addon-worktree): " + in;
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
        return "the overlay folder is inside the game folder (" + game_dir + "), which the engine already searches: overlaying "
               "it would only reorder the game's own paths. Pass a folder outside the game, e.g. your git worktree";
    return "";
}

}  // namespace overlay
