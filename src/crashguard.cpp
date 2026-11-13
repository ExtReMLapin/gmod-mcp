#include "crashguard.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace crashguard {

static bool parse_marker(const fs::path &p, uint32_t &pid, uint64_t &stamp) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    unsigned long long a = 0, b = 0;
    if (!(f >> a >> b) || a == 0 || a > 0xFFFFFFFFull) return false;
    pid = (uint32_t)a;
    stamp = b;
    return true;
}

static bool alive(StampFn stamp_of, uint32_t pid, uint64_t recorded) {
    uint64_t now = stamp_of(pid);
    if (now == 0) return false;
    if (now == kStampUnknown || recorded == 0 || recorded == kStampUnknown) return true;
    return now == recorded;  // another stamp: the pid was reused by an unrelated process
}

Result begin(const std::string &dir_u8, uint32_t pid, StampFn stamp_of) {
    Result r;
    std::error_code ec;
    fs::path dir = fs::u8path(dir_u8);
    fs::path loading = dir / "loading";
    fs::create_directories(loading, ec);

    // Single marker of 0.3.x and earlier: shared by every instance, it blocked all later launches after one crash.
    if (fs::remove(dir / "load_in_progress.txt", ec)) r.notes.push_back("removed the marker of an older version");

    for (fs::directory_iterator it(loading, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        if (p.extension() != ".txt") continue;
        uint32_t other = 0;
        uint64_t stamp = 0;
        std::error_code ec2;
        if (!parse_marker(p, other, stamp) || other == pid) {
            fs::remove(p, ec2);  // unreadable, or left by an earlier process that had this same pid
            continue;
        }
        if (alive(stamp_of, other, stamp)) continue;  // another instance is starting right now
        // Dead while loading: a crash. Claim it atomically, so that among launches started together only one skips.
        fs::path claimed = loading / ("crashed-" + std::to_string(other) + "-by-" + std::to_string(pid));
        fs::rename(p, claimed, ec2);
        if (ec2) continue;  // claimed by someone else (or gone)
        fs::remove(claimed, ec2);
        r.skip = true;
        std::ofstream(dir / "last_crash.txt", std::ios::binary)
            << "A gmod-mcp load did not finish (process " << other << " ended during it, probably a crash).\n"
            << "Process " << pid << " skipped gmod-mcp once because of it; the next launch loads it again.\n";
        r.notes.push_back("the gmod-mcp load of process " + std::to_string(other) +
                          " did not finish (crash?): gmod-mcp is skipped for this launch only, the next one loads it");
    }
    if (r.skip) return r;
    std::ofstream(loading / (std::to_string(pid) + ".txt"), std::ios::binary) << pid << " " << stamp_of(pid) << "\n";
    return r;
}

void end(const std::string &dir_u8, uint32_t pid) {
    std::error_code ec;
    fs::remove(fs::u8path(dir_u8) / "loading" / (std::to_string(pid) + ".txt"), ec);
}

}  // namespace crashguard
