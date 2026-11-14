// Mounted folder check (pure: shared by the module and the bridge).
#include <filesystem>

#include "lua_mount.h"
#include "util.h"

namespace lua_mount {

namespace fs = std::filesystem;

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

}  // namespace lua_mount
