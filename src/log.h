#pragma once

namespace logx {
void init();  // resolves Msg/Warning from tier0
void info(const char *fmt, ...);
void warn(const char *fmt, ...);
}  // namespace logx
