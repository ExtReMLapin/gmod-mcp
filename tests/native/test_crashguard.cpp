// Unit tests for the load crash guard (no game needed). Built as gmod-mcp-unittests, run by the CI.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "crashguard.h"
#include "platform/platform.h"

namespace fs = std::filesystem;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_pass;                                                            \
        } else {                                                                 \
            ++g_fail;                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

// Simulated process table: pid -> start stamp (absent = dead).
static std::map<uint32_t, uint64_t> g_procs;
static uint64_t fake_stamp(uint32_t pid) {
    auto it = g_procs.find(pid);
    return it == g_procs.end() ? 0 : it->second;
}

static fs::path fresh_dir(const char *name) {
    fs::path d = fs::temp_directory_path() / "gmod-mcp-unittests" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

static void write(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

static std::string marker(uint32_t pid) { return "loading/" + std::to_string(pid) + ".txt"; }

int main() {
    // 1. Nothing on disk: load, and the marker exists until end().
    {
        g_procs = {{100, 5000}};
        fs::path d = fresh_dir("empty");
        auto r = crashguard::begin(d.u8string(), 100, fake_stamp);
        CHECK(!r.skip);
        CHECK(fs::exists(d / marker(100)));
        crashguard::end(d.u8string(), 100);
        CHECK(!fs::exists(d / marker(100)));
    }
    // 2. Another instance is loading right now (live pid, same stamp): this one loads too (RETEX 2026-11-13 #1).
    {
        g_procs = {{100, 5000}, {200, 7000}};
        fs::path d = fresh_dir("concurrent");
        write(d / marker(200), "200 7000\n");
        auto r = crashguard::begin(d.u8string(), 100, fake_stamp);
        CHECK(!r.skip);
        CHECK(fs::exists(d / marker(200)));  // the other instance's marker is left alone
        crashguard::end(d.u8string(), 100);
    }
    // 3. A previous load crashed (dead pid): skip this launch once, and only once.
    {
        g_procs = {{100, 5000}, {101, 5100}};
        fs::path d = fresh_dir("crashed");
        write(d / marker(300), "300 9000\n");
        auto r = crashguard::begin(d.u8string(), 100, fake_stamp);
        CHECK(r.skip);
        CHECK(!fs::exists(d / marker(300)));
        CHECK(!fs::exists(d / marker(100)));  // a skipped launch leaves no marker
        CHECK(fs::exists(d / "last_crash.txt"));
        auto r2 = crashguard::begin(d.u8string(), 101, fake_stamp);
        CHECK(!r2.skip);
        crashguard::end(d.u8string(), 101);
    }
    // 4. The pid of a crashed load was reused by an unrelated process (other start stamp): still a crash.
    {
        g_procs = {{100, 5000}, {300, 1234567}};
        fs::path d = fresh_dir("pid_reused");
        write(d / marker(300), "300 9000\n");
        CHECK(crashguard::begin(d.u8string(), 100, fake_stamp).skip);
    }
    // 5. Liveness known but stamp unreadable (another user's process): treated as alive.
    {
        g_procs = {{100, 5000}, {300, crashguard::kStampUnknown}};
        fs::path d = fresh_dir("stamp_unknown");
        write(d / marker(300), "300 9000\n");
        CHECK(!crashguard::begin(d.u8string(), 100, fake_stamp).skip);
        crashguard::end(d.u8string(), 100);
    }
    // 6. Legacy single marker (0.3.x and earlier): removed, never blocks (it blocked every later launch).
    {
        g_procs = {{100, 5000}};
        fs::path d = fresh_dir("legacy");
        write(d / "load_in_progress.txt", "gmod-mcp load in progress\n");
        auto r = crashguard::begin(d.u8string(), 100, fake_stamp);
        CHECK(!r.skip);
        CHECK(!fs::exists(d / "load_in_progress.txt"));
        crashguard::end(d.u8string(), 100);
    }
    // 7. Garbage and own stale marker (pid reuse of this very process): cleaned, load.
    {
        g_procs = {{100, 5000}};
        fs::path d = fresh_dir("garbage");
        write(d / "loading/abc.txt", "zzz");
        write(d / marker(100), "100 42\n");
        auto r = crashguard::begin(d.u8string(), 100, fake_stamp);
        CHECK(!r.skip);
        CHECK(!fs::exists(d / "loading/abc.txt"));
        crashguard::end(d.u8string(), 100);
    }
    // 8. Five launches at once, one old crash: exactly one of them skips.
    {
        g_procs = {{1, 11}, {2, 22}, {3, 33}, {4, 44}, {5, 55}};
        fs::path d = fresh_dir("burst");
        write(d / marker(999), "999 1\n");
        int skipped = 0;
        for (uint32_t p = 1; p <= 5; ++p)
            if (crashguard::begin(d.u8string(), p, fake_stamp).skip) ++skipped;
        CHECK(skipped == 1);
    }
    // 9. Real process table: this process is alive with a stamp, an absurd pid is not.
    {
        uint64_t s = plat::process_start_stamp(plat::process_id());
        CHECK(s != 0);
        CHECK(plat::process_start_stamp(plat::process_id()) == s);
        CHECK(plat::process_start_stamp(0x7FFFFFF0u) == 0);
    }
    // 10. Real end-to-end: a marker of this live process seen by "another" pid does not block it.
    {
        fs::path d = fresh_dir("real");
        uint32_t me = plat::process_id();
        write(d / marker(me), std::to_string(me) + " " + std::to_string(plat::process_start_stamp(me)) + "\n");
        CHECK(!crashguard::begin(d.u8string(), 0x7FFFFFF1u, plat::process_start_stamp).skip);
    }

    std::printf("crashguard: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
