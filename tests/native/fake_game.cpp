// Stand-in for gmod_win64.exe in the bridge tests: a game in which gmod-mcp never loads (it never answers).
// Lives 120 s at most, so a failed test leaves nothing behind for long. When GMCP_FAKE_ARGV names a file, it appends
// its raw command line there (one line per start, UTF-8), so a test can check what the bridge passed.
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
    (void)argc;
    (void)argv;
    std::this_thread::sleep_for(std::chrono::seconds(120));
    return 0;
}
