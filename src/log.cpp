#include "log.h"

#include <cstdarg>
#include <cstdio>

#include "platform/platform.h"

namespace logx {

using MsgFn = void (*)(const char *, ...);
static MsgFn g_msg = nullptr;
static MsgFn g_warn = nullptr;

void init() {
    void *t0 = plat::find_loaded_module(plat::tier0_names());
    g_msg = reinterpret_cast<MsgFn>(plat::find_symbol(t0, "Msg"));
    g_warn = reinterpret_cast<MsgFn>(plat::find_symbol(t0, "Warning"));
}

static void emit(MsgFn fn, const char *fmt, va_list ap) {
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (fn)
        fn("[mcpagent] %s\n", buf);
    else
        std::fprintf(stderr, "[mcpagent] %s\n", buf);
}

void info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(g_msg, fmt, ap);
    va_end(ap);
}

void warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(g_warn ? g_warn : g_msg, fmt, ap);
    va_end(ap);
}

}  // namespace logx
