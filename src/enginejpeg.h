// `screenshot` without a client realm (main menu, a VGUI2 dialog, a loading screen): the engine's own `jpeg` command. The module
// sends `jpeg` through the engine path (the menu state's ClientCmd), waits for the console line the engine prints when it is
// done (`Wrote 'screenshots/<name>.jpg':  ... (WxH) ...`), reads that very file under garrysmod/ and deletes it, and nothing
// else. This file is the part that needs neither the game nor Lua: which source a screenshot uses, the parsing of the engine's
// line, the validation of the path it names, and the capture sequence with its game access injected (native unit tests drive
// it against a temporary folder). The tool itself is in tools_world.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace enginejpeg {

// --- Which source ---

enum class Source { Render, EngineJpeg };
const char *source_name(Source s);  // "render" | "engine_jpeg"

struct SourceFacts {
    std::string requested;       // the `source` argument: "", "auto", "render" or "engine_jpeg"
    bool client_mode = true;     // false: srcds (no window, no `jpeg` command)
    bool client_realm_present = false;
    bool client_allowed = true;  // the network lock (agent::client_allowed)
};
struct SourceChoice {
    Source source = Source::Render;
    std::string code, message;  // code != "": refused (unsupported, invalid_params, realm_unavailable, client_locked)
};
// "auto" (and no argument): the engine's render capture when the client realm exists, the engine's `jpeg` command otherwise. A
// forced source that cannot work is refused with its own code instead of falling back silently.
SourceChoice choose_source(const SourceFacts &f);

// --- The engine's console line ---

struct Wrote {
    std::string path;  // as the engine printed it, relative to the game folder (garrysmod/): "screenshots/Snapshot0000.jpg"
    int w = 0, h = 0;  // the size in the line (the backbuffer's)
};
// `Wrote 'screenshots/Snapshot0000.jpg':  691.20 KB (640x360) compresssed (quality 90) to 62.06 KB`. The name may hold spaces
// and quotes. Any other line (the `screenshot` command's, a user's echo of that text) is not one.
bool parse_wrote_line(const std::string &line, Wrote &out);
// The first line of `lines` (in the order they were printed) that parses.
bool find_wrote(const std::vector<std::string> &lines, Wrote &out);

// "" when `rel` may be read and deleted: exactly screenshots/<one name>.jpg, no "..", no drive, no backslash, no absolute path,
// no character a file name cannot hold. Otherwise why not.
std::string check_rel_path(const std::string &rel);
// The full path of `rel` under `game_dir` (the garrysmod folder), or "" with `err` set.
std::string resolve_path(const std::string &game_dir, const std::string &rel, std::string &err);
// The size in a JPEG's frame header (SOFn); false when `bytes` is not a JPEG that has one.
bool jpeg_size(const std::string &bytes, int &w, int &h);

// --- The capture ---

// Everything the capture needs from the game, injected (the tool wires the console ring buffer and the engine command; the tests
// wire a list of lines and a temporary folder).
struct Io {
    std::string game_dir;  // the garrysmod folder (absolute)
    double timeout_s = 4.0;
    int poll_ms = 20;
    std::function<uint64_t()> cursor_now;  // the console cursor
    // The console lines printed since `cursor` (which is advanced past them).
    std::function<std::vector<std::string>(uint64_t &cursor)> read_lines;
    // Sends `jpeg`; "" when sent, else why not (with the error code in `code`).
    std::function<std::string(std::string &code)> send;
    std::function<void(int ms)> sleep_ms;
    std::function<bool()> cancelled;
};
struct Capture {
    bool ok = false;
    std::string code, message;  // when !ok: capture_timeout | capture_failed | cancelled | (the sender's code)
    std::string data;           // the JPEG file's bytes
    std::string file;           // the path the engine printed
    std::string full_path;
    int w = 0, h = 0;
    bool deleted = false;
    std::string warning;  // the file could not be deleted (it names the path)
    double capture_ms = 0;
};
// One capture at a time per process: a second call waits for the first. Takes the cursor, sends `jpeg`, waits for the first
// `Wrote` line after the cursor, reads that file (it must be a regular file, not older than the command) and deletes it. A file
// it cannot read or that does not look like the capture is left where it is and named in the error.
Capture capture(const Io &io);

}  // namespace enginejpeg
