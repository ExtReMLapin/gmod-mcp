// Window capture, the pure part (src/windowshot.cpp): the source decision, the crop of the client area out of the captured frame,
// the size of the image. No game, no Win32.
#include <string>

#include "check.h"
#include "windowshot.h"

using namespace windowshot;

static bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

static SourceFacts facts(const char *requested, bool client_realm, bool allowed = true) {
    SourceFacts f;
    f.requested = requested;
    f.client_realm_present = client_realm;
    f.client_allowed = allowed;
    return f;
}

static void test_source() {
    CHECK(std::string(source_name(Source::Render)) == "render");
    CHECK(std::string(source_name(Source::WindowCapture)) == "window_capture");

    // auto (and no argument): the render capture when the client realm exists, the window capture otherwise.
    SourceChoice c = choose_source(facts("auto", true));
    CHECK(c.code.empty() && c.source == Source::Render);
    c = choose_source(facts("", true));
    CHECK(c.code.empty() && c.source == Source::Render);
    c = choose_source(facts("auto", false));
    CHECK(c.code.empty() && c.source == Source::WindowCapture);
    c = choose_source(facts("", false));
    CHECK(c.code.empty() && c.source == Source::WindowCapture);

    // Forced: render needs the client realm, window_capture works with or without it.
    c = choose_source(facts("render", true));
    CHECK(c.code.empty() && c.source == Source::Render);
    c = choose_source(facts("render", false));
    CHECK(c.code == "realm_unavailable" && contains(c.message, "window_capture"));
    c = choose_source(facts("window_capture", true));
    CHECK(c.code.empty() && c.source == Source::WindowCapture);
    c = choose_source(facts("window_capture", false));
    CHECK(c.code.empty() && c.source == Source::WindowCapture);

    // A locked client (connected to a server that is not on this machine): auto keeps the render path, which answers
    // client_locked itself; a forced window capture is refused too (the lock covers what the client shows).
    c = choose_source(facts("auto", true, false));
    CHECK(c.code.empty() && c.source == Source::Render);
    c = choose_source(facts("window_capture", true, false));
    CHECK(c.code == "client_locked");
    // Without a client realm there is nothing to lock.
    c = choose_source(facts("auto", false, false));
    CHECK(c.code.empty() && c.source == Source::WindowCapture);

    // srcds has no window at all; a platform without window capture cannot fall back.
    SourceFacts f = facts("auto", false);
    f.client_mode = false;
    CHECK(choose_source(f).code == "unsupported");
    f = facts("auto", false);
    f.windows = false;
    c = choose_source(f);
    CHECK(c.code == "realm_unavailable" && contains(c.message, "map"));  // the old answer: load a map first
    f = facts("window_capture", false);
    f.windows = false;
    CHECK(choose_source(f).code == "unsupported");
    f = facts("auto", true);
    f.windows = false;
    CHECK(choose_source(f).code.empty() && choose_source(f).source == Source::Render);

    // An unknown value.
    c = choose_source(facts("gpu", true));
    CHECK(c.code == "invalid_params" && contains(c.message, "window_capture"));
}

static CropRequest crop_req(int fw, int fh, int fx, int fy, int cx, int cy, int cw, int ch) {
    CropRequest r;
    r.frame = {fw, fh};
    r.frame_x = fx;
    r.frame_y = fy;
    r.client_x = cx;
    r.client_y = cy;
    r.client = {cw, ch};
    return r;
}

static void test_crop() {
    // A borderless window: the frame is the client area.
    Crop c = client_crop(crop_req(1280, 720, 100, 50, 100, 50, 1280, 720));
    CHECK(c.ok && !c.clipped && c.rect.x == 0 && c.rect.y == 0 && c.rect.w == 1280 && c.rect.h == 720);
    // A window with a title bar (31 px) and 1 px borders: the client area starts below and inside.
    c = client_crop(crop_req(1282, 752, 100, 50, 101, 81, 1280, 720));
    CHECK(c.ok && !c.clipped && c.rect.x == 1 && c.rect.y == 31 && c.rect.w == 1280 && c.rect.h == 720);
    // The window on a monitor to the left of the primary one (negative screen coordinates).
    c = client_crop(crop_req(1282, 752, -1300, -40, -1299, -9, 1280, 720));
    CHECK(c.ok && !c.clipped && c.rect.x == 1 && c.rect.y == 31);
    // The client area reaches past the frame (a frame one pixel short): clamped and flagged, not refused.
    c = client_crop(crop_req(1280, 720, 0, 0, 0, 0, 1281, 721));
    CHECK(c.ok && c.clipped && c.rect.w == 1280 && c.rect.h == 720);
    // The client area starts before the frame (a rounding of one pixel): the origin is clamped to 0.
    c = client_crop(crop_req(1280, 720, 10, 10, 9, 9, 1280, 720));
    CHECK(c.ok && c.clipped && c.rect.x == 0 && c.rect.y == 0 && c.rect.w == 1279 && c.rect.h == 719);
    // Empty or absent: refused with a message.
    c = client_crop(crop_req(1280, 720, 0, 0, 0, 0, 0, 720));
    CHECK(!c.ok && !c.message.empty());
    c = client_crop(crop_req(1280, 720, 0, 0, 0, 0, 1280, 0));
    CHECK(!c.ok);
    c = client_crop(crop_req(0, 0, 0, 0, 0, 0, 1280, 720));
    CHECK(!c.ok);
    // The client area entirely outside the frame.
    c = client_crop(crop_req(1280, 720, 0, 0, 2000, 0, 1280, 720));
    CHECK(!c.ok);
    c = client_crop(crop_req(1280, 720, 0, 0, -2000, 0, 1280, 720));
    CHECK(!c.ok);
    // The last pixel is in, the next one out.
    c = client_crop(crop_req(1280, 720, 0, 0, 1279, 719, 1, 1));
    CHECK(c.ok && !c.clipped && c.rect.x == 1279 && c.rect.y == 719 && c.rect.w == 1 && c.rect.h == 1);
    c = client_crop(crop_req(1280, 720, 0, 0, 1280, 0, 1, 1));
    CHECK(!c.ok);
}

static SizeRequest size_req(int cw, int ch, int bw, int bh, int w = 0, int h = 0) {
    SizeRequest r;
    r.crop = {cw, ch};
    r.backbuffer = {bw, bh};
    r.w = w;
    r.h = h;
    return r;
}

static void test_target() {
    // Same size as the backbuffer: nothing to scale. This is what makes ui_click's coordinates match.
    Target t = target_size(size_req(1280, 720, 1280, 720));
    CHECK(t.size.w == 1280 && t.size.h == 720 && !t.scaled && t.screen.w == 1280 && t.screen.h == 720);
    // DPI scaling: the client area (150 %) is larger than the backbuffer: scaled down to the backbuffer.
    t = target_size(size_req(1920, 1080, 1280, 720));
    CHECK(t.size.w == 1280 && t.size.h == 720 && t.scaled && t.screen.w == 1280 && t.screen.h == 720);
    // A client area smaller than the backbuffer is scaled up to it (the image is read in backbuffer coordinates).
    t = target_size(size_req(1000, 600, 1280, 720));
    CHECK(t.size.w == 1280 && t.size.h == 720 && t.scaled);
    // The backbuffer is not known (menu realm unreachable): the client area, reported as the screen.
    t = target_size(size_req(1280, 720, 0, 0));
    CHECK(t.size.w == 1280 && t.size.h == 720 && !t.scaled && t.screen.w == 1280 && t.screen.h == 720);
    t = target_size(size_req(1280, 720, 1280, 0));
    CHECK(t.size.w == 1280 && t.size.h == 720 && !t.scaled);
    // w and h given: that size, screen_w/screen_h stay the backbuffer's.
    t = target_size(size_req(1280, 720, 1280, 720, 640, 360));
    CHECK(t.size.w == 640 && t.size.h == 360 && t.scaled && t.screen.w == 1280 && t.screen.h == 720);
    // Only one of them: the other keeps the default (as the render path does).
    t = target_size(size_req(1280, 720, 1280, 720, 640, 0));
    CHECK(t.size.w == 640 && t.size.h == 720 && t.scaled);
    t = target_size(size_req(1280, 720, 1280, 720, 0, 360));
    CHECK(t.size.w == 1280 && t.size.h == 360 && t.scaled);
    // Asking for exactly the crop's size scales nothing.
    t = target_size(size_req(1280, 720, 1280, 720, 1280, 720));
    CHECK(!t.scaled);
    // Asking for the crop's size when the backbuffer differs: that size, scaled is false (the pixels are the crop).
    t = target_size(size_req(1920, 1080, 1280, 720, 1920, 1080));
    CHECK(t.size.w == 1920 && t.size.h == 1080 && !t.scaled);
}

void test_windowshot() {
    test_source();
    test_crop();
    test_target();
}
