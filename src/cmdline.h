// Console command lines: splitting into statements and reading what the console answered (pure functions, unit-tested).
#pragma once

#include <string>
#include <vector>

namespace cmdline {

// Splits a console line into statements the way the engine does: on ';' and line breaks outside double quotes.
// Statements are trimmed; empty ones are dropped. "a 1;b 2" -> {"a 1", "b 2"}; "echo \"x;y\"" stays one statement.
std::vector<std::string> split_statements(const std::string &line);

// First word of a statement, lower case ("" if none).
std::string command_name(const std::string &statement);

// True when one of the statements is `connect` and it is not the only statement (connect must be alone on its line).
bool connect_not_alone(const std::vector<std::string> &statements);

// True for a statement that must not run while this game hosts a map: `killserver` disconnects the local client and
// switches the network to single-player at once, but the engine shuts the server down only later (host state). When
// a frame runs several server ticks (low fps, e.g. a test client at fps_max 20), the next tick sends a snapshot from a
// half-torn server and the game crashes (engine.dll+0x2069e0, see docs/REVERSE.md). console_exec leaves the game first.
bool leave_game_first(const std::string &statement);

// Names (lower case) of the commands the engine reported as unknown in these console lines
// (the engine prints: Unknown command "name").
std::vector<std::string> unknown_commands(const std::vector<std::string> &lines);

// gmod-mcp's own log lines ("[mcpagent] ..."): not part of a command's output.
bool own_log_line(const std::string &line);

}  // namespace cmdline
