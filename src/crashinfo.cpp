#include "crashinfo.h"

#include "mdmp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace crashinfo {

std::vector<std::string> dump_dirs(const std::string &game_dir_u8) {
    std::vector<std::string> out;
    if (game_dir_u8.empty()) return out;
    std::string d = game_dir_u8;
    while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    fs::path g = fs::u8path(d);
    out.push_back((g.parent_path() / "crashes").u8string());
    out.push_back((g / "crashes").u8string());
    return out;
}

// Unix time of a file's last write. The file clock's epoch differs by platform and standard library: the offset between
// the two clocks is measured instead of assumed.
static double mtime_unix(const fs::file_time_type &t) {
    double sys_now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    double age = std::chrono::duration<double>(fs::file_time_type::clock::now() - t).count();
    return sys_now - age;
}

std::vector<Dump> list_dumps(const std::vector<std::string> &dirs_u8) {
    std::vector<Dump> out;
    for (const auto &d : dirs_u8) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::u8path(d), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string ext = it->path().extension().u8string();
            for (auto &c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext != ".mdmp") continue;
            auto t = it->last_write_time(e2);
            if (e2) continue;
            out.push_back(Dump{it->path().filename().u8string(), mtime_unix(t)});
        }
    }
    std::sort(out.begin(), out.end(), [](const Dump &a, const Dump &b) { return a.mtime > b.mtime; });
    return out;
}

bool is_exception_exit(uint32_t code) { return (code & 0xC0000000u) == 0xC0000000u; }

bool match_dump(const std::vector<Dump> &dumps, double not_before, double not_after, double near,
                const std::set<std::string> &claimed, Dump &out, bool &ambiguous) {
    ambiguous = false;
    int n = 0;
    double best = 0;
    for (const auto &d : dumps) {
        if (d.mtime < not_before || d.mtime > not_after || claimed.count(d.name)) continue;
        double dist = std::fabs(d.mtime - near);
        if (n == 0 || dist < best) {
            best = dist;
            out = d;
        }
        ++n;
    }
    ambiguous = n > 1;
    return n > 0;
}

static std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

Verdict judge(const Facts &f, const std::vector<Dump> &dumps, const std::set<std::string> &claimed) {
    Verdict v;
    if (f.requested) {
        v.cause = Cause::Quit;
        v.evidence.push_back("it was asked to end (game_quit or game_terminate)");
        return v;
    }
    if (f.goodbye) {
        v.cause = Cause::Quit;
        v.evidence.push_back("its module closed with the game (clean shutdown)");
        return v;
    }
    // A dump written shortly before the process ended. With the end time known the window is tight; without it, the process
    // ended somewhere between its last heartbeat and now.
    double lo, hi, near;
    if (f.exit_known && f.exit_unix > 0) {
        lo = f.exit_unix - kDumpBefore;
        hi = f.exit_unix + kDumpAfter;
        near = f.exit_unix;
    } else {
        lo = f.last_seen_unix - kDumpBefore;
        hi = f.now_unix + kDumpAfter;
        near = f.last_seen_unix + 2;
    }
    // Not before the process started, with a second of slack: the start time and a file's time come from clocks with a
    // coarse tick, and a dump written within the same tick can read a hair earlier than the start.
    if (f.start_unix > 0) lo = std::max(lo, f.start_unix - 1.0);
    Dump d;
    bool amb = false;
    if (match_dump(dumps, lo, hi, near, claimed, d, amb)) {
        v.cause = Cause::Crashed;
        v.dump = d.name;
        v.dump_ambiguous = amb;
        v.evidence.push_back("a crash dump was written just before it ended");
        if (amb) v.evidence.push_back("another dump of the same minutes exists (several games crashed together): the match is by time only");
        if (f.exit_known) v.evidence.push_back("exit code " + hex(f.exit_code));
        return v;
    }
    if (f.exit_known) {
        if (f.exit_code == 0) {
            v.cause = Cause::Quit;
            v.evidence.push_back("exit code 0");
        } else if (is_exception_exit(f.exit_code)) {
            v.cause = Cause::Crashed;
            v.evidence.push_back("exit code " + hex(f.exit_code) + " (unhandled exception), no crash dump found");
        } else {
            v.cause = Cause::Crashed;
            v.evidence.push_back("exit code " + std::to_string(f.exit_code) +
                                 " without a crash dump: a crash that wrote none, or killed from outside (not told apart)");
        }
        return v;
    }
    v.cause = Cause::Unknown;
    return v;
}

void read_dump_facts(const std::vector<std::string> &dirs_u8, Verdict &v) {
    if (v.dump.empty()) return;
    for (const auto &d : dirs_u8) {
        std::error_code ec;
        fs::path p = fs::u8path(d) / fs::u8path(v.dump);
        if (!fs::is_regular_file(p, ec)) continue;
        mdmp::Facts f = mdmp::parse_file(p.u8string());
        if (f.ok) {
            v.fault = f.fault;
            v.stack = f.candidates;
        }
        return;
    }
}

void add_fault_details(nlohmann::json &o, const Verdict &v) {
    if (!v.fault.empty()) o["fault"] = v.fault;
    if (!v.stack.empty()) o["stack_candidates"] = v.stack;
}

const char *crash_code(const std::string &mode) { return mode == "srcds" ? "server_crashed" : "client_crashed"; }

std::string crash_message(const std::string &mode, const std::string &id, uint32_t pid, double seconds_ago, const Verdict &v,
                          bool exit_known, uint32_t exit_code) {
    bool server = mode == "srcds";
    std::string who = std::string(server ? "dedicated server " : "client ") + id + (pid ? " (pid " + std::to_string(pid) + ")" : "");
    std::string m = who + " crashed (its process ended " + std::to_string((int)seconds_ago) + " s ago without a clean shutdown)";
    if (exit_known) m += "; exit code " + (is_exception_exit(exit_code) ? hex(exit_code) : std::to_string(exit_code));
    m += ". ";
    if (!v.dump.empty())
        m += "Crash dump: crashes/" + v.dump + " in the game folder (for a human with a debugger; nothing to do with it here)" +
             (v.dump_ambiguous ? " (matched by time only: several games crashed together)" : "") + ". " +
             (v.fault.empty() ? "" : "It faulted at " + v.fault + " (details.stack_candidates lists return-address candidates of its stack). ");
    else
        m += "No crash dump was found for it (it may have been killed from outside, or crashed before writing one). ";
    if (server) m += "Restart the server; the session and what was in memory are lost.";
    else m += "Relaunch a client with instances_launch; its map, Lua state and anything it held are lost, so a test that was running on it must be run again.";
    return m;
}

std::string quit_message(const std::string &id, uint32_t pid, double seconds_ago, const std::string &how) {
    std::string who = "instance " + id + (pid ? " (pid " + std::to_string(pid) + ")" : "");
    return who + " stopped answering " + std::to_string((int)seconds_ago) + " s ago: its process has exited and it ended normally (" + how +
           "). Relaunch a client with instances_launch.";
}

nlohmann::json crashed_entry(const std::string &id, const std::string &mode, uint32_t pid, double seconds_ago, const Verdict &v,
                   bool exit_known, uint32_t exit_code) {
    nlohmann::json crash{{"dump", v.dump.empty() ? nlohmann::json(nullptr) : nlohmann::json(v.dump)}, {"evidence", v.evidence}};
    if (exit_known) crash["exit_code"] = exit_code;
    if (v.dump_ambiguous) crash["dump_matched_by_time_only"] = true;
    add_fault_details(crash, v);
    return nlohmann::json{{"id", id}, {"mode", mode}, {"pid", pid}, {"state", "crashed"}, {"crashed_s_ago", (int)seconds_ago}, {"crash", crash},
                {"self", false}, {"role", "member"},
                {"note", "this game crashed: relaunch a client with instances_launch; the dump (crashes/*.mdmp in the game folder) is for a human"}};
}

}  // namespace crashinfo
