// Per-request context of the thread running a tool: cooperative cancellation and progress reporting.
// Set by the HTTP layer (SSE response stream), by MCP Tasks and by the legacy notifications/cancelled registry.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace reqctx {

using ProgressFn = std::function<void(double progress, double total, const std::string &message)>;

struct Ctx {
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    ProgressFn progress;  // may be empty
};

// Installs ctx on the calling thread for the lifetime of the scope (restores the previous one).
class Scope {
public:
    explicit Scope(Ctx *ctx);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    Ctx *prev_;
};

Ctx *current();
// True if the request handled by the calling thread was cancelled (client closed the stream, task cancelled...).
bool cancelled();
// Reports progress (total <= 0: unknown). No-op without a context or a listener.
void progress(double done, double total, const std::string &message);

// Legacy era: in-flight requests by JSON-RPC id, so that notifications/cancelled can reach them.
void track(const std::string &request_id, std::shared_ptr<std::atomic<bool>> cancel);
void untrack(const std::string &request_id);
bool cancel_tracked(const std::string &request_id);

}  // namespace reqctx
