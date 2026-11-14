"""Tests of tests/conformance/check_fixtures_absent.py on fake binaries."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_fixtures_absent as chk  # noqa: E402


class BinaryScanTests(unittest.TestCase):
    def test_in_binary(self):
        self.assertTrue(chk.in_binary("greet", b"a\x00greet\x00b"))
        self.assertTrue(chk.in_binary("greet", b'{"name":"greet"}'))
        self.assertFalse(chk.in_binary("greet", b"a\x00greeting\x00b"))   # only whole strings count
        self.assertFalse(chk.in_binary("greet", b"say greet to all"))

    def host_bytes(self):
        """A fake conformance host holding every fixture name the check knows (what the real host looks like)."""
        fixed, auto = chk.fixture_names()
        return b"".join(b"\x00" + n.encode() + b"\x00" for n in sorted(fixed | auto))

    def write_build(self, d, host=True):
        with open(os.path.join(d, "gmsv_mcpagent_linux64.so"), "wb") as f:
            f.write(b"\x00lua_exec\x00")
        with open(os.path.join(d, "gmod-mcp-bridge"), "wb") as f:
            f.write(b"\x00ents_find\x00")
        if host:
            with open(os.path.join(d, "gmod-mcp-conformance-host"), "wb") as f:
                f.write(self.host_bytes())

    def test_main(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(chk.main(["--build-dir", d]), 1)             # nothing to scan is a failure
            self.write_build(d)
            self.assertEqual(chk.main(["--build-dir", d]), 0)
            with open(os.path.join(d, "gmod-mcp-bridge"), "ab") as f:
                f.write(b"slow_compute\x00")
            self.assertEqual(chk.main(["--build-dir", d]), 1)

    def test_positive_control_needs_the_host(self):
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d, host=False)
            self.assertEqual(chk.main(["--build-dir", d]), 1)             # clean module and bridge, nothing to compare

    def test_positive_control_fails_when_the_scan_is_blind(self):
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d)
            with open(os.path.join(d, "gmod-mcp-conformance-host"), "wb") as f:
                f.write(b"\x00test_simple_text\x00greet\x00")           # a few names only: the strings are hidden
            self.assertEqual(chk.main(["--build-dir", d]), 1)

    def test_positive_control_threshold(self):
        fixed, auto = chk.fixture_names()
        names = sorted(fixed | auto)
        need = int(chk.MIN_HOST_FRACTION * len(names) + 0.999999)
        for count, expected in ((need - 1, 1), (need, 0)):
            with tempfile.TemporaryDirectory() as d:
                self.write_build(d)
                with open(os.path.join(d, "gmod-mcp-conformance-host"), "wb") as f:
                    f.write(b"".join(b"\x00" + n.encode() + b"\x00" for n in names[:count]))
                self.assertEqual(chk.main(["--build-dir", d]), expected, count)

    def make_root(self, d, src_files):
        for rel_path, text in src_files.items():
            full = os.path.join(d, rel_path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w") as f:
                f.write(text)
        os.makedirs(os.path.join(d, "tests", "conformance"), exist_ok=True)
        return d

    def test_hook_setter_only_in_its_declaration_and_definition(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_root(d, {
                "src/mcp.h": "// set_request_hooks is for the conformance host\nvoid set_request_hooks(RequestHooks hooks);\n",
                "src/mcp.cpp": "void set_request_hooks(RequestHooks hooks) { g_hooks = std::move(hooks); }\n",
            })
            self.assertEqual(chk.hook_setter_uses(d), [])
            self.make_root(d, {"src/http_server.cpp": "void f() { mcp::set_request_hooks({}); }\n"})
            self.assertEqual(len(chk.hook_setter_uses(d)), 1)
            self.assertIn("src/http_server.cpp:1", chk.hook_setter_uses(d)[0])

    def test_hook_setter_call_in_mcp_cpp_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_root(d, {"src/mcp.cpp": "void init() { set_request_hooks({}); }\n"})
            self.assertEqual(len(chk.hook_setter_uses(d)), 1)

    def test_hook_setter_check_runs_in_main(self):
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as b:
            self.make_root(root, {"src/module.cpp": "void f() { mcp::set_request_hooks({}); }\n"})
            self.write_build(b)
            self.assertEqual(chk.main(["--build-dir", b], root=root), 1)
            self.make_root(root, {"src/module.cpp": "void f() {}\n"})
            self.assertEqual(chk.main(["--build-dir", b], root=root), 0)

    def test_host_literals_are_collected(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "host.cpp"), "w") as f:
                f.write('add("my_new_fixture"); add("lua_exec");')
            with open(os.path.join(d, "fixture-names.txt"), "w") as f:
                f.write("# comment\ngreet2  # inline\n")
            fixed, auto = chk.fixture_names(d)
            self.assertIn("greet2", fixed)
            self.assertIn("my_new_fixture", auto)


if __name__ == "__main__":
    unittest.main()
