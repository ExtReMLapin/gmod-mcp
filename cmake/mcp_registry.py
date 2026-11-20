"""Builds what the official MCP Registry (registry.modelcontextprotocol.io) needs: the MCPB bundle and the server.json
that points at it. Standard library only.

  python cmake/mcp_registry.py pack --win-bridge <gmod-mcp-bridge.exe> --linux-bridge <gmod-mcp-bridge> --out gmod-mcp.mcpb
      One bundle for both platforms: manifest.json (version taken from CMakeLists.txt), the two bridges under server/
      (the manifest's platform_overrides picks the .exe on Windows), LICENSE and THIRD_PARTY_NOTICES.md. The zip is
      reproducible (fixed timestamps, sorted entries) and keeps the Unix mode of the Linux binary.
  python cmake/mcp_registry.py server-json --tag v0.3.7 --mcpb gmod-mcp.mcpb --out server.json
      The root server.json with this release's version, the URL of the release asset and the SHA-256 of the bundle.

Exits non-zero (with a message) on any inconsistency."""
import argparse
import hashlib
import json
import os
import re
import sys
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ASSET = "gmod-mcp.mcpb"  # name of the release asset: its URL must contain "mcp" (registry rule), the repository name does too
REPO = "ExtReMLapin/gmod-mcp"
SERVER_NAME = "io.github.ExtReMLapin/gmod-mcp"  # case matters: the registry matches the GitHub owner claim byte for byte
ZIP_TIME = (1980, 1, 1, 0, 0, 0)  # reproducible: the bundle only changes when its content does


def die(msg):
    sys.exit("mcp_registry: " + msg)


def cmake_version():
    text = read(os.path.join(ROOT, "CMakeLists.txt"), "r")
    m = re.search(r"^project\(gmod_mcp VERSION (\d+\.\d+\.\d+)", text, re.M)
    if not m:
        die("no project(gmod_mcp VERSION x.y.z) in CMakeLists.txt")
    return m.group(1)


def read(path, mode="rb"):
    with open(path, mode, **({"encoding": "utf-8"} if "b" not in mode else {})) as f:
        return f.read()


def check_binary(path, magic, what):
    head = read(path)[:4]
    if not head.startswith(magic):
        die("%s is not %s (first bytes %r)" % (path, what, head))


def pack(win_bridge, linux_bridge, out, version=None):
    version = version or cmake_version()
    manifest = json.loads(read(os.path.join(ROOT, "mcpb", "manifest.json"), "r"))
    manifest["version"] = version
    entry = manifest["server"]["entry_point"]  # server/gmod-mcp-bridge
    exe = manifest["server"]["mcp_config"]["platform_overrides"]["win32"]["command"].replace("${__dirname}/", "")
    if entry != "server/gmod-mcp-bridge" or exe != entry + ".exe":
        die("mcpb/manifest.json no longer names server/gmod-mcp-bridge[.exe]")
    check_binary(win_bridge, b"MZ", "a Windows executable")
    check_binary(linux_bridge, b"\x7fELF", "a Linux executable")
    files = [  # (archive name, content, Unix mode)
        ("manifest.json", (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8"), 0o644),
        ("LICENSE", read(os.path.join(ROOT, "LICENSE")), 0o644),
        ("THIRD_PARTY_NOTICES.md", read(os.path.join(ROOT, "THIRD_PARTY_NOTICES.md")), 0o644),
        (entry, read(linux_bridge), 0o755),
        (exe, read(win_bridge), 0o755),
    ]
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data, mode in sorted(files):
            info = zipfile.ZipInfo(name, ZIP_TIME)
            info.create_system = 3  # Unix: the mode below is read by the clients (and by `mcpb unpack`)
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data, compresslevel=9)
    return version


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def asset_url(tag, name=ASSET):
    return "https://github.com/%s/releases/download/%s/%s" % (REPO, tag, name)


def server_json(tag, mcpb):
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
        die("tag %r is not vX.Y.Z" % tag)
    version = tag[1:]
    if version != cmake_version():
        die("tag %s does not match CMakeLists.txt version %s" % (tag, cmake_version()))
    j = json.loads(read(os.path.join(ROOT, "server.json"), "r"))
    j["version"] = version
    (pkg,) = [p for p in j["packages"] if p["registryType"] == "mcpb"]
    pkg["identifier"] = asset_url(tag, os.path.basename(mcpb))
    pkg["version"] = version
    pkg["fileSha256"] = sha256(mcpb)
    return j


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("--win-bridge", required=True)
    p.add_argument("--linux-bridge", required=True)
    p.add_argument("--out", required=True)
    s = sub.add_parser("server-json")
    s.add_argument("--tag", required=True)
    s.add_argument("--mcpb", required=True)
    s.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.cmd == "pack":
        v = pack(a.win_bridge, a.linux_bridge, a.out)
        print("%s: version %s, sha256 %s" % (a.out, v, sha256(a.out)))
    else:
        j = server_json(a.tag, a.mcpb)
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(j, indent=2, ensure_ascii=False) + "\n")
        print("%s: %s %s -> %s" % (a.out, j["name"], j["version"], j["packages"][0]["identifier"]))


if __name__ == "__main__":
    main()
