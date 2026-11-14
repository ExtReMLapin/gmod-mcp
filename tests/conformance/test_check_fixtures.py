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

    def test_main(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(chk.main(["--build-dir", d]), 1)             # nothing to scan is a failure
            with open(os.path.join(d, "gmsv_mcpagent_linux64.so"), "wb") as f:
                f.write(b"\x00lua_exec\x00")
            with open(os.path.join(d, "gmod-mcp-bridge"), "wb") as f:
                f.write(b"\x00ents_find\x00")
            self.assertEqual(chk.main(["--build-dir", d]), 0)
            with open(os.path.join(d, "gmod-mcp-bridge"), "ab") as f:
                f.write(b"slow_compute\x00")
            self.assertEqual(chk.main(["--build-dir", d]), 1)

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
