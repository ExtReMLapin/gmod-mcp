// Window capture through Windows Graphics Capture (WGC), C++/WinRT. The module runs inside the game process, so the window it
// captures is its own: GraphicsCaptureItem from the HWND (IGraphicsCaptureItemInterop), a D3D11 device, a free-threaded frame
// pool, one frame, a GPU copy of the client area into a staging texture, then the pixels. The encoder is WIC (JPEG or PNG, with
// a scaler for DPI scaling and w/h).
//
// What this never does: SetForegroundWindow, BringWindowToTop, SendInput, a screen copy (BitBlt of the screen shows what covers
// the window, PrintWindow gives a white image for a D3D9 window) or a file on disk. WGC reads the window's own composition
// surface, so other windows in front of it do not matter. It runs on a helper thread (COM apartment and DPI context of its own)
// and waits a bounded time for the frame: the game's main thread is never involved.
#ifdef _WIN32
#include "platform.h"

#include "../windowshot.h"

// C++/WinRT (C++17 build) pulls <experimental/coroutine>, which MSVC deprecates; nothing here uses a coroutine.
#ifndef _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <wincodec.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "user32.lib")

namespace plat {

namespace {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace dx = winrt::Windows::Graphics::DirectX;
namespace dx11 = winrt::Windows::Graphics::DirectX::Direct3D11;
using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// Keeps the process's multithreaded apartment alive for good (CoIncrementMTAUsage, never decremented). Without it, the last
// thread that leaves COM tears the apartment down and COM unloads Windows.Graphics.Capture while its own threads still run
// (an access violation in an unloaded GraphicsCapture.dll, seen in the tests). Threads need no CoInitialize of their own then:
// they are in the implicit MTA.
void keep_mta() {
    static std::once_flag once;
    std::call_once(once, [] {
        CO_MTA_USAGE_COOKIE cookie = nullptr;
        CoIncrementMTAUsage(&cookie);
    });
}

// What a failed step reports: an error code of the tool and a message for a human.
struct Failure {
    std::string code, message;
};

// The window to capture: this process's own, visible, not minimized.
bool resolve_window(uint64_t hwnd, HWND &out, Failure &f) {
    if (hwnd == 0) {
        UiWindow w;
        if (!ui_window(w) || !w.found) {
            f = {"window_unavailable", "no visible game window was found in this process"};
            return false;
        }
        out = reinterpret_cast<HWND>(w.hwnd);
    } else {
        out = reinterpret_cast<HWND>(hwnd);
        DWORD pid = 0;
        if (!IsWindow(out) || !GetWindowThreadProcessId(out, &pid)) {
            f = {"window_unavailable", "not a window: it is gone"};
            return false;
        }
        if (pid != GetCurrentProcessId()) {
            f = {"window_unavailable", "not a window of this process: only the game's own window is captured"};
            return false;
        }
    }
    if (IsIconic(out)) {
        f = {"window_unavailable", "the game window is minimized (Windows captures nothing from a minimized window): restore it first"};
        return false;
    }
    if (!IsWindowVisible(out)) {
        f = {"window_unavailable", "the game window is not visible"};
        return false;
    }
    return true;
}

// Creates the D3D11 device WGC copies into: the GPU, else the software rasterizer.
winrt::com_ptr<ID3D11Device> make_device(winrt::com_ptr<ID3D11DeviceContext> &ctx) {
    winrt::com_ptr<ID3D11Device> dev;
    const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE t : types) {
        hr = D3D11CreateDevice(nullptr, t, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put());
        if (SUCCEEDED(hr)) return dev;
    }
    winrt::throw_hresult(hr);
    return dev;
}

// The capture itself. Runs on the helper thread, COM (multithreaded apartment) already initialized by the caller.
bool capture_frame(HWND hwnd, int timeout_ms, WindowBitmap &out, Failure &fail) {
    // Physical pixels from every API below (a DPI-unaware game window reports virtualized ones otherwise, and DWM does not).
    DPI_AWARENESS_CONTEXT old_ctx = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    struct Restore {
        DPI_AWARENESS_CONTEXT c;
        ~Restore() {
            if (c) SetThreadDpiAwarenessContext(c);
        }
    } restore{old_ctx};

    if (!wgc::GraphicsCaptureSession::IsSupported()) {
        fail = {"unsupported", "Windows Graphics Capture is not available on this Windows (needs Windows 10 version 1803 or later)"};
        return false;
    }
    // Where the client area is inside the frame (the frame is the whole window).
    RECT bounds{}, client{};
    POINT origin{0, 0};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof bounds)) && !GetWindowRect(hwnd, &bounds)) {
        fail = {"window_unavailable", "the game window's bounds could not be read"};
        return false;
    }
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) {
        fail = {"window_unavailable", "the game window's client area could not be read"};
        return false;
    }

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    HRESULT hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr)) {
        fail = {"capture_failed", "GraphicsCaptureItem could not be created for the game window (HRESULT 0x" + std::to_string((unsigned)hr) + ")"};
        return false;
    }

    winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::com_ptr<ID3D11Device> dev = make_device(ctx);
    auto dxgi = dev.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    auto winrt_dev = inspectable.as<dx11::IDirect3DDevice>();

    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(winrt_dev, dx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());
    auto session = pool.CreateCaptureSession(item);
    // Optional (Windows 10 2004 / Windows 11 and later): no cursor, no yellow border. Failure is fine.
    try {
        session.IsCursorCaptureEnabled(false);
    } catch (...) {
    }
    try {
        session.IsBorderRequired(false);
    } catch (...) {
    }
    session.StartCapture();

    // One frame: the pool delivers the window's current content as soon as the session starts.
    wgc::Direct3D11CaptureFrame frame{nullptr};
    auto t0 = Clock::now();
    while (!(frame = pool.TryGetNextFrame())) {
        if (ms_since(t0) > timeout_ms) {
            session.Close();
            pool.Close();
            fail = {"capture_timeout", "Windows delivered no frame of the game window within " + std::to_string(timeout_ms) + " ms"};
            return false;
        }
        Sleep(4);
    }

    auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void()));
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    auto content = frame.ContentSize();

    windowshot::CropRequest cr;
    cr.frame = {std::min<int>(content.Width, (int)desc.Width), std::min<int>(content.Height, (int)desc.Height)};
    cr.frame_x = bounds.left;
    cr.frame_y = bounds.top;
    cr.client_x = origin.x;
    cr.client_y = origin.y;
    cr.client = {client.right - client.left, client.bottom - client.top};
    windowshot::Crop crop = windowshot::client_crop(cr);
    if (!crop.ok) {
        frame.Close();
        session.Close();
        pool.Close();
        fail = {"capture_failed", crop.message};
        return false;
    }

    // Copy the client area into a staging texture (GPU side), then read it.
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = (UINT)crop.rect.w;
    sd.Height = (UINT)crop.rect.h;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(dev->CreateTexture2D(&sd, nullptr, staging.put()));
    D3D11_BOX box{(UINT)crop.rect.x, (UINT)crop.rect.y, 0, (UINT)(crop.rect.x + crop.rect.w), (UINT)(crop.rect.y + crop.rect.h), 1};
    ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE map{};
    winrt::check_hresult(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map));
    out.w = crop.rect.w;
    out.h = crop.rect.h;
    out.bgra.assign((size_t)out.w * out.h * 4, 0);
    for (int y = 0; y < out.h; ++y) {
        const uint8_t *src = static_cast<const uint8_t *>(map.pData) + (size_t)y * map.RowPitch;
        uint8_t *dst = &out.bgra[(size_t)y * out.w * 4];
        std::memcpy(dst, src, (size_t)out.w * 4);
        for (int x = 0; x < out.w; ++x) dst[x * 4 + 3] = 255;  // opaque: the window's alpha is not an image's
    }
    ctx->Unmap(staging.get(), 0);
    out.frame_w = cr.frame.w;
    out.frame_h = cr.frame.h;
    out.clipped = crop.clipped;

    frame.Close();
    session.Close();
    pool.Close();
    return true;
}

// The helper thread's result, shared with the caller so that a caller that gave up can leave it behind.
struct Shared {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, ok = false;
    WindowBitmap bmp;
    Failure fail;
};

void run_capture(std::shared_ptr<Shared> sh, HWND hwnd, int timeout_ms) {
    WindowBitmap bmp;
    Failure fail;
    bool ok = false;
    try {
        ok = capture_frame(hwnd, timeout_ms, bmp, fail);
    } catch (winrt::hresult_error const &e) {
        fail = {"capture_failed", "Windows Graphics Capture failed (HRESULT 0x" + std::to_string((unsigned)e.code().value) + "): " + winrt::to_string(e.message())};
    } catch (std::exception const &e) {
        fail = {"capture_failed", std::string("window capture failed: ") + e.what()};
    } catch (...) {
        fail = {"capture_failed", "window capture failed"};
    }
    std::lock_guard<std::mutex> lk(sh->m);
    sh->ok = ok;
    sh->bmp = std::move(bmp);
    sh->fail = fail;
    sh->done = true;
    sh->cv.notify_all();
}

}  // namespace

bool window_capture_supported() {
    // Cheap and local: the OS build is what decides (the session API exists from Windows 10 1803).
    OSVERSIONINFOEXW vi{};
    vi.dwOSVersionInfoSize = sizeof vi;
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOEXW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
    if (fn && fn(&vi) == 0) return vi.dwMajorVersion > 10 || (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 17134);
    return true;
}

bool window_capture_bitmap(uint64_t hwnd, int timeout_ms, WindowBitmap &out, std::string &code, std::string &err) {
    out = WindowBitmap{};
    if (!window_capture_supported()) {
        code = "unsupported";
        err = "Windows Graphics Capture needs Windows 10 version 1803 or later";
        return false;
    }
    HWND w = nullptr;
    Failure f;
    if (!resolve_window(hwnd, w, f)) {
        code = f.code;
        err = f.message;
        return false;
    }
    if (timeout_ms < 100) timeout_ms = 100;
    keep_mta();
    auto t0 = Clock::now();
    auto sh = std::make_shared<Shared>();
    // Detached: a capture that hangs inside the driver cannot hang the caller; the thread ends by itself and drops its result.
    std::thread(run_capture, sh, w, timeout_ms).detach();
    std::unique_lock<std::mutex> lk(sh->m);
    if (!sh->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms + 1500), [&] { return sh->done; })) {
        code = "capture_timeout";
        err = "the window capture did not finish within " + std::to_string(timeout_ms + 1500) + " ms";
        return false;
    }
    if (!sh->ok) {
        code = sh->fail.code.empty() ? "capture_failed" : sh->fail.code;
        err = sh->fail.message;
        return false;
    }
    out = std::move(sh->bmp);
    out.capture_ms = ms_since(t0);
    return true;
}

bool window_encode(const WindowBitmap &bmp, int out_w, int out_h, const std::string &format, int quality, std::string &data, std::string &err) {
    data.clear();
    if (format != "jpeg" && format != "png") {
        err = "unknown image format \"" + format + "\" (jpeg or png)";
        return false;
    }
    if (bmp.w <= 0 || bmp.h <= 0 || bmp.bgra.size() != (size_t)bmp.w * bmp.h * 4) {
        err = "there is no image to encode";
        return false;
    }
    int ow = out_w > 0 ? out_w : bmp.w, oh = out_h > 0 ? out_h : bmp.h;
    if (ow > 16384 || oh > 16384) {
        err = "the image would be larger than 16384 pixels";
        return false;
    }
    try {
        keep_mta();
        winrt::com_ptr<IWICImagingFactory> factory;
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())));
        winrt::com_ptr<IWICBitmap> src;
        winrt::check_hresult(factory->CreateBitmapFromMemory((UINT)bmp.w, (UINT)bmp.h, GUID_WICPixelFormat32bppBGR, (UINT)bmp.w * 4,
                                                             (UINT)bmp.bgra.size(), const_cast<BYTE *>(bmp.bgra.data()), src.put()));
        winrt::com_ptr<IWICBitmapSource> frame_src = src;
        if (ow != bmp.w || oh != bmp.h) {
            winrt::com_ptr<IWICBitmapScaler> scaler;
            winrt::check_hresult(factory->CreateBitmapScaler(scaler.put()));
            winrt::check_hresult(scaler->Initialize(src.get(), (UINT)ow, (UINT)oh, WICBitmapInterpolationModeFant));
            frame_src = scaler;
        }
        winrt::com_ptr<IWICFormatConverter> conv;
        winrt::check_hresult(factory->CreateFormatConverter(conv.put()));
        winrt::check_hresult(conv->Initialize(frame_src.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));

        winrt::com_ptr<IStream> stream;
        winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
        winrt::com_ptr<IWICBitmapEncoder> enc;
        winrt::check_hresult(factory->CreateEncoder(format == "png" ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, nullptr, enc.put()));
        winrt::check_hresult(enc->Initialize(stream.get(), WICBitmapEncoderNoCache));
        winrt::com_ptr<IWICBitmapFrameEncode> fe;
        winrt::com_ptr<IPropertyBag2> props;
        winrt::check_hresult(enc->CreateNewFrame(fe.put(), props.put()));
        if (format == "jpeg" && props) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = (float)std::max(1, std::min(100, quality)) / 100.0f;
            props->Write(1, &opt, &v);  // best effort: the default quality is fine too
        }
        winrt::check_hresult(fe->Initialize(props.get()));
        winrt::check_hresult(fe->SetSize((UINT)ow, (UINT)oh));
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        winrt::check_hresult(fe->SetPixelFormat(&pf));
        winrt::check_hresult(fe->WriteSource(conv.get(), nullptr));
        winrt::check_hresult(fe->Commit());
        winrt::check_hresult(enc->Commit());

        STATSTG st{};
        winrt::check_hresult(stream->Stat(&st, STATFLAG_NONAME));
        LARGE_INTEGER zero{};
        winrt::check_hresult(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
        data.resize((size_t)st.cbSize.QuadPart);
        ULONG got = 0;
        winrt::check_hresult(stream->Read(data.data(), (ULONG)data.size(), &got));
        data.resize(got);
        return true;
    } catch (winrt::hresult_error const &e) {
        err = "image encoding failed (HRESULT 0x" + std::to_string((unsigned)e.code().value) + ")";
    } catch (std::exception const &e) {
        err = std::string("image encoding failed: ") + e.what();
    }
    data.clear();
    return false;
}

}  // namespace plat
#endif
