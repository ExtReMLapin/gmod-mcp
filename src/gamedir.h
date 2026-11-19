// Where the 64-bit Garry's Mod executable of a game folder is.
//
// Older x86-64 installs have gmod_win64.exe at the root of the game folder. Current ones have bin/win64/gmod.exe and
// no gmod_win64.exe; their root gmod.exe is the 32-bit game (issue #4). Pure file checks, no Windows API: tested
// without a game.
#pragma once

#include <cstdint>
#include <string>

namespace gamedir {

constexpr uint16_t kMachineX86 = 0x014C;
constexpr uint16_t kMachineX64 = 0x8664;

// The "machine" field of the PE header of a file (0x8664 = x86-64, 0x014C = 32-bit x86), 0 when the file is missing or
// is not a PE image.
uint16_t pe_machine(const std::string &path_u8);

// The 64-bit game executable under `root` (a folder holding garrysmod/): "<root>/gmod_win64.exe", else
// "<root>/bin/win64/gmod.exe". A candidate whose PE header says x86-64 wins over one that is not a PE image; one that
// says 32-bit is never taken. Empty when there is none.
std::string find_game_exe(const std::string &root_u8);

// What to tell the user when find_game_exe found nothing: names both paths and what was seen.
std::string missing_exe_message(const std::string &root_u8);

}  // namespace gamedir
