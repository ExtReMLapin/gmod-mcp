// Address of the server the client is connected to (network lock). Main thread only.
#pragma once

#include <string>

namespace netaddr {

struct Result {
    bool ok = false;
    std::string address;  // "ip:port" or "loopback"
    std::string method;
    std::string error;
};

Result current_server_address();
// Runs a command line via IVEngineClient::ClientCmd (bypasses RunConsoleCommand's Lua blacklist).
bool client_cmd(const std::string &cmd, std::string &err);
bool is_loopback(const std::string &address);
// Main thread, at load: checks the engine slots this module calls (layout.h) and publishes the verdict for `status`.
void verify_layout(bool client_mode);
// Tests only: use this object as IVEngineClient (nullptr: look the interface up again) and forget the checks made on the previous one.
void set_engine_for_tests(void *engine);
// True when the address designates this machine: loopback, or exactly one of its interface IPv4 addresses.
// `why` receives a short explanation. Any other address (even on the LAN) is NOT this machine.
bool is_this_machine(const std::string &address, std::string *why = nullptr);

}  // namespace netaddr
