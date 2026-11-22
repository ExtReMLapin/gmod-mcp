#include "modcheck.h"

#include <cctype>
#include <cstdlib>

namespace modcheck {

const char kMarkerPrefix[] = "GMCP_MODULE_VERSION=";

std::string find_version(const std::string &bytes) {
    const std::string prefix = kMarkerPrefix;
    size_t pos = 0;
    while ((pos = bytes.find(prefix, pos)) != std::string::npos) {
        size_t a = pos + prefix.size(), b = a;
        while (b < bytes.size() && (std::isdigit((unsigned char)bytes[b]) || bytes[b] == '.')) ++b;
        if (b > a && std::isdigit((unsigned char)bytes[a])) return bytes.substr(a, b - a);
        pos = a;  // a copy of the prefix with no version after it (the bridge's own constant): keep looking
    }
    return "";
}

int compare_versions(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        auto field = [](const std::string &s, size_t &k) {
            unsigned long v = 0;
            size_t e = s.find('.', k);
            if (e == std::string::npos) e = s.size();
            if (k < s.size()) v = std::strtoul(s.substr(k, e - k).c_str(), nullptr, 10);
            k = e < s.size() ? e + 1 : s.size();
            return v;
        };
        unsigned long x = field(a, i), y = field(b, j);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

const char *Verdict::name() const {
    switch (state) {
        case State::Missing: return "missing";
        case State::Outdated: return "outdated";
        case State::Current: return "current";
        case State::Newer: return "newer";
        default: return "unknown";
    }
}

Verdict decide(bool shipped_read, const std::string &shipped_version, bool installed_read, const std::string &installed_version) {
    Verdict v;
    v.shipped = shipped_version;
    v.installed = installed_version;
    if (!shipped_read || shipped_version.empty()) return v;
    if (!installed_read) {
        v.state = State::Missing;
        return v;
    }
    if (installed_version.empty()) {
        v.state = State::Outdated;
        return v;
    }
    int c = compare_versions(installed_version, shipped_version);
    v.state = c < 0 ? State::Outdated : c == 0 ? State::Current : State::Newer;
    return v;
}

}  // namespace modcheck
