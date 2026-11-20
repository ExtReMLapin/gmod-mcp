#include "windowshot.h"

#include <algorithm>

namespace windowshot {

const char *source_name(Source s) { return s == Source::Render ? "render" : "window_capture"; }

SourceChoice choose_source(const SourceFacts &f) {
    SourceChoice c;
    auto refuse = [&](const char *code, const char *message) {
        c.code = code;
        c.message = message;
        return c;
    };
    const std::string &want = f.requested;
    if (!want.empty() && want != "auto" && want != "render" && want != "window_capture")
        return refuse("invalid_params", "source must be \"auto\", \"render\" or \"window_capture\"");
    if (!f.client_mode) return refuse("unsupported", "no renderer and no game window in srcds mode");
    if (want == "render") {
        if (!f.client_realm_present)
            return refuse("realm_unavailable",
                          "source \"render\" needs the client realm: load a map first, or use source \"window_capture\" (it captures the game window "
                          "without one: main menu, dialogs, loading screens)");
        c.source = Source::Render;
        return c;
    }
    if (want == "window_capture") {
        if (!f.windows) return refuse("unsupported", "window capture needs Windows (Windows Graphics Capture)");
        if (f.client_realm_present && !f.client_allowed)
            return refuse("client_locked", "the client is connected to a server that is not on this machine: its window is not captured either");
        c.source = Source::WindowCapture;
        return c;
    }
    // auto
    if (f.client_realm_present) {
        c.source = Source::Render;  // a locked client is answered by the render path itself (client_locked)
        return c;
    }
    if (!f.windows) return refuse("realm_unavailable", "screenshots need the client realm: load a map first (this platform cannot capture the window without one)");
    c.source = Source::WindowCapture;
    return c;
}

Crop client_crop(const CropRequest &r) {
    Crop c;
    if (r.frame.w <= 0 || r.frame.h <= 0) {
        c.message = "the captured frame is empty";
        return c;
    }
    if (r.client.w <= 0 || r.client.h <= 0) {
        c.message = "the window's client area is empty";
        return c;
    }
    // The client area in frame pixels, then clamped to the frame.
    long long x0 = (long long)r.client_x - r.frame_x, y0 = (long long)r.client_y - r.frame_y;
    long long x1 = x0 + r.client.w, y1 = y0 + r.client.h;
    long long cx0 = std::max<long long>(x0, 0), cy0 = std::max<long long>(y0, 0);
    long long cx1 = std::min<long long>(x1, r.frame.w), cy1 = std::min<long long>(y1, r.frame.h);
    if (cx1 <= cx0 || cy1 <= cy0) {
        c.message = "the window's client area is outside the captured frame";
        return c;
    }
    c.ok = true;
    c.rect = Rect{(int)cx0, (int)cy0, (int)(cx1 - cx0), (int)(cy1 - cy0)};
    c.clipped = cx0 != x0 || cy0 != y0 || cx1 != x1 || cy1 != y1;
    return c;
}

Target target_size(const SizeRequest &r) {
    Target t;
    Size base = (r.backbuffer.w > 0 && r.backbuffer.h > 0) ? r.backbuffer : r.crop;
    t.screen = base;
    t.size.w = r.w > 0 ? r.w : base.w;
    t.size.h = r.h > 0 ? r.h : base.h;
    t.scaled = t.size.w != r.crop.w || t.size.h != r.crop.h;
    return t;
}

}  // namespace windowshot
