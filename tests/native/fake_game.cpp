// Stand-in for gmod_win64.exe in the bridge tests: a game in which gmod-mcp never loads (it never answers).
// Lives 120 s at most, so a failed test leaves nothing behind for long.
#include <chrono>
#include <thread>

int main() {
    std::this_thread::sleep_for(std::chrono::seconds(120));
    return 0;
}
