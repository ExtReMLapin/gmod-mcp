// Per-client data/ isolation of test clients (Issue #20). The Lua side is src/lua/12_data_overlay.lua; this file is the pure
// part around it, shared by the bridge and the module and unit-tested without a game:
//   - the options of instances_launch (data_isolation, data_audit, early_lua) as command-line values,
//   - the text of the early autorun file the module mounts so that the overlay (and the user's early Lua) runs before any
//     addon in the client and server realms,
//   - reading what a client changed under data/ from the folder the Lua side wrote, and removing that folder.
// Layout (inside garrysmod/data, the only place the Lua file library can write): see the head of 12_data_overlay.lua.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace dataoverlay {

enum class Mode { Off, Overlay, Audit };

constexpr size_t kMaxEarlyLua = 16 * 1024;  // bytes of Lua `early_lua` may carry
constexpr size_t kMaxChanges = 500;         // entries returned per client (the total is reported)
constexpr const char *kFolder = "mcp_overlay";  // data/<kFolder>/<token>/

// Token of one client's folder: 1..40 characters of [A-Za-z0-9_-]. Anything else is refused (it becomes a path and a Lua string).
bool valid_token(const std::string &token);
// A new random token: "c" + 11 lower-case hex digits.
std::string new_token();

const char *mode_name(Mode m);              // "off", "overlay", "audit"
Mode parse_mode(const std::string &s);      // unknown = Off

// "" if `lua` is acceptable as early_lua (at most kMaxEarlyLua bytes, no NUL), else why not.
std::string check_early_lua(const std::string &lua);

// The text of the early autorun file. `overlay_chunk` is the source of 12_data_overlay.lua (empty when the mode is Off).
// Order: the configuration, the overlay (so that early_lua already runs isolated), then early_lua in its own protected chunk
// (an error in it is printed, never stops the files that follow).
std::string build_shim(const std::string &overlay_chunk, Mode mode, const std::string &token, const std::string &early_lua);
// File name of the shim inside <root>/lua/autorun: sorted before any addon's file.
constexpr const char *kShimName = "!!!mcpagent_early.lua";

// One thing a client changed under data/.
struct Change {
    std::string path;    // relative to data/, '/' separators
    std::string change;  // "created", "modified", "deleted", "created_dir"
    long long size = -1; // bytes (created, modified), -1 when unknown
};
// What the client of `token` changed, from its folder under `data_dir` (absolute path of garrysmod/data). Overlay mode reads the
// copies and tombstones and compares them with the real data; audit mode replays audit.txt. Sorted by path. Empty when the
// folder does not exist (nothing was written, or the Lua side never ran).
std::vector<Change> read_changes(const std::string &data_dir, const std::string &token, Mode mode);
// {mode, token, changes: [{path, change, size?}], total, truncated?}
nlohmann::json changes_json(const std::vector<Change> &changes, Mode mode, const std::string &token);
// Removes data/<kFolder>/<token>. False when the token is invalid or the removal failed (a missing folder is fine: true).
bool remove_folder(const std::string &data_dir, const std::string &token);
// Removes the folders under data/<kFolder> not modified for `max_age_hours`, except the tokens in `keep`; returns how many.
// Clients killed with their bridge leave their folder behind; the next launch cleans up.
int sweep_old(const std::string &data_dir, double max_age_hours, const std::vector<std::string> &keep);
// The same for any folder of token-named sub-folders (the bridge's temporary early folders).
int sweep_dir(const std::string &base_dir, double max_age_hours, const std::vector<std::string> &keep);
constexpr double kSweepHours = 72;  // a folder nothing wrote to for this long belongs to a client that is gone

}  // namespace dataoverlay
