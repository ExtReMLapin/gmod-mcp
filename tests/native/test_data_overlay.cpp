// Issue #20 (docs/TESTS.md DI-*): the pure part of the per-client data/ isolation (src/data_overlay.cpp): tokens, the early
// autorun file, what a client changed (overlay folder or audit log), removing and sweeping the folders. No game.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.h"
#include "data_overlay.h"

namespace fs = std::filesystem;
using namespace dataoverlay;
using nlohmann::json;

static void put(const fs::path &p, const std::string &content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}
static bool has(const std::vector<Change> &v, const std::string &path, const std::string &change, long long size = -2) {
    for (const auto &c : v)
        if (c.path == path && c.change == change && (size == -2 || c.size == size)) return true;
    return false;
}

static void test_tokens_and_modes() {
    CHECK(valid_token("c0123456789a") && valid_token("a_b-C9") && valid_token(std::string(40, 'x')));
    CHECK(!valid_token("") && !valid_token(std::string(41, 'x')) && !valid_token("a b") && !valid_token("../x") && !valid_token("a/b") &&
          !valid_token("a\"b") && !valid_token("a;b") && !valid_token("é"));
    std::string a = new_token(), b = new_token();
    CHECK(valid_token(a) && a.size() == 12 && a[0] == 'c' && a != b);
    CHECK(parse_mode("overlay") == Mode::Overlay && parse_mode("audit") == Mode::Audit && parse_mode("off") == Mode::Off && parse_mode("x") == Mode::Off);
    CHECK(std::string(mode_name(Mode::Overlay)) == "overlay" && std::string(mode_name(Mode::Audit)) == "audit" && std::string(mode_name(Mode::Off)) == "off");
    CHECK(check_early_lua("print(1)").empty() && check_early_lua("").empty());
    CHECK(!check_early_lua(std::string(kMaxEarlyLua + 1, 'x')).empty() && check_early_lua(std::string(kMaxEarlyLua, 'x')).empty());
    CHECK(!check_early_lua(std::string("a\0b", 3)).empty());
}

static void test_shim() {
    std::string chunk = "local x = 1 -- the overlay";
    std::string s = build_shim(chunk, Mode::Overlay, "c1", "");
    CHECK(s.find("__mcpagent_data_cfg = { mode = \"overlay\", token = \"c1\" }") != std::string::npos);
    CHECK(s.find(chunk) != std::string::npos);
    CHECK(s.find("CompileString") == std::string::npos);  // no early_lua: no compile step
    CHECK(s.find("cfg") < s.find(chunk));                  // configuration first
    std::string a = build_shim(chunk, Mode::Audit, "c2", "");
    CHECK(a.find("mode = \"audit\"") != std::string::npos);
    // Off with early_lua only: no overlay at all; early_lua runs in its own protected chunk.
    std::string e = build_shim("", Mode::Off, "", "RunConsoleCommand('x_enable', '0')");
    CHECK(e.find("__mcpagent_data_cfg") == std::string::npos && e.find("RunConsoleCommand('x_enable', '0')") != std::string::npos);
    CHECK(e.find("CompileString(src, \"mcpagent_early_lua\", false)") != std::string::npos && e.find("pcall(f)") != std::string::npos);
    // The overlay comes before the early code, so that the early code already runs isolated.
    std::string both = build_shim(chunk, Mode::Overlay, "c3", "print('early')");
    CHECK(both.find(chunk) < both.find("print('early')"));
    // Lua long brackets inside the user's code do not end the string early.
    std::string nasty = "local t = [[a]] local u = [=[b]=] return ]==]";
    std::string n = build_shim("", Mode::Off, "", nasty);
    CHECK(n.find("local src = [===[") != std::string::npos && n.find("\n]===]\n") != std::string::npos);
    if (const char *dump = std::getenv("GMOD_MCP_SHIM_DUMP")) {  // run by hand: luajit -bl on the file proves it parses
        std::ofstream(std::string(dump) + ".full.lua") << build_shim("local function f() return 1 end", Mode::Audit, "c9", "print('hi') local x = [[a]]");
    }
}

static fs::path make_data() {
    fs::path d = fs::temp_directory_path() / ("gmcp_do_" + new_token());
    fs::create_directories(d);
    return d;
}

static void test_overlay_changes() {
    fs::path data = make_data();
    // The real data folder.
    put(data / "cache" / "index.txt", "v1");
    put(data / "cache" / "gm_construct" / "t1.dat", "tile1");
    put(data / "cache" / "gm_construct" / "t2.dat", "tile2");
    // What the client wrote: a changed file, a new one in a new folder, an empty new folder, a deleted real file, a deleted
    // file that only existed in the overlay (no tombstone, nothing to report), a tombstone of a file that is not real.
    fs::path root = data / kFolder / "c1";
    put(root / "f" / "cache" / "index.txt", "version two");
    put(root / "f" / "cache" / "gm_construct" / "new.dat", "n");
    put(root / "f" / "fresh" / "a.txt", "AA");
    fs::create_directories(root / "f" / "emptydir");
    fs::create_directories(root / "f" / "cache" / "gm_construct");  // folder that also exists for real: implied, not reported
    put(root / "w" / "cache" / "gm_construct" / "t2.dat.txt", "x");
    put(root / "w" / "ghost.txt.txt", "x");
    auto ch = read_changes(data.u8string(), "c1", Mode::Overlay);
    CHECK(has(ch, "cache/index.txt", "modified", 11));
    CHECK(has(ch, "cache/gm_construct/new.dat", "created", 1));
    CHECK(has(ch, "fresh/a.txt", "created", 2));
    CHECK(has(ch, "emptydir", "created_dir"));
    CHECK(has(ch, "cache/gm_construct/t2.dat", "deleted"));
    CHECK(!has(ch, "ghost.txt", "deleted") && ch.size() == 5);
    CHECK(ch.front().path <= ch.back().path);  // sorted
    // The real files are untouched by reading.
    CHECK(fs::exists(data / "cache" / "gm_construct" / "t2.dat"));
    // Other token / invalid token / mode off / no folder.
    CHECK(read_changes(data.u8string(), "c2", Mode::Overlay).empty() && read_changes(data.u8string(), "../x", Mode::Overlay).empty() &&
          read_changes(data.u8string(), "c1", Mode::Off).empty());
    json j = changes_json(ch, Mode::Overlay, "c1");
    CHECK(j["mode"] == "overlay" && j["token"] == "c1" && j["total"] == 5 && j["changes"].size() == 5 && !j.contains("truncated"));
    CHECK(j["changes"][0].contains("path") && j["changes"][0].contains("change"));
    // Removing the folder.
    CHECK(remove_folder(data.u8string(), "c1") && !fs::exists(root));
    CHECK(remove_folder(data.u8string(), "c1"));         // a missing folder is fine
    CHECK(!remove_folder(data.u8string(), "../cache"));  // invalid token: nothing removed
    CHECK(fs::exists(data / "cache" / "index.txt"));
    std::error_code ec;
    fs::remove_all(data, ec);
}

static void test_audit_changes() {
    fs::path data = make_data();
    put(data / "cache" / "index.txt", "v1");
    put(data / "keep.txt", "k");
    put(data / "newfile.txt", "1234");  // the state at the end: written by the client
    put(data / "made" / "x.txt", "x");
    fs::path root = data / kFolder / "c1";
    std::string log =
        "W\t1\tcache/index.txt\n"       // existed, written: modified
        "W\t0\tnewfile.txt\n"           // did not exist, written: created
        "W\t0\ttemp.txt\nD\t1\ttemp.txt\n"  // created then deleted: nothing
        "D\t1\tgone.txt\n"              // existed, deleted
        "D\t1\tkeep.txt\nW\t0\tkeep.txt\n"  // deleted then written again: modified
        "M\t0\tmade\n"                  // a new folder
        "W\t1\tCache/Index.txt\n"       // the same file under another case: one entry
        "garbage line\nX\t1\tfoo\nW\t2\tbad\n";
    put(root / "audit.txt", log);
    auto ch = read_changes(data.u8string(), "c1", Mode::Audit);
    CHECK(has(ch, "cache/index.txt", "modified", 2));
    CHECK(has(ch, "newfile.txt", "created", 4));
    CHECK(!has(ch, "temp.txt", "created") && !has(ch, "temp.txt", "deleted"));
    CHECK(has(ch, "gone.txt", "deleted"));
    CHECK(has(ch, "keep.txt", "modified"));
    CHECK(has(ch, "made", "created_dir"));
    CHECK(ch.size() == 5);
    CHECK(read_changes(data.u8string(), "c2", Mode::Audit).empty());
    // CRLF line ends (a log edited or copied by a Windows tool) read the same.
    put(data / kFolder / "c5" / "audit.txt", std::string("W\t0\tnewfile.txt") + char(13) + "\n" + "D\t1\tgone.txt" + char(13) + "\n");
    auto crlf = read_changes(data.u8string(), "c5", Mode::Audit);
    CHECK(crlf.size() == 2 && has(crlf, "newfile.txt", "created", 4) && has(crlf, "gone.txt", "deleted"));
    // A client that wrote nothing has no audit file.
    fs::create_directories(data / kFolder / "c3");
    CHECK(read_changes(data.u8string(), "c3", Mode::Audit).empty());
    // More than kMaxChanges entries: the list is cut, the total is not.
    std::string big;
    for (size_t i = 0; i < kMaxChanges + 20; ++i) big += "W\t0\tf" + std::to_string(i) + ".txt\n";
    put(data / kFolder / "c4" / "audit.txt", big);
    auto many = read_changes(data.u8string(), "c4", Mode::Audit);
    json j = changes_json(many, Mode::Audit, "c4");
    CHECK(many.size() == kMaxChanges + 20 && j["changes"].size() == kMaxChanges && j["total"] == kMaxChanges + 20 && j["truncated"] == true);
    std::error_code ec;
    fs::remove_all(data, ec);
}

static void test_sweep() {
    fs::path data = make_data();
    put(data / kFolder / "old1" / "f" / "a.txt", "a");
    put(data / kFolder / "old2" / "audit.txt", "x");
    put(data / kFolder / "fresh" / "audit.txt", "x");
    put(data / kFolder / "kept" / "audit.txt", "x");
    put(data / kFolder / "bad name" / "audit.txt", "x");
    put(data / "user.txt", "u");
    auto old = fs::file_time_type::clock::now() - std::chrono::hours(48);
    std::error_code ec;
    for (const char *n : {"old1", "old2", "kept", "bad name"})
        for (fs::recursive_directory_iterator it(data / kFolder / n, ec), end; it != end; it.increment(ec)) fs::last_write_time(it->path(), old, ec);
    for (const char *n : {"old1", "old2", "kept", "bad name"}) fs::last_write_time(data / kFolder / n, old, ec);
    int n = sweep_old(data.u8string(), 24, {"kept"});
    CHECK(n == 2 && !fs::exists(data / kFolder / "old1") && !fs::exists(data / kFolder / "old2"));
    CHECK(fs::exists(data / kFolder / "fresh") && fs::exists(data / kFolder / "kept") && fs::exists(data / kFolder / "bad name"));  // young, listed, not a token
    CHECK(fs::exists(data / "user.txt"));
    CHECK(sweep_old((data / "nothing").u8string(), 24, {}) == 0);
    fs::remove_all(data, ec);
}

void run_data_overlay_tests(int *pass, int *fail) {
    test_tokens_and_modes();
    test_shim();
    test_overlay_changes();
    test_audit_changes();
    test_sweep();
    *pass = g_pass;
    *fail = g_fail;
}
