"""Tests of tests/conformance/check_fixtures_absent.py on fake binaries."""
import json
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

    def host_bytes(self, names=True):
        """A fake conformance host: the canary and (optionally) every fixture name the check knows."""
        fixed, auto = chk.fixture_names()
        out = b"GMOD\x00" + chk.CANARY.encode() + b"\x00"
        if names:
            out += b"".join(b"\x00" + n.encode() + b"\x00" for n in sorted(fixed | auto))
        return out

    def write_reply(self, d, targets, source_dir=None, configs=("Release",)):
        """A fake CMake File API reply (codemodel-v2) in build dir d. targets: {name: [source paths]} or
        {name: {"sources": [...], "includes": [...]}}, paths relative to the source dir."""
        source_dir = source_dir or chk.ROOT
        reply = os.path.join(d, ".cmake", "api", "v1", "reply")
        os.makedirs(reply, exist_ok=True)
        configurations = []
        for config in configs:
            entries = []
            for name, spec in targets.items():
                if isinstance(spec, list):
                    spec = {"sources": spec, "includes": []}
                tfile = "target-%s-%s.json" % (name, config)
                tj = {"name": name, "type": "EXECUTABLE", "sources": [{"path": p} for p in spec["sources"]],
                      "compileGroups": [{"includes": [{"path": p} for p in spec["includes"]]}]}
                with open(os.path.join(reply, tfile), "w") as f:
                    json.dump(tj, f)
                entries.append({"name": name, "jsonFile": tfile})
            configurations.append({"name": config, "targets": entries})
        with open(os.path.join(reply, "codemodel-v2-x.json"), "w") as f:
            json.dump({"paths": {"source": source_dir, "build": d}, "configurations": configurations}, f)
        with open(os.path.join(reply, "index-2026-01-01T00-00-00-0000.json"), "w") as f:
            json.dump({"objects": [{"kind": "codemodel", "version": {"major": 2, "minor": 0},
                                    "jsonFile": "codemodel-v2-x.json"}]}, f)

    GOOD_GRAPH = {
        "gmod-mcp-conformance-host": ["tests/conformance/conformance_host.cpp", "tests/conformance/fixtures.cpp",
                                      "src/mcp.cpp"],
        "mcpagent": ["src/mcp.cpp", "src/module.cpp"],
        "gmod-mcp-bridge": ["src/bridge/bridge.cpp"],
    }

    def write_build(self, d, host=True, graph=GOOD_GRAPH):
        with open(os.path.join(d, "gmsv_mcpagent_linux64.so"), "wb") as f:
            f.write(b"\x00lua_exec\x00")
        with open(os.path.join(d, "gmod-mcp-bridge"), "wb") as f:
            f.write(b"\x00ents_find\x00")
        if host:
            with open(os.path.join(d, "gmod-mcp-conformance-host"), "wb") as f:
                f.write(self.host_bytes())
        if graph is not None:
            self.write_reply(d, graph)

    def test_main(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(chk.main(["--build-dir", d]), 1)             # nothing to scan is a failure
            self.write_build(d)
            self.assertEqual(chk.main(["--build-dir", d]), 0)
            with open(os.path.join(d, "gmod-mcp-bridge"), "ab") as f:
                f.write(b"slow_compute\x00")
            self.assertEqual(chk.main(["--build-dir", d]), 1)

    def test_canary_needs_the_host(self):
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d, host=False)
            self.assertEqual(chk.main(["--build-dir", d]), 1)             # clean module and bridge, nothing to compare

    def test_canary_missing_from_the_host_fails(self):
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d)
            with open(os.path.join(d, "gmod-mcp-conformance-host"), "wb") as f:
                f.write(b"\x00test_simple_text\x00greet\x00")           # the scan is blind to the host's own strings
            self.assertEqual(chk.main(["--build-dir", d]), 1)

    def test_hidden_short_names_do_not_fail_when_the_canary_is_there(self):
        # GCC may build short names from immediate stores: the host then shows few or none of them. Not a failure.
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d)
            with open(os.path.join(d, "gmod-mcp-conformance-host"), "wb") as f:
                f.write(self.host_bytes(names=False))
            self.assertEqual(chk.main(["--build-dir", d]), 0)

    def test_canary_in_the_module_or_bridge_fails(self):
        for name in ("gmsv_mcpagent_linux64.so", "gmod-mcp-bridge"):
            with tempfile.TemporaryDirectory() as d:
                self.write_build(d)
                with open(os.path.join(d, name), "ab") as f:
                    f.write(b"x" + chk.CANARY.encode() + b"y")            # a substring match is enough
                self.assertEqual(chk.main(["--build-dir", d]), 1, name)

    def test_build_graph_fixture_source_in_another_target_fails(self):
        graph = {k: list(v) for k, v in self.GOOD_GRAPH.items()}
        graph["mcpagent"].append("tests/conformance/fixtures.cpp")
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d, graph=graph)
            self.assertEqual(chk.main(["--build-dir", d]), 1)             # binaries are clean: only the graph catches it

    def test_build_graph_conformance_include_dir_in_another_target_fails(self):
        graph = dict(self.GOOD_GRAPH)
        graph["gmod-mcp-bridge"] = {"sources": ["src/bridge/bridge.cpp"],
                                    "includes": [os.path.join(chk.ROOT, "tests", "conformance")]}
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d, graph=graph)
            self.assertEqual(chk.main(["--build-dir", d]), 1)

    def test_build_graph_needs_the_host_target(self):
        graph = {k: v for k, v in self.GOOD_GRAPH.items() if k != "gmod-mcp-conformance-host"}
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d, graph=graph)
            self.assertEqual(chk.main(["--build-dir", d]), 1)             # nothing proves where the fixtures are

    def test_build_graph_needs_a_reply(self):
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d, graph=None)
            self.assertEqual(chk.main(["--build-dir", d]), 1)             # no reply, no CMakeCache.txt: cannot check

    def test_build_graph_multi_config_and_path_spellings(self):
        # Visual Studio lists a target once per configuration; paths may use another case or separators.
        with tempfile.TemporaryDirectory() as d:
            self.write_build(d, graph=None)
            self.write_reply(d, self.GOOD_GRAPH, configs=("Debug", "Release"))
            self.assertEqual(chk.main(["--build-dir", d]), 0)
            graph = dict(self.GOOD_GRAPH)
            graph["mcpagent"] = ["src\\mcp.cpp", "Tests\\Conformance\\fixtures.cpp" if os.name == "nt"
                                 else "tests/../tests/conformance/fixtures.cpp"]
            self.write_reply(d, graph, configs=("Debug", "Release"))
            self.assertEqual(chk.main(["--build-dir", d]), 1)

    def make_root(self, d, src_files):
        for rel_path, text in src_files.items():
            full = os.path.join(d, rel_path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w") as f:
                f.write(text)
        os.makedirs(os.path.join(d, "tests", "conformance"), exist_ok=True)
        with open(os.path.join(d, "tests", "conformance", chk.CANARY_SOURCE), "w") as f:
            f.write('extern "C" const char c[] = "%s";' % chk.CANARY + chr(10))
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

    def test_shipped_sources_must_not_hold_a_fixture_name_or_the_canary(self):
        for text in ('const char *n = "slow_compute";', 'const char *n = "%s";' % chk.CANARY):
            with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as b:
                self.make_root(root, {"src/module.cpp": text})
                self.write_build(b)
                self.assertEqual(chk.main(["--build-dir", b], root=root), 1, text)

    def test_host_source_must_define_the_canary(self):
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as b:
            self.make_root(root, {"src/module.cpp": "void f() {}"})
            self.write_build(b)
            self.assertEqual(chk.main(["--build-dir", b], root=root), 0)
            os.remove(os.path.join(root, "tests", "conformance", chk.CANARY_SOURCE))
            self.assertEqual(chk.main(["--build-dir", b], root=root), 1)

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
