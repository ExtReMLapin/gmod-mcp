// Images a tool returns (screenshot, screenshot_from, ui_click/ui_key screenshot_after). Pure and game-free:
//  - the size cap: an MCP client drops the whole connection when one message is too big (a full-window PNG once passed 16 MB
//    in base64, Issue #17), so no image over kMaxBase64 characters is ever sent; capture_capped() retries a too-big capture
//    in JPEG at lower quality and answers result_too_large when nothing fits;
//  - crop is done by the game (render.Capture takes a rectangle); scale is done here: a small PNG decoder, a nearest or
//    bilinear enlargement and a PNG encoder (Issue #11, item 8).
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace imagecap {

using json = nlohmann::json;

// The largest base64 payload of one image. The client's limit is 16 MB for a whole message: this leaves room for the rest.
constexpr size_t kMaxBase64 = 8u << 20;
// The largest image in bytes before base64.
constexpr size_t kMaxRaw = kMaxBase64 / 4 * 3;
// Most pixels of an enlarged crop (scale is lowered to stay under it).
constexpr long long kMaxScaledPixels = 4000000;

// True when `raw_bytes` of image data are at most kMaxRaw (their base64 is then at most kMaxBase64).
bool fits(size_t raw_bytes);

// --- Capture with a size cap ---

struct Step {
    std::string format;  // "png" | "jpeg"
    int quality = 90;    // jpeg only
};
// The attempts for a request, best first: the requested format and quality, then JPEG at decreasing quality (90, 70, 50, 30,
// never above the requested quality). A PNG request is followed by JPEG 90; a JPEG request is not retried at the same quality.
std::vector<Step> ladder(const std::string &format, int quality);

// One capture: the tool's result {ok, result, image:{data (base64), mimeType}} or an error {ok:false, error}.
using Attempt = std::function<json(const Step &)>;
// Runs the ladder until an image fits. An error from an attempt is returned as it is. The image of the result that fits is
// returned with result.size_capped = true and result.requested_format / requested_quality when it is not what was asked for.
// When nothing fits: {ok:false, error:{code:"result_too_large"}} with no image.
json capture_capped(const std::string &format, int quality, const Attempt &attempt);

// The error answered for an image that cannot be sent (code result_too_large).
json too_large_error(size_t base64_bytes);

// --- Pixels ---

struct Rgb {
    int w = 0, h = 0;
    std::vector<unsigned char> px;  // w*h*3, row by row, no padding
};
// An 8-bit non-interlaced PNG (gray, gray+alpha, RGB, RGBA, palette) to RGB. False with `err` otherwise.
bool decode_png(const std::string &bytes, Rgb &out, std::string &err);
// A PNG (deflate with a fixed Huffman code and a 32 KB window).
std::string encode_png(const Rgb &img);
// `factor` >= 1, nearest neighbour or bilinear. The result is round(w*factor) x round(h*factor).
Rgb scale(const Rgb &src, double factor, bool smooth);
// The largest factor <= `factor` for which w*h*factor^2 <= max_pixels (never below 1).
double clamp_scale(int w, int h, double factor, long long max_pixels);

// The enlargement step of `screenshot {scale}`: `out` is a capture result {ok, result, image} whose image is a PNG. The picture is
// decoded, enlarged by `factor` (lowered when the result would pass kMaxScaledPixels, then result.scale_requested says so) and
// encoded again as a PNG; result.w/h/bytes/format describe the new picture and result.scaled_from the old size. An error
// result (or a picture that still cannot fit) is returned as an error with no image.
json apply_scale(json out, double factor, bool smooth);

}  // namespace imagecap
