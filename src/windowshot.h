// Window capture (screenshot without a client realm): the pure part. Which source a screenshot uses, where the client area is in
// the captured frame, and the size of the image. No Win32 and no game access here (native unit tests cover it); the Windows
// Graphics Capture side is plat::window_capture in src/platform/window_capture.cpp, the tool is in tools_world.cpp.
#pragma once

#include <string>

namespace windowshot {

struct Size {
    int w = 0, h = 0;
};
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

// --- Which source ---

enum class Source { Render, WindowCapture };
const char *source_name(Source s);  // "render" | "window_capture"

struct SourceFacts {
    std::string requested;  // the `source` argument: "", "auto", "render" or "window_capture"
    bool client_mode = true;   // false: srcds (no window)
    bool windows = true;       // false: no window capture on this platform
    bool client_realm_present = false;
    bool client_allowed = true;  // the network lock (agent::client_allowed)
};
struct SourceChoice {
    Source source = Source::Render;
    std::string code, message;  // code != "": refused (unsupported, invalid_params, realm_unavailable, client_locked)
};
// "auto" (and no argument): the engine's render capture when the client realm exists, the window capture otherwise. A forced
// source that cannot work is refused with its own code instead of falling back silently.
SourceChoice choose_source(const SourceFacts &f);

// --- Geometry ---

// The captured frame is the whole window (title bar and borders included); the image is its client area. Positions are
// screen coordinates: the frame's top-left is the window's visible bounds (DWMWA_EXTENDED_FRAME_BOUNDS), the client area's is
// ClientToScreen(0, 0). Both must be read in the same DPI awareness.
struct CropRequest {
    Size frame;
    int frame_x = 0, frame_y = 0;
    int client_x = 0, client_y = 0;
    Size client;
};
struct Crop {
    bool ok = false;
    Rect rect;            // in frame pixels, clamped to the frame
    bool clipped = false;  // the client area reaches outside the frame: rect is smaller than the client area
    std::string message;  // when !ok
};
Crop client_crop(const CropRequest &r);

// The size of the image. By default the backbuffer's (what `screenshot` returns at screen size and what ui_click's
// coordinates are read on), else the cropped client area's. w/h override it, a missing one keeps the default (as the render
// path does); the crop is scaled when its size differs.
struct SizeRequest {
    Size crop;
    Size backbuffer;  // 0 when not known
    int w = 0, h = 0;  // the caller's w/h argument, 0 = not given
};
struct Target {
    Size size;
    Size screen;        // what the result reports as screen_w/screen_h
    bool scaled = false;  // size differs from the crop
};
Target target_size(const SizeRequest &r);

}  // namespace windowshot
