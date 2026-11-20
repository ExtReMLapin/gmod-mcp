// A tiny shared object without SONAME, loaded by the platform test from a folder that is not on any library path (the
// layout of the Linux dev-branch server's lua_shared_srv.so). Linux only.
extern "C" int gmcp_testlib_answer() { return 42; }
