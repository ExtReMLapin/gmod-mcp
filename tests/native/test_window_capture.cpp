// Window capture through Windows Graphics Capture (src/platform/window_capture_win.cpp), no game. The test makes its own plain
// Win32 windows in this process: window A (a title bar and four colored quadrants in its client area), then window B, a
// topmost window of another color that covers A completely. The capture of A must show A's pixels, not B's: that is the whole
// point of Windows Graphics Capture over a screen copy (which shows whatever is in front) or PrintWindow (white for a D3D9
// window). Windows are shown without activation: the test never takes the focus.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "platform/platform.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr int kClientW = 320, kClientH = 200;
const COLORREF kTL = RGB(220, 20, 20), kTR = RGB(20, 20, 220), kBL = RGB(20, 200, 20), kBR = RGB(240, 240, 20);
const COLORREF kCover = RGB(255, 0, 255);

constexpr UINT kMsgCover = WM_APP + 1, kMsgProbe = WM_APP + 2;
HWND g_cover = nullptr;

LRESULT CALLBACK proc_a(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kMsgCover) {  // on the window's own thread: the cover is a window of that thread (it needs the message loop to paint)
        RECT r{};
        GetWindowRect(h, &r);
        g_cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"gmcp_wgc_test_b", L"cover", WS_POPUP, r.left - 20, r.top - 20,
                                  r.right - r.left + 40, r.bottom - r.top + 40, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(g_cover, SW_SHOWNOACTIVATE);
        SetWindowPos(g_cover, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        UpdateWindow(g_cover);
        return 0;
    }
    if (m == kMsgProbe) {  // what a screen copy sees at the center of A's client area, in this thread's own coordinates
        POINT c{0, 0};
        RECT rc{};
        ClientToScreen(h, &c);
        GetClientRect(h, &rc);
        HDC dc = GetDC(nullptr);
        COLORREF px = GetPixel(dc, c.x + (rc.right - rc.left) / 2, c.y + (rc.bottom - rc.top) / 2);
        ReleaseDC(nullptr, dc);
        return (LRESULT)px;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        int mx = (rc.right - rc.left) / 2, my = (rc.bottom - rc.top) / 2;
        auto fill = [&](int x0, int y0, int x1, int y1, COLORREF c) {
            HBRUSH b = CreateSolidBrush(c);
            RECT r{x0, y0, x1, y1};
            FillRect(dc, &r, b);
            DeleteObject(b);
        };
        fill(0, 0, mx, my, kTL);
        fill(mx, 0, rc.right, my, kTR);
        fill(0, my, mx, rc.bottom, kBL);
        fill(mx, my, rc.right, rc.bottom, kBR);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK proc_b(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(kCover);
        FillRect(dc, &rc, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

// The windows live on a thread of their own with a message loop, as the game's window does. aware: that thread is
// per-monitor DPI aware (physical pixels everywhere); otherwise it is unaware like the test process itself (on a 150 % screen the
// window is then virtualized: its client area is 320x200 for the thread and 480x300 on the screen, as a game that never
// declared DPI awareness).
struct Windows {
    std::atomic<HWND> a{nullptr};
    std::atomic<bool> ready{false}, quit{false};
    std::thread th;

    void start(bool aware) {
        th = std::thread([this, aware] {
            if (aware) SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            WNDCLASSW ca{};
            ca.lpfnWndProc = proc_a;
            ca.hInstance = GetModuleHandleW(nullptr);
            ca.lpszClassName = L"gmcp_wgc_test_a";
            RegisterClassW(&ca);
            WNDCLASSW cb = ca;
            cb.lpfnWndProc = proc_b;
            cb.lpszClassName = L"gmcp_wgc_test_b";
            RegisterClassW(&cb);
            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT rc{0, 0, kClientW, kClientH};
            AdjustWindowRectEx(&rc, style, FALSE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
            HWND wa = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, ca.lpszClassName, L"gmod-mcp capture test", style, 120, 120, rc.right - rc.left,
                                      rc.bottom - rc.top, nullptr, nullptr, ca.hInstance, nullptr);
            ShowWindow(wa, SW_SHOWNOACTIVATE);
            UpdateWindow(wa);
            a = wa;
            ready = true;
            MSG msg;
            while (!quit) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            }
            if (g_cover) DestroyWindow(g_cover);
            g_cover = nullptr;
            DestroyWindow(wa);
        });
        while (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // A topmost window over A (A's whole outer rectangle, and more).
    void cover() { SendMessageW(a.load(), kMsgCover, 0, 0); }
    COLORREF screen_color_at_center() { return (COLORREF)SendMessageW(a.load(), kMsgProbe, 0, 0); }
    // The client area in physical pixels, whatever the window's DPI awareness.
    SIZE physical_client() {
        DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        RECT rc{};
        GetClientRect(a.load(), &rc);
        if (old) SetThreadDpiAwarenessContext(old);
        return SIZE{rc.right - rc.left, rc.bottom - rc.top};
    }
    void stop() {
        quit = true;
        if (th.joinable()) th.join();
    }
};

int chan_r(COLORREF c) { return (int)(c & 0xFF); }
int chan_g(COLORREF c) { return (int)((c >> 8) & 0xFF); }
int chan_b(COLORREF c) { return (int)((c >> 16) & 0xFF); }

// The screen shows the cover's magenta, perhaps dimmed by the display's own color handling (HDR, night light, a profile): the
// hue is what counts, and none of A's quadrants has it.
bool is_cover(COLORREF c) {
    int r = chan_r(c), g = chan_g(c), b = chan_b(c);
    return r > 100 && b > 100 && g < 40 && std::abs(r - b) < 40;
}

void pause_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool near_color(const std::vector<uint8_t> &bgra, int w, int x, int y, COLORREF c, int tol = 8) {
    const uint8_t *p = &bgra[((size_t)y * w + x) * 4];
    return std::abs((int)p[0] - (int)chan_b(c)) <= tol && std::abs((int)p[1] - (int)chan_g(c)) <= tol &&
           std::abs((int)p[2] - (int)chan_r(c)) <= tol;
}

// Decodes an encoded image with WIC: size and one pixel (BGRA).
bool decode(const std::string &data, UINT &w, UINT &h, uint8_t px[4], int at_x, int at_y) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICStream *st = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *fr = nullptr;
    IWICFormatConverter *conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((WICInProcPointer)data.data(), (DWORD)data.size())) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(fr->GetSize(&w, &h)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        (UINT)at_x < w && (UINT)at_y < h) {
        WICRect r{at_x, at_y, 1, 1};
        ok = SUCCEEDED(conv->CopyPixels(&r, 4, 4, px));
    }
    if (conv) conv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    f->Release();
    return ok;
}

}  // namespace

// Windows Graphics Capture of a window of this process, in one DPI awareness. WGC-1..WGC-4 in docs/TESTS.md.
static void capture_variant(bool aware) {
    std::printf("window capture: %s windows\n", aware ? "DPI-aware" : "DPI-unaware");
    plat::WindowBitmap bmp;
    std::string code, err;
    Windows w;
    w.start(aware);
    pause_ms(400);  // the first paint
    uint64_t ha = reinterpret_cast<uint64_t>(w.a.load());
    SIZE expect = w.physical_client();  // 320x200 for an aware window, scaled for an unaware one
    CHECK(expect.cx >= kClientW && expect.cy >= kClientH);

    // Uncovered: the client area only (no title bar, no borders), in physical pixels, the four quadrants where they belong.
    bool ok = plat::window_capture_bitmap(ha, 2000, bmp, code, err);
    CHECK(ok);
    if (!ok) std::printf("  %s: %s\n", code.c_str(), err.c_str());
    if (ok) {
        CHECK(bmp.w == expect.cx && bmp.h == expect.cy);
        if (bmp.w != expect.cx || bmp.h != expect.cy) std::printf("  got %dx%d, expected %ldx%ld (frame %dx%d)\n", bmp.w, bmp.h, expect.cx, expect.cy, bmp.frame_w, bmp.frame_h);
        CHECK(bmp.frame_w > bmp.w && bmp.frame_h > bmp.h);  // the frame is the whole window: it has a title bar
        CHECK(!bmp.clipped);
        CHECK((int)bmp.bgra.size() == bmp.w * bmp.h * 4);
        if ((int)bmp.bgra.size() == bmp.w * bmp.h * 4 && bmp.w == expect.cx && bmp.h == expect.cy) {
            int W = bmp.w, H = bmp.h;
            CHECK(near_color(bmp.bgra, W, W / 4, H / 4, kTL));
            CHECK(near_color(bmp.bgra, W, 3 * W / 4, H / 4, kTR));
            CHECK(near_color(bmp.bgra, W, W / 4, 3 * H / 4, kBL));
            CHECK(near_color(bmp.bgra, W, 3 * W / 4, 3 * H / 4, kBR));
            // The crop is exact: the pixels on the four edges (not the corners: Windows 11 rounds the window's bottom corners)
            // are the quadrants' own colors, not the title bar's or a border's. A DPI-unaware window is stretched by DWM, which
            // blends the outermost pixels with the border: two pixels in.
            int e = aware ? 0 : 2;
            CHECK(near_color(bmp.bgra, W, W / 4, e, kTL));
            CHECK(near_color(bmp.bgra, W, 3 * W / 4, e, kTR));
            CHECK(near_color(bmp.bgra, W, W / 4, H - 1 - e, kBL));
            CHECK(near_color(bmp.bgra, W, 3 * W / 4, H - 1 - e, kBR));
            CHECK(near_color(bmp.bgra, W, e, H / 4, kTL));
            CHECK(near_color(bmp.bgra, W, W - 1 - e, H / 4, kTR));
            CHECK(near_color(bmp.bgra, W, e, 3 * H / 4, kBL));
            CHECK(near_color(bmp.bgra, W, W - 1 - e, 3 * H / 4, kBR));
        }
        CHECK(bmp.capture_ms < 2000);
    }

    // Covered by a topmost window of another color: the capture still shows A. A screen copy shows B (checked first, so
    // that a test that cannot cover the window does not pass for the wrong reason).
    w.cover();
    pause_ms(400);
    CHECK(is_cover(w.screen_color_at_center()));
    plat::WindowBitmap covered;
    ok = plat::window_capture_bitmap(ha, 2000, covered, code, err);
    CHECK(ok);
    if (ok && covered.w == expect.cx && covered.h == expect.cy && (int)covered.bgra.size() == covered.w * covered.h * 4) {
        int W = covered.w, H = covered.h;
        CHECK(near_color(covered.bgra, W, W / 4, H / 4, kTL));
        CHECK(near_color(covered.bgra, W, 3 * W / 4, 3 * H / 4, kBR));
        CHECK(near_color(covered.bgra, W, W / 4, 3 * H / 4, kBL));
        CHECK(near_color(covered.bgra, W, 3 * W / 4, H / 4, kTR));
        CHECK(!near_color(covered.bgra, W, W / 2, H / 2, kCover) && !near_color(covered.bgra, W, W / 4, H / 4, kCover));
    } else {
        CHECK(false);
    }
    // The capture took neither the focus nor the top: B is still in front of A.
    CHECK(GetForegroundWindow() != w.a.load());
    CHECK(is_cover(w.screen_color_at_center()));

    // Encoding: the size asked for, whichever the encoder.
    if (ok && aware) {  // once is enough: the encoder does not depend on the DPI
        std::string png, jpg;
        CHECK(plat::window_encode(covered, 0, 0, "png", 90, png, err));
        UINT dw = 0, dh = 0;
        uint8_t px[4] = {};
        CHECK(png.size() > 8 && png.compare(1, 3, "PNG") == 0);
        CHECK(decode(png, dw, dh, px, covered.w / 4, covered.h / 4) && dw == (UINT)covered.w && dh == (UINT)covered.h);
        CHECK(std::abs((int)px[2] - chan_r(kTL)) <= 8 && std::abs((int)px[1] - chan_g(kTL)) <= 8);
        // Scaled down (DPI scaling or w/h): half size keeps the quadrants.
        int hw = covered.w / 2, hh = covered.h / 2;
        CHECK(plat::window_encode(covered, hw, hh, "png", 90, png, err));
        CHECK(decode(png, dw, dh, px, 3 * hw / 4, 3 * hh / 4) && dw == (UINT)hw && dh == (UINT)hh);
        CHECK(std::abs((int)px[2] - chan_r(kBR)) <= 8 && std::abs((int)px[1] - chan_g(kBR)) <= 8 && std::abs((int)px[0] - chan_b(kBR)) <= 8);
        // JPEG: starts with the SOI marker, the size is right, the color is close (lossy).
        CHECK(plat::window_encode(covered, 0, 0, "jpeg", 90, jpg, err));
        const unsigned char *j = reinterpret_cast<const unsigned char *>(jpg.data());
        CHECK(jpg.size() > 4 && j[0] == 0xFF && j[1] == 0xD8);
        CHECK(decode(jpg, dw, dh, px, covered.w / 4, covered.h / 4) && dw == (UINT)covered.w && dh == (UINT)covered.h);
        CHECK(std::abs((int)px[2] - chan_r(kTL)) <= 24);
        // A lower quality is smaller.
        std::string low;
        CHECK(plat::window_encode(covered, 0, 0, "jpeg", 10, low, err));
        CHECK(!low.empty() && low.size() < jpg.size());
        // Refusals: an unknown format, an empty bitmap.
        CHECK(!plat::window_encode(covered, 0, 0, "bmp", 90, low, err) && !err.empty());
        plat::WindowBitmap empty;
        CHECK(!plat::window_encode(empty, 0, 0, "png", 90, low, err));
    }

    // Minimized: refused, and the window is neither restored nor activated by the call.
    ShowWindow(w.a.load(), SW_SHOWMINNOACTIVE);
    pause_ms(200);
    CHECK(!plat::window_capture_bitmap(ha, 2000, bmp, code, err));
    CHECK(code == "window_unavailable" && err.find("minimized") != std::string::npos);
    CHECK(IsIconic(w.a.load()) != 0);

    // A window that is gone.
    HWND gone = w.a.load();
    w.stop();
    CHECK(!plat::window_capture_bitmap(reinterpret_cast<uint64_t>(gone), 2000, bmp, code, err));
    CHECK(code == "window_unavailable");
}

void test_window_capture() {
    if (!plat::window_capture_supported()) {
        std::printf("window capture: SKIPPED (Windows Graphics Capture is not available here)\n");
        return;
    }
    plat::WindowBitmap bmp;
    std::string code, err;

    // No window of this process yet: the game's main window is not found.
    CHECK(!plat::window_capture_bitmap(0, 2000, bmp, code, err));
    CHECK(code == "window_unavailable" && !err.empty());
    // Never anything but this process's own windows: the desktop and the shell belong to other processes.
    CHECK(!plat::window_capture_bitmap(reinterpret_cast<uint64_t>(GetDesktopWindow()), 2000, bmp, code, err));
    CHECK(code == "window_unavailable" && err.find("not a window of this process") != std::string::npos);
    HWND shell = GetShellWindow();
    if (shell) {
        CHECK(!plat::window_capture_bitmap(reinterpret_cast<uint64_t>(shell), 2000, bmp, code, err));
        CHECK(code == "window_unavailable");
    }
    CHECK(!plat::window_capture_bitmap(0x1234, 2000, bmp, code, err));  // not a window
    CHECK(code == "window_unavailable");

    capture_variant(false);
    capture_variant(true);
}
#else
void test_window_capture() {}
#endif
