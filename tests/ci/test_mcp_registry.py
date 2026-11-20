"""Tests of cmake/mcp_registry.py and of the files it feeds to the official MCP Registry (mcpb/manifest.json, server.json):
the rules the registry checks at publish time (https://modelcontextprotocol.io/registry/package-types, the registry's own
validator internal/validators/registries/mcpb.go), so that a release fails here and not after the tag."""
import hashlib
import json
import os
import re
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "cmake"))
import mcp_registry  # noqa: E402


def load(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return json.load(f)


def fake_bridges(d):
    win, lin = os.path.join(d, "b.exe"), os.path.join(d, "b")
    with open(win, "wb") as f:
        f.write(b"MZ" + b"win" * 100)
    with open(lin, "wb") as f:
        f.write(b"\x7fELF" + b"lin" * 100)
    return win, lin


class ServerJsonFile(unittest.TestCase):
    def setUp(self):
        self.j = load("server.json")
        self.pkg = self.j["packages"][0]

    def test_matches_the_registry_schema_when_jsonschema_is_installed(self):
        try:
            import jsonschema
        except ImportError:
            self.skipTest("jsonschema not installed")
        schema = load("tests", "ci", "registry-server.schema.json")
        self.assertEqual(self.j["$schema"], schema["$id"])
        jsonschema.validate(self.j, schema, cls=jsonschema.Draft7Validator)

    def test_name_is_the_github_namespace_of_the_owner(self):
        # The registry grants io.github.<owner>/* to the repository owner (OIDC) and matches the prefix case-sensitively.
        self.assertEqual(self.j["name"], mcp_registry.SERVER_NAME)
        self.assertEqual(self.j["name"], "io.github.%s/gmod-mcp" % mcp_registry.REPO.split("/")[0])

    def test_description_and_version(self):
        self.assertLessEqual(len(self.j["description"]), 100)  # the registry refuses longer ones
        self.assertEqual(self.j["version"], mcp_registry.cmake_version())
        self.assertEqual(self.pkg["version"], self.j["version"])

    def test_one_mcpb_package_with_the_registry_rules(self):
        self.assertEqual(len(self.j["packages"]), 1)
        self.assertEqual(self.pkg["registryType"], "mcpb")
        self.assertEqual(self.pkg["transport"], {"type": "stdio"})
        self.assertNotIn("registryBaseUrl", self.pkg)  # refused for mcpb
        url = self.pkg["identifier"]
        self.assertTrue(url.startswith("https://github.com/"))
        self.assertIn("mcp", url.lower())
        self.assertRegex(url[len("https://github.com"):], r"^/[A-Za-z0-9-]+/[A-Za-z0-9._-]+/releases/download/[^/]+/[^/]+$")
        self.assertEqual(url, mcp_registry.asset_url("v" + self.j["version"]))
        self.assertRegex(self.pkg["fileSha256"], r"^[a-f0-9]{64}$")

    def test_repository_is_this_one(self):
        self.assertEqual(self.j["repository"]["url"], "https://github.com/" + mcp_registry.REPO)
        self.assertEqual(self.j["repository"]["source"], "github")


class ManifestFile(unittest.TestCase):
    def setUp(self):
        self.m = load("mcpb", "manifest.json")

    def test_version_follows_cmake(self):
        self.assertEqual(self.m["version"], mcp_registry.cmake_version())

    def test_binary_server_for_both_platforms(self):
        s = self.m["server"]
        self.assertEqual(s["type"], "binary")
        self.assertEqual(s["entry_point"], "server/gmod-mcp-bridge")
        self.assertEqual(s["mcp_config"]["command"], "${__dirname}/server/gmod-mcp-bridge")
        self.assertEqual(s["mcp_config"]["platform_overrides"]["win32"]["command"], "${__dirname}/server/gmod-mcp-bridge.exe")
        self.assertEqual(sorted(self.m["compatibility"]["platforms"]), ["linux", "win32"])

    def test_every_user_config_is_substituted_with_a_default(self):
        # An unset ${user_config.x} must never reach the bridge as an empty string.
        env = self.m["server"]["mcp_config"]["env"]
        for var, value in env.items():
            for name in re.findall(r"\$\{user_config\.(\w+)\}", value):
                self.assertIn("default", self.m["user_config"][name], var)


class Pack(unittest.TestCase):
    def pack(self, d, name="out.mcpb", version=None):
        win, lin = fake_bridges(d)
        out = os.path.join(d, name)
        mcp_registry.pack(win, lin, out, version)
        return out

    def test_content_modes_and_version(self):
        with tempfile.TemporaryDirectory() as d:
            out = self.pack(d, version="9.8.7")
            with zipfile.ZipFile(out) as z:
                self.assertIsNone(z.testzip())
                names = sorted(z.namelist())
                self.assertEqual(names, ["LICENSE", "THIRD_PARTY_NOTICES.md", "manifest.json", "server/gmod-mcp-bridge",
                                         "server/gmod-mcp-bridge.exe"])
                self.assertEqual(json.loads(z.read("manifest.json"))["version"], "9.8.7")
                for n in ("server/gmod-mcp-bridge", "server/gmod-mcp-bridge.exe"):
                    self.assertEqual((z.getinfo(n).external_attr >> 16) & 0o777, 0o755)  # runnable once unpacked on Linux
                self.assertEqual((z.getinfo("manifest.json").external_attr >> 16) & 0o777, 0o644)
                self.assertTrue(z.read("server/gmod-mcp-bridge").startswith(b"\x7fELF"))
                self.assertTrue(z.read("server/gmod-mcp-bridge.exe").startswith(b"MZ"))

    def test_reproducible(self):
        with tempfile.TemporaryDirectory() as d:
            a = self.pack(d, "a.mcpb")
            b = self.pack(d, "b.mcpb")
            self.assertEqual(mcp_registry.sha256(a), mcp_registry.sha256(b))

    def test_swapped_binaries_are_refused(self):
        with tempfile.TemporaryDirectory() as d:
            win, lin = fake_bridges(d)
            with self.assertRaises(SystemExit):
                mcp_registry.pack(lin, win, os.path.join(d, "x.mcpb"))


class ServerJsonGeneration(unittest.TestCase):
    def test_release_values(self):
        v = mcp_registry.cmake_version()
        with tempfile.TemporaryDirectory() as d:
            bundle = os.path.join(d, "gmod-mcp.mcpb")
            with open(bundle, "wb") as f:
                f.write(b"bundle bytes")
            j = mcp_registry.server_json("v" + v, bundle)
        pkg = j["packages"][0]
        self.assertEqual(j["version"], v)
        self.assertEqual(pkg["fileSha256"], hashlib.sha256(b"bundle bytes").hexdigest())
        self.assertEqual(pkg["identifier"], "https://github.com/ExtReMLapin/gmod-mcp/releases/download/v%s/gmod-mcp.mcpb" % v)

    def test_tag_that_is_not_the_cmake_version_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            bundle = os.path.join(d, "gmod-mcp.mcpb")
            open(bundle, "wb").close()
            for tag in ("v99.0.0", "0.3.7", "v0.3", "main"):
                with self.assertRaises(SystemExit, msg=tag):
                    mcp_registry.server_json(tag, bundle)


if __name__ == "__main__":
    unittest.main()
