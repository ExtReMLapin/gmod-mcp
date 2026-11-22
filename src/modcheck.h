// Issue #13: is the module installed in the game folder older than the one this bridge ships? Pure (no file access): the
// bridge reads the two files and hands the bytes over. The module carries a marker string with its version
// (`GMCP_MODULE_VERSION=<version>`, src/module.cpp), found here in the raw bytes of the file, so the check works while no game
// runs and does not load anything.
#pragma once

#include <string>

namespace modcheck {

// The text in front of the version inside the module file. Never change it: the bridge of an older release reads it too.
extern const char kMarkerPrefix[];

// The version the marker of a module file names ("" when the file has none: a module built before this check existed).
std::string find_version(const std::string &file_bytes);

// -1, 0, 1: dotted numbers compared field by field ("0.3.10" > "0.3.9"). A field that is not a number counts as 0.
int compare_versions(const std::string &a, const std::string &b);

enum class State {
    Unknown,   // the shipped module cannot be read, or carries no marker (a development checkout): nothing can be said
    Missing,   // no module file in the game folder (the game may still load one from an addon)
    Outdated,  // older than the shipped one, or older than the marker itself (installed carries none)
    Current,   // same version
    Newer,     // a newer version than the shipped one (a development build): not outdated
};
struct Verdict {
    State state = State::Unknown;
    std::string shipped, installed;  // versions; installed is "" for Missing and for a module without marker
    bool outdated() const { return state == State::Outdated; }
    const char *name() const;  // "unknown", "missing", "outdated", "current", "newer"
};
// shipped_read / installed_read: the file could be read; *_version: what find_version gave.
Verdict decide(bool shipped_read, const std::string &shipped_version, bool installed_read, const std::string &installed_version);

}  // namespace modcheck
