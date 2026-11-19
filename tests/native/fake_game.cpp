// Stand-in for the 64-bit game (gmod_win64.exe or bin/win64/gmod.exe) in the bridge tests: a game in which gmod-mcp never loads (it never answers).
// Lives 120 s at most, so a failed test leaves nothing behind for long. When GMCP_FAKE_ARGV names a file, it appends
// its raw command line there (one line per start, UTF-8), so a test can check what the bridge passed.
// Crash mode (tests of the crash report): when GMCP_FAKE_TRIGGER names a file, the game waits for it to exist, then writes
// a stand-in minidump into the folder GMCP_FAKE_CRASH_DIR (unless GMCP_FAKE_DUMP is 0) and ends at once with the exit code
// GMCP_FAKE_EXIT (decimal, default 3221225477 = 0xC0000005), like a game that crashed (or, with 0, that quit).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char **argv) {
    const char *out = std::getenv("GMCP_FAKE_ARGV");
    if (out && *out) {
        if (FILE *f = std::fopen(out, "ab")) {
#ifdef _WIN32
            // The wide command line, written as UTF-8: what the bridge really passed, whatever the ANSI code page.
            const wchar_t *w = GetCommandLineW();
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            std::string u(n > 0 ? (size_t)n : 1, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, u.data(), n, nullptr, nullptr);
            std::fprintf(f, "%s\n", u.c_str());
#else
            for (int i = 0; i < argc; ++i) std::fprintf(f, "%s%s", i ? " " : "", argv[i]);
            std::fprintf(f, "\n");
#endif
            std::fclose(f);
        }
    }
    // GMCP_FAKE_CWD names a file that gets the working directory (UTF-8): a test checks where the bridge started the game.
    const char *cwdf = std::getenv("GMCP_FAKE_CWD");
    if (cwdf && *cwdf) {
        if (FILE *f = std::fopen(cwdf, "wb")) {
#ifdef _WIN32
            wchar_t w[1024] = {0};
            DWORD wn = GetCurrentDirectoryW(1024, w);
            int n = wn ? WideCharToMultiByte(CP_UTF8, 0, w, (int)wn, nullptr, 0, nullptr, nullptr) : 0;
            std::string u((size_t)n, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, (int)wn, u.data(), n, nullptr, nullptr);
            std::fwrite(u.data(), 1, u.size(), f);
#endif
            std::fclose(f);
        }
    }
    // GMCP_FAKE_PID names a file that gets this process's id: a fake game server (tests) answers instances_list with it.
    const char *pidf = std::getenv("GMCP_FAKE_PID");
    if (pidf && *pidf) {
        if (FILE *f = std::fopen(pidf, "wb")) {
#ifdef _WIN32
            std::fprintf(f, "%lu", (unsigned long)GetCurrentProcessId());
#endif
            std::fclose(f);
        }
    }
    // GMCP_FAKE_PID_DIR: like GMCP_FAKE_PID but one file per process (<dir>/<pid>.pid), for tests that start several.
    const char *piddir = std::getenv("GMCP_FAKE_PID_DIR");
    if (piddir && *piddir) {
#ifdef _WIN32
        std::string path = std::string(piddir) + "\\" + std::to_string((unsigned long)GetCurrentProcessId()) + ".pid";
        if (FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "%lu", (unsigned long)GetCurrentProcessId());
            std::fclose(f);
        }
#endif
    }
    (void)argc;
    (void)argv;
    const char *trigger = std::getenv("GMCP_FAKE_TRIGGER");
    for (int i = 0; i < 1200; ++i) {  // 120 s
        if (trigger && *trigger) {
            if (FILE *t = std::fopen(trigger, "rb")) {
                std::fclose(t);
                const char *dir = std::getenv("GMCP_FAKE_CRASH_DIR");
                const char *dump = std::getenv("GMCP_FAKE_DUMP");
                if (dir && *dir && !(dump && std::string(dump) == "0")) {
                    std::string p = std::string(dir) + "/gmod_win64_20261114_120000_1_accessviolation.mdmp";
                    if (FILE *d = std::fopen(p.c_str(), "wb")) {
                        std::fputs("MDMP", d);
                        std::fclose(d);
                    }
                }
                const char *code = std::getenv("GMCP_FAKE_EXIT");
#ifdef _WIN32
                ExitProcess(code && *code ? (UINT)std::strtoul(code, nullptr, 10) : 0xC0000005u);
#else
                return code && *code ? std::atoi(code) : 1;
#endif
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}
