// Self-installation of the main-menu loader: the module keeps a small, versioned block inside
// garrysmod/lua/menu/menu.lua so that it is loaded by the menu state at every game start.
#pragma once

#include <functional>
#include <string>

namespace installer {

// Ensures menu.lua contains the current loader block (between "-- MCPAGENT BEGIN" and "-- MCPAGENT END").
// Returns "unchanged", "installed" or "updated"; "" and fills err on failure. Keeps the file's line endings,
// backs up the original once (menu.lua.mcpagent.bak) and writes atomically.
// `compiles` (optional) must return true when the given Lua source compiles; the file is never written otherwise.
using CompileCheck = std::function<bool(const std::string &source, std::string &err)>;
std::string ensure_menu_patch(const std::string &game_dir, std::string &err, const CompileCheck &compiles = nullptr);

// Removes the loader block. Returns false and fills err on failure ("not installed" is not a failure).
bool remove_menu_patch(const std::string &game_dir, std::string &err);

}  // namespace installer
