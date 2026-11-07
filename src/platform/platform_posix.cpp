#ifndef _WIN32
#include "platform.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace plat {

void *find_loaded_module(const std::vector<std::string> &names) {
    for (const auto &n : names) {
        void *h = dlopen(n.c_str(), RTLD_LAZY | RTLD_NOLOAD);
        if (h) return h;
    }
    return nullptr;
}

void *find_symbol(void *module, const char *name) {
    if (!module) return nullptr;
    return dlsym(module, name);
}

#ifdef __APPLE__
std::vector<std::string> lua_shared_names() { return {"lua_shared.dylib"}; }
std::vector<std::string> tier0_names() { return {"libtier0.dylib"}; }
std::vector<std::string> engine_names() { return {"engine.dylib"}; }
const char *platform_tag() { return "osx64"; }
#else
std::vector<std::string> lua_shared_names() { return {"lua_shared.so", "lua_shared_srv.so", "lua_shared_client.so"}; }
std::vector<std::string> tier0_names() { return {"libtier0.so", "libtier0_srv.so", "libtier0_client.so"}; }
std::vector<std::string> engine_names() { return {"engine.so", "engine_srv.so", "engine_client.so"}; }
const char *platform_tag() { return "linux64"; }
#endif

std::string process_command_line() {
#ifdef __linux__
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::replace(raw.begin(), raw.end(), '\0', ' ');
    return raw;
#else
    return "";
#endif
}

std::string process_exe_name() {
#ifdef __linux__
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string s = n > 0 ? std::string(buf, (size_t)n) : "";
    auto p = s.find_last_of('/');
    return p == std::string::npos ? s : s.substr(p + 1);
#else
    return "";
#endif
}

std::string current_directory() {
    char buf[4096] = {0};
    return getcwd(buf, sizeof buf) ? buf : "";
}

void terminate_process(int code) {
    (void)code;
    kill(getpid(), SIGKILL);
    _exit(code);
}

uint32_t process_id() { return (uint32_t)getpid(); }

bool read_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string &path, const std::string &data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

bool file_exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

bool make_dirs(const std::string &path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

bool random_bytes(void *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, static_cast<char *>(buf) + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == n;
}

bool safe_read(const void *addr, void *out, size_t n) {
#ifdef __linux__
    iovec local{out, n};
    iovec remote{const_cast<void *>(addr), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
#else
    std::memcpy(out, addr, n);
    return true;
#endif
}

std::string user_config_dir() {
    const char *x = std::getenv("XDG_CONFIG_HOME");
    if (x && *x) return x;
    const char *h = std::getenv("HOME");
    return h ? std::string(h) + "/.config" : current_directory();
}

bool pin_self_module() {
    Dl_info info;
    if (!dladdr(reinterpret_cast<void *>(&pin_self_module), &info) || !info.dli_fname) return false;
    return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != nullptr;
}

std::vector<std::string> list_files(const std::string &dir, bool recursive) {
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::path base(dir);
    if (recursive) {
        for (auto it = std::filesystem::recursive_directory_iterator(base, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file()) out.push_back(std::filesystem::relative(it->path(), base, ec).generic_string());
    } else {
        for (auto it = std::filesystem::directory_iterator(base, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
            if (it->is_regular_file()) out.push_back(it->path().filename().generic_string());
    }
    return out;
}

bool remove_file(const std::string &path) {
    std::error_code ec;
    return std::filesystem::remove(path, ec);
}

void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

std::vector<std::string> local_ipv4_addresses() {
    std::vector<std::string> out;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        char ip[INET_ADDRSTRLEN] = {0};
        auto *sin = reinterpret_cast<sockaddr_in *>(i->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip) && std::string(ip).rfind("127.", 0) != 0) out.push_back(ip);
    }
    freeifaddrs(list);
    return out;
}

}  // namespace plat
#endif
