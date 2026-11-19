// Options of instances_launch that pass settings through to the clients it starts (issue #10): fps_max, fps_max_nofocus,
// commands (console statements run once a client answers) and launch_args (extra command-line tokens). Pure functions,
// unit-tested; the bridge validates before anything starts, the module reads back the list of convars to restore.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace launchopts {

constexpr size_t kMaxItems = 32;       // commands and launch_args: at most this many elements
constexpr size_t kMaxItemLength = 160; // ... of at most this many characters
constexpr size_t kMaxRestoreNames = 64;

// "" if `v` is a valid frame cap: an integer, 0 (no cap) or 10..1000. Else a message naming `name`.
std::string fps_error(const nlohmann::json &v, const std::string &name);

// Validates `commands` (an array of console lines) and splits them into statements the way the engine does (';' and line
// breaks outside quotes). "" if all are accepted (`statements` then holds them in order), else a message naming the first
// refused command. Refused: quit/exit, anything that connects to a server (connect, retry, redirect, connect_lobby) and map
// changes (map, changelevel, load...: use the map option).
std::string check_commands(const nlohmann::json &list, std::vector<std::string> &statements);

// Validates `launch_args` (an array of command-line tokens, one per element: `["+cl_showfps", "1"]`, not "+cl_showfps 1").
// "" if all are accepted (`tokens` then holds them), else a message naming the first refused option. Refused: what gmod-mcp
// sets itself (-multirun, -mcp*, window size and mode, -dxlevel, -condebug, -game), anything that connects to a server and
// map changes (+map...), and tokens that could end the argument or start another one (spaces, quotes, ';', control or
// non-ASCII characters).
std::string check_launch_args(const nlohmann::json &list, std::vector<std::string> &tokens);

// Names (lower case, once each, plain names only) of the convars `statements` and the `+name` tokens of `tokens` may change:
// the first word of each statement (the second one after toggle / incrementvar / decrementvar / multvar). Not every name is a
// convar: the module ignores the others.
std::vector<std::string> restore_names(const std::vector<std::string> &statements, const std::vector<std::string> &tokens);

// "a,b,c" and back (the value of -mcp_restore_convars). parse_restore_list keeps plain names only, lower case, once each.
std::string join_list(const std::vector<std::string> &names);
std::vector<std::string> parse_restore_list(const std::string &csv);

}  // namespace launchopts
