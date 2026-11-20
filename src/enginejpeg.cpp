#include "enginejpeg.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>

namespace enginejpeg {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

const char *source_name(Source s) { return s == Source::Render ? "render" : "engine_jpeg"; }

SourceChoice choose_source(const SourceFacts &f) {
    SourceChoice c;
    auto refuse = [&](const char *code, const char *message) {
        c.code = code;
        c.message = message;
        return c;
    };
    const std::string &want = f.requested;
    if (!want.empty() && want != "auto" && want != "render" && want != "engine_jpeg")
        return refuse("invalid_params", "source must be \"auto\", \"render\" or \"engine_jpeg\"");
    if (!f.client_mode) return refuse("unsupported", "no renderer and no game window in srcds mode");
    if (want == "render") {
        if (!f.client_realm_present)
            return refuse("realm_unavailable",
                          "source \"render\" needs the client realm: load a map first, or use source \"engine_jpeg\" (the engine's own jpeg command: it "
                          "works without one, at the main menu, in a dialog or on a loading screen)");
        c.source = Source::Render;
        return c;
    }
    if (want == "engine_jpeg") {
        if (f.client_realm_present && !f.client_allowed)
            return refuse("client_locked", "the client is connected to a server that is not on this machine: its screen is not captured either");
        c.source = Source::EngineJpeg;
        return c;
    }
    // auto: a locked client is answered by the render path itself (client_locked)
    c.source = f.client_realm_present ? Source::Render : Source::EngineJpeg;
    return c;
}

// --- The engine's console line ---

bool parse_wrote_line(const std::string &line, Wrote &out) {
    size_t at = line.find_first_not_of(" \t");
    if (at == std::string::npos || line.compare(at, 7, "Wrote '") != 0) return false;
    // Greedy name (it may hold quotes), then the size the engine prints right after it: "':  691.20 KB (640x360)".
    static const std::regex re(R"(^\s*Wrote '(.+)':\s+[0-9][0-9.,]*\s*[KMGT]?B\s+\((\d{1,5})x(\d{1,5})\))");
    std::smatch m;
    if (!std::regex_search(line, m, re)) return false;
    int w = std::stoi(m[2].str()), h = std::stoi(m[3].str());
    if (w <= 0 || h <= 0) return false;
    out.path = m[1].str();
    out.w = w;
    out.h = h;
    return true;
}

bool find_wrote(const std::vector<std::string> &lines, Wrote &out) {
    for (const std::string &l : lines)
        if (parse_wrote_line(l, out)) return true;
    return false;
}

// --- The path ---

static std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A device name Windows resolves in any folder, extension or not ("CON.jpg" is the console).
static bool windows_device_name(const std::string &stem) {
    std::string s = lower(stem);
    if (s == "con" || s == "prn" || s == "aux" || s == "nul") return true;
    return s.size() == 4 && (s.compare(0, 3, "com") == 0 || s.compare(0, 3, "lpt") == 0) && s[3] >= '0' && s[3] <= '9';
}

std::string check_rel_path(const std::string &rel) {
    if (rel.empty()) return "empty path";
    size_t slash = rel.find('/');
    if (slash == std::string::npos) return "not in the screenshots folder";
    if (lower(rel.substr(0, slash)) != "screenshots") return "not in the screenshots folder";
    std::string name = rel.substr(slash + 1);
    if (name.empty()) return "no file name";
    if (name.find('/') != std::string::npos) return "not a file directly in the screenshots folder";
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (c < 0x20 || c == 0x7F || std::string("<>:\"|?*\\").find(ch) != std::string::npos) return "the file name holds a character a screenshot name does not";
    }
    if (name == "." || name == ".." || name.size() <= 4 || lower(name.substr(name.size() - 4)) != ".jpg") return "not a .jpg file";
    if (windows_device_name(name.substr(0, name.size() - 4))) return "a device name";
    return "";
}

std::string resolve_path(const std::string &game_dir, const std::string &rel, std::string &err) {
    if (game_dir.empty()) {
        err = "the game folder is unknown";
        return "";
    }
    std::string why = check_rel_path(rel);
    if (!why.empty()) {
        err = "'" + rel + "' is not screenshots/<name>.jpg: " + why;
        return "";
    }
    std::string base = game_dir;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
    return base + "/" + rel;
}

bool jpeg_size(const std::string &b, int &w, int &h) {
    auto u = [&](size_t i) { return (unsigned)(unsigned char)b[i]; };
    if (b.size() < 4 || u(0) != 0xFF || u(1) != 0xD8) return false;
    size_t o = 2;
    while (o + 4 <= b.size()) {
        if (u(o) != 0xFF) return false;
        unsigned m = u(o + 1);
        if (m == 0xFF) {  // fill byte
            ++o;
            continue;
        }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) {  // markers without a length
            o += 2;
            continue;
        }
        if (m == 0xD9 || m == 0xDA) return false;  // end of image or start of scan before any frame header
        size_t len = (u(o + 2) << 8) | u(o + 3);
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (o + 9 > b.size()) return false;
            int hh = (int)((u(o + 5) << 8) | u(o + 6)), ww = (int)((u(o + 7) << 8) | u(o + 8));
            if (ww <= 0 || hh <= 0) return false;
            w = ww;
            h = hh;
            return true;
        }
        if (len < 2) return false;
        o += 2 + len;
    }
    return false;
}

// --- The capture ---

namespace {

enum class Take { Done, Retry, Fatal };

// Reads the file the engine named and deletes it. Retry: the file is not there (yet) or does not look like a JPEG (yet).
Take take_file(const Io &io, const Wrote &wrote, fs::file_time_type not_before, Capture &out, std::string &problem) {
    std::string err;
    std::string full = resolve_path(io.game_dir, wrote.path, err);
    if (full.empty()) {
        problem = err + "; nothing was read or deleted";
        return Take::Fatal;
    }
    std::error_code ec;
    fs::path p = fs::u8path(full);
    fs::file_status st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) {
        problem = "the file '" + wrote.path + "' the engine named does not exist";
        return Take::Retry;
    }
    if (!fs::is_regular_file(st)) {
        problem = "'" + wrote.path + "' is not a regular file (a link or a folder): left alone";
        return Take::Fatal;
    }
    fs::file_time_type mtime = fs::last_write_time(p, ec);
    if (!ec && mtime < not_before) {
        problem = "'" + wrote.path + "' is older than the command (an earlier picture under the same name): left in place";
        return Take::Fatal;
    }
    std::string data;
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            problem = "cannot open '" + wrote.path + "' for reading";
            return Take::Retry;
        }
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    int w = 0, h = 0;
    if (!jpeg_size(data, w, h)) {
        problem = "'" + wrote.path + "' is not a readable JPEG (" + std::to_string(data.size()) + " bytes): left in place";
        return Take::Retry;
    }
    out.data = std::move(data);
    out.file = wrote.path;
    out.full_path = full;
    out.w = w;
    out.h = h;
    // Delete exactly this file.
    ec.clear();
    fs::remove(p, ec);
    out.deleted = !fs::exists(fs::symlink_status(p, ec));
    if (!out.deleted) out.warning = "could not delete the engine's picture '" + full + "': delete it by hand";
    return Take::Done;
}

}  // namespace

Capture capture(const Io &io) {
    static std::mutex serial;
    std::lock_guard<std::mutex> one_at_a_time(serial);
    auto t0 = Clock::now();
    auto elapsed_ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
    Capture out;
    auto fail = [&](const char *code, const std::string &message) {
        out.ok = false;
        out.code = code;
        out.message = message;
        out.capture_ms = elapsed_ms();
        return out;
    };
    if (io.game_dir.empty()) return fail("capture_failed", "the game folder is unknown, so the engine's picture cannot be read");

    uint64_t cursor = io.cursor_now();
    fs::file_time_type not_before = fs::file_time_type::clock::now() - std::chrono::seconds(2);  // slack for file system time granularity
    std::string code;
    std::string why = io.send(code);
    if (!why.empty()) return fail(code.empty() ? "capture_failed" : code.c_str(), why);

    Wrote wrote;
    bool found = false;
    std::string problem;
    for (;;) {
        if (io.cancelled && io.cancelled()) return fail("cancelled", "screenshot cancelled");
        if (!found) found = find_wrote(io.read_lines(cursor), wrote);
        if (found) {
            Take t = take_file(io, wrote, not_before, out, problem);
            if (t == Take::Done) {
                out.ok = true;
                out.capture_ms = elapsed_ms();
                return out;
            }
            if (t == Take::Fatal) return fail("capture_failed", problem);
        }
        if (std::chrono::duration<double>(Clock::now() - t0).count() >= io.timeout_s) break;
        io.sleep_ms(io.poll_ms);
    }
    if (found) return fail("capture_failed", "the engine reported its picture but it could not be taken: " + problem);
    return fail("capture_timeout", "the engine's jpeg command printed no 'Wrote ...' line within " + std::to_string((int)io.timeout_s) +
                                       " s (game minimized or frozen, or the console capture is off)");
}

}  // namespace enginejpeg
