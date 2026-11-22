#include "imagecap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "util.h"

namespace imagecap {

bool fits(size_t raw_bytes) { return raw_bytes <= kMaxRaw; }

// --- Capture with a size cap ---

std::vector<Step> ladder(const std::string &format, int quality) {
    std::vector<Step> v;
    if (format == "png") {
        v.push_back({"png", 90});
        quality = std::min(quality, 90);
    }
    v.push_back({"jpeg", quality});
    int last = quality;
    for (int q : {90, 70, 50, 30}) {
        if (q < last) {
            v.push_back({"jpeg", q});
            last = q;
        }
    }
    return v;
}

json too_large_error(size_t base64_bytes) {
    char mb[32], cap[32];
    std::snprintf(mb, sizeof mb, "%.1f", (double)base64_bytes / (1024.0 * 1024.0));
    std::snprintf(cap, sizeof cap, "%.0f", (double)kMaxBase64 / (1024.0 * 1024.0));
    std::string msg = std::string("the image is ") + mb + " MB once encoded, over the " + cap +
                      " MB limit for one image (an MCP client drops the whole connection above 16 MB per message), so it was not sent. Ask for "
                      "less: a crop {x, y, w, h}, a smaller w/h, or format jpeg with a lower quality";
    return json{{"ok", false}, {"error", {{"code", "result_too_large"}, {"message", msg}}}};
}

json capture_capped(const std::string &format, int quality, const Attempt &attempt) {
    std::vector<Step> steps = ladder(format, quality);
    size_t last_size = 0;
    for (size_t i = 0; i < steps.size(); ++i) {
        json r = attempt(steps[i]);
        if (!r.value("ok", false) || !r.contains("image") || !r["image"].is_object() || !r["image"].contains("data")) return r;
        const json &d = r["image"]["data"];
        size_t n = d.is_string() ? d.get_ref<const std::string &>().size() : 0;
        if (n <= kMaxBase64) {
            if (i > 0) {
                r["result"]["size_capped"] = true;
                r["result"]["requested_format"] = format;
                if (steps[i].format == "jpeg") r["result"]["quality"] = steps[i].quality;
            }
            return r;
        }
        last_size = n;
    }
    return too_large_error(last_size);
}

// --- Inflate (RFC 1951), after zlib's "puff" ---

namespace {

struct Bits {
    const unsigned char *p;
    size_t n, pos = 0;
    unsigned buf = 0;
    int cnt = 0;
    bool eof = false;
    int get(int need) {
        long val = (long)buf;
        while (cnt < need) {
            if (pos >= n) {
                eof = true;
                return 0;
            }
            val |= (long)p[pos++] << cnt;
            cnt += 8;
        }
        buf = (unsigned)(val >> need);
        cnt -= need;
        return (int)(val & ((1L << need) - 1));
    }
};

struct Huff {
    short count[16];
    short symbol[320];
};

// 0 for a complete code, > 0 incomplete, < 0 over-subscribed.
int build(Huff &h, const short *len, int n) {
    for (int i = 0; i < 16; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) h.count[len[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; ++l) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = (short)(offs[l] + h.count[l]);
    for (int i = 0; i < n; ++i)
        if (len[i] != 0) h.symbol[offs[len[i]]++] = (short)i;
    return left;
}

int decode_sym(Bits &s, const Huff &h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
        code |= s.get(1);
        if (s.eof) return -1;
        int count = h.count[l];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits &s, std::vector<unsigned char> &out, size_t max_out, const Huff &lc, const Huff &dc, std::string &err) {
    for (;;) {
        int sym = decode_sym(s, lc);
        if (sym < 0) {
            err = "corrupt deflate data";
            return false;
        }
        if (sym < 256) {
            if (out.size() >= max_out) {
                err = "more pixel data than the header announces";
                return false;
            }
            out.push_back((unsigned char)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) {
                err = "corrupt deflate data";
                return false;
            }
            int len = kLenBase[sym] + s.get(kLenExtra[sym]);
            int ds = decode_sym(s, dc);
            if (ds < 0 || ds >= 30) {
                err = "corrupt deflate data";
                return false;
            }
            size_t dist = (size_t)kDistBase[ds] + (size_t)s.get(kDistExtra[ds]);
            if (s.eof || dist > out.size() || out.size() + (size_t)len > max_out) {
                err = "corrupt deflate data";
                return false;
            }
            size_t from = out.size() - dist;
            for (int i = 0; i < len; ++i) out.push_back(out[from + (size_t)i]);
        }
    }
}

bool inflate_raw(const unsigned char *data, size_t n, std::vector<unsigned char> &out, size_t max_out, std::string &err) {
    Bits s{data, n};
    int last;
    do {
        last = s.get(1);
        int type = s.get(2);
        if (s.eof) {
            err = "truncated deflate data";
            return false;
        }
        if (type == 0) {
            s.buf = 0;
            s.cnt = 0;
            if (s.pos + 4 > n) {
                err = "truncated deflate data";
                return false;
            }
            unsigned len = data[s.pos] | ((unsigned)data[s.pos + 1] << 8), nlen = data[s.pos + 2] | ((unsigned)data[s.pos + 3] << 8);
            s.pos += 4;
            if ((len ^ 0xFFFFu) != nlen || s.pos + len > n || out.size() + len > max_out) {
                err = "corrupt stored block";
                return false;
            }
            out.insert(out.end(), data + s.pos, data + s.pos + len);
            s.pos += len;
        } else if (type == 1) {
            static Huff lc, dc;
            static bool ready = false;
            if (!ready) {
                short l[288];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                build(lc, l, 288);
                for (i = 0; i < 30; ++i) l[i] = 5;
                build(dc, l, 30);
                ready = true;
            }
            if (!codes(s, out, max_out, lc, dc, err)) return false;
        } else if (type == 2) {
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int nlen = s.get(5) + 257, ndist = s.get(5) + 1, ncode = s.get(4) + 4;
            if (s.eof || nlen > 286 || ndist > 30) {
                err = "corrupt deflate data";
                return false;
            }
            short lengths[320];
            int idx = 0;
            for (; idx < ncode; ++idx) lengths[order[idx]] = (short)s.get(3);
            for (; idx < 19; ++idx) lengths[order[idx]] = 0;
            Huff lencode, lc, dc;
            if (build(lencode, lengths, 19) != 0) {
                err = "corrupt deflate data";
                return false;
            }
            idx = 0;
            while (idx < nlen + ndist) {
                int sym = decode_sym(s, lencode);
                if (sym < 0) {
                    err = "corrupt deflate data";
                    return false;
                }
                if (sym < 16) {
                    lengths[idx++] = (short)sym;
                } else {
                    int len = 0, rep;
                    if (sym == 16) {
                        if (idx == 0) {
                            err = "corrupt deflate data";
                            return false;
                        }
                        len = lengths[idx - 1];
                        rep = 3 + s.get(2);
                    } else if (sym == 17) {
                        rep = 3 + s.get(3);
                    } else {
                        rep = 11 + s.get(7);
                    }
                    if (idx + rep > nlen + ndist) {
                        err = "corrupt deflate data";
                        return false;
                    }
                    while (rep--) lengths[idx++] = (short)len;
                }
            }
            if (s.eof || lengths[256] == 0) {
                err = "corrupt deflate data";
                return false;
            }
            int e = build(lc, lengths, nlen);
            if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) {
                err = "corrupt deflate data";
                return false;
            }
            e = build(dc, lengths + nlen, ndist);
            if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) {
                err = "corrupt deflate data";
                return false;
            }
            if (!codes(s, out, max_out, lc, dc, err)) return false;
        } else {
            err = "corrupt deflate data";
            return false;
        }
    } while (!last);
    return true;
}

uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

}  // namespace

bool decode_png(const std::string &bytes, Rgb &out, std::string &err) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    const unsigned char *d = (const unsigned char *)bytes.data();
    size_t n = bytes.size();
    out = Rgb();
    if (n < 8 + 25 || std::memcmp(d, sig, 8) != 0) {
        err = "not a PNG";
        return false;
    }
    size_t pos = 8;
    int w = 0, h = 0, ctype = -1;
    bool have_ihdr = false, have_end = false;
    std::string idat;
    unsigned char palette[256 * 3];
    int palette_n = 0;
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos);
        const unsigned char *type = d + pos + 4;
        if ((size_t)len > n - pos - 12) {
            err = "truncated PNG";
            return false;
        }
        const unsigned char *body = d + pos + 8;
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len != 13) {
                err = "bad PNG header";
                return false;
            }
            uint32_t uw = be32(body), uh = be32(body + 4);
            if (uw == 0 || uh == 0 || uw > 65535 || uh > 65535 || (unsigned long long)uw * uh > 40000000ull) {
                err = "unreasonable PNG size";
                return false;
            }
            w = (int)uw;
            h = (int)uh;
            if (body[8] != 8) {
                err = "only 8-bit PNG pictures are supported";
                return false;
            }
            ctype = body[9];
            if (ctype != 0 && ctype != 2 && ctype != 3 && ctype != 4 && ctype != 6) {
                err = "unsupported PNG color type";
                return false;
            }
            if (body[12] != 0) {
                err = "interlaced PNG pictures are not supported";
                return false;
            }
            have_ihdr = true;
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette_n = (int)std::min<uint32_t>(len / 3, 256);
            std::memcpy(palette, body, (size_t)palette_n * 3);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.append((const char *)body, len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            have_end = true;
            break;
        }
        pos += 12 + (size_t)len;
    }
    if (!have_ihdr) {
        err = "PNG without a header";
        return false;
    }
    if (idat.size() < 6 || !have_end) {
        err = "truncated PNG";
        return false;
    }
    unsigned char cmf = (unsigned char)idat[0], flg = (unsigned char)idat[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) {
        err = "unsupported zlib header";
        return false;
    }
    int bpp = ctype == 2 ? 3 : ctype == 4 ? 2 : ctype == 6 ? 4 : 1;
    size_t stride = (size_t)w * bpp, expected = (size_t)h * (stride + 1);
    std::vector<unsigned char> raw;
    raw.reserve(expected);
    if (!inflate_raw((const unsigned char *)idat.data() + 2, idat.size() - 2, raw, expected, err)) return false;
    if (raw.size() != expected) {
        err = "PNG pixel data is shorter than its header says";
        return false;
    }
    // Unfilter in place.
    for (int y = 0; y < h; ++y) {
        unsigned char *row = &raw[(size_t)y * (stride + 1)];
        unsigned char f = row[0];
        unsigned char *cur = row + 1;
        const unsigned char *up = y > 0 ? &raw[(size_t)(y - 1) * (stride + 1) + 1] : nullptr;
        if (f > 4) {
            err = "bad PNG filter";
            return false;
        }
        for (size_t i = 0; i < stride; ++i) {
            int a = i >= (size_t)bpp ? cur[i - (size_t)bpp] : 0;
            int b = up ? up[i] : 0;
            int c = (up && i >= (size_t)bpp) ? up[i - (size_t)bpp] : 0;
            int pred = 0;
            switch (f) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            cur[i] = (unsigned char)(cur[i] + pred);
        }
    }
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h * 3);
    for (int y = 0; y < h; ++y) {
        const unsigned char *src = &raw[(size_t)y * (stride + 1) + 1];
        unsigned char *dst = &out.px[(size_t)y * w * 3];
        for (int x = 0; x < w; ++x) {
            const unsigned char *s = src + (size_t)x * bpp;
            unsigned char r, g, b;
            if (ctype == 2 || ctype == 6) {
                r = s[0], g = s[1], b = s[2];
            } else if (ctype == 3) {
                int i = s[0];
                if (i >= palette_n) {
                    err = "PNG palette index out of range";
                    out = Rgb();
                    return false;
                }
                r = palette[i * 3], g = palette[i * 3 + 1], b = palette[i * 3 + 2];
            } else {
                r = g = b = s[0];
            }
            dst[x * 3] = r;
            dst[x * 3 + 1] = g;
            dst[x * 3 + 2] = b;
        }
    }
    return true;
}

// --- Deflate (fixed Huffman codes, LZ77 with hash chains) and PNG ---

namespace {

struct BitWriter {
    std::string out;
    uint32_t acc = 0;
    int cnt = 0;
    void put(uint32_t v, int nbits) {  // LSB first
        acc |= v << cnt;
        cnt += nbits;
        while (cnt >= 8) {
            out.push_back((char)(acc & 0xFF));
            acc >>= 8;
            cnt -= 8;
        }
    }
    void put_code(uint32_t code, int nbits) {  // Huffman codes go MSB first
        uint32_t r = 0;
        for (int i = 0; i < nbits; ++i) r |= ((code >> i) & 1u) << (nbits - 1 - i);
        put(r, nbits);
    }
    void flush() {
        if (cnt > 0) out.push_back((char)(acc & 0xFF));
        acc = 0;
        cnt = 0;
    }
};

void put_literal(BitWriter &w, int sym) {
    if (sym < 144) w.put_code(0x30 + (uint32_t)sym, 8);
    else if (sym < 256) w.put_code(0x190 + (uint32_t)(sym - 144), 9);
    else if (sym < 280) w.put_code((uint32_t)(sym - 256), 7);
    else w.put_code(0xC0 + (uint32_t)(sym - 280), 8);
}

std::string deflate_fixed(const std::string &data) {
    BitWriter w;
    w.put(1, 1);  // final block
    w.put(1, 2);  // fixed Huffman
    const size_t n = data.size();
    const unsigned char *d = (const unsigned char *)data.data();
    const int kWindow = 32768, kHashBits = 15, kChain = 32;
    std::vector<int32_t> head((size_t)1 << kHashBits, -1), prev(kWindow, -1);
    auto hash = [&](size_t i) { return (((uint32_t)d[i] << 10) ^ ((uint32_t)d[i + 1] << 5) ^ d[i + 2]) & (((uint32_t)1 << kHashBits) - 1); };
    auto insert = [&](size_t i) {
        if (i + 3 > n) return;
        uint32_t h = hash(i);
        prev[i & (kWindow - 1)] = head[h];
        head[h] = (int32_t)i;
    };
    size_t i = 0;
    while (i < n) {
        int best_len = 0;
        size_t best_dist = 0;
        if (i + 3 <= n) {
            int32_t cand = head[hash(i)];
            int chain = 0;
            size_t maxlen = std::min<size_t>(258, n - i);
            while (cand >= 0 && chain++ < kChain && i - (size_t)cand <= (size_t)kWindow) {
                size_t l = 0;
                while (l < maxlen && d[(size_t)cand + l] == d[i + l]) ++l;
                if ((int)l > best_len) {
                    best_len = (int)l;
                    best_dist = i - (size_t)cand;
                    if (l == maxlen) break;
                }
                int32_t next = prev[(size_t)cand & (kWindow - 1)];
                if (next >= cand) break;  // an overwritten slot: the chain is no longer in order
                cand = next;
            }
        }
        if (best_len >= 3) {
            int ls = 28;
            while (kLenBase[ls] > best_len) --ls;
            put_literal(w, 257 + ls);
            if (kLenExtra[ls]) w.put((uint32_t)(best_len - kLenBase[ls]), kLenExtra[ls]);
            int ds = 29;
            while (kDistBase[ds] > (int)best_dist) --ds;
            w.put_code((uint32_t)ds, 5);
            if (kDistExtra[ds]) w.put((uint32_t)(best_dist - (size_t)kDistBase[ds]), kDistExtra[ds]);
            for (int k = 0; k < best_len; ++k) insert(i + (size_t)k);
            i += (size_t)best_len;
        } else {
            put_literal(w, d[i]);
            insert(i);
            ++i;
        }
    }
    put_literal(w, 256);
    w.flush();
    return w.out;
}

uint32_t crc32(const unsigned char *p, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t adler32(const std::string &s) {
    uint32_t a = 1, b = 0;
    for (unsigned char c : s) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void put32(std::string &s, uint32_t v) {
    for (int sh = 24; sh >= 0; sh -= 8) s.push_back((char)((v >> sh) & 0xFF));
}

void chunk(std::string &png, const char *type, const std::string &body) {
    put32(png, (uint32_t)body.size());
    std::string tb = std::string(type, 4) + body;
    png += tb;
    put32(png, crc32((const unsigned char *)tb.data(), tb.size()));
}

}  // namespace

std::string encode_png(const Rgb &img) {
    const size_t stride = (size_t)img.w * 3;
    std::string raw;
    raw.reserve((stride + 1) * (size_t)img.h);
    std::string cand[3];
    for (int y = 0; y < img.h; ++y) {
        const unsigned char *cur = img.px.data() + (size_t)y * stride;
        const unsigned char *up = y > 0 ? cur - stride : nullptr;
        long best_sum = -1;
        int best = 0;
        for (int f = 0; f < 3; ++f) {
            if (f == 2 && !up) continue;
            cand[f].resize(stride);
            long sum = 0;
            for (size_t i = 0; i < stride; ++i) {
                int a = i >= 3 ? cur[i - 3] : 0;
                int v = f == 0 ? cur[i] : f == 1 ? cur[i] - a : cur[i] - up[i];
                unsigned char b = (unsigned char)v;
                cand[f][i] = (char)b;
                sum += b < 128 ? b : 256 - b;
            }
            if (best_sum < 0 || sum < best_sum) {
                best_sum = sum;
                best = f;
            }
        }
        raw.push_back((char)best);
        raw += cand[best];
    }
    std::string z;
    z.push_back(0x78);
    z.push_back(0x01);
    z += deflate_fixed(raw);
    put32(z, adler32(raw));
    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    put32(ihdr, (uint32_t)img.w);
    put32(ihdr, (uint32_t)img.h);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}

// --- Enlargement ---

Rgb scale(const Rgb &src, double factor, bool smooth) {
    Rgb o;
    if (src.w <= 0 || src.h <= 0) return o;
    o.w = std::max(1, (int)std::lround(src.w * factor));
    o.h = std::max(1, (int)std::lround(src.h * factor));
    o.px.resize((size_t)o.w * o.h * 3);
    if (!smooth) {
        std::vector<int> sx((size_t)o.w);
        for (int x = 0; x < o.w; ++x) sx[(size_t)x] = std::min(src.w - 1, (int)((long long)x * src.w / o.w));
        for (int y = 0; y < o.h; ++y) {
            int sy = std::min(src.h - 1, (int)((long long)y * src.h / o.h));
            const unsigned char *row = &src.px[(size_t)sy * src.w * 3];
            unsigned char *dst = &o.px[(size_t)y * o.w * 3];
            for (int x = 0; x < o.w; ++x) std::memcpy(dst + (size_t)x * 3, row + (size_t)sx[(size_t)x] * 3, 3);
        }
        return o;
    }
    for (int y = 0; y < o.h; ++y) {
        double fy = std::min(std::max(((y + 0.5) * src.h / o.h) - 0.5, 0.0), (double)(src.h - 1));
        int y0 = (int)fy, y1 = std::min(y0 + 1, src.h - 1);
        double wy = fy - y0;
        for (int x = 0; x < o.w; ++x) {
            double fx = std::min(std::max(((x + 0.5) * src.w / o.w) - 0.5, 0.0), (double)(src.w - 1));
            int x0 = (int)fx, x1 = std::min(x0 + 1, src.w - 1);
            double wx = fx - x0;
            for (int k = 0; k < 3; ++k) {
                double a = src.px[((size_t)y0 * src.w + x0) * 3 + k] * (1 - wx) + src.px[((size_t)y0 * src.w + x1) * 3 + k] * wx;
                double b = src.px[((size_t)y1 * src.w + x0) * 3 + k] * (1 - wx) + src.px[((size_t)y1 * src.w + x1) * 3 + k] * wx;
                o.px[((size_t)y * o.w + x) * 3 + k] = (unsigned char)std::lround(a * (1 - wy) + b * wy);
            }
        }
    }
    return o;
}

double clamp_scale(int w, int h, double factor, long long max_pixels) {
    if (w <= 0 || h <= 0) return 1;
    double px = (double)w * h;
    if (px * factor * factor <= (double)max_pixels) return factor;
    double f = std::floor(std::sqrt((double)max_pixels / px) * 1000.0) / 1000.0;
    return std::max(1.0, std::min(f, factor));
}

json apply_scale(json out, double factor, bool smooth) {
    if (!out.value("ok", false) || !out.contains("image")) return out;
    auto fail = [](const std::string &why) { return json{{"ok", false}, {"error", {{"code", "capture_failed"}, {"message", why}}}}; };
    if (out["image"].value("mimeType", "") != "image/png") return fail("scale needs the capture as a PNG");
    Rgb src;
    std::string err;
    if (!decode_png(util::base64_decode(out["image"].value("data", "")), src, err)) return fail("could not read the captured PNG to enlarge it: " + err);
    double used = clamp_scale(src.w, src.h, factor, kMaxScaledPixels);
    Rgb big = scale(src, used, smooth);
    std::string png = encode_png(big);
    if (!fits(png.size())) return too_large_error(png.size() / 3 * 4);
    json &r = out["result"];
    r["scaled_from"] = json{{"w", src.w}, {"h", src.h}};
    r["w"] = big.w;
    r["h"] = big.h;
    r["format"] = "png";
    r["bytes"] = png.size();
    r["scale"] = used;
    r["scale_filter"] = smooth ? "smooth" : "nearest";
    if (used != factor) {
        r["scale_requested"] = factor;
        r["note"] = "scale was lowered so that the picture stays under " + std::to_string(kMaxScaledPixels / 1000000) + " million pixels: crop a smaller area to enlarge more";
    }
    out["image"] = json{{"data", util::base64_encode(png)}, {"mimeType", "image/png"}};
    return out;
}

}  // namespace imagecap
