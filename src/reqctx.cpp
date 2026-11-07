#include "reqctx.h"

#include <map>
#include <mutex>

namespace reqctx {

static thread_local Ctx *t_ctx = nullptr;
static std::mutex g_mu;
static std::map<std::string, std::shared_ptr<std::atomic<bool>>> g_tracked;

Scope::Scope(Ctx *ctx) : prev_(t_ctx) { t_ctx = ctx; }
Scope::~Scope() { t_ctx = prev_; }

Ctx *current() { return t_ctx; }

bool cancelled() { return t_ctx && t_ctx->cancel && t_ctx->cancel->load(); }

void progress(double done, double total, const std::string &message) {
    if (t_ctx && t_ctx->progress) t_ctx->progress(done, total, message);
}

void track(const std::string &request_id, std::shared_ptr<std::atomic<bool>> cancel) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_tracked[request_id] = std::move(cancel);
}

void untrack(const std::string &request_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_tracked.erase(request_id);
}

bool cancel_tracked(const std::string &request_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_tracked.find(request_id);
    if (it == g_tracked.end()) return false;
    it->second->store(true);
    return true;
}

}  // namespace reqctx
