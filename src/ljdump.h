// Reader for LuaJIT bytecode dumps (string.dump), enough to tell whether a function loaded in the game is the same
// code as the file on disk: each prototype's code and constants are compared, debug info (line numbers) aside.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ljdump {

struct Proto {
    uint32_t firstline = 0;  // 0 when the dump is stripped
    uint32_t numline = 0;
    std::string body;        // flags, sizes, bytecode, upvalues and constants: everything but the debug info
};

// Prototypes in dump order: children before their parent, the dumped function last.
bool parse(const std::string &dump, std::vector<Proto> &out, std::string &err);

struct Match {
    bool found = false;
    uint32_t disk_line = 0;  // where the matching function starts in the file on disk
};
// Looks for the live function (its dump: the function and its nested functions) among the prototypes of the file
// compiled from disk. Equal code and constants, wherever the function now sits in the file.
Match find(const std::vector<Proto> &live, const std::vector<Proto> &disk);

}  // namespace ljdump
