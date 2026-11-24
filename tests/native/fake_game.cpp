// Stand-in for the 64-bit game (gmod_win64.exe or bin/win64/gmod.exe) in the bridge tests: a game in which gmod-mcp never loads (it never answers).
// Lives 120 s at most, so a failed test leaves nothing behind for long. When GMCP_FAKE_ARGV names a file, it appends
// its raw command line there (one line per start, UTF-8), so a test can check what the bridge passed.
// Crash mode (tests of the crash report): when GMCP_FAKE_TRIGGER names a file, the game waits for it to exist, then writes
// a stand-in minidump into the folder GMCP_FAKE_CRASH_DIR (unless GMCP_FAKE_DUMP is 0) and ends at once with the exit code
// GMCP_FAKE_EXIT (decimal, default 3221225477 = 0xC0000005), like a game that crashed (or, with 0, that quit). With
// GMCP_FAKE_EXIT_DELAY_MS it keeps running that long after the trigger (the dump is written at once): a game whose
// connections broke before the process object is signalled (issue #8). GMCP_FAKE_CHILDREN=n starts n helper processes of the same
// executable (the real game has about 2), see main.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

// The bytes of the fake dump: "MDMP", or the file GMCP_FAKE_DUMP_SRC names (a minidump built by the test: issue #8, the crash answer reads it).
static void write_dump_body(FILE *d) {
    const char *src = std::getenv("GMCP_FAKE_DUMP_SRC");
    if (src && *src) {
        if (FILE *in = std::fopen(src, "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) std::fwrite(buf, 1, n, d);
            std::fclose(in);
            return;
        }
    }
    std::fputs("MDMP", d);
}

// Appends one line to a file that several stand-in games write at once (one line per start). fopen "ab" seeks to the end and
// then writes, so two games starting together could write at the same offset and one line was lost (RETURN_FOCUS saw one of
// two). A handle opened with FILE_APPEND_DATA only makes each WriteFile an atomic append; POSIX "ab" is O_APPEND already.
static void append_line(const char *path, const std::string &line) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string l = line + "\n";
    DWORD written = 0;
    WriteFile(h, l.data(), (DWORD)l.size(), &written, nullptr);
    CloseHandle(h);
#else
    if (FILE *f = std::fopen(path, "ab")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
#endif
}

int main(int argc, char **argv) {
    // The real game starts helper processes of the same executable (about 2 per game). GMCP_FAKE_CHILDREN=n makes this stand-in start n
    // children (the same executable with --fake-child, no breakaway: they join the parent's job object, if any). A child writes its pid
    // to <GMCP_FAKE_CHILD_PID_DIR>/<pid>.pid when that is set, does nothing else, and ends after 120 s like the parent.
    if (argc > 1 && std::string(argv[1]) == "--fake-child") {
#ifdef _WIN32
        const char *cdir = std::getenv("GMCP_FAKE_CHILD_PID_DIR");
        if (cdir && *cdir) {
            std::string path = std::string(cdir) + "\\" + std::to_string((unsigned long)GetCurrentProcessId()) + ".pid";
            if (FILE *f = std::fopen(path.c_str(), "wb")) {
                std::fprintf(f, "%lu", (unsigned long)GetCurrentProcessId());
                std::fclose(f);
            }
        }
#endif
        for (int i = 0; i < 1200; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 0;
    }
#ifdef _WIN32
    if (const char *nk = std::getenv("GMCP_FAKE_CHILDREN")) {
        char self[MAX_PATH * 2] = {};
        GetModuleFileNameA(nullptr, self, sizeof self);
        for (int i = 0, n = std::atoi(nk); i < n; ++i) {
            std::string cmd = std::string("\"") + self + "\" --fake-child";
            STARTUPINFOA si{};
            si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
#endif
    // GMCP_FAKE_DIE_ONCE names a file: the first process to create it is "the one that dies" (issue #8: a client that exits ~1 s
    // after the bridge started it, while its sibling keeps starting). It registers nowhere, writes a dump into
    // GMCP_FAKE_CRASH_DIR (when set) after GMCP_FAKE_DIE_AFTER_MS (default 1000) and ends with GMCP_FAKE_EXIT (default 0xC0000005).
    const char *once = std::getenv("GMCP_FAKE_DIE_ONCE");
    if (once && *once) {
#ifdef _WIN32
        HANDLE h = CreateFileA(once, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            const char *after = std::getenv("GMCP_FAKE_DIE_AFTER_MS");
            std::this_thread::sleep_for(std::chrono::milliseconds(after && *after ? std::atoi(after) : 1000));
            const char *dir = std::getenv("GMCP_FAKE_CRASH_DIR");
            if (dir && *dir) {
                std::string p = std::string(dir) + "/gmod_win64_20261114_120000_1_accessviolation.mdmp";
                if (FILE *d = std::fopen(p.c_str(), "wb")) {
                    write_dump_body(d);
                    std::fclose(d);
                }
            }
            const char *code = std::getenv("GMCP_FAKE_EXIT");
            ExitProcess(code && *code ? (UINT)std::strtoul(code, nullptr, 10) : 0xC0000005u);
        }
#endif
    }
#ifdef _WIN32
    // GMCP_FAKE_STARTUP names a file: append "show=<wShowWindow> uses_show=<0|1>" from the STARTUPINFO this process got (the bridge asks
    // for a window shown without activation, so a client does not take the user's foreground).
    if (const char *so = std::getenv("GMCP_FAKE_STARTUP")) {
        if (*so) {
            STARTUPINFOW si{};
            si.cb = sizeof si;
            GetStartupInfoW(&si);
            append_line(so, "show=" + std::to_string((unsigned)si.wShowWindow) + " uses_show=" + ((si.dwFlags & STARTF_USESHOWWINDOW) ? "1" : "0"));
        }
    }
#endif
#ifdef _WIN32
    // GMCP_FAKE_DESKTOP_DIR names a folder: <dir>/<pid>.desktop gets the name of the desktop this process runs on ("Default" for the user's
    // input desktop; the bridge starts test clients on a private desktop of its own, issue #14).
    if (const char *dd = std::getenv("GMCP_FAKE_DESKTOP_DIR")) {
        if (*dd) {
            wchar_t name[256] = {0};
            DWORD need = 0;
            HDESK d = GetThreadDesktop(GetCurrentThreadId());
            if (!d || !GetUserObjectInformationW(d, UOI_NAME, name, sizeof name, &need)) wcscpy_s(name, L"?");
            std::string path = std::string(dd) + "\\" + std::to_string((unsigned long)GetCurrentProcessId()) + ".desktop";
            if (FILE *f = std::fopen(path.c_str(), "wb")) {
                char u[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, name, -1, u, (int)sizeof u - 1, nullptr, nullptr);
                std::fputs(u, f);
                std::fclose(f);
            }
        }
    }
#endif
    const char *out = std::getenv("GMCP_FAKE_ARGV");
    if (out && *out) {
#ifdef _WIN32
        // The wide command line, written as UTF-8: what the bridge really passed, whatever the ANSI code page.
        const wchar_t *w = GetCommandLineW();
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string u(n > 0 ? (size_t)n : 1, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, u.data(), n, nullptr, nullptr);
        u.resize(std::strlen(u.c_str()));  // without the terminating NUL that WideCharToMultiByte counted
#else
        std::string u;
        for (int i = 0; i < argc; ++i) u += std::string(i ? " " : "") + argv[i];
#endif
        append_line(out, u);
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
                        write_dump_body(d);
                        std::fclose(d);
                    }
                }
                const char *delay = std::getenv("GMCP_FAKE_EXIT_DELAY_MS");
                if (delay && *delay) std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(delay)));
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
