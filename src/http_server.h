#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace httpsrv {

// Starts the server on 127.0.0.1, trying base_port then the following ones (max_tries). Returns the port obtained, or 0.
int start(int base_port, int max_tries, const std::string &token);
void stop();
int port();

// Additional internal route (POST, JSON -> JSON), authenticated with the same token (coordinator).
using InternalFn = std::function<nlohmann::json(const nlohmann::json &)>;
void add_internal_route(const std::string &path, InternalFn fn);

// Tries to bind a specific port (coordinator re-election): opens a second listener. Returns true if obtained.
bool try_bind_extra(int port);
bool has_extra();
int extra_port();

// Security checks (exposed for tests).
bool host_allowed(const std::string &host, int port);

}  // namespace httpsrv
