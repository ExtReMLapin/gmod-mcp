// early_lua of test clients (instances_launch {early_lua}, Issue #20): Lua that runs in the client and server realms before any
// addon's file. Pure part shared by the bridge and the module, unit-tested without a game:
//   - the checks of the option and the token that names a client's temporary folder,
//   - the text of the early autorun file the module mounts in front of the addons (it replaces nothing in the game),
//   - sweeping the temporary folders of clients that are gone.
#pragma once

#include <string>
#include <vector>

namespace earlylua {

constexpr size_t kMaxEarlyLua = 16 * 1024;  // bytes of Lua `early_lua` may carry

// Token of one client's temporary folder: 1..40 characters of [A-Za-z0-9_-]. Anything else is refused (it becomes a path).
bool valid_token(const std::string &token);
// A new random token: "c" + 11 lower-case hex digits.
std::string new_token();

// "" if `lua` is acceptable as early_lua (at most kMaxEarlyLua bytes, no NUL), else why not.
std::string check_early_lua(const std::string &lua);

// The text of the early autorun file: `early_lua` in its own protected chunk (an error in it is printed, never stops the files
// that follow). Nothing else: no library of the game is replaced or wrapped.
std::string build_shim(const std::string &early_lua);
// File name of the shim inside <root>/lua/autorun: sorted before any addon's file.
constexpr const char *kShimName = "!!!mcpagent_early.lua";

// Removes the folders of token names under `base_dir` not modified for `max_age_hours`, except the tokens in `keep`; returns how
// many. Clients killed with their bridge leave their folder behind (<temp>/gmod-mcp-early/<token>); the next launch cleans up.
int sweep_dir(const std::string &base_dir, double max_age_hours, const std::vector<std::string> &keep);
constexpr double kSweepHours = 72;  // a folder nothing wrote to for this long belongs to a client that is gone

}  // namespace earlylua
