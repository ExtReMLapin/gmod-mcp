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

// Issue #8 (chaos test G3): the owner label of instances_launch carried on the command line (-mcp_owner) so that it survives the
// bridge session that launched the client. The label is free text (up to 64 characters): characters outside [A-Za-z0-9._-] are
// written as %XX (the UTF-8 bytes, upper-case hex), which is a printable ASCII token without spaces, quotes or ';'.
// decode_owner reads it back; a malformed escape, a control character or an over-long result gives "" (no owner).
constexpr size_t kMaxOwnerChars = 64;
std::string encode_owner(const std::string &owner);
std::string decode_owner(const std::string &token);

// Giving the foreground back (Windows). A test client (-mcp_test_client) takes the foreground window when it starts and keeps it, which pulls
// the user out of what they were doing. The bridge records the foreground window before it starts a client and passes it as
// `-mcp_return_focus <window handle, decimal>`; once the client's own window is up, the module hands the foreground back to that window.
// A window handle is 32 significant bits on 64-bit Windows: the value is 1..4294967295, plain decimal digits, no sign, space or leading zero.
constexpr unsigned long long kMaxWindowHandle = 0xFFFFFFFFull;
unsigned long long parse_return_focus(const std::string &token);  // 0 when the token is not a valid handle
std::string format_return_focus(unsigned long long hwnd);         // "" for 0 or an out-of-range value

// The window the bridge asks the client to give the foreground back to: the foreground window it sees right now (`foreground`, 0 = none), unless
// that window belongs to a client this bridge just started (`foreground_is_ours`: an earlier client of the same launch has not given the
// foreground back yet), in which case the window recorded before the first start (`remembered`) stays the target. 0 = no target.
unsigned long long pick_return_target(unsigned long long foreground, bool foreground_is_ours, unsigned long long remembered);

// What the module does about the foreground, decided from facts it reads from the system.
struct FocusFacts {
    bool test_client = false;         // started by instances_launch (-mcp_test_client); the user's own game is never touched
    unsigned long long target = 0;    // -mcp_return_focus (0 = none)
    bool target_exists = false;       // the window handle is still a window (not destroyed)
    bool target_usable = false;       // ... and it is visible, not minimized and not a window of this process (SetForegroundWindow would be wrong otherwise)
    bool game_window_found = false;   // this process's main window exists
    bool game_is_foreground = false;  // and a window of this process is the foreground window
};
enum class FocusAction { Wait, Return, Stop };
// The module checks every few frames. Wait: nothing to do yet. Return: SetForegroundWindow(target). Stop: done for good (nothing to give
// back, the window is gone, or the budget is spent). `returns` counts earlier returns; `elapsed_s` is the time since the module started.
constexpr int kMaxFocusReturns = 3;           // the game can take the foreground again once (the first map load): never more than this
constexpr double kFocusWatchSeconds = 180.0;  // after this a window the user gave the focus to on purpose is left alone
FocusAction decide_return_focus(const FocusFacts &f, int returns, double elapsed_s);

}  // namespace launchopts
