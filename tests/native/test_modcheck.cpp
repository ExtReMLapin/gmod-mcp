// Issue #13: the module installed in the game against the one the bridge ships (src/modcheck.cpp), from the bytes of the two files.
#include <string>

#include "check.h"
#include "modcheck.h"

using namespace modcheck;

void test_modcheck() {
    std::string pre = std::string("MZ\0\0junk", 8);
    std::string marked = pre + std::string(kMarkerPrefix) + std::string("0.3.7\0more", 10) + "tail";
    CHECK(find_version(marked) == "0.3.7");
    CHECK(find_version(pre + "no marker here").empty());
    // The prefix with nothing after it (a copy in the bridge itself, a damaged file) is skipped; a later real marker still counts.
    CHECK(find_version(pre + std::string(kMarkerPrefix) + std::string("\0x", 2) + std::string(kMarkerPrefix) + "1.2.3").empty() == false);
    CHECK(find_version(std::string(kMarkerPrefix) + "abc").empty());
    CHECK(find_version(std::string(kMarkerPrefix)).empty());
    CHECK(find_version("").empty());

    CHECK(compare_versions("0.3.7", "0.3.7") == 0);
    CHECK(compare_versions("0.3.6", "0.3.7") < 0);
    CHECK(compare_versions("0.3.10", "0.3.9") > 0);  // numbers, not text
    CHECK(compare_versions("0.4", "0.3.9") > 0);
    CHECK(compare_versions("1.0", "1.0.0") == 0);

    // Issue #13: the verdicts.
    CHECK(decide(true, "0.3.8", true, "0.3.6").outdated());
    CHECK(decide(true, "0.3.8", true, "0.3.6").state == State::Outdated);
    CHECK(decide(true, "0.3.8", true, "").state == State::Outdated);       // no marker: built before this check existed
    CHECK(decide(true, "0.3.8", true, "0.3.8").state == State::Current);
    CHECK(decide(true, "0.3.8", true, "0.3.9").state == State::Newer);     // a development build is not outdated
    CHECK(!decide(true, "0.3.8", true, "0.3.9").outdated());
    CHECK(decide(true, "0.3.8", false, "").state == State::Missing);
    CHECK(!decide(true, "0.3.8", false, "").outdated());
    CHECK(decide(false, "", true, "0.3.6").state == State::Unknown);       // shipped module unreadable: nothing to compare with
    CHECK(decide(true, "", true, "0.3.6").state == State::Unknown);        // shipped module without marker (a checkout)
    CHECK(std::string(decide(true, "0.3.8", true, "0.3.6").name()) == "outdated");
    CHECK(std::string(decide(true, "0.3.8", true, "0.3.8").name()) == "current");
    CHECK(decide(true, "0.3.8", true, "0.3.6").shipped == "0.3.8" && decide(true, "0.3.8", true, "0.3.6").installed == "0.3.6");
}
