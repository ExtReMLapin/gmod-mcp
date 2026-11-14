#include "relay.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t kBadSock = INVALID_SOCKET;
static void close_sock(sock_t s) { closesocket(s); }
static const int kSendFlags = 0;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kBadSock = -1;
static void close_sock(sock_t s) { close(s); }
static const int kSendFlags = MSG_NOSIGNAL;
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace conformance {

namespace {

std::atomic<bool> g_running{false};
sock_t g_listen = kBadSock;
std::thread g_accept;
int g_listen_port = 0, g_upstream_port = 0;
std::string g_token;

void shutdown_both(sock_t s) { shutdown(s, 2); }  // SD_BOTH / SHUT_RDWR

bool send_all(sock_t s, const char *p, size_t n) {
    while (n > 0) {
        int k = static_cast<int>(send(s, p, static_cast<int>(std::min<size_t>(n, 1 << 20)), kSendFlags));
        if (k <= 0) return false;
        p += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

sock_t connect_loopback(int port) {
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) return kBadSock;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
        close_sock(s);
        return kBadSock;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, 1 /* TCP_NODELAY */, reinterpret_cast<const char *>(&one), sizeof one);
    return s;
}

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// A Host naming the relay's own port becomes the module's port; anything else (a rebinding probe) goes through as is.
std::string rewrite_host(const std::string &value) {
    std::string v = trim(value), name = v, port;
    if (!v.empty() && v[0] == '[') {
        size_t e = v.find(']');
        if (e == std::string::npos) return value;
        name = v.substr(0, e + 1);
        if (e + 1 < v.size() && v[e + 1] == ':') port = v.substr(e + 2);
    } else {
        size_t c = v.rfind(':');
        if (c != std::string::npos) name = v.substr(0, c), port = v.substr(c + 1);
    }
    std::string n = lower(name);
    if ((n == "127.0.0.1" || n == "localhost" || n == "[::1]") && port == std::to_string(g_listen_port))
        return name + ":" + std::to_string(g_upstream_port);
    return value;
}

struct Conn {
    sock_t client = kBadSock;
    sock_t up = kBadSock;
    std::thread pump;
    std::mutex mu;
    std::atomic<bool> done{false};

    void shutdown_all() {
        std::lock_guard<std::mutex> lk(mu);
        if (client != kBadSock) shutdown_both(client);
        if (up != kBadSock) shutdown_both(up);
    }
};

std::mutex g_conns_mu;
std::set<Conn *> g_conns;

// Buffered reader over the client socket.
struct Reader {
    sock_t s;
    std::string buf;

    bool fill() {
        char tmp[16384];
        int k = static_cast<int>(recv(s, tmp, sizeof tmp, 0));
        if (k <= 0) return false;
        buf.append(tmp, static_cast<size_t>(k));
        return true;
    }
    bool read_head(std::string &head) {
        for (;;) {
            size_t e = buf.find("\r\n\r\n");
            if (e != std::string::npos) {
                head = buf.substr(0, e);
                buf.erase(0, e + 4);
                return true;
            }
            if (buf.size() > (1u << 20) || !fill()) return false;
        }
    }
    bool read_line(std::string &line) {  // without the CRLF
        for (;;) {
            size_t e = buf.find("\r\n");
            if (e != std::string::npos) {
                line = buf.substr(0, e);
                buf.erase(0, e + 2);
                return true;
            }
            if (!fill()) return false;
        }
    }
    // Forwards exactly n bytes to `to`.
    bool forward(size_t n, sock_t to) {
        while (n > 0) {
            if (buf.empty() && !fill()) return false;
            size_t k = std::min(n, buf.size());
            if (!send_all(to, buf.data(), k)) return false;
            buf.erase(0, k);
            n -= k;
        }
        return true;
    }
};

bool forward_chunked(Reader &r, sock_t up) {
    for (;;) {
        std::string line;
        if (!r.read_line(line) || !send_all(up, (line + "\r\n").data(), line.size() + 2)) return false;
        size_t size = std::strtoull(line.c_str(), nullptr, 16);
        if (size == 0) {  // trailers up to the empty line
            for (;;) {
                if (!r.read_line(line) || !send_all(up, (line + "\r\n").data(), line.size() + 2)) return false;
                if (line.empty()) return true;
            }
        }
        if (!r.forward(size + 2, up)) return false;
    }
}

void pump_up_to_client(std::shared_ptr<Conn> c, sock_t up) {
    char tmp[16384];
    for (;;) {
        int k = static_cast<int>(recv(up, tmp, sizeof tmp, 0));
        if (k <= 0) break;
        if (!send_all(c->client, tmp, static_cast<size_t>(k))) break;
    }
    // Upstream is gone: end the client connection too (the client reconnects).
    std::lock_guard<std::mutex> lk(c->mu);
    if (c->client != kBadSock) shutdown_both(c->client);
}

void serve(std::shared_ptr<Conn> c) {
    Reader r{c->client, {}};
    std::string head;
    while (g_running && r.read_head(head)) {
        // Rewrite the request head.
        std::string out;
        size_t content_length = 0;
        bool chunked = false;
        size_t pos = 0;
        bool first = true;
        while (pos <= head.size()) {
            size_t e = head.find("\r\n", pos);
            std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? head.size() + 1 : e + 2;
            if (first) {
                out += line + "\r\n";
                first = false;
                continue;
            }
            size_t colon = line.find(':');
            std::string name = colon == std::string::npos ? "" : lower(trim(line.substr(0, colon)));
            std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
            if (name == "authorization") continue;  // replaced below
            if (name == "host") line = line.substr(0, colon + 1) + " " + rewrite_host(value);
            if (name == "content-length") content_length = std::strtoull(trim(value).c_str(), nullptr, 10);
            if (name == "transfer-encoding" && lower(value).find("chunked") != std::string::npos) chunked = true;
            out += line + "\r\n";
        }
        out += "Authorization: Bearer " + g_token + "\r\n\r\n";
        {
            std::lock_guard<std::mutex> lk(c->mu);
            if (c->up == kBadSock) {
                c->up = connect_loopback(g_upstream_port);
                if (c->up == kBadSock) break;
                sock_t up = c->up;
                c->pump = std::thread(pump_up_to_client, c, up);
            }
        }
        if (!send_all(c->up, out.data(), out.size())) break;
        if (chunked) {
            if (!forward_chunked(r, c->up)) break;
        } else if (content_length > 0) {
            if (!r.forward(content_length, c->up)) break;
        }
    }
    // The client left (or the upstream did): closing the upstream connection cancels a streamed request.
    c->shutdown_all();
    if (c->pump.joinable()) c->pump.join();
    {
        std::lock_guard<std::mutex> lk(g_conns_mu);
        g_conns.erase(c.get());
    }
    std::lock_guard<std::mutex> lk(c->mu);
    if (c->up != kBadSock) close_sock(c->up);
    if (c->client != kBadSock) close_sock(c->client);
    c->up = c->client = kBadSock;
}

void accept_loop() {
    while (g_running) {
        sock_t s = accept(g_listen, nullptr, nullptr);
        if (s == kBadSock) {
            if (!g_running) break;
            continue;
        }
        int one = 1;
        setsockopt(s, IPPROTO_TCP, 1 /* TCP_NODELAY */, reinterpret_cast<const char *>(&one), sizeof one);
        auto c = std::make_shared<Conn>();
        c->client = s;
        {
            std::lock_guard<std::mutex> lk(g_conns_mu);
            g_conns.insert(c.get());
        }
        std::thread([c] { serve(c); }).detach();
    }
}

}  // namespace

bool relay_start(int listen_port, int upstream_port, const std::string &token) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    g_listen_port = listen_port;
    g_upstream_port = upstream_port;
    g_token = token;
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == kBadSock) return false;
#ifndef _WIN32
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(listen_port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_listen, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(g_listen, 128) != 0) {
        close_sock(g_listen);
        g_listen = kBadSock;
        return false;
    }
    g_running = true;
    g_accept = std::thread(accept_loop);
    return true;
}

void relay_stop() {
    if (!g_running.exchange(false)) return;
    if (g_listen != kBadSock) {
        shutdown_both(g_listen);
        close_sock(g_listen);
        g_listen = kBadSock;
    }
    {
        // Wake the open connections: their threads end on their own.
        std::lock_guard<std::mutex> lk(g_conns_mu);
        for (Conn *c : g_conns) c->shutdown_all();
    }
    if (g_accept.joinable()) g_accept.join();
}

}  // namespace conformance
