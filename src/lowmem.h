// Memory check before instances_launch starts games, and the memory facts added to a crash report (issue #8).
//
// What kills a game that runs out of memory is the commit charge (RAM + page file reserved by every process), not the free
// RAM: with 11 clients running, a machine whose free commit fell to 1.8 GB saw every client die with 0xC000041D and no dump.
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace lowmem {

struct Memory {
    bool known = false;
    uint64_t free_commit_mb = 0;  // Windows: ullAvailPageFile; Linux: CommitLimit - Committed_AS
    uint64_t free_ram_mb = 0;     // Windows: ullAvailPhys; Linux: MemAvailable
};

// A test client (light preset, gm_construct) holds about this much commit and RAM (measured: 1459 MB private + 4 browser subprocesses about 85 MB = 1.55 GB commit, 958 + 250 MB working set; docs/TESTS.md I-28).
constexpr uint64_t kPerClientCommitMb = 1600;
constexpr uint64_t kPerClientRamMb = 1200;
// Commit kept free for the rest of the machine after the new clients started.
constexpr uint64_t kReserveCommitMb = 1024;
// A crash report adds a memory hint when the free commit is under this.
constexpr uint64_t kLowCommitHintMb = 4096;

// Linux /proc/meminfo text to Memory (unknown when the fields are missing).
Memory parse_meminfo(const std::string &text);
// The machine now (the platform layer). The environment variables GMOD_MCP_FAKE_FREE_COMMIT_MB and
// GMOD_MCP_FAKE_FREE_RAM_MB replace the measured values (tests).
Memory current();

struct Verdict {
    bool refuse = false;
    std::string message;
    nlohmann::json details;  // {free_commit_mb, free_ram_mb, running_clients, per_client_mb, count, needed_commit_mb, needed_ram_mb}
};
// May `count` more clients start while `running_clients` run? Never refuses when the memory is unknown or `ignore` is set.
Verdict check_launch(const Memory &m, int count, int running_clients, bool ignore);

// {free_commit_mb, free_ram_mb} for a crash report, null when unknown.
nlohmann::json memory_json(const Memory &m);
// Adds the memory facts to a client_crashed / server_crashed answer: details.memory, and a sentence at the end of the message
// when the free commit is low (out of memory is a likely cause).
void annotate_crash(const Memory &m, nlohmann::json &details, std::string &message);

}  // namespace lowmem
