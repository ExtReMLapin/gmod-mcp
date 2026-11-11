"""Drives gmod-mcp-bridge over stdio. Cases: A (nothing on the port), B (live game on 27080, read-only calls), C (the
game appears mid-session through a local HTTP relay started later; needs the game too). Prints PASS/FAIL per check and
exits non-zero on a failure.

Usage: python test_bridge.py <path to gmod-mcp-bridge> [a] [b] [c]   (default: all cases)"""
import http.server, json, os, queue, socketserver, subprocess, sys, threading, time, urllib.request

BRIDGE = r"D:\dev\gmod-mcp\build\gmod-mcp-bridge.exe"
results = []

def check(name, cond, info=""):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  -> " + str(info)[:400]), flush=True)

class Bridge:
    def __init__(self, port):
        self.p = subprocess.Popen([BRIDGE, "--port", str(port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, bufsize=0)
        self.q = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.n = 0
    def _read(self):
        for line in self.p.stdout:
            self.q.put(json.loads(line))
    def send(self, m):
        self.p.stdin.write((json.dumps(m) + "\n").encode()); self.p.stdin.flush()
    def req(self, method, params=None, timeout=15):
        self.n += 1; rid = self.n
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}})
        others, end = [], time.time() + timeout
        while time.time() < end:
            try: m = self.q.get(timeout=0.2)
            except queue.Empty: continue
            if m.get("id") == rid: return m, others
            others.append(m)
        return None, others
    def wait_notif(self, method, timeout):
        end = time.time() + timeout
        seen = []
        while time.time() < end:
            try: m = self.q.get(timeout=0.2)
            except queue.Empty: continue
            seen.append(m.get("method"))
            if m.get("method") == method: return True, seen
        return False, seen
    def close(self):
        self.p.stdin.close(); self.p.wait(5)

INIT = {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "bridge-test", "version": "0"}}

def case_a():
    print("== A: nothing listening", flush=True)
    b = Bridge(27999)
    r, _ = b.req("initialize", INIT)
    res = (r or {}).get("result", {})
    check("A initialize answered", res.get("protocolVersion") == "2025-06-18", r)
    check("A tools.listChanged true", res.get("capabilities", {}).get("tools", {}).get("listChanged") is True, res)
    b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    r, _ = b.req("tools/list")
    tools = (r or {}).get("result", {}).get("tools", [])
    check("A tools/list non-empty (cache or fallback)", len(tools) >= 1, r)
    r, _ = b.req("tools/call", {"name": "status", "arguments": {}})
    res = (r or {}).get("result", {})
    check("A tools/call says not running", res.get("isError") and "game_not_running" in json.dumps(res), r)
    r, _ = b.req("ping")
    check("A ping ok", r and "result" in r, r)
    b.close()

def case_b():
    print("== B: live game on 27080 (read-only)", flush=True)
    b = Bridge(27080)
    r, _ = b.req("initialize", INIT)
    res = (r or {}).get("result", {})
    check("B initialize from game", "gmod" in json.dumps(res.get("serverInfo", {})).lower() and len(res.get("instructions", "")) > 200, res.get("serverInfo"))
    b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    r, _ = b.req("tools/list")
    tools = [t["name"] for t in (r or {}).get("result", {}).get("tools", [])]
    check("B tools/list live (>40 tools)", len(tools) > 40, tools[:5])
    r, _ = b.req("tools/call", {"name": "instances_list", "arguments": {}})
    check("B instances_list", r and not r["result"].get("isError") and "instances" in r["result"]["structuredContent"], r)
    me = next(i["id"] for i in r["result"]["structuredContent"]["instances"] if i.get("self"))
    r, others = b.req("tools/call", {"name": "wait_frames", "arguments": {"n": 240, "instance": me}, "_meta": {"progressToken": "p1"}}, timeout=30)
    check("B wait_frames result", r and not r["result"].get("isError"), r)
    check("B progress notifications relayed before the result", any(o.get("method") == "notifications/progress" for o in others), others)
    # Modern era: version in _meta, headers derived by the bridge.
    meta = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
            "io.modelcontextprotocol/clientCapabilities": {}, "io.modelcontextprotocol/clientInfo": {"name": "t", "version": "0"}}
    r, _ = b.req("server/discover", {"_meta": meta})
    check("B server/discover", r and "2026-07-28" in r["result"].get("supportedVersions", []), r)
    r, _ = b.req("tools/call", {"name": "status", "arguments": {"instance": me}, "_meta": meta})
    res = (r or {}).get("result", {})
    check("B modern tools/call status", res.get("resultType") == "complete" and not res.get("isError"), r)
    r, _ = b.req("tools/call", {"name": "console_exec", "arguments": {"command": "echo hi"}, "_meta": meta})
    txt = json.dumps((r or {}).get("result", {}))
    check("B bad argument -> invalid_params (old DLL: no hint expected)", "invalid_params" in txt, txt)
    b.close()

class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        h = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "content-length", "connection")}
        req = urllib.request.Request("http://127.0.0.1:27080/mcp", data=body, headers=h, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read(); code = r.status; ct = r.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as e:
            data = e.read(); code = e.code; ct = e.headers.get("Content-Type", "application/json")
        self.send_response(code); self.send_header("Content-Type", ct); self.send_header("Content-Length", str(len(data)))
        self.end_headers(); self.wfile.write(data)
    def log_message(self, *a): pass

def case_c():
    print("== C: game appears mid-session", flush=True)
    b = Bridge(27998)
    r, _ = b.req("initialize", INIT)
    b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    r, _ = b.req("tools/list")
    check("C tools/list while down", r and "result" in r, r)
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 27998), Relay)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    ok, seen = b.wait_notif("notifications/tools/list_changed", 10)
    check("C list_changed sent when the game appears", ok, seen)
    r, _ = b.req("tools/list")
    check("C tools/list now live", r and len(r["result"]["tools"]) > 40, r)
    srv.shutdown(); srv.server_close()
    r, _ = b.req("tools/call", {"name": "status", "arguments": {}})
    check("C call after the game left says not running", r and "game_not_running" in json.dumps(r), r)
    b.close()

for c in sys.argv[2:] or ["a", "b", "c"]:
    {"a": case_a, "b": case_b, "c": case_c}[c]()
passed = sum(1 for _, ok in results if ok)
print("TOTAL %d/%d" % (passed, len(results)))
sys.exit(0 if passed == len(results) else 1)
