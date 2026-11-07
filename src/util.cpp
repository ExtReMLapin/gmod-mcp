#include "util.h"

#ifdef _WIN32
#include <winsock2.h>
#endif
#include <httplib.h>

#include <algorithm>
#include <cctype>

namespace util {

std::string base64_encode(const std::string &in) { return httplib::detail::base64_encode(in); }

std::string base64_decode(const std::string &in) {
    static int table[256];
    static bool init = false;
    if (!init) {
        for (int &v : table) v = -1;
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[(unsigned char)a[i]] = i;
        init = true;
    }
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (table[c] < 0) continue;  // '=' and whitespace are skipped
        val = ((val << 6) | table[c]) & 0xFFFFFF;
        bits += 6;
        if (bits >= 0) {
            out.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::vector<std::string> split_args(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false, any = false;
    for (char c : s) {
        if (c == '"') {
            inq = !inq;
            any = true;
        } else if (!inq && std::isspace((unsigned char)c)) {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

bool has_flag(const std::vector<std::string> &args, const std::string &flag) {
    for (const auto &a : args)
        if (to_lower(a) == flag) return true;
    return false;
}

std::string flag_value(const std::vector<std::string> &args, const std::string &flag, const std::string &def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (to_lower(args[i]) == flag) return args[i + 1];
    return def;
}

bool constant_time_eq(const std::string &a, const std::string &b) {
    unsigned char d = a.size() == b.size() ? 0 : 1;
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char x = i < a.size() ? (unsigned char)a[i] : 0;
        unsigned char y = i < b.size() ? (unsigned char)b[i] : 0;
        d |= x ^ y;
    }
    return d == 0;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string hex(const unsigned char *p, size_t n) {
    static const char *h = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out += h[p[i] >> 4];
        out += h[p[i] & 15];
    }
    return out;
}

}  // namespace util
