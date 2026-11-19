#include "gamedir.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace gamedir {

uint16_t pe_machine(const std::string &path_u8) {
    std::ifstream f(fs::u8path(path_u8), std::ios::binary);
    if (!f) return 0;
    unsigned char dos[0x40] = {};
    if (!f.read(reinterpret_cast<char *>(dos), sizeof dos) || dos[0] != 'M' || dos[1] != 'Z') return 0;
    uint32_t lfanew = (uint32_t)dos[0x3C] | (uint32_t)dos[0x3D] << 8 | (uint32_t)dos[0x3E] << 16 | (uint32_t)dos[0x3F] << 24;
    if (lfanew > (1u << 20)) return 0;  // a real header sits near the start
    f.seekg((std::streamoff)lfanew);
    unsigned char nt[6] = {};
    if (!f.read(reinterpret_cast<char *>(nt), sizeof nt) || nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) return 0;
    return (uint16_t)(nt[4] | nt[5] << 8);
}

static std::string join(const std::string &root_u8, const char *rel) {
    std::string r = root_u8;
    while (!r.empty() && (r.back() == '/' || r.back() == '\\')) r.pop_back();
    return r + "/" + rel;
}

static bool is_file(const std::string &p_u8) {
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(p_u8), ec);
}

static const char *const kCandidates[] = {"gmod_win64.exe", "bin/win64/gmod.exe"};

std::string find_game_exe(const std::string &root_u8) {
    if (root_u8.empty()) return "";
    std::string unknown;  // the first candidate that is a file but not a PE image
    for (const char *rel : kCandidates) {
        std::string p = join(root_u8, rel);
        if (!is_file(p)) continue;
        uint16_t m = pe_machine(p);
        if (m == kMachineX64) return p;
        if (m == 0 && unknown.empty()) unknown = p;
    }
    return unknown;
}

std::string missing_exe_message(const std::string &root_u8) {
    std::string msg = "no 64-bit game executable in " + root_u8 + ": looked for " + join(root_u8, kCandidates[0]) + " and " +
                      join(root_u8, kCandidates[1]);
    std::string seen;
    for (const char *rel : {"gmod.exe", kCandidates[0], kCandidates[1]}) {
        std::string p = join(root_u8, rel);
        if (is_file(p) && pe_machine(p) == kMachineX86) seen += std::string(seen.empty() ? "" : ", ") + rel;
    }
    if (!seen.empty()) msg += " (" + seen + " is a 32-bit executable: the game is on the 32-bit branch, and gmod-mcp needs the 64-bit one)";
    return msg;
}

}  // namespace gamedir
