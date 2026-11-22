// Issue #17 and Issue #11 (item 8): the size cap of the images a tool returns, and the PNG decode / scale / encode behind
// `screenshot {crop, scale}` (src/imagecap.cpp). No game, no Lua.
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "check.h"
#include "imagecap.h"
#include "util.h"
#include "png_fixtures.h"

using namespace imagecap;

static std::string bytes_of(const unsigned char *p, size_t n) { return std::string((const char *)p, n); }

// The pixel formulas of tests/native/png_fixtures.h.
static void rgb_px(int x, int y, int out[3]) {
    out[0] = (x * 40 + y) & 255;
    out[1] = (y * 50 + x) & 255;
    out[2] = (x * y * 7 + (x ^ y)) & 255;
}

static Rgb make_rgb(int w, int h, unsigned seed) {
    Rgb r;
    r.w = w;
    r.h = h;
    r.px.resize((size_t)w * h * 3);
    for (size_t i = 0; i < r.px.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        r.px[i] = (unsigned char)(seed >> 24);
    }
    return r;
}

static void test_fits_and_ladder() {
    CHECK(fits(0) && fits(kMaxRaw) && !fits(kMaxRaw + 1));
    // The base64 of the largest image stays under the cap that the module and the bridge enforce.
    CHECK((kMaxRaw + 2) / 3 * 4 <= kMaxBase64);
    CHECK(kMaxBase64 <= (9u << 20));  // well under the 16 MB the client drops the connection at

    auto fmts = [](const std::vector<Step> &v) {
        std::string s;
        for (const Step &st : v) s += st.format + (st.format == "jpeg" ? std::to_string(st.quality) : std::string()) + " ";
        return s;
    };
    CHECK(fmts(ladder("png", 90)) == "png jpeg90 jpeg70 jpeg50 jpeg30 ");
    CHECK(fmts(ladder("jpeg", 90)) == "jpeg90 jpeg70 jpeg50 jpeg30 ");
    CHECK(fmts(ladder("jpeg", 60)) == "jpeg60 jpeg50 jpeg30 ");
    CHECK(fmts(ladder("png", 50)) == "png jpeg50 jpeg30 ");
    CHECK(fmts(ladder("jpeg", 20)) == "jpeg20 ");
    CHECK(fmts(ladder("jpeg", 100)) == "jpeg100 jpeg90 jpeg70 jpeg50 jpeg30 ");
}

// A fake capture: the size (in base64 characters) it returns for each step.
static json fake_result(const Step &s, size_t b64_chars) {
    json res = {{"format", s.format}, {"w", 100}, {"h", 50}, {"bytes", b64_chars / 4 * 3}};
    return json{{"ok", true}, {"result", res}, {"image", {{"data", std::string(b64_chars, 'A')}, {"mimeType", s.format == "png" ? "image/png" : "image/jpeg"}}}};
}

static void test_capture_capped() {
    // Fits at once: returned as it is, no flag.
    int calls = 0;
    json r = capture_capped("png", 90, [&](const Step &s) { ++calls; return fake_result(s, 1000); });
    CHECK(calls == 1 && r.value("ok", false) && !r["result"].contains("size_capped") && r["result"]["format"] == "png");

    // The PNG is too big, the JPEG fits: the JPEG is returned and the result says so.
    calls = 0;
    std::vector<std::string> seen;
    r = capture_capped("png", 90, [&](const Step &s) {
        ++calls;
        seen.push_back(s.format + std::to_string(s.quality));
        return fake_result(s, s.format == "png" ? kMaxBase64 + 4 : 2000);
    });
    CHECK(calls == 2 && r.value("ok", false));
    CHECK(r["result"].value("size_capped", false) && r["result"].value("requested_format", "") == "png" && r["result"]["format"] == "jpeg");
    CHECK(r["result"].value("quality", 0) == 90 && r["image"]["mimeType"] == "image/jpeg" && r["image"]["data"].get<std::string>().size() == 2000);

    // The first JPEG is still too big: the quality goes down until one fits.
    calls = 0;
    r = capture_capped("png", 90, [&](const Step &s) {
        ++calls;
        return fake_result(s, s.format == "png" || s.quality > 50 ? kMaxBase64 + 4 : 3000);
    });
    CHECK(calls == 4 && r.value("ok", false) && r["result"].value("quality", 0) == 50 && r["result"].value("size_capped", false));

    // Exactly the cap fits; one character more does not.
    r = capture_capped("jpeg", 90, [&](const Step &s) { return fake_result(s, kMaxBase64); });
    CHECK(r.value("ok", false) && !r["result"].contains("size_capped"));

    // Nothing fits: an error, no image, the code of the family of lua_exec's.
    calls = 0;
    r = capture_capped("png", 90, [&](const Step &s) { ++calls; return fake_result(s, kMaxBase64 + 4); });
    CHECK(calls == 5 && !r.value("ok", true) && r["error"].value("code", "") == "result_too_large" && !r.contains("image"));
    CHECK(r["error"].value("message", "").find("crop") != std::string::npos && r["error"].value("message", "").find("w/h") != std::string::npos);

    // An error from an attempt is the answer, nothing else is tried.
    calls = 0;
    r = capture_capped("png", 90, [&](const Step &) {
        ++calls;
        return json{{"ok", false}, {"error", {{"code", "timeout"}, {"message", "no frame"}}}};
    });
    CHECK(calls == 1 && r["error"].value("code", "") == "timeout");

    json te = too_large_error(20u << 20);
    CHECK(!te.value("ok", true) && te["error"].value("code", "") == "result_too_large" && te["error"].value("message", "").find("20") != std::string::npos);
}

static void test_decode() {
    Rgb img;
    std::string err;
    CHECK(decode_png(bytes_of(kPngRgb, sizeof kPngRgb), img, err) && img.w == 64 && img.h == 48 && img.px.size() == 64 * 48 * 3);
    bool same = img.px.size() == 64 * 48 * 3;
    for (int y = 0; y < 48 && same; ++y)
        for (int x = 0; x < 64 && same; ++x) {
            int e[3];
            rgb_px(x, y, e);
            for (int k = 0; k < 3; ++k) same = same && img.px[((size_t)y * 64 + x) * 3 + k] == e[k];
        }
    CHECK(same);  // five filter types and a dynamic Huffman block decoded exactly

    // RGBA: alpha dropped.
    CHECK(decode_png(bytes_of(kPngRgba, sizeof kPngRgba), img, err) && img.w == 16 && img.h == 12);
    CHECK(img.px.size() == 16 * 12 * 3 && img.px[(5 * 16 + 3) * 3 + 0] == 48 && img.px[(5 * 16 + 3) * 3 + 1] == 100 && img.px[(5 * 16 + 3) * 3 + 2] == 15);
    // Palette: index (x+y)%4.
    CHECK(decode_png(bytes_of(kPngPalette, sizeof kPngPalette), img, err) && img.w == 8 && img.h == 6);
    CHECK(img.px[0] == 255 && img.px[1] == 0 && img.px[2] == 0);                // (0,0): index 0
    CHECK(img.px[(1) * 3] == 0 && img.px[(1) * 3 + 1] == 255);                  // (1,0): index 1
    CHECK(img.px[(3) * 3] == 10 && img.px[(3) * 3 + 2] == 30);                  // (3,0): index 3
    // Gray and gray+alpha become R=G=B.
    CHECK(decode_png(bytes_of(kPngGray, sizeof kPngGray), img, err) && img.w == 10 && img.h == 7);
    CHECK(img.px[(2 * 10 + 4) * 3] == ((4 * 25 + 2 * 3) & 255) && img.px[(2 * 10 + 4) * 3 + 1] == img.px[(2 * 10 + 4) * 3 + 2]);
    CHECK(decode_png(bytes_of(kPngGrayAlpha, sizeof kPngGrayAlpha), img, err) && img.w == 6 && img.h == 5);
    CHECK(img.px[(0 * 6 + 3) * 3] == 120 && img.px[(0 * 6 + 3) * 3 + 2] == 120);

    // Refused, with a reason, never a crash.
    CHECK(!decode_png("", img, err) && !err.empty());
    CHECK(!decode_png("not a png at all, not at all", img, err) && !err.empty());
    std::string good = bytes_of(kPngRgb, sizeof kPngRgb);
    CHECK(!decode_png(good.substr(0, 40), img, err));
    CHECK(!decode_png(good.substr(0, good.size() / 2), img, err));
    std::string bad = good;
    bad[24] = 16;  // bit depth 16
    CHECK(!decode_png(bad, img, err) && err.find("8-bit") != std::string::npos);
    bad = good;
    bad[28] = 1;  // interlaced
    CHECK(!decode_png(bad, img, err) && err.find("interlaced") != std::string::npos);
    bad = good;
    bad[18] = 0x7f;  // width 0x7f00 + ...: the pixel data cannot be that big
    CHECK(!decode_png(bad, img, err));
    for (size_t i = 41; i < good.size() - 20; i += 97) {  // corrupted deflate data: false or a wrong picture, never a crash
        bad = good;
        bad[i] = (char)(bad[i] ^ 0xA5);
        Rgb tmp;
        std::string e2;
        (void)decode_png(bad, tmp, e2);
    }
    CHECK(true);
}

static void test_encode() {
    Rgb noise = make_rgb(70, 33, 7);
    Rgb out;
    std::string err, png = encode_png(noise);
    CHECK(png.size() > 8 && png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(decode_png(png, out, err) && out.w == 70 && out.h == 33 && out.px == noise.px);

    // A flat image is small, a one-pixel image works, and a long run crosses the 258-byte match limit.
    Rgb flat;
    flat.w = 400;
    flat.h = 300;
    flat.px.assign((size_t)400 * 300 * 3, 77);
    png = encode_png(flat);
    CHECK(png.size() < 6000 && decode_png(png, out, err) && out.px == flat.px);
    Rgb one;
    one.w = one.h = 1;
    one.px = {1, 2, 3};
    CHECK(decode_png(encode_png(one), out, err) && out.px == one.px);

    // Repeating patterns (what a nearest enlargement makes) and a big noisy picture round-trip.
    Rgb pat = make_rgb(9, 7, 3);
    Rgb big = scale(pat, 8, false);
    png = encode_png(big);
    CHECK(decode_png(png, out, err) && out.w == 72 && out.h == 56 && out.px == big.px && png.size() < big.px.size() / 2);
    Rgb huge = make_rgb(640, 480, 11);
    CHECK(decode_png(encode_png(huge), out, err) && out.px == huge.px);

    // For a cross-check with another PNG reader (see TESTS.md): GMCP_PNG_DUMP names a file that receives one picture.
    if (const char *dump = std::getenv("GMCP_PNG_DUMP")) {
        std::string p = encode_png(scale(pat, 8, false));
        std::ofstream(dump, std::ios::binary).write(p.data(), (std::streamsize)p.size());
    }
}

static void test_scale() {
    Rgb src = make_rgb(6, 4, 5);
    Rgb n = scale(src, 3, false);
    CHECK(n.w == 18 && n.h == 12 && n.px.size() == 18 * 12 * 3);
    bool ok = true;
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 18; ++x)
            for (int k = 0; k < 3; ++k) ok = ok && n.px[((size_t)y * 18 + x) * 3 + k] == src.px[((size_t)(y / 3) * 6 + x / 3) * 3 + k];
    CHECK(ok);  // nearest: every source pixel becomes a 3x3 block, no mixing

    Rgb same = scale(src, 1, true);
    CHECK(same.w == 6 && same.h == 4 && same.px == src.px);

    // A non-integer factor rounds the size; nearest still samples inside the source.
    Rgb f = scale(src, 2.5, false);
    CHECK(f.w == 15 && f.h == 10);

    // Smooth: between two pixels the value is between them, the corners keep their color, and it is not a plain repeat.
    Rgb two;
    two.w = 2;
    two.h = 1;
    two.px = {0, 0, 0, 200, 100, 50};
    Rgb s = scale(two, 4, true);
    CHECK(s.w == 8 && s.h == 4);
    CHECK(s.px[0] == 0 && s.px[(7) * 3] == 200 && s.px[(7) * 3 + 1] == 100 && s.px[(7) * 3 + 2] == 50);
    bool mono = true;
    for (int x = 1; x < 8; ++x) mono = mono && s.px[(size_t)x * 3] >= s.px[(size_t)(x - 1) * 3];
    CHECK(mono && s.px[3 * 3] > 0 && s.px[3 * 3] < 200 && s.px[3 * 3] != s.px[4 * 3]);
    Rgb nn = scale(two, 4, false);
    CHECK(nn.px[3 * 3] == 0 && nn.px[4 * 3] == 200);  // nearest: a hard edge at the middle

    // clamp_scale: stays under the pixel budget, never below 1, never above the request.
    CHECK(clamp_scale(100, 50, 8, 4000000) == 8);
    double c = clamp_scale(1000, 1000, 8, 4000000);
    CHECK(c >= 1 && c <= 2.0 + 1e-9 && 1000.0 * c * 1000.0 * c <= 4000000.0 + 1);
    CHECK(clamp_scale(4000, 4000, 8, 4000000) == 1);
}

static void test_apply_scale() {
    std::string png = bytes_of(kPngRgb, sizeof kPngRgb);
    auto capture = [&](const std::string &data, const char *mime) {
        json res = {{"format", "png"}, {"w", 64}, {"h", 48}, {"screen_w", 640}, {"screen_h", 360}, {"bytes", data.size()}, {"method", "backbuffer"}};
        return json{{"ok", true}, {"result", res}, {"image", {{"data", util::base64_encode(data)}, {"mimeType", mime}}}};
    };
    json r = apply_scale(capture(png, "image/png"), 4, false);
    CHECK(r.value("ok", false) && r["result"].value("w", 0) == 256 && r["result"].value("h", 0) == 192 && r["result"]["format"] == "png");
    CHECK(r["result"].value("scale", 0.0) == 4 && r["result"].value("scale_filter", "") == "nearest" && !r["result"].contains("scale_requested"));
    CHECK(r["result"]["scaled_from"] == json({{"w", 64}, {"h", 48}}) && r["result"].value("screen_w", 0) == 640);
    CHECK(r["image"]["mimeType"] == "image/png");
    Rgb out, src;
    std::string err, bin = util::base64_decode(r["image"]["data"].get<std::string>());
    CHECK(decode_png(bin, out, err) && out.w == 256 && out.h == 192 && (int)r["result"].value("bytes", 0) == (int)bin.size());
    CHECK(decode_png(png, src, err));
    CHECK(out.px[((size_t)(5 * 4 + 1) * 256 + (7 * 4 + 2)) * 3 + 1] == src.px[((size_t)5 * 64 + 7) * 3 + 1]);

    r = apply_scale(capture(png, "image/png"), 2.5, true);
    CHECK(r.value("ok", false) && r["result"].value("w", 0) == 160 && r["result"].value("scale_filter", "") == "smooth");

    // Too large a factor is lowered, and the result says so.
    json big = capture(png, "image/png");
    big["result"]["w"] = 2000;
    big["result"]["h"] = 2000;  // the result's own numbers are not what is used: the picture is
    r = apply_scale(big, 16, false);
    CHECK(r.value("ok", false) && r["result"].value("scale", 0.0) == 16);  // 64x48 at 16 = 1024x768, under the budget
    // An error passes through unchanged; a picture that is not a PNG is an error, not a crash.
    json e = json{{"ok", false}, {"error", {{"code", "timeout"}, {"message", "x"}}}};
    CHECK(apply_scale(e, 2, false) == e);
    r = apply_scale(capture("this is not a png", "image/png"), 2, false);
    CHECK(!r.value("ok", true) && r["error"].value("code", "") == "capture_failed" && !r.contains("image"));
    r = apply_scale(capture(png, "image/jpeg"), 2, false);
    CHECK(!r.value("ok", true) && r["error"].value("code", "") == "capture_failed");
}

void test_imagecap() {
    test_apply_scale();
    test_fits_and_ladder();
    test_capture_capped();
    test_decode();
    test_encode();
    test_scale();
}
