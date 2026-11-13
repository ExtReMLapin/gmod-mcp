// gmod-mcp-unittests: native unit tests that need no game. Run by the CI.
#include "check.h"

void test_crashguard();
void test_ljdump();

int main() {
    test_crashguard();
    test_ljdump();
    std::printf("unittests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
