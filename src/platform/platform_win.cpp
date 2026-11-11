#ifdef _WIN32
#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <direct.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "user32.lib")

namespace plat {

void *find_loaded_module(const std::vector<std::string> &names) {
    for (const auto &n : names) {
        HMODULE h = GetModuleHandleA(n.c_str());
        if (h) return h;
    }
    return nullptr;
}

void *find_symbol(void *module, const char *name) {
    if (!module) return nullptr;
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(module), name));
}

std::vector<std::string> lua_shared_names() { return {"lua_shared.dll"}; }
std::vector<std::string> tier0_names() { return {"tier0.dll"}; }
std::vector<std::string> engine_names() { return {"engine.dll"}; }

std::string process_command_line() {
    const char *c = GetCommandLineA();
    return c ? c : "";
}

std::string process_exe_name() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s = buf;
    auto p = s.find_last_of("\\/");
    if (p != std::string::npos) s = s.substr(p + 1);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string current_directory() {
    wchar_t buf[MAX_PATH] = {0};
    GetCurrentDirectoryW(MAX_PATH, buf);
    std::error_code ec;
    // canonical() resolves the actual case of the path (GetFinalPathNameByHandle).
    auto p = std::filesystem::canonical(std::filesystem::path(buf), ec);
    std::string out = (ec ? std::filesystem::path(buf) : p).u8string();
    return out;
}

void terminate_process(int code) {
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    ExitProcess((UINT)code);
}

const char *platform_tag() { return "win64"; }
uint32_t process_id() { return (uint32_t)GetCurrentProcessId(); }

bool read_file(const std::string &path, std::string &out) {
    FILE *f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool write_file(const std::string &path, const std::string &data) {
    FILE *f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

bool file_exists(const std::string &path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

bool make_dirs(const std::string &path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

bool random_bytes(void *buf, size_t n) {
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

bool safe_read(const void *addr, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
}

std::string user_config_dir() {
    const char *a = std::getenv("APPDATA");
    return a ? a : current_directory();
}

bool pin_self_module() {
    HMODULE h = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCSTR>(&pin_self_module), &h) != 0;
}

std::vector<std::string> list_files(const std::string &dir, bool recursive) {
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::path base(std::filesystem::u8path(dir));
    if (recursive) {
        for (auto it = std::filesystem::recursive_directory_iterator(base, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file()) out.push_back(std::filesystem::relative(it->path(), base, ec).generic_u8string());
    } else {
        for (auto it = std::filesystem::directory_iterator(base, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
            if (it->is_regular_file()) out.push_back(it->path().filename().generic_u8string());
    }
    return out;
}

bool remove_file(const std::string &path) {
    std::error_code ec;
    return std::filesystem::remove(std::filesystem::u8path(path), ec);
}

void sleep_ms(int ms) { Sleep((DWORD)ms); }

std::vector<std::string> local_ipv4_addresses() {
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto *a = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()); a; a = a->Next) {
        for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN] = {0};
            auto *sin = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
            if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip) && std::string(ip).rfind("127.", 0) != 0) out.push_back(ip);
        }
    }
    return out;
}

int process_has_foreground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return -1;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() ? 1 : 0;
}

// Main game window: a visible, unowned top-level window of this process (the engine's class is "Valve001").
static BOOL CALLBACK find_main_window(HWND w, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    HWND *best = reinterpret_cast<HWND *>(lp);
    char cls[64] = {};
    GetClassNameA(w, cls, sizeof cls);
    if (std::strcmp(cls, "Valve001") == 0) {
        *best = w;
        return FALSE;
    }
    if (!*best) *best = w;
    return TRUE;
}

bool send_focus_lost() {
    HWND w = nullptr;
    EnumWindows(find_main_window, reinterpret_cast<LPARAM>(&w));
    if (!w) return false;
    SendMessageA(w, WM_ACTIVATE, WA_INACTIVE, 0);
    return true;
}

}  // namespace plat
#endif
