// Crash or clean quit? What the coordinator (and the bridge) can tell about a game process that is gone.
//
// Signals, strongest first:
//  - the game was asked to end (game_quit / game_terminate relayed to it) or said goodbye itself (its module closed with
//    the menu state, which only happens when the game shuts down): ended on purpose;
//  - a crash dump (<game root>/crashes/*.mdmp) written shortly before the process ended: crashed;
//  - the exit code, read through a handle opened while the process lived: 0 = quit, an NTSTATUS error (0xC0000005...) =
//    crashed, another non-zero code = abnormal (a crash that wrote no dump, or a kill from outside: not told apart);
//  - nothing known: unknown (the caller keeps its generic "gone" answer).
// The decision is pure (judge) so it is tested without a game; only list_dumps touches the disk.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace crashinfo {

struct Dump {
    std::string name;  // file name, no folder
    double mtime = 0;  // seconds since the Unix epoch
};

// The folders where a game started from `game_dir` (<root>/garrysmod) writes its minidumps: <root>/crashes (Garry's Mod
// client), then <root>/garrysmod/crashes (checked as well: where a dedicated server may write).
std::vector<std::string> dump_dirs(const std::string &game_dir_u8);
// *.mdmp files of these folders (missing folders are fine), newest first.
std::vector<Dump> list_dumps(const std::vector<std::string> &dirs_u8);

// How long before the end of the process (seconds) a dump still counts as its own, and how long after (clock slack).
constexpr double kDumpBefore = 20.0;
constexpr double kDumpAfter = 5.0;

struct Facts {
    bool requested = false;     // game_quit / game_terminate was relayed to it
    bool goodbye = false;       // its module closed (clean shutdown)
    bool exit_known = false;    // exit_code / exit_unix are valid (a handle was held)
    uint32_t exit_code = 0;
    double exit_unix = 0;       // 0 = unknown
    double start_unix = 0;      // process start, 0 = unknown: nothing older than it can be its dump
    double last_seen_unix = 0;  // last heartbeat (when the end time is unknown)
    double now_unix = 0;
};

enum class Cause { Quit, Crashed, Unknown };

struct Verdict {
    Cause cause = Cause::Unknown;
    std::string dump;               // matched dump file name ("" = none)
    bool dump_ambiguous = false;    // another unclaimed dump was just as close in time
    std::vector<std::string> evidence;  // short sentences, for the answer
};

// NTSTATUS error code as exit code (0xC0000005 access violation, 0xC0000094 integer divide by zero...): the process was
// ended by an unhandled exception.
bool is_exception_exit(uint32_t code);

// Which dump belongs to a process that ended around `near`: inside [not_before, not_after], the one closest to `near`,
// never one in `claimed` (already given to another dead instance). ambiguous is set when several qualify.
bool match_dump(const std::vector<Dump> &dumps, double not_before, double not_after, double near,
                const std::set<std::string> &claimed, Dump &out, bool &ambiguous);

Verdict judge(const Facts &f, const std::vector<Dump> &dumps, const std::set<std::string> &claimed);

// The words of the answers. Error code for a crashed instance: client_crashed (client) or server_crashed (srcds). The dump named
// is a file for a human (crashes/*.mdmp in the game folder).
const char *crash_code(const std::string &mode);
std::string crash_message(const std::string &mode, const std::string &id, uint32_t pid, double seconds_ago, const Verdict &v, bool exit_known,
                          uint32_t exit_code);
std::string quit_message(const std::string &id, uint32_t pid, double seconds_ago, const std::string &how);
// The entry instances_list shows for an instance that crashed: {id, mode, pid, state: "crashed", crashed_s_ago, crash}.
nlohmann::json crashed_entry(const std::string &id, const std::string &mode, uint32_t pid, double seconds_ago, const Verdict &v, bool exit_known,
                             uint32_t exit_code);

}  // namespace crashinfo
