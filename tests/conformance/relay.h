// Loopback relay of the conformance host: the official suite cannot send a bearer token, so it talks to this relay, which
// forwards every request to the module's HTTP server adding `Authorization: Bearer <token>`.
#pragma once

#include <string>

namespace conformance {

// Listens on 127.0.0.1:listen_port and forwards to 127.0.0.1:upstream_port. Returns false if it cannot listen.
// Every header is forwarded as sent, except: any Authorization is replaced by the bearer token, and a Host naming the
// relay's own port is rewritten to the module's port (the module only accepts its own port): exactly one Host header
// goes upstream. Bodies and responses (SSE included) are streamed unchanged; closing a client connection closes its
// upstream one, which is how a client cancels a streamed request.
bool relay_start(int listen_port, int upstream_port, const std::string &token);
void relay_stop();

}  // namespace conformance
