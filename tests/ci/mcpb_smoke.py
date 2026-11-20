"""Smoke test of the MCPB bundle, the way an MCP client uses it: unpack it into an empty folder (restoring the Unix modes
from the zip), read manifest.json, start the server exactly as server.mcp_config says for this platform (platform_overrides
and ${__dirname} / ${user_config.*} substituted), then speak MCP over stdio: initialize, tools/list, and one tool call that
must answer game_not_running (no game runs: the bridge works without it).

Usage: python mcpb_smoke.py <bundle.mcpb> [--expect-tools N]   Exits non-zero on any failure."""
import json
import os
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import zipfile


def unpack(bundle, dest):
    with zipfile.ZipFile(bundle) as z:
        for info in z.infolist():
            z.extract(info, dest)
            mode = (info.external_attr >> 16) & 0o777
            if mode and os.name != "nt":
                os.chmod(os.path.join(dest, info.filename), mode)


def launch_command(manifest, folder):
    """command, args, env of the manifest for this platform, with the variables substituted."""
    cfg = manifest["server"]["mcp_config"]
    cfg = {**cfg, **cfg.get("platform_overrides", {}).get("win32" if os.name == "nt" else "linux", {})}
    defaults = {k: v.get("default") for k, v in manifest.get("user_config", {}).items()}

    def sub(s):
        s = s.replace("${__dirname}", folder)
        for k, v in defaults.items():
            s = s.replace("${user_config.%s}" % k, str(v).lower() if isinstance(v, bool) else str(v))
        return s
    return sub(cfg["command"]), [sub(a) for a in cfg.get("args", [])], {k: sub(v) for k, v in cfg.get("env", {}).items()}


def main():
    args = sys.argv[1:]
    expect_tools = int(args[args.index("--expect-tools") + 1]) if "--expect-tools" in args else 1
    bundle = args[0]
    failures = []

    def check(name, ok, detail=""):
        print(("PASS " if ok else "FAIL ") + name + ("" if ok else ": %s" % (detail,)))
        if not ok:
            failures.append(name)

    folder = tempfile.mkdtemp(prefix="mcpb-smoke-")
    unpack(bundle, folder)
    manifest = json.load(open(os.path.join(folder, "manifest.json"), encoding="utf-8"))
    cmd, cargs, cenv = launch_command(manifest, folder)
    check("manifest server.type is binary", manifest["server"]["type"] == "binary")
    check("entry_point exists in the bundle", os.path.isfile(os.path.join(folder, manifest["server"]["entry_point"])))
    check("command exists in the bundle", os.path.isfile(cmd), cmd)
    if os.name != "nt":
        check("command is executable", os.access(cmd, os.X_OK), cmd)
    if failures:
        sys.exit(1)

    with socket.socket() as s:  # a free port, so that a game running on this machine is never probed
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    env = {**os.environ, **cenv, "GMOD_MCP_PORT": str(port)}
    p = subprocess.Popen([cmd] + cargs, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
                         cwd=tempfile.gettempdir())
    out = queue.Queue()
    threading.Thread(target=lambda: [out.put(l) for l in p.stdout], daemon=True).start()
    threading.Thread(target=lambda: [None for _ in p.stderr], daemon=True).start()

    def request(i, method, params):
        p.stdin.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": method, "params": params}) + "\n").encode())
        p.stdin.flush()
        while True:
            try:
                m = json.loads(out.get(timeout=20))
            except queue.Empty:
                return None
            if m.get("id") == i and "method" not in m:
                return m

    try:
        r = request(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                      "clientInfo": {"name": "mcpb-smoke", "version": "0"}})
        res = (r or {}).get("result", {})
        check("initialize answered", "protocolVersion" in res and "tools" in res.get("capabilities", {}), r)
        info = res.get("serverInfo", {})
        check("serverInfo version is the bundle's version", info.get("version") == manifest["version"],
              (info, manifest["version"]))
        p.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        p.stdin.flush()
        r = request(2, "tools/list", {})
        tools = ((r or {}).get("result") or {}).get("tools", [])
        check("tools/list lists tools (%d)" % len(tools), len(tools) >= expect_tools, r)
        print("     first tools: %s" % ", ".join(t["name"] for t in tools[:6]))
        r = request(3, "tools/call", {"name": "status", "arguments": {}})
        text = json.dumps(r)
        check("tools/call status answers game_not_running", "game_not_running" in text, text[:300])
    finally:
        try:
            p.stdin.close()
            p.wait(5)
        except Exception:
            p.kill()
    print("mcpb smoke: %s" % ("FAILED (%s)" % ", ".join(failures) if failures else "OK"))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
