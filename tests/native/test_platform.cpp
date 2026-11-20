// Platform layer (src/platform/): finding a module the game loaded by full path, and asking the process to quit.
#include <string>
#include <vector>

#include "check.h"
#include "platform/platform.h"

#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static fs::path exe_dir() {
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? fs::path(std::string(buf, (size_t)n)).parent_path() : fs::path(".");
}

// Linux finding LF-2: on the dev-branch server lua_shared_srv.so sits in garrysmod/bin/linux64, has no SONAME and is not on
// LD_LIBRARY_PATH: the game loads it by full path, so dlopen("lua_shared_srv.so", RTLD_NOLOAD) never finds it.
static void test_find_module_by_full_path() {
    fs::path src = exe_dir() / "gmcp_testlib.so";
    CHECK(fs::exists(src));
    fs::path dir = fs::temp_directory_path() / ("gmcp-testlib-" + std::to_string((long)getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "garrysmod" / "bin" / "linux64");
    fs::path lib = dir / "garrysmod" / "bin" / "linux64" / "gmcp_probe_srv.so";
    fs::copy_file(src, lib, ec);
    CHECK(!ec);
    // Not loaded yet: nothing found, and the search does not load anything.
    CHECK(plat::find_loaded_module({"gmcp_probe.so", "gmcp_probe_srv.so"}) == nullptr);
    void *h = dlopen(lib.c_str(), RTLD_NOW);  // by full path, like the game
    CHECK(h != nullptr);
    if (h) {
        void *found = plat::find_loaded_module({"gmcp_probe.so", "gmcp_probe_srv.so", "gmcp_probe_client.so"});
        CHECK(found != nullptr);
        auto fn = reinterpret_cast<int (*)()>(plat::find_symbol(found, "gmcp_testlib_answer"));
        CHECK(fn != nullptr && fn() == 42);
        // The match is by exact file name: another name, or a suffix of the name, is not found.
        CHECK(plat::find_loaded_module({"gmcp_other_srv.so"}) == nullptr);
        CHECK(plat::find_loaded_module({"cp_probe_srv.so"}) == nullptr);  // a suffix is not a basename
        if (found) dlclose(found);  // the handle find_loaded_module returned
        dlclose(h);
    }
    fs::remove_all(dir, ec);
    // A name the process has through the usual way (the C library) still resolves by its bare name.
    void *c = plat::find_loaded_module({"libc.so.6"});
    CHECK(c != nullptr);
    if (c) dlclose(c);
}

// Linux finding LF-1: game_quit on srcds answered quitting:true and nothing quit (the console `quit` is blocked from server
// Lua). request_quit ends the process through SIGTERM, which srcds handles as a clean shutdown.
static void test_request_quit() {
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa = {};
        sa.sa_handler = [](int) { _exit(42); };
        sigaction(SIGTERM, &sa, nullptr);
        if (!plat::request_quit(50)) _exit(7);
        // The caller keeps running until the signal comes (the answer of the tool goes out meanwhile).
        for (int i = 0; i < 100; ++i) usleep(100000);
        _exit(9);
    }
    close(fds[0]);
    close(fds[1]);
    int st = 0;
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);
}
#endif

void test_platform() {
#ifndef _WIN32
    test_find_module_by_full_path();
    test_request_quit();
#else
    CHECK(!plat::request_quit(0));  // no clean quit path for a dedicated server on Windows: the tool must say so
#endif
}
