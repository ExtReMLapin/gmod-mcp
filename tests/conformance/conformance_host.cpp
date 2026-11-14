// gmod-mcp-conformance-host: the module's real MCP/HTTP server (src/*.cpp, the code of the DLL) without the game, plus the
// fixtures of the official MCP conformance suite (fixtures.cpp) and a loopback relay that adds the bearer token (the suite
// cannot send one). Test-only: built by CMake next to the other test executables, never shipped.
//
//   gmod-mcp-conformance-host [--port 27090] [--no-stdin]
//
// Prints exactly one line `READY http://127.0.0.1:<port>/mcp` once the relay and the server listen. Exits cleanly on
// SIGINT / SIGTERM / Ctrl+C, or when stdin reaches EOF (--no-stdin ignores stdin: for a manual run in the background).
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "agent.h"
#include "fixtures.h"
#include "http_server.h"
#include "log.h"
#include "mcp.h"
#include "platform/platform.h"
#include "relay.h"
#include "resources.h"
#include "tools.h"
#include "util.h"

static std::atomic<bool> g_stop{false};

static void on_signal(int) { g_stop = true; }

// A free loopback port (bound then released), for the module's server.
static int free_port() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    auto s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int port = 0;
    if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0) {
        socklen_t len = sizeof a;
        if (getsockname(s, reinterpret_cast<sockaddr *>(&a), &len) == 0) port = ntohs(a.sin_port);
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return port;
}

int main(int argc, char **argv) {
    int relay_port = 27090;
    bool watch_stdin = true;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) relay_port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-stdin")) watch_stdin = false;
        else {
            std::fprintf(stderr, "usage: gmod-mcp-conformance-host [--port <port>] [--no-stdin]\n");
            return 2;
        }
    }
    if (relay_port <= 0 || relay_port > 65535) {
        std::fprintf(stderr, "invalid --port\n");
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // The module's own configuration, with neutral values: there is no game folder and no Lua state.
    auto &cfg = agent::config();
    cfg.mode = agent::Mode::Dedicated;
    cfg.instance_id = "conformance-host";
    cfg.game_dir = cfg.tmp_dir = cfg.data_dir = "<none: conformance host>";
    unsigned char raw[32];
    if (!plat::random_bytes(raw, sizeof raw)) {
        std::fprintf(stderr, "no random bytes for the token\n");
        return 1;
    }
    cfg.token = util::hex(raw, sizeof raw);  // never the user's real token: one per run

    tools::register_all();           // the module's real tools (their handlers answer realm_unavailable: no game)
    conformance::register_fixtures();  // the suite's fixtures: test-only
    mcp::set_instructions(resources::instructions());

    int upstream = free_port();
    cfg.port = upstream > 0 ? httpsrv::start(upstream, 1, cfg.token) : 0;
    if (cfg.port == 0) {
        std::fprintf(stderr, "cannot start the module's HTTP server\n");
        return 1;
    }
    if (!conformance::relay_start(relay_port, cfg.port, cfg.token)) {
        std::fprintf(stderr, "cannot listen on 127.0.0.1:%d\n", relay_port);
        httpsrv::stop();
        return 1;
    }
    std::printf("READY http://127.0.0.1:%d/mcp\n", relay_port);
    std::fflush(stdout);

    if (watch_stdin) {
        std::thread([] {
            while (std::fgetc(stdin) != EOF) {
            }
            g_stop = true;
        }).detach();
    }
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    conformance::relay_stop();
    httpsrv::stop();
    std::fflush(stdout);
    std::_Exit(0);  // the module's worker threads (SSE, tasks) are detached
}
