#include "http_server.h"

#ifdef _WIN32
#include <winsock2.h>
#endif
#include <httplib.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "log.h"
#include "mcp.h"
#include "util.h"

using nlohmann::json;

namespace httpsrv {

struct Listener {
    std::unique_ptr<httplib::Server> svr;
    std::thread th;
    int port = 0;
};

static std::string g_token;
static Listener g_main, g_extra;
static std::mutex g_routes_mu;
static std::map<std::string, InternalFn> g_routes;

int port() { return g_main.port; }
bool has_extra() { return g_extra.svr != nullptr; }
int extra_port() { return g_extra.port; }

void add_internal_route(const std::string &path, InternalFn fn) {
    std::lock_guard<std::mutex> lk(g_routes_mu);
    g_routes[path] = std::move(fn);
}

// Socket options: NO SO_REUSEADDR on Windows (it would allow stealing an already-bound port) and no SO_REUSEPORT
// on Linux (several processes on the same port): port uniqueness is the basis of the coordinator election.
static void socket_options(socket_t sock) {
#ifdef _WIN32
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&one), sizeof one);
#else
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
}

bool host_allowed(const std::string &host, int /*unused*/) {
    std::string h = util::to_lower(host);
    std::string name = h, p;
    if (!h.empty() && h[0] == '[') {
        auto e = h.find(']');
        if (e == std::string::npos) return false;
        name = h.substr(0, e + 1);
        if (e + 1 < h.size() && h[e + 1] == ':') p = h.substr(e + 2);
    } else {
        auto c = h.rfind(':');
        if (c != std::string::npos) {
            name = h.substr(0, c);
            p = h.substr(c + 1);
        }
    }
    if (name != "127.0.0.1" && name != "localhost" && name != "[::1]") return false;
    // The port must be purely numeric ("127.0.0.1:27080.evil.com" must not pass).
    if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
    int pn = std::atoi(p.c_str());
    return pn == g_main.port || (g_extra.svr && pn == g_extra.port);
}

static void reject(httplib::Response &res, int status, const std::string &code, const std::string &msg) {
    res.status = status;
    res.set_content(json{{"error", {{"code", code}, {"message", msg}}}}.dump(), "application/json");
}

static void setup_routes(httplib::Server &svr) {
    svr.set_socket_options(socket_options);
    // Long-polls (events_wait, reports_read, tasks/result) hold a worker thread: keep plenty of them.
    svr.new_task_queue = [] { return new httplib::ThreadPool(64); };
    svr.set_keep_alive_max_count(1000);
    svr.set_keep_alive_timeout(30);
    svr.set_read_timeout(30, 0);
    svr.set_write_timeout(60, 0);
    svr.set_payload_max_length(32 * 1024 * 1024);
    svr.set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
        // 1. Origin: any request carrying a browser Origin is rejected (DNS rebinding).
        if (req.has_header("Origin")) {
            reject(res, 403, "origin_forbidden", "requests with an Origin header are refused");
            return httplib::Server::HandlerResponse::Handled;
        }
        // 2. Host: loopback on our ports only.
        if (!host_allowed(req.get_header_value("Host"), 0)) {
            reject(res, 403, "host_forbidden", "Host header not allowed");
            return httplib::Server::HandlerResponse::Handled;
        }
        // 3. Bearer token.
        std::string auth = req.get_header_value("Authorization");
        const std::string pfx = "Bearer ";
        if (auth.size() <= pfx.size() || util::to_lower(auth.substr(0, pfx.size())) != "bearer " ||
            !util::constant_time_eq(auth.substr(pfx.size()), g_token)) {
            res.set_header("WWW-Authenticate", "Bearer");
            reject(res, 401, "unauthorized", "missing or invalid bearer token");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    svr.Post("/mcp", [](const httplib::Request &req, httplib::Response &res) {
        int status = 200;
        mcp::Headers h;
        for (const char *k : {"MCP-Protocol-Version", "Mcp-Method", "Mcp-Name"})
            if (req.has_header(k)) h[util::to_lower(k)] = req.get_header_value(k);
        std::string body = mcp::handle(req.body, h, status);
        res.status = status;
        if (!body.empty()) res.set_content(body, "application/json");
    });
    svr.Get("/mcp", [](const httplib::Request &, httplib::Response &res) {
        res.status = 405;
        res.set_header("Allow", "POST");
    });
    svr.Delete("/mcp", [](const httplib::Request &, httplib::Response &res) {
        res.status = 405;
        res.set_header("Allow", "POST");
    });
    svr.Post(R"(/internal/(\w+))", [](const httplib::Request &req, httplib::Response &res) {
        InternalFn fn;
        {
            std::lock_guard<std::mutex> lk(g_routes_mu);
            auto it = g_routes.find(req.matches[1].str());
            if (it != g_routes.end()) fn = it->second;
        }
        if (!fn) {
            reject(res, 404, "not_found", "unknown internal route");
            return;
        }
        json in;
        try {
            in = json::parse(req.body);
        } catch (...) {
            reject(res, 400, "bad_json", "invalid JSON");
            return;
        }
        res.set_content(fn(in).dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
    });
}

static bool open_listener(Listener &l, int port) {
    auto svr = std::make_unique<httplib::Server>();
    setup_routes(*svr);
    if (!svr->bind_to_port("127.0.0.1", port)) return false;
    l.port = port;
    l.svr = std::move(svr);
    httplib::Server *raw = l.svr.get();
    l.th = std::thread([raw] { raw->listen_after_bind(); });
    return true;
}

int start(int base_port, int max_tries, const std::string &token) {
    g_token = token;
    for (int i = 0; i < max_tries; ++i)
        if (open_listener(g_main, base_port + i)) return g_main.port;
    return 0;
}

bool try_bind_extra(int port) {
    if (g_extra.svr) return g_extra.port == port;
    return open_listener(g_extra, port);
}

static void close_listener(Listener &l) {
    if (!l.svr) return;
    l.svr->stop();
    if (l.th.joinable()) l.th.join();
    l.svr.reset();
    l.port = 0;
}

void stop() {
    close_listener(g_extra);
    close_listener(g_main);
}

}  // namespace httpsrv
