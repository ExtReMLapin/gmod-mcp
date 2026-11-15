#include "installer.h"

#include <filesystem>
#include <vector>

#include "platform/platform.h"

namespace installer {

static const char *kBegin = "-- MCPAGENT BEGIN";
static const char *kEnd = "-- MCPAGENT END";

// Bump the version whenever the block content changes: installed copies are then rewritten automatically.
// Kept simple on purpose:
//  - everything runs inside pcall: a missing or broken module never breaks the menu;
//  - a quick sanity check of the file (binary signature, minimum size) avoids the blocking Windows "bad image" dialog
//    for an obviously wrong or empty file;
//  - the crash guard (no crash loop) lives in the module itself: see crashguard.h.
static const char *kBlockSource = R"LUA(-- MCPAGENT BEGIN v7 (written by the gmod-mcp module; anything inside this block is overwritten)
pcall( function()
	local suffix = ( system.IsWindows() and "win" or system.IsLinux() and "linux" or "osx" ) .. ( jit.arch == "x64" and "64" or ( system.IsWindows() and "32" or "" ) )
	local path = "lua/bin/gmsv_mcpagent_" .. suffix .. ".dll"
	local f = file.Open( path, "rb", "GAME" )
	if ( not f ) then return end
	local head, size = f:Read( 4 ) or "", f:Size()
	f:Close()
	if ( size < 65536 or ( string.sub( head, 1, 2 ) ~= "MZ" and head ~= "\127ELF" ) ) then
		MsgN( "[mcpagent] not loaded: ", path, " is not a valid module (corrupted download?)" )
		return
	end
	local ok, err = pcall( require, "mcpagent" )
	if ( not ok ) then MsgN( "[mcpagent] failed to load: ", err ) end
end )
-- MCPAGENT END)LUA";

static std::string menu_path(const std::string &game_dir) {
    return (std::filesystem::u8path(game_dir) / "lua" / "menu" / "menu.lua").u8string();
}

static std::string block(const std::string &eol) {
    std::string s;
    for (const char *p = kBlockSource; *p; ++p) {
        if (*p == '\r') continue;  // the source file's own line endings must not leak into menu.lua
        if (*p == '\n') s += eol;
        else s += *p;
    }
    return s;
}

// Locates the existing block: [begin, end) covers from the BEGIN line start to the end of the END line (excluded EOL).
static bool find_block(const std::string &s, size_t &begin, size_t &end) {
    size_t b = s.find(kBegin);
    if (b == std::string::npos) return false;
    size_t e = s.find(kEnd, b);
    if (e == std::string::npos) return false;
    begin = s.rfind('\n', b);
    begin = begin == std::string::npos ? 0 : begin + 1;
    end = e + std::string(kEnd).size();
    while (end < s.size() && s[end] != '\r' && s[end] != '\n') ++end;  // rest of the END line
    return true;
}

static bool write_atomic(const std::string &path, const std::string &data, std::string &err) {
    std::string tmp = path + ".mcpagent.tmp";
    if (!plat::write_file(tmp, data)) {
        err = "cannot write " + tmp;
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
    if (ec) {
        plat::remove_file(tmp);
        err = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

std::string ensure_menu_patch(const std::string &game_dir, std::string &err, const CompileCheck &compiles) {
    std::string path = menu_path(game_dir), s;
    if (!plat::read_file(path, s)) {
        err = "cannot read " + path;
        return "";
    }
    std::string eol = s.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string want = block(eol);
    size_t b = 0, e = 0;
    std::string out, state;
    if (find_block(s, b, e)) {
        if (s.compare(b, e - b, want) == 0) return "unchanged";
        out = s.substr(0, b) + want + s.substr(e);
        state = "updated";
    } else {
        std::string bak = path + ".mcpagent.bak";
        if (!plat::file_exists(bak) && !plat::write_file(bak, s)) {
            err = "cannot back up " + path;
            return "";
        }
        // Always one line break, the block, one line break: remove_menu_patch takes exactly these two away, so the
        // original comes back byte for byte (with or without a final line break, empty, CRLF...).
        out = s + eol + want + eol;
        state = "installed";
    }
    // Never write a menu.lua that does not compile.
    if (compiles) {
        std::string cerr;
        if (!compiles(out, cerr)) {
            err = "patched menu.lua would not compile, left untouched: " + cerr;
            return "";
        }
    }
    if (!write_atomic(path, out, err)) return "";
    return state;
}

bool remove_menu_patch(const std::string &game_dir, std::string &err) {
    std::string path = menu_path(game_dir), s;
    if (!plat::read_file(path, s)) {
        err = "cannot read " + path;
        return false;
    }
    size_t b = 0, e = 0;
    if (!find_block(s, b, e)) return true;
    // Take away exactly what ensure_menu_patch added: the line break before the block and the one after it, in the
    // block's own line-ending style.
    std::string eol = s.find('\r', b) < e ? "\r\n" : "\n";
    size_t after = e;
    if (s.compare(after, eol.size(), eol) == 0) after += eol.size();
    size_t before = b;
    if (before >= eol.size() && s.compare(before - eol.size(), eol.size(), eol) == 0) before -= eol.size();
    std::string out = s.substr(0, before) + s.substr(after);
    // Installed by an older version, which added a blank line even when the original had no final line break: the
    // backup of the original says so when it is exactly the result without one line break.
    std::string orig;
    if (out.size() >= eol.size() && out.compare(out.size() - eol.size(), eol.size(), eol) == 0 &&
        plat::read_file(path + ".mcpagent.bak", orig) && orig.size() + eol.size() == out.size() &&
        out.compare(0, orig.size(), orig) == 0)
        out = orig;
    return write_atomic(path, out, err);
}

}  // namespace installer
