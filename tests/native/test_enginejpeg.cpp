// `screenshot` through the engine's `jpeg` command (src/enginejpeg.cpp): the source decision, the parsing of the console line the
// engine prints, the validation of the path it names, and the capture sequence (cursor, send, wait for the line, read the file,
// delete it) against a temporary folder with the game faked. No game, no Lua.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "enginejpeg.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace enginejpeg;
namespace fs = std::filesystem;

static bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

// --- Which source ---

static SourceFacts facts(const char *requested, bool client_realm, bool allowed = true) {
    SourceFacts f;
    f.requested = requested;
    f.client_realm_present = client_realm;
    f.client_allowed = allowed;
    return f;
}

static void test_source() {
    CHECK(std::string(source_name(Source::Render)) == "render");
    CHECK(std::string(source_name(Source::EngineJpeg)) == "engine_jpeg");

    // auto (and no argument): the render capture when the client realm exists, the engine's jpeg otherwise.
    SourceChoice c = choose_source(facts("auto", true));
    CHECK(c.code.empty() && c.source == Source::Render);
    c = choose_source(facts("", true));
    CHECK(c.code.empty() && c.source == Source::Render);
    c = choose_source(facts("auto", false));
    CHECK(c.code.empty() && c.source == Source::EngineJpeg);
    c = choose_source(facts("", false));
    CHECK(c.code.empty() && c.source == Source::EngineJpeg);

    // Forced: render needs the client realm, engine_jpeg works with or without it.
    c = choose_source(facts("render", true));
    CHECK(c.code.empty() && c.source == Source::Render);
    c = choose_source(facts("render", false));
    CHECK(c.code == "realm_unavailable" && contains(c.message, "engine_jpeg"));
    c = choose_source(facts("engine_jpeg", true));
    CHECK(c.code.empty() && c.source == Source::EngineJpeg);
    c = choose_source(facts("engine_jpeg", false));
    CHECK(c.code.empty() && c.source == Source::EngineJpeg);

    // A locked client (connected to a server that is not on this machine): auto keeps the render path, which answers
    // client_locked itself; a forced engine_jpeg is refused too (the lock covers what the client shows).
    c = choose_source(facts("auto", true, false));
    CHECK(c.code.empty() && c.source == Source::Render);
    c = choose_source(facts("engine_jpeg", true, false));
    CHECK(c.code == "client_locked");
    // Without a client realm there is nothing to lock.
    c = choose_source(facts("auto", false, false));
    CHECK(c.code.empty() && c.source == Source::EngineJpeg);

    // srcds has no window and no jpeg command.
    SourceFacts f = facts("auto", false);
    f.client_mode = false;
    CHECK(choose_source(f).code == "unsupported");
    f = facts("engine_jpeg", false);
    f.client_mode = false;
    CHECK(choose_source(f).code == "unsupported");

    // An unknown value, among them the removed window_capture.
    c = choose_source(facts("gpu", true));
    CHECK(c.code == "invalid_params" && contains(c.message, "engine_jpeg"));
    CHECK(choose_source(facts("window_capture", false)).code == "invalid_params");
}

// --- The console line ---

static void test_parse() {
    Wrote w;
    // The line the engine printed in a live game.
    CHECK(parse_wrote_line("Wrote 'screenshots/Snapshot0000.jpg':  691.20 KB (640x360) compresssed (quality 90) to 62.06 KB", w));
    CHECK(w.path == "screenshots/Snapshot0000.jpg" && w.w == 640 && w.h == 360);
    // The console keeps the line break; a console prefix of spaces is allowed.
    w = {};
    CHECK(parse_wrote_line("  Wrote 'screenshots/gm_flatgrass0003.jpg':  2.64 MB (1280x720) compresssed (quality 70) to 180.00 KB\r\n", w));
    CHECK(w.path == "screenshots/gm_flatgrass0003.jpg" && w.w == 1280 && w.h == 720);
    // A name with spaces, and one with a quote in it.
    w = {};
    CHECK(parse_wrote_line("Wrote 'screenshots/my shot 01.jpg':  10.00 KB (64x32) compresssed (quality 90) to 1.00 KB", w));
    CHECK(w.path == "screenshots/my shot 01.jpg" && w.w == 64 && w.h == 32);
    w = {};
    CHECK(parse_wrote_line("Wrote 'screenshots/it's.jpg':  10.00 KB (64x32) compresssed (quality 90) to 1.00 KB", w));
    CHECK(w.path == "screenshots/it's.jpg");
    // The path is returned as printed, validation is a separate step (a hostile name still parses).
    w = {};
    CHECK(parse_wrote_line("Wrote '../../x.jpg':  10.00 KB (64x32) compresssed (quality 90) to 1.00 KB", w));
    CHECK(w.path == "../../x.jpg");

    // Not the line: other screenshot commands, text that merely contains it, partial lines.
    CHECK(!parse_wrote_line("Wrote screenshots/Snapshot0000.jpg", w));
    CHECK(!parse_wrote_line("Wrote 'screenshots/Snapshot0000.jpg'", w));
    CHECK(!parse_wrote_line("Wrote 'screenshots/Snapshot0000.jpg':", w));
    CHECK(!parse_wrote_line("Wrote 'screenshots/Snapshot0000.jpg': done", w));
    CHECK(!parse_wrote_line("echo Wrote 'screenshots/Snapshot0000.jpg':  1.00 KB (1x1) compresssed", w));
    CHECK(!parse_wrote_line("] jpeg", w));
    CHECK(!parse_wrote_line("", w));
    CHECK(!parse_wrote_line("Wrote '':  1.00 KB (1x1)", w));
    CHECK(!parse_wrote_line("Wrote 'screenshots/a.jpg':  1.00 KB (0x0)", w));

    // The first matching line wins; the others (a user's own jpeg after ours) do not.
    std::vector<std::string> lines = {"Sound: init", "Wrote 'screenshots/a.jpg':  1.00 KB (8x8) compresssed (quality 90) to 1.00 KB",
                                      "Wrote 'screenshots/b.jpg':  1.00 KB (8x8) compresssed (quality 90) to 1.00 KB"};
    w = {};
    CHECK(find_wrote(lines, w) && w.path == "screenshots/a.jpg");
    CHECK(!find_wrote({"Sound: init", "Wrote 'x'", "screenshots/a.jpg"}, w));
    CHECK(!find_wrote({}, w));
}

// --- The path ---

static void test_paths() {
    CHECK(check_rel_path("screenshots/Snapshot0000.jpg").empty());
    CHECK(check_rel_path("screenshots/my shot 01.jpg").empty());
    CHECK(check_rel_path("Screenshots/A.JPG").empty());  // Windows is case-insensitive
    CHECK(check_rel_path("screenshots/it's.jpg").empty());
    for (const char *bad : {"", "screenshots", "screenshots/", "screenshots/.jpg/", "../screenshots/a.jpg", "screenshots/../a.jpg",
                            "screenshots/../../a.jpg", "screenshots/..", "screenshots/sub/a.jpg", "other/a.jpg", "a.jpg", "/screenshots/a.jpg",
                            "screenshots\\a.jpg", "screenshots/a.png", "screenshots/a.jpg.png", "screenshots/a", "screenshots//a.jpg",
                            "C:/screenshots/a.jpg", "screenshots/C:a.jpg", "screenshots/a:b.jpg", "screenshots/a?.jpg", "screenshots/a*.jpg",
                            "screenshots/a<b.jpg", "screenshots/a|b.jpg", "screenshots/a\"b.jpg", "garrysmod/screenshots/a.jpg",
                            "screenshots/ .jpg/..", "./screenshots/a.jpg", "screenshots/./a.jpg"})
        CHECK(!check_rel_path(bad).empty());
    std::string with_nul = std::string("screenshots/a") + '\0' + "b.jpg";
    CHECK(!check_rel_path(with_nul).empty());
    CHECK(!check_rel_path("screenshots/a\nb.jpg").empty());

    std::string err;
    std::string p = resolve_path("D:/Games/garrysmod", "screenshots/a.jpg", err);
    CHECK(err.empty() && p == "D:/Games/garrysmod/screenshots/a.jpg");
    p = resolve_path("D:/Games/garrysmod/", "screenshots/a.jpg", err);  // a trailing slash is not doubled
    CHECK(err.empty() && p == "D:/Games/garrysmod/screenshots/a.jpg");
    err.clear();
    p = resolve_path("D:/Games/garrysmod", "../x.jpg", err);
    CHECK(p.empty() && !err.empty());
    err.clear();
    p = resolve_path("", "screenshots/a.jpg", err);
    CHECK(p.empty() && !err.empty());
}

static std::string tiny_jpeg(int w, int h) {
    std::string s = "\xFF\xD8";
    s += std::string("\xFF\xE0\x00\x10JFIF\0\x01\x01\x00\x00\x01\x00\x01\x00\x00", 18);  // APP0
    s += std::string("\xFF\xDB\x00\x43\x00", 5) + std::string(64, '\x08');                // DQT
    s += std::string("\xFF\xC0\x00\x11\x08", 5);                                          // SOF0, 8 bits
    s += (char)(h >> 8);
    s += (char)(h & 255);
    s += (char)(w >> 8);
    s += (char)(w & 255);
    s += std::string("\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01", 10);
    s += std::string("\xFF\xDA\x00\x08\x01\x01\x00\x00\x3F\x00", 10) + std::string("\x00\x00", 2) + "\xFF\xD9";
    return s;
}

static void test_jpeg_size() {
    int w = 0, h = 0;
    CHECK(jpeg_size(tiny_jpeg(640, 360), w, h) && w == 640 && h == 360);
    CHECK(jpeg_size(tiny_jpeg(1920, 1080), w, h) && w == 1920 && h == 1080);
    CHECK(!jpeg_size("", w, h));
    CHECK(!jpeg_size("not a jpeg at all", w, h));
    CHECK(!jpeg_size("\xFF\xD8", w, h));
    CHECK(!jpeg_size(tiny_jpeg(640, 360).substr(0, 30), w, h));  // cut before the frame header
    std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(40, '\0');
    CHECK(!jpeg_size(png, w, h));
}

// --- The capture ---

namespace {

struct Fake {
    fs::path root, game;  // root = a temporary folder, game = root/garrysmod
    std::mutex mu;
    std::vector<std::string> console;
    std::vector<std::string> sent;  // the commands the module sent
    std::function<void(Fake &)> on_send;  // what the "engine" does when `jpeg` arrives
    std::string send_error, send_code;
    bool cancel = false;
    std::atomic<int> active{0}, max_active{0};

    Fake() {
        static int n = 0;
#ifdef _WIN32
        unsigned long pid = GetCurrentProcessId();
#else
        unsigned long pid = (unsigned long)getpid();
#endif
        root = fs::temp_directory_path() / ("gmcp_jpeg_test_" + std::to_string(pid) + "_" + std::to_string(++n));
        game = root / "garrysmod";
        fs::create_directories(game / "screenshots");
    }
    ~Fake() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void say(const std::string &l) {
        std::lock_guard<std::mutex> g(mu);
        console.push_back(l);
    }
    Io io() {
        Io o;
        o.game_dir = game.string();
        o.timeout_s = 0.4;
        o.poll_ms = 5;
        o.cursor_now = [this] {
            std::lock_guard<std::mutex> g(mu);
            return (uint64_t)console.size();
        };
        o.read_lines = [this](uint64_t &cur) {
            std::lock_guard<std::mutex> g(mu);
            std::vector<std::string> out;
            for (; cur < console.size(); ++cur) out.push_back(console[(size_t)cur]);
            return out;
        };
        o.send = [this](std::string &code) -> std::string {
            int a = ++active;
            int m = max_active.load();
            while (a > m && !max_active.compare_exchange_weak(m, a)) {}
            {
                std::lock_guard<std::mutex> g(mu);
                sent.push_back("jpeg");
            }
            if (!send_error.empty()) {
                code = send_code;
                --active;
                return send_error;
            }
            if (on_send) on_send(*this);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));  // the engine takes a frame: lets a second caller try to overlap
            --active;
            return "";
        };
        o.sleep_ms = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        o.cancelled = [this] { return cancel; };
        return o;
    }
    void write(const fs::path &p, const std::string &bytes) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f.write(bytes.data(), (std::streamsize)bytes.size());
    }
    static std::string wrote(const std::string &name, int w = 640, int h = 360) {
        return "Wrote '" + name + "':  691.20 KB (" + std::to_string(w) + "x" + std::to_string(h) + ") compresssed (quality 90) to 62.06 KB";
    }
    // The engine writes screenshots/<name>, then prints the line.
    std::function<void(Fake &)> engine_writes(const std::string &name, const std::string &bytes, int w = 640, int h = 360) {
        return [name, bytes, w, h](Fake &f) {
            f.write(f.game / name, bytes);
            f.say(Fake::wrote(name, w, h));
        };
    }
};

static std::vector<std::string> listing(const fs::path &dir) {
    std::vector<std::string> v;
    for (auto &e : fs::recursive_directory_iterator(dir)) v.push_back(fs::relative(e.path(), dir).generic_string());
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

static void test_capture_success() {
    Fake f;
    // Files that are not ours: a user's earlier screenshot and a neighbor file. They must survive, byte for byte.
    f.write(f.game / "screenshots/Snapshot0000.jpg", "user's own earlier picture");
    f.write(f.game / "screenshots/notes.txt", "keep");
    f.say(Fake::wrote("screenshots/Snapshot0000.jpg"));  // its console line is from before the cursor
    std::string jpg = tiny_jpeg(640, 360);
    f.on_send = f.engine_writes("screenshots/Snapshot0001.jpg", jpg);
    Capture c = capture(f.io());
    CHECK(c.ok && c.code.empty());
    CHECK(c.data == jpg);
    CHECK(c.file == "screenshots/Snapshot0001.jpg" && c.w == 640 && c.h == 360 && c.deleted && c.warning.empty());
    CHECK(c.full_path.find("Snapshot0001.jpg") != std::string::npos);
    CHECK(f.sent.size() == 1);
    // Exactly our file is gone; the user's file (its line was before our cursor) and the neighbor are intact.
    CHECK(!fs::exists(f.game / "screenshots/Snapshot0001.jpg"));
    CHECK((listing(f.game) == std::vector<std::string>{"screenshots", "screenshots/Snapshot0000.jpg", "screenshots/notes.txt"}));
    std::ifstream u(f.game / "screenshots/Snapshot0000.jpg", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(u)), std::istreambuf_iterator<char>());
    CHECK(content == "user's own earlier picture");

    // The size comes from the JPEG header when the line and the header disagree (the file is the truth).
    Fake g;
    g.on_send = g.engine_writes("screenshots/Snapshot0000.jpg", tiny_jpeg(800, 450), 640, 360);
    c = capture(g.io());
    CHECK(c.ok && c.w == 800 && c.h == 450);
    // A name with spaces.
    Fake s;
    s.on_send = s.engine_writes("screenshots/my shot 01.jpg", jpg);
    c = capture(s.io());
    CHECK(c.ok && c.file == "screenshots/my shot 01.jpg" && c.deleted && !fs::exists(s.game / "screenshots/my shot 01.jpg"));
}

static void test_capture_cursor() {
    // Other screenshot lines around ours: only the first `Wrote` line after the cursor counts, an older one (before the cursor)
    // and the `screenshot` command's own wording (a different format) never do.
    Fake f;
    std::string jpg = tiny_jpeg(640, 360);
    f.write(f.game / "screenshots/older.jpg", "older");
    f.say(Fake::wrote("screenshots/older.jpg"));
    f.on_send = [&](Fake &x) {
        x.say("] jpeg");
        x.say("Writing screenshots/other0000.tga");  // a user's `screenshot`, wording of another command
        x.write(x.game / "screenshots/ours.jpg", jpg);
        x.say(Fake::wrote("screenshots/ours.jpg"));
        x.write(x.game / "screenshots/theirs.jpg", "theirs");  // a user's own jpeg right after ours
        x.say(Fake::wrote("screenshots/theirs.jpg"));
    };
    Capture c = capture(f.io());
    CHECK(c.ok && c.file == "screenshots/ours.jpg" && c.data == jpg);
    CHECK((listing(f.game) == std::vector<std::string>{"screenshots", "screenshots/older.jpg", "screenshots/theirs.jpg"}));

    // The line arrives a little after the command (the engine runs it on its next frame).
    Fake d;
    d.on_send = [&](Fake &x) {
        std::thread([&x, jpg] {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            x.write(x.game / "screenshots/late.jpg", jpg);
            x.say(Fake::wrote("screenshots/late.jpg"));
        }).detach();
    };
    auto io = d.io();
    io.timeout_s = 2.0;
    c = capture(io);
    CHECK(c.ok && c.file == "screenshots/late.jpg" && !fs::exists(d.game / "screenshots/late.jpg"));
}

static void test_capture_failures() {
    std::string jpg = tiny_jpeg(640, 360);
    // No line within the timeout: capture_timeout, nothing touched. An old screenshot line is not an answer.
    {
        Fake f;
        f.write(f.game / "screenshots/older.jpg", "older");
        f.say(Fake::wrote("screenshots/older.jpg"));
        Capture c = capture(f.io());
        CHECK(!c.ok && c.code == "capture_timeout" && !c.message.empty());
        CHECK(fs::exists(f.game / "screenshots/older.jpg"));
    }
    // The sender fails: its code and message come back and nothing is waited for.
    {
        Fake f;
        f.send_error = "menu state unavailable";
        f.send_code = "realm_unavailable";
        auto t0 = std::chrono::steady_clock::now();
        auto io = f.io();
        io.timeout_s = 5.0;
        Capture c = capture(io);
        CHECK(!c.ok && c.code == "realm_unavailable" && contains(c.message, "menu state unavailable"));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    }
    // Cancelled while waiting.
    {
        Fake f;
        f.on_send = [](Fake &x) { x.cancel = true; };
        auto io = f.io();
        io.timeout_s = 5.0;
        auto t0 = std::chrono::steady_clock::now();
        Capture c = capture(io);
        CHECK(!c.ok && c.code == "cancelled" && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    }
    // A line naming a path that is not screenshots/<name>.jpg: refused, and the file it names is neither read nor deleted.
    for (const char *bad : {"../outside.jpg", "../../outside.jpg", "screenshots/../../outside.jpg", "screenshots/sub/inner.jpg", "other/x.jpg", "screenshots/x.png",
                            "/etc/x.jpg", "screenshots\\..\\..\\outside.jpg", "C:/Windows/x.jpg"}) {
        Fake f;
        f.write(f.root / "outside.jpg", "outside");
        f.write(f.game / "screenshots/sub/inner.jpg", jpg);
        f.write(f.game / "other/x.jpg", jpg);
        f.write(f.game / "screenshots/x.png", jpg);
        auto before = listing(f.root);
        std::string name = bad;
        f.on_send = [name](Fake &x) { x.say(Fake::wrote(name)); };
        Capture c = capture(f.io());
        CHECK(!c.ok && c.code == "capture_failed" && contains(c.message, "screenshots"));
        CHECK(c.data.empty() && listing(f.root) == before);
    }
    // A file the engine named that is older than the command (an old picture under a reused name): left alone.
    {
        Fake f;
        f.write(f.game / "screenshots/old.jpg", jpg);
        fs::last_write_time(f.game / "screenshots/old.jpg", fs::file_time_type::clock::now() - std::chrono::hours(1));
        f.on_send = [](Fake &x) { x.say(Fake::wrote("screenshots/old.jpg")); };
        Capture c = capture(f.io());
        CHECK(!c.ok && c.code == "capture_failed" && contains(c.message, "screenshots/old.jpg") && c.data.empty());
        CHECK(fs::exists(f.game / "screenshots/old.jpg"));
    }
    // A file that is not a JPEG: left in place, named.
    {
        Fake f;
        f.on_send = f.engine_writes("screenshots/bad.jpg", "this is not an image");
        Capture c = capture(f.io());
        CHECK(!c.ok && c.code == "capture_failed" && contains(c.message, "screenshots/bad.jpg"));
        CHECK(fs::exists(f.game / "screenshots/bad.jpg"));
    }
    // The line is printed but the file never shows up: capture_failed naming it, after the timeout.
    {
        Fake f;
        f.on_send = [](Fake &x) { x.say(Fake::wrote("screenshots/ghost.jpg")); };
        Capture c = capture(f.io());
        CHECK(!c.ok && c.code == "capture_failed" && contains(c.message, "screenshots/ghost.jpg"));
    }
    // A directory with the file's name: not a regular file, never read or removed.
    {
        Fake f;
        fs::create_directories(f.game / "screenshots/dir.jpg");
        f.write(f.game / "screenshots/dir.jpg/inside.txt", "inside");
        f.on_send = [](Fake &x) { x.say(Fake::wrote("screenshots/dir.jpg")); };
        Capture c = capture(f.io());
        CHECK(!c.ok && c.code == "capture_failed");
        CHECK(fs::exists(f.game / "screenshots/dir.jpg/inside.txt"));
    }
    // An unknown game folder.
    {
        Fake f;
        auto io = f.io();
        io.game_dir = "";
        f.on_send = f.engine_writes("screenshots/a.jpg", jpg);
        Capture c = capture(io);
        CHECK(!c.ok && c.code == "capture_failed");
    }
}

#ifdef _WIN32
static void test_capture_delete_failure() {
    // The picture is read but cannot be deleted (another program keeps it open without delete sharing): the result is still the
    // image, `deleted` is false and the warning names the path. Nothing else is touched.
    Fake f;
    std::string jpg = tiny_jpeg(640, 360);
    HANDLE h = INVALID_HANDLE_VALUE;
    f.on_send = [&](Fake &x) {
        x.write(x.game / "screenshots/held.jpg", jpg);
        h = CreateFileW((x.game / "screenshots/held.jpg").wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        x.say(Fake::wrote("screenshots/held.jpg"));
    };
    Capture c = capture(f.io());
    CHECK(c.ok && c.data == jpg && !c.deleted && contains(c.warning, "held.jpg"));
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(fs::exists(f.game / "screenshots/held.jpg"));
}
#endif

static void test_capture_serialized() {
    // Two captures at once (two tool calls, or screenshot_after next to a screenshot) never overlap: the second starts its
    // command after the first has its file, so each reads its own picture.
    Fake f;
    std::atomic<int> n{0};
    f.on_send = [&](Fake &x) {
        int k = ++n;
        std::string name = "screenshots/Snapshot000" + std::to_string(k) + ".jpg";
        x.write(x.game / name, tiny_jpeg(100 + k, 50));
        x.say(Fake::wrote(name, 100 + k, 50));
    };
    Capture a, b;
    std::thread t1([&] { a = capture(f.io()); });
    std::thread t2([&] { b = capture(f.io()); });
    t1.join();
    t2.join();
    CHECK(a.ok && b.ok && f.max_active.load() == 1);
    CHECK(a.file != b.file && a.deleted && b.deleted);
    CHECK((a.w == 101 && b.w == 102) || (a.w == 102 && b.w == 101));
    CHECK((listing(f.game) == std::vector<std::string>{"screenshots"}));
}

void test_enginejpeg() {
    test_source();
    test_parse();
    test_paths();
    test_jpeg_size();
    test_capture_success();
    test_capture_cursor();
    test_capture_failures();
#ifdef _WIN32
    test_capture_delete_failure();
#endif
    test_capture_serialized();
}
