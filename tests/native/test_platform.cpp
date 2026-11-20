// Platform layer (src/platform/): finding a module the game loaded by full path.
#include <string>
#include <vector>

#include "check.h"
#include "platform/platform.h"

#ifndef _WIN32
#include <dlfcn.h>
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

#endif

void test_platform() {
#ifndef _WIN32
    test_find_module_by_full_path();
#endif
}
