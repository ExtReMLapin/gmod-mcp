// Platform layer: all OS-specific code goes through here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plat {

// Returns the handle of a module already loaded in the process (tries each name), or nullptr.
void *find_loaded_module(const std::vector<std::string> &names);
// Resolves an exported symbol.
void *find_symbol(void *module, const char *name);

// File names of the engine modules for the current platform.
std::vector<std::string> lua_shared_names();
std::vector<std::string> tier0_names();
std::vector<std::string> engine_names();

// Process command line (fallback if tier0 does not expose it).
std::string process_command_line();
// Executable name (without path), lowercased.
std::string process_exe_name();
// Path of the current working directory.
std::string current_directory();

// Immediate process termination, callable from any thread.
[[noreturn]] void terminate_process(int code);

// Platform identifier ("win64", "linux64", "osx64").
const char *platform_tag();
// Current pid.
uint32_t process_id();

// Files: simple binary read/write.
bool read_file(const std::string &path, std::string &out);
bool write_file(const std::string &path, const std::string &data);
bool file_exists(const std::string &path);
bool make_dirs(const std::string &path);

// Cryptographically secure random bytes (for the token).
bool random_bytes(void *buf, size_t n);

// User configuration directory (%APPDATA% / $XDG_CONFIG_HOME or ~/.config).
std::string user_config_dir();
// Prevents the current module from being unloaded (active threads).
bool pin_self_module();
// Recursive list of files under a directory (relative paths, '/' separator).
std::vector<std::string> list_files(const std::string &dir, bool recursive);
bool remove_file(const std::string &path);
// Sleeps without blocking other threads.
void sleep_ms(int ms);

// Protected memory read: copies n bytes if the address is readable.
bool safe_read(const void *addr, void *out, size_t n);

// IPv4 addresses of this machine's network interfaces (dotted strings, loopback excluded).
std::vector<std::string> local_ipv4_addresses();

// Window focus (Windows only). 1 = a window of this process is in the foreground, 0 = another process's window is,
// -1 = unknown (no foreground window, or not supported on this platform).
int process_has_foreground();
// Sends WM_ACTIVATE(WA_INACTIVE) to this process's main game window, as Windows does when another window takes the
// focus. Returns false if no such window was found (or on other platforms).
bool send_focus_lost();

}  // namespace plat
