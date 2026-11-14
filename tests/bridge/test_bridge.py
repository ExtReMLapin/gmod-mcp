"""Tests of gmod-mcp-bridge, driven over stdio.

Cases (default: a f):
  a      nothing listening on the port
  f      a fake gmod-mcp server run by this script: progress order, cancellation of a silent stream, game dying
         mid-stream, rejected token, 8 MB result, 30 concurrent calls, non-ASCII header values, malformed input,
         stdin closed during a call, game appearing mid-session, version mismatch
  m      2026-07-28 client against the fake server: list_changed only on a subscriptions/listen stream (acknowledged
         first, carrying the subscription id), resource subscriptions relayed and reopened after a game restart,
         cancellation, required _meta fields, unsupported version, resultType on the bridge's own answers, tool
         annotations kept
  multi  6 bridges at once (one per client session) sharing the config folder: each is told when the game appears,
         the shared cache stays valid under concurrent writes, no temporary file left behind
  session the bridge's own tools: listed offline and live, default instance injected (and mirrored in the header),
         explicit instance wins, instances_close refuses what the session did not start, bad game_dir refused
  orphan the process that started the bridge dies without closing the pipe: the bridge exits (and stays with
         --no-parent-watch)
  fuzz   400 random messages (random JSON shapes, ids, params, invalid UTF-8): the bridge survives and still answers
  fslow  the fake server keeps a stream silent for 65 s (a client request has no idle limit)
  launch live: instances_launch of 2 light clients with a map, default instance, instances_close (Windows, Steam,
         no other game; the game saves their window size as its video setting)
  b      live game on 27080, read-only calls (needs the game and the user's token)
  c      the live game appears mid-session through a local HTTP relay (needs the game)
Cases a, f and fslow use a temporary config folder: the user's token and cache are not touched.

Usage: python test_bridge.py <path to gmod-mcp-bridge> [case ...]   (default: a f m multi session orphans orphan fuzz)
Prints PASS/FAIL per check and exits non-zero on a failure."""
import base64, http.server, json, os, queue, random, select, socket, socketserver, subprocess, sys, tempfile, threading, time
import urllib.error, urllib.request

BRIDGE = os.path.abspath(sys.argv[1])
results = []
TOKEN = "test-token-0123456789abcdef0123456789abcdef"
MODERN = "2026-07-28"


def check(name, cond, info=""):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  -> " + str(info)[:400]), flush=True)


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def isolated_config(with_token):
    d = tempfile.mkdtemp(prefix="gmcp-bridge-test-")
    os.makedirs(os.path.join(d, "gmod-mcp"))
    if with_token:
        with open(os.path.join(d, "gmod-mcp", "token"), "w") as f:
            f.write(TOKEN + "\n")
    env = dict(os.environ, APPDATA=d, XDG_CONFIG_HOME=d)
    env["_TEST_CONFIG_DIR"] = d
    return env


class Bridge:
    def __init__(self, port, env=None):
        self.p = subprocess.Popen([BRIDGE, "--port", str(port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env)  # buffered: unbuffered line reads go byte by byte
        self.lock = threading.Lock()
        self.by_id, self.notes, self.order = {}, [], []
        self.cv = threading.Condition(self.lock)
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=lambda: [None for _ in self.p.stderr], daemon=True).start()
        self.n = 0

    def _read(self):
        for line in self.p.stdout:
            m = json.loads(line)
            with self.cv:
                self.order.append(m)
                if "id" in m and "method" not in m: self.by_id[json.dumps(m["id"])] = m
                else: self.notes.append(m)
                self.cv.notify_all()

    def raw(self, text):
        try:
            self.p.stdin.write(text.encode() + b"\n"); self.p.stdin.flush()
        except OSError:
            pass  # the bridge died: the waits time out and the checks fail

    def send(self, m):
        self.raw(json.dumps(m))

    def start(self, method, params=None, rid=None, with_params=True):
        if rid is None:
            self.n += 1; rid = self.n
        m = {"jsonrpc": "2.0", "id": rid, "method": method}
        if with_params: m["params"] = params or {}
        self.send(m)
        return rid

    def wait(self, rid, timeout=15):
        key, end = json.dumps(rid), time.time() + timeout
        with self.cv:
            while key not in self.by_id:
                left = end - time.time()
                if left <= 0: return None
                self.cv.wait(left)
            return self.by_id[key]

    def req(self, method, params=None, timeout=15, **kw):
        return self.wait(self.start(method, params, **kw), timeout)

    def wait_notif(self, method, timeout):
        end = time.time() + timeout
        with self.cv:
            while not any(n.get("method") == method for n in self.notes):
                left = end - time.time()
                if left <= 0: return False
                self.cv.wait(left)
            return True

    def close(self):
        try: self.p.stdin.close()
        except OSError: pass
        try: self.p.wait(5)
        except subprocess.TimeoutExpired: self.p.kill()


INIT = {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "bridge-test", "version": "0"}}


def modern_meta():
    return {"io.modelcontextprotocol/protocolVersion": MODERN, "io.modelcontextprotocol/clientCapabilities": {},
            "io.modelcontextprotocol/clientInfo": {"name": "t", "version": "0"}}


def decode_header(v):
    if v and v.startswith("=?base64?") and v.endswith("?="):
        return base64.b64decode(v[9:-2]).decode("utf-8")
    return v


# --- Fake gmod-mcp server ---
class Fake(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a): pass

    def setup(self):
        super().setup()
        with self.server.lock: self.server.conns.append(self.connection)

    def reply_json(self, obj, status=200):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def sse_start(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def sse(self, obj):
        self.wfile.write(b"event: message\ndata: " + json.dumps(obj).encode() + b"\n\n")
        self.wfile.flush()

    def client_gone(self, timeout):
        r, _, _ = select.select([self.connection], [], [], timeout)
        if not r: return False
        try: return self.connection.recv(1, socket.MSG_PEEK) == b""
        except OSError: return True

    def do_POST(self):
        srv = self.server
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with srv.lock: srv.seen.append((body, dict(self.headers)))
        if self.headers.get("Authorization") != "Bearer " + srv.token:
            return self.reply_json({"error": "unauthorized"}, 401)
        if "id" not in body:
            self.send_response(202); self.send_header("Content-Length", "0"); self.end_headers(); return
        rid, method, params = body["id"], body.get("method"), body.get("params", {})
        res = lambda r: {"jsonrpc": "2.0", "id": rid, "result": r}
        if method == "initialize":
            return self.reply_json(res({"protocolVersion": params.get("protocolVersion", "2025-06-18"),
                                        "capabilities": {"tools": {"listChanged": False}},
                                        "serverInfo": {"name": "gmod-mcp", "version": srv.version},
                                        "instructions": "fake instructions"}))
        if method == "ping": return self.reply_json(res({}))
        if method == "server/discover":
            return self.reply_json(res({"resultType": "complete", "supportedVersions": [MODERN, "2025-11-25"],
                                        "capabilities": {"tools": {"listChanged": False}, "resources": {"subscribe": True}},
                                        "serverInfo": {"name": "gmod-mcp", "version": srv.version},
                                        "instructions": "fake instructions"}))
        if method == "tools/list":
            tools = [{"name": n, "inputSchema": {"type": "object", "properties": {"instance": {"type": "string"}}}} for n in
                     ("echo", "progress", "idle", "nofinal", "huge", "silent_hang")]
            tools[0]["annotations"] = {"title": "Echo", "readOnlyHint": True, "destructiveHint": False,
                                       "idempotentHint": True, "openWorldHint": False}
            return self.reply_json(res({"tools": tools}))
        if method == "subscriptions/listen":
            asked = params.get("notifications", {}).get("resourceSubscriptions", [])
            honored = [u for u in asked if u.startswith("gmod://")]
            meta = {"io.modelcontextprotocol/subscriptionId": rid}
            self.sse_start()
            self.sse({"jsonrpc": "2.0", "method": "notifications/subscriptions/acknowledged",
                      "params": {"notifications": {"resourceSubscriptions": honored}, "_meta": meta}})
            with srv.lock: srv.listens += 1
            while True:
                if self.client_gone(0.3):
                    with srv.lock: srv.disconnects.append(time.time())
                    return
                if honored:
                    try: self.sse({"jsonrpc": "2.0", "method": "notifications/resources/updated",
                                   "params": {"uri": honored[0], "_meta": meta}})
                    except OSError:
                        with srv.lock: srv.disconnects.append(time.time())
                        return
        if method != "tools/call":
            return self.reply_json({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "unknown"}}, 404)
        name, args = params.get("name"), params.get("arguments", {})
        text = lambda t: res({"content": [{"type": "text", "text": t}], "structuredContent": {"text": t}})
        token = params.get("_meta", {}).get("progressToken", "pt")
        prog = lambda i: {"jsonrpc": "2.0", "method": "notifications/progress",
                          "params": {"progressToken": token, "progress": i}}
        if name == "echo":
            h = {k.lower(): v for k, v in self.headers.items() if k.lower().startswith("mcp")}
            return self.reply_json(res({"content": [{"type": "text", "text": "echo"}],
                                        "structuredContent": {"args": args, "headers": h}}))
        if name == "progress":
            self.sse_start()
            for i in range(1, 4): self.sse(prog(i)); time.sleep(0.05)
            return self.sse(text("done"))
        if name == "idle":
            self.sse_start(); self.sse(prog(1)); time.sleep(args.get("s", 1)); return self.sse(text("woke up"))
        if name == "nofinal":
            self.sse_start(); self.sse(prog(1)); return  # stream ends without the response (game crashed)
        if name == "huge":
            return self.reply_json(text("x" * args.get("n", 8 << 20)))
        if name == "silent_hang":
            self.sse_start()
            t0 = time.time()
            while time.time() - t0 < 30:
                if self.client_gone(0.1):
                    with srv.lock: srv.disconnects.append(time.time())
                    return
            return self.sse(text("never cancelled"))
        return self.reply_json(text("unknown tool"))


class FakeServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 128  # like the module (cpp-httplib listen backlog)

    def __init__(self, port, version="0.2.0", token=TOKEN):
        super().__init__(("127.0.0.1", port), Fake)
        self.lock, self.seen, self.disconnects, self.listens, self.conns = threading.Lock(), [], [], 0, []
        self.version, self.token = version, token
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def handle_error(self, request, client_address):
        pass  # connections cut on purpose (cancellation, stop) are expected

    def stop(self):
        """Like the game closing: stop listening and cut every open connection."""
        self.shutdown(); self.server_close()
        with self.lock: conns = list(self.conns)
        for c in conns:
            try: c.shutdown(socket.SHUT_RDWR)
            except OSError: pass


def bridge_version():
    out = subprocess.run([BRIDGE, "--version"], capture_output=True, text=True).stdout.split()
    return out[-1] if out else "?"


def case_a():
    print("== A: nothing listening", flush=True)
    b = Bridge(free_port(), isolated_config(with_token=False))
    r = b.req("initialize", INIT)
    res = (r or {}).get("result", {})
    check("A initialize answered", res.get("protocolVersion") == "2025-06-18", r)
    ins = res.get("instructions", "")
    check("A offline instructions: the module's guide, prefixed by 'not running', no machine path", ins.startswith("The game is not running") and "Realms:" in ins and "<game folder>" in ins, ins[:200])
    check("A tools.listChanged true", res.get("capabilities", {}).get("tools", {}).get("listChanged") is True, res)
    b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    r = b.req("tools/list")
    names = [t["name"] for t in (r or {}).get("result", {}).get("tools", [])]
    check("A never saw the game, no cache: the full tool list from the embedded snapshot", len(names) > 40 and "lua_exec" in names and "status" in names, names[:5])
    r = b.req("prompts/list")
    check("A prompts from the snapshot", len((r or {}).get("result", {}).get("prompts", [])) >= 1, r)
    r = b.req("resources/list")
    check("A resources from the snapshot", "mcpagent://guide" in json.dumps(r), r)
    r = b.req("tools/call", {"name": "status", "arguments": {}})
    res = (r or {}).get("result", {})
    check("A tools/call says not running", res.get("isError") and "game_not_running" in json.dumps(res), r)
    check("A ping ok", (b.req("ping") or {}).get("result") == {}, r)
    b.close()


def case_f(slow=False):
    print("== F: fake server" + (" (slow)" if slow else ""), flush=True)
    port, env = free_port(), isolated_config(with_token=True)
    if slow:
        srv = FakeServer(port, version=bridge_version()); b = Bridge(port, env)
        b.req("initialize", INIT)
        t0 = time.time()
        r = b.req("tools/call", {"name": "idle", "arguments": {"s": 65}}, timeout=90)
        check("F stream silent for 65 s still delivers its result",
              r and r.get("result", {}).get("structuredContent", {}).get("text") == "woke up", r)
        print("   (took %.0f s)" % (time.time() - t0))
        b.close(); srv.stop(); return

    # Game appears mid-session: list while nothing listens, then start the fake.
    b = Bridge(port, env)
    r = b.req("initialize", INIT)
    check("F initialize while closed", r and "result" in r, r)
    b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    r = b.req("tools/list")
    check("F fallback list while closed", r and r["result"]["tools"][0]["name"] == "status", r)
    srv = FakeServer(port, version=bridge_version())
    check("F list_changed within 5 s of the game appearing", b.wait_notif("notifications/tools/list_changed", 5))
    r = b.req("tools/list")
    check("F live list afterwards", r and "echo" in [t["name"] for t in r["result"]["tools"]], r)

    # Progress notifications come before the result, in order.
    rid = b.start("tools/call", {"name": "progress", "arguments": {}, "_meta": {"progressToken": "p"}})
    r = b.wait(rid)
    with b.lock:
        idx = next(i for i, m in enumerate(b.order) if m.get("id") == rid)
        progs = [m["params"]["progress"] for m in b.order[:idx]
                 if m.get("method") == "notifications/progress" and m["params"]["progressToken"] == "p"]
    check("F 3 progress notifications, in order, before the result", progs == [1, 2, 3] and r["result"]["structuredContent"]["text"] == "done", (progs, r))

    # Cancelling a silent stream closes the connection at once and sends no response.
    rid = b.start("tools/call", {"name": "silent_hang", "arguments": {}})
    time.sleep(0.5)
    t_cancel = time.time()
    b.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": rid}})
    time.sleep(1.5)
    with srv.lock: gone = [t - t_cancel for t in srv.disconnects]
    check("F cancel closes the silent stream within 1 s", gone and gone[0] < 1.0, gone)
    check("F no response for a cancelled request", b.wait(rid, 0.5) is None)
    with srv.lock:
        fwd = any(s[0].get("method") == "notifications/cancelled" for s in srv.seen)
    check("F notifications/cancelled forwarded to the game", fwd)

    # The game dies mid-stream: an error, not silence.
    r = b.req("tools/call", {"name": "nofinal", "arguments": {}})
    check("F stream ending without a response -> JSON-RPC error", r and "error" in r, r)

    # 8 MB result.
    # The duration is printed: well under a second when the reader is buffered (it took 20 s byte by byte).
    t0 = time.time()
    r = b.req("tools/call", {"name": "huge", "arguments": {}}, timeout=60)
    dt = time.time() - t0
    check("F 8 MB result intact (%.1f s)" % dt, r and len(r["result"]["structuredContent"]["text"]) == 8 << 20, "no answer after %.0f s" % dt)

    # 30 concurrent calls with string ids.
    ids = ["c-%d" % i for i in range(30)]
    for i, rid in enumerate(ids): b.start("tools/call", {"name": "echo", "arguments": {"i": i}}, rid=rid)
    got = [b.wait(rid, 20) for rid in ids]
    ok = all(g and g.get("result", {}).get("structuredContent", {}).get("args", {}).get("i") == i for i, g in enumerate(got))
    check("F 30 concurrent calls: every id gets its own answer", ok, [g for g in got if not (g and "args" in g.get("result", {}).get("structuredContent", {}))][:2])

    # Non-ASCII values in the 2026-07-28 headers.
    inst = "inst-é漢 "
    r = b.req("tools/call", {"name": "echo", "arguments": {"instance": inst}, "_meta": modern_meta()})
    h = (r or {}).get("result", {}).get("structuredContent", {}).get("headers", {})
    check("F modern headers: version, method, name", h.get("mcp-protocol-version") == MODERN and h.get("mcp-method") == "tools/call" and h.get("mcp-name") == "echo", h)
    check("F non-ASCII Mcp-Param-Instance sent base64 and decodes back", h.get("mcp-param-instance", "").startswith("=?base64?") and decode_header(h.get("mcp-param-instance")) == inst, h)
    r = b.req("tools/call", {"name": "echo", "arguments": {}, "_meta": {"io.modelcontextprotocol/protocolVersion": 5}})
    check("F non-string version in _meta: answered (legacy), bridge alive", r and "result" in r, r)

    # Malformed input.
    b.raw("this is not json")
    b.raw("[1, 2]")
    b.raw("")
    time.sleep(0.5)
    with b.lock: errs = [m["error"]["code"] for m in b.order if m.get("id") is None and "error" in m]
    check("F invalid JSON -> -32700, batch -> -32600, blank line ignored", errs[-2:] == [-32700, -32600], errs)
    r = b.req("initialize", None, with_params=False)
    check("F initialize without params answered", r and "result" in r, r)
    check("F still alive after malformed input", (b.req("ping") or {}).get("result") == {})

    # stdin closed while a call is in flight: the bridge exits.
    b.start("tools/call", {"name": "idle", "arguments": {"s": 20}})
    time.sleep(0.3)
    t0 = time.time(); b.p.stdin.close()
    try: b.p.wait(3); exited = time.time() - t0
    except subprocess.TimeoutExpired: exited = None; b.p.kill()
    check("F exits within 3 s of stdin closing, call in flight", exited is not None, exited)
    srv.stop()

    # Rejected token.
    srv = FakeServer(port, token="another-token")
    b = Bridge(port, env)
    b.req("initialize", INIT)
    r = b.req("tools/call", {"name": "echo", "arguments": {}})
    check("F rejected token: game_not_running naming the token", r and "rejected the token" in json.dumps(r), r)
    b.close(); srv.stop()

    # Version mismatch between the module and the bridge.
    srv = FakeServer(port, version="9.9.9")
    b = Bridge(port, env)
    r = b.req("initialize", INIT)
    check("F version mismatch announced in the instructions", r and r["result"]["instructions"].startswith("Version mismatch") and "9.9.9" in r["result"]["instructions"], r)
    b.close(); srv.stop()


def mreq(b, method, params=None, timeout=15, meta=None):
    p = dict(params or {})
    p["_meta"] = meta if meta is not None else modern_meta()
    return b.req(method, p, timeout)


def case_m():
    print("== M: 2026-07-28 client, fake server", flush=True)
    port, env = free_port(), isolated_config(with_token=True)
    b = Bridge(port, env)
    # Game closed: the bridge answers like a 2026-07-28 server.
    r = mreq(b, "server/discover")
    res = (r or {}).get("result", {})
    check("M offline discover: resultType, serverInfo, versions", res.get("resultType") == "complete" and MODERN in res.get("supportedVersions", []) and "io.modelcontextprotocol/serverInfo" in res.get("_meta", {}), r)
    r = mreq(b, "tools/list")
    res = (r or {}).get("result", {})
    st = res.get("tools", [{}])[0]
    check("M offline list: resultType complete, ttlMs 0", res.get("resultType") == "complete" and res.get("ttlMs") == 0, r)
    check("M fallback status tool is annotated read-only", st.get("annotations", {}).get("readOnlyHint") is True, st)
    r = mreq(b, "tools/call", {"name": "status", "arguments": {}})
    check("M offline call: resultType complete + game_not_running", r and r["result"].get("resultType") == "complete" and "game_not_running" in json.dumps(r), r)
    r = mreq(b, "ping")
    check("M ping (removed in 2026-07-28) -> -32601", r and r.get("error", {}).get("code") == -32601, r)
    bad = modern_meta(); del bad["io.modelcontextprotocol/clientCapabilities"]
    r = mreq(b, "tools/list", meta=bad)
    check("M missing clientCapabilities -> -32602", r and r.get("error", {}).get("code") == -32602, r)
    old = modern_meta(); old["io.modelcontextprotocol/protocolVersion"] = "1999-01-01"
    r = mreq(b, "tools/list", meta=old)
    check("M unsupported version -> -32022 with the supported list", r and r.get("error", {}).get("code") == -32022 and MODERN in r["error"].get("data", {}).get("supported", []), r)

    # Subscriptions while closed: acknowledged first, with the subscription id.
    sub = b.start("subscriptions/listen", {"_meta": modern_meta(), "notifications": {"toolsListChanged": True, "promptsListChanged": True}})
    time.sleep(0.5)
    with b.lock: acks = [n for n in b.notes if n.get("method") == "notifications/subscriptions/acknowledged"]
    ok = acks and acks[0]["params"]["_meta"].get("io.modelcontextprotocol/subscriptionId") == sub and acks[0]["params"]["notifications"] == {"toolsListChanged": True, "promptsListChanged": True}
    check("M listen acknowledged first, with subscription id and honored filter", ok, acks)
    srv = FakeServer(port, version=bridge_version())
    got = b.wait_notif("notifications/tools/list_changed", 6)
    time.sleep(0.3)
    with b.lock:
        lc = [n for n in b.notes if n.get("method", "").endswith("list_changed")]
    on_sub = all(n.get("params", {}).get("_meta", {}).get("io.modelcontextprotocol/subscriptionId") == sub for n in lc)
    kinds = sorted(n["method"] for n in lc)
    check("M game appears: tools and prompts list_changed on the subscription only", got and on_sub and kinds == ["notifications/prompts/list_changed", "notifications/tools/list_changed"], lc)
    r = mreq(b, "tools/list")
    tools = {t["name"]: t for t in (r or {}).get("result", {}).get("tools", [])}
    check("M live list keeps the game's annotations", tools.get("echo", {}).get("annotations", {}).get("idempotentHint") is True, tools.get("echo"))
    b.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": sub}})
    time.sleep(0.3)
    check("M cancelled subscription: no response", b.wait(sub, 0.5) is None)

    # Resource subscription relayed to the game, reopened after a restart, ended by cancel.
    sub2 = b.start("subscriptions/listen", {"_meta": modern_meta(), "notifications": {"resourceSubscriptions": ["gmod://errors", "file:///nope"]}})
    time.sleep(1.5)
    with b.lock:
        ack2 = [n for n in b.notes if n.get("method") == "notifications/subscriptions/acknowledged" and n["params"]["_meta"].get("io.modelcontextprotocol/subscriptionId") == sub2]
        upd = [n for n in b.notes if n.get("method") == "notifications/resources/updated" and n["params"]["_meta"].get("io.modelcontextprotocol/subscriptionId") == sub2]
    check("M resource subscription: only the game's URIs honored", ack2 and ack2[0]["params"]["notifications"].get("resourceSubscriptions") == ["gmod://errors"], ack2)
    check("M resources/updated relayed with the subscription id", len(upd) >= 2 and upd[0]["params"]["uri"] == "gmod://errors", upd[:2])
    srv.stop()
    time.sleep(1)
    srv = FakeServer(port, version=bridge_version())
    with b.lock: n_before = len([n for n in b.notes if n.get("method") == "notifications/resources/updated"])
    time.sleep(4)
    with b.lock: n_after = len([n for n in b.notes if n.get("method") == "notifications/resources/updated"])
    check("M game restarted: the resource subscription is reopened", n_after > n_before and srv.listens >= 1, (n_before, n_after, srv.listens))
    b.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": sub2}})
    time.sleep(1)
    with b.lock: n_cancel = len([n for n in b.notes if n.get("method") == "notifications/resources/updated"])
    time.sleep(1.5)
    with b.lock: n_later = len([n for n in b.notes if n.get("method") == "notifications/resources/updated"])
    check("M cancelled resource subscription: the game stream is closed, nothing more arrives", n_later == n_cancel and len(srv.disconnects) >= 1, (n_cancel, n_later, srv.disconnects))
    with b.lock: unsolicited = [n for n in b.notes if n.get("method", "").endswith("list_changed") and "_meta" not in n.get("params", {})]
    check("M no list_changed outside a subscription for a 2026-07-28 client", not unsolicited, unsolicited)

    # Cancelling a 2026-07-28 call: the stream is closed, no notification is forwarded.
    with srv.lock: srv.seen.clear()
    rid = b.start("tools/call", {"name": "silent_hang", "arguments": {}, "_meta": modern_meta()})
    time.sleep(0.5)
    b.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": rid}})
    time.sleep(1.2)
    with srv.lock: fwd = [x[0] for x in srv.seen if x[0].get("method") == "notifications/cancelled"]
    check("M cancel of a 2026-07-28 call closes the stream without forwarding the notification", not fwd and b.wait(rid, 0.3) is None, fwd)
    b.close(); srv.stop()


def case_multi():
    print("== MULTI: 6 bridges sharing one config folder", flush=True)
    port, env = free_port(), isolated_config(with_token=True)
    bs = [Bridge(port, env) for _ in range(6)]
    for i, b in enumerate(bs):
        if i % 2:  # a 2026-07-28 session (lists, then subscribes): told through its subscription
            mreq(b, "tools/list")
            b.start("subscriptions/listen", {"_meta": modern_meta(), "notifications": {"toolsListChanged": True}})
        else:  # a legacy session: told with a plain notification
            b.req("initialize", INIT); b.req("tools/list")
    srv = FakeServer(port, version=bridge_version())
    told = [b.wait_notif("notifications/tools/list_changed", 8) for b in bs]
    check("MULTI every bridge (legacy and 2026-07-28 sessions) is told the game appeared", all(told), told)
    # Concurrent cache writes: every bridge stores the live lists and descriptions at the same time.
    threads = []
    for b in bs:
        for m in ("tools/list", "prompts/list", "resources/list"):
            t = threading.Thread(target=b.req, args=(m, {"_meta": modern_meta()} if bs.index(b) % 2 else None))
            t.start(); threads.append(t)
    for t in threads: t.join(20)
    folder = os.path.join(env["_TEST_CONFIG_DIR"], "gmod-mcp")
    try:
        cache = json.load(open(os.path.join(folder, "bridge-cache.json")))
        ok = "tools/list|legacy" in cache and "tools/list|modern" in cache
    except Exception as e:
        cache, ok = str(e), False
    check("MULTI shared cache valid JSON with the entries of both eras", ok, sorted(cache) if isinstance(cache, dict) else cache)
    leftovers = [f for f in os.listdir(folder) if f.endswith(".tmp")]
    check("MULTI no temporary file left behind", not leftovers, leftovers)
    for b in bs: b.close()
    srv.stop()


def alive(pid):
    if os.name == "nt":
        import ctypes
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h: return False
        code = ctypes.c_ulong()
        k.GetExitCodeProcess(h, ctypes.byref(code)); k.CloseHandle(h)
        return code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0); return True
    except OSError:
        return False


# The launcher lives half a second, like any real client that started the bridge (not microseconds).
LAUNCHER = """import subprocess, sys, time
p = subprocess.Popen(sys.argv[1:], stdin=sys.stdin, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(p.pid, flush=True)
time.sleep(0.5)
"""


def orphan_run(extra):
    env = isolated_config(with_token=False)
    l = subprocess.Popen([sys.executable, "-c", LAUNCHER, BRIDGE, "--port", str(free_port())] + extra,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
    pid = int(l.stdout.readline())
    l.wait(5)  # the launcher is gone; this script still holds the bridge's stdin: no end-of-file
    end = time.time() + 5
    while time.time() < end and alive(pid): time.sleep(0.2)
    still = alive(pid)
    if still and os.name == "nt": subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    elif still: os.kill(pid, 9)
    l.stdin.close()
    return still


def case_orphan():
    print("== ORPHAN: the process that started the bridge dies", flush=True)
    check("ORPHAN bridge exits within 5 s of its parent dying (pipe still open)", not orphan_run([]))
    check("ORPHAN with --no-parent-watch it stays (control)", orphan_run(["--no-parent-watch"]))


def case_fuzz():
    print("== FUZZ: 400 random messages", flush=True)
    rnd = random.Random(1234)
    port, env = free_port(), isolated_config(with_token=True)
    srv = FakeServer(port, version=bridge_version())
    b = Bridge(port, env)
    methods = ["initialize", "server/discover", "tools/list", "tools/call", "prompts/get", "resources/read",
               "subscriptions/listen", "notifications/cancelled", "ping", "completion/complete", "nope", "", None, 7]
    def junk(depth=0):
        k = rnd.randrange(8 if depth < 3 else 5)
        if k == 0: return None
        if k == 1: return rnd.random() < 0.5
        if k == 2: return rnd.randint(-2**63, 2**63)
        if k == 3: return rnd.random() * 1e308
        if k == 4: return "x" * rnd.randrange(50)
        if k == 5: return [junk(depth + 1) for _ in range(rnd.randrange(4))]
        if k == 6:
            keys = ["_meta", "name", "arguments", "requestId", "notifications", "io.modelcontextprotocol/protocolVersion", "a"]
            return {rnd.choice(keys): junk(depth + 1) for _ in range(rnd.randrange(4))}
        return {"_meta": {"io.modelcontextprotocol/protocolVersion": rnd.choice([MODERN, "2025-06-18", 5, None]),
                          "io.modelcontextprotocol/clientCapabilities": junk(depth + 1)}}
    for i in range(400):
        r = rnd.random()
        if r < 0.1:
            b.p.stdin.write(bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 60))).replace(b"\n", b" ") + b"\n")
            b.p.stdin.flush()
            continue
        m = {"jsonrpc": rnd.choice(["2.0", "1.0", None])}
        if rnd.random() < 0.8: m["method"] = rnd.choice(methods)
        if rnd.random() < 0.7: m["id"] = rnd.choice([i, "s%d" % i, None, 1.5, [1], {"a": 1}])
        if rnd.random() < 0.8: m["params"] = junk()
        b.raw(json.dumps(m))
    time.sleep(2)
    r = b.req("ping", timeout=10)
    check("FUZZ bridge alive and answering after 400 random messages", r is not None and b.p.poll() is None, r)
    r = b.req("tools/call", {"name": "echo", "arguments": {"ok": 1}}, timeout=10)
    check("FUZZ still relays to the game", r and r.get("result", {}).get("structuredContent", {}).get("args", {}).get("ok") == 1, r)
    with b.lock: bad = [m for m in b.order if not isinstance(m, dict) or m.get("jsonrpc") != "2.0"]
    check("FUZZ every line written to stdout is a JSON-RPC 2.0 message", not bad, bad[:2])
    b.close(); srv.stop()


def case_session():
    print("== SESSION: the bridge's own tools (no game launched)", flush=True)
    port, env = free_port(), isolated_config(with_token=True)
    b = Bridge(port, env)
    r = b.req("tools/list")
    names = [t["name"] for t in (r or {}).get("result", {}).get("tools", [])]
    check("SESSION offline list has instances_launch, instances_close, instance_select", all(n in names for n in ("instances_launch", "instances_close", "instance_select")), names[-4:])
    srv = FakeServer(port, version=bridge_version())
    b.wait_notif("notifications/tools/list_changed", 6)
    r = b.req("tools/list")
    names = [t["name"] for t in (r or {}).get("result", {}).get("tools", [])]
    check("SESSION live list = game tools + bridge tools", "echo" in names and "instances_launch" in names, names)
    r = b.req("tools/call", {"name": "echo", "arguments": {}})
    check("SESSION no default: nothing injected", r and "instance" not in r["result"]["structuredContent"]["args"], r)
    r = b.req("tools/call", {"name": "instance_select", "arguments": {"instance": "client-42"}})
    check("SESSION instance_select answers", r and r["result"]["structuredContent"].get("default_instance") == "client-42", r)
    r = b.req("tools/call", {"name": "echo", "arguments": {}})
    check("SESSION default injected into calls without instance", r and r["result"]["structuredContent"]["args"].get("instance") == "client-42", r)
    r = b.req("tools/call", {"name": "echo", "arguments": {}, "_meta": modern_meta()})
    h = (r or {}).get("result", {}).get("structuredContent", {})
    check("SESSION 2026-07-28: injected value mirrored in Mcp-Param-Instance", h.get("args", {}).get("instance") == "client-42" and h.get("headers", {}).get("mcp-param-instance") == "client-42", h)
    r = b.req("tools/call", {"name": "echo", "arguments": {"instance": "client-7"}})
    check("SESSION an explicit instance wins over the default", r and r["result"]["structuredContent"]["args"].get("instance") == "client-7", r)
    r = b.req("tools/call", {"name": "instance_select", "arguments": {"instance": None}})
    r = b.req("tools/call", {"name": "echo", "arguments": {}})
    check("SESSION null clears the default", r and "instance" not in r["result"]["structuredContent"]["args"], r)
    r = b.req("tools/call", {"name": "instances_close", "arguments": {"instances": ["client-1234"]}})
    check("SESSION instances_close refuses an instance this session did not start", r and r["result"].get("isError") and "client-1234" in json.dumps(r["result"]["structuredContent"].get("refused")), r)
    r = b.req("tools/call", {"name": "instances_close", "arguments": {}})
    check("SESSION instances_close without targets -> invalid_params", r and "invalid_params" in json.dumps(r), r)
    r = b.req("tools/call", {"name": "instances_launch", "arguments": {"game_dir": "C:/definitely/not/here"}})
    check("SESSION instances_launch with a wrong game_dir -> game_not_found, nothing started", r and "game_not_found" in json.dumps(r) or "unsupported" in json.dumps(r), r)
    b.close(); srv.stop()


def case_orphans():
    """instances_launch with a stand-in game in which gmod-mcp never loads (RETEX 2026-11-13: such clients could only
    be killed by hand): the timeout lists their pids, instances_close closes them by pid and by owner label."""
    print("== ORPHANS: clients that never answer, closed by pid and by owner", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("ORPHANS stand-in game built next to the bridge", False, fake)
        return
    import shutil
    game = tempfile.mkdtemp(prefix="gmcp-fake-game-")
    shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
    port, env = free_port(), isolated_config(with_token=True)
    b = Bridge(port, env)
    r = b.req("tools/call", {"name": "instances_launch", "arguments": {"count": 3, "owner": "fork-a", "timeout_s": 10, "game_dir": game}}, 40)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    pids = sc.get("silent_pids", [])
    check("ORPHANS timeout names the silent clients' pids", r and r["result"].get("isError") and len(pids) == 3 and all(alive(p) for p in pids), sc)
    check("ORPHANS timeout says which clients answered (none here)", sc.get("answered") == [], sc)
    # instances_list while no game answers (SW2: it said "not reachable"; SB1: the silent clients were invisible).
    r = b.req("tools/call", {"name": "instances_list", "arguments": {}}, 20)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    silent = sc.get("launched_not_answering", [])
    check("ORPHANS instances_list without a game: no error, no instance, a clear note",
          r and not r["result"].get("isError") and sc.get("instances") == [] and "no game instance" in sc.get("note", ""), sc)
    check("ORPHANS instances_list lists the launched clients that never answered, with their owner",
          sorted(x.get("pid") for x in silent) == sorted(pids) and all(x.get("owner") == "fork-a" for x in silent), silent)
    r = b.req("tools/call", {"name": "instances_list", "arguments": {"owner": "fork-b"}}, 20)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    check("ORPHANS instances_list {owner} filters by owner", r and not sc.get("launched_not_answering"), sc)
    r = b.req("tools/call", {"name": "instances_close", "arguments": {"pids": [pids[0] if pids else 0, 4]}}, 20)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    check("ORPHANS close by pid: closed, and a pid this session did not start is refused",
          [c.get("pid") for c in sc.get("closed", [])] == pids[:1] and sc.get("refused") == [4], sc)
    time.sleep(1)
    check("ORPHANS the closed one is gone, the others still run", pids and not alive(pids[0]) and all(alive(p) for p in pids[1:]), [alive(p) for p in pids])
    r = b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "nobody"}}, 20)
    check("ORPHANS an unknown owner closes nothing", r and r["result"].get("isError") and not r["result"]["structuredContent"].get("closed"), r)
    r = b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "fork-a"}}, 20)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    check("ORPHANS close by owner closes the rest, labelled", sorted(c.get("pid") for c in sc.get("closed", [])) == sorted(pids[1:]) and all(c.get("owner") == "fork-a" for c in sc.get("closed", [])), sc)
    time.sleep(1)
    check("ORPHANS no stand-in left running", not any(alive(p) for p in pids), [p for p in pids if alive(p)])
    r = b.req("tools/call", {"name": "instances_list", "arguments": {}}, 20)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    check("ORPHANS closed clients no longer listed", r and not sc.get("launched_not_answering"), sc)
    b.close()
    shutil.rmtree(game, ignore_errors=True)


def case_launch():
    """Live (Windows, Steam running, no other game): starts 2 light clients with a map, uses the default instance,
    closes them. Note: the game saves their window size as its video setting (documented)."""
    print("== LAUNCH: instances_launch / instance_select / instances_close (live)", flush=True)
    b = Bridge(27080)
    rid = b.start("tools/call", {"name": "instances_launch", "arguments": {"count": 2, "map": "gm_construct"}, "_meta": {"progressToken": "L"}})
    r = b.wait(rid, 300)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    ins = sc.get("instances", [])
    check("LAUNCH 2 clients answered with ids, pids and ports", len(ins) == 2 and all(i.get("instance") and i.get("pid") for i in ins), sc)
    check("LAUNCH map loaded on both", all(i.get("map") == "gm_construct" for i in ins), ins)
    with b.lock: progs = [m for m in b.notes if m.get("method") == "notifications/progress" and m["params"]["progressToken"] == "L"]
    check("LAUNCH progress reported", len(progs) >= 2, len(progs))
    if len(ins) == 2:
        target = ins[1]["instance"]
        b.req("tools/call", {"name": "instance_select", "arguments": {"instance": target}})
        r = b.req("tools/call", {"name": "status", "arguments": {}}, 20)
        check("LAUNCH a call without instance reaches the default one", r and r["result"]["structuredContent"].get("answered_by") == target, r)
        r = b.req("tools/call", {"name": "lua_exec", "arguments": {"realm": "client", "code": "return ScrW(), ScrH()"}}, 20)
        check("LAUNCH light preset window is 640x360", r and r["result"]["structuredContent"].get("values") == [640, 360], r)
    pids = [i.get("pid") for i in ins]
    r = b.req("tools/call", {"name": "instances_close", "arguments": {"all": True}}, 30)
    check("LAUNCH instances_close all closes both", r and len(r["result"]["structuredContent"].get("closed", [])) == 2, r)
    time.sleep(2)
    check("LAUNCH their processes are gone", not any(alive(p) for p in pids if p), [p for p in pids if p and alive(p)])
    b.close()


def case_b():
    print("== B: live game on 27080 (read-only)", flush=True)
    b = Bridge(27080)
    r = b.req("initialize", INIT)
    res = (r or {}).get("result", {})
    check("B initialize from game", "gmod" in json.dumps(res.get("serverInfo", {})).lower() and len(res.get("instructions", "")) > 200, res.get("serverInfo"))
    b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    r = b.req("tools/list")
    tools = [t["name"] for t in (r or {}).get("result", {}).get("tools", [])]
    check("B tools/list live (>40 tools)", len(tools) > 40, tools[:5])
    r = b.req("tools/call", {"name": "instances_list", "arguments": {}})
    check("B instances_list", r and not r["result"].get("isError") and "instances" in r["result"]["structuredContent"], r)
    me = next(i["id"] for i in r["result"]["structuredContent"]["instances"] if i.get("self"))
    rid = b.start("tools/call", {"name": "wait_frames", "arguments": {"n": 240, "instance": me}, "_meta": {"progressToken": "p1"}})
    r = b.wait(rid, 30)
    check("B wait_frames result", r and not r["result"].get("isError"), r)
    with b.lock: progs = [m for m in b.notes if m.get("method") == "notifications/progress"]
    check("B progress notifications relayed", len(progs) > 0)
    r = b.req("server/discover", {"_meta": modern_meta()})
    check("B server/discover", r and MODERN in r["result"].get("supportedVersions", []), r)
    r = b.req("tools/call", {"name": "status", "arguments": {"instance": me}, "_meta": modern_meta()})
    res = (r or {}).get("result", {})
    check("B modern tools/call status", res.get("resultType") == "complete" and not res.get("isError"), r)
    r = b.req("tools/call", {"name": "console_exec", "arguments": {"command": "echo hi"}, "_meta": modern_meta()})
    check("B bad argument -> invalid_params", "invalid_params" in json.dumps((r or {}).get("result", {})), r)
    sub = b.start("subscriptions/listen", {"_meta": modern_meta(), "notifications": {"toolsListChanged": True, "resourceSubscriptions": ["gmod://console", "gmod://nope"]}})
    time.sleep(1.5)
    with b.lock: ack = [n for n in b.notes if n.get("method") == "notifications/subscriptions/acknowledged"]
    hon = ack[0]["params"]["notifications"] if ack else {}
    check("B listen: the game's resource subscriptions honored through the bridge", hon.get("resourceSubscriptions") == ["gmod://console"] and hon.get("toolsListChanged") is True, ack)
    b.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": sub}})
    check("B cancelled listen: no response", b.wait(sub, 1.0) is None)
    b.close()


class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        h = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "content-length", "connection")}
        req = urllib.request.Request("http://127.0.0.1:27080/mcp", data=body, headers=h, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data, code, ct = r.read(), r.status, r.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as e:
            data, code, ct = e.read(), e.code, e.headers.get("Content-Type", "application/json")
        self.send_response(code); self.send_header("Content-Type", ct); self.send_header("Content-Length", str(len(data)))
        self.end_headers(); self.wfile.write(data)

    def log_message(self, *a): pass


def case_c():
    print("== C: live game appears mid-session", flush=True)
    port = free_port()
    b = Bridge(port)
    b.req("initialize", INIT)
    b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    r = b.req("tools/list")
    check("C tools/list while down", r and "result" in r, r)
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", port), Relay)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    check("C list_changed sent when the game appears", b.wait_notif("notifications/tools/list_changed", 10))
    r = b.req("tools/list")
    check("C tools/list now live", r and len(r["result"]["tools"]) > 40, r)
    srv.shutdown(); srv.server_close()
    r = b.req("tools/call", {"name": "status", "arguments": {}})
    check("C call after the game left says not running", r and "game_not_running" in json.dumps(r), r)
    b.close()

    print("== C2: live game appears mid-session, 2026-07-28 client", flush=True)
    port = free_port()
    b = Bridge(port)
    sub = b.start("subscriptions/listen", {"_meta": modern_meta(), "notifications": {"toolsListChanged": True}})
    r = b.req("tools/list", {"_meta": modern_meta()})
    check("C2 offline list", r and r["result"].get("resultType") == "complete", r)
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", port), Relay)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    ok = b.wait_notif("notifications/tools/list_changed", 10)
    with b.lock: lc = [n for n in b.notes if n.get("method") == "notifications/tools/list_changed"]
    check("C2 list_changed on the subscription", ok and lc[0]["params"]["_meta"].get("io.modelcontextprotocol/subscriptionId") == sub, lc)
    r = b.req("tools/list", {"_meta": modern_meta()})
    check("C2 live list", r and len(r["result"].get("tools", [])) > 40, r)
    srv.shutdown(); srv.server_close()
    b.close()


def case_retex():
    """Live (one game with gmod-mcp on 27080, a map loaded; give `instance` with RETEX_INSTANCE if several run):
    the 2026-11-14 field feedback changes that need parameters older clients do not send."""
    print("== RETEX: live checks of the 2026-11-14 field feedback changes", flush=True)
    b = Bridge(27080)
    inst = os.environ.get("RETEX_INSTANCE")

    def call(tool, args, timeout=60):
        a = dict(args)
        if inst: a["instance"] = inst
        r = b.req("tools/call", {"name": tool, "arguments": a}, timeout)
        res = (r or {}).get("result", {})
        return res.get("structuredContent", {}), bool(res.get("isError"))

    def ex(code, **kw):
        return call("lua_exec", dict(kw, realm=kw.get("realm", "server"), code=code))

    sc, err = ex("return string.rep('x', 70000)", max_string=100)
    check("RETEX explicit max_string cuts and says truncated", not err and sc.get("truncated") == 1 and sc["values"][0].get("length") == 70000, sc)
    sc, err = ex("return {{{{1}}}}", depth=2)
    check("RETEX explicit depth cuts and says truncated", not err and sc.get("truncated") == 1, sc)
    sc, err = ex("return debug.getinfo(1, 'S').short_src", chunkname="@addons/pba/lua/pba/x.lua")
    check("RETEX chunkname is the source", sc.get("values") == ["addons/pba/lua/pba/x.lua"], sc)
    sc, err = ex("local t = {} for i = 1, 3000000 do t[i] = i end return t", timeout_s=60)
    check("RETEX a result of 3 million values fails with result_too_large", err and "result_too_large" in json.dumps(sc), str(sc)[:300])
    # lua_job: async, status, concurrent, cancel, fresh_env.
    sc, err = call("lua_job", {"realm": "server", "async": True, "code": "job:sleep(1) return args.v", "args": {"v": 7}})
    jid = sc.get("job_id")
    check("RETEX async lua_job returns its id at once", not err and jid and sc.get("state") == "running", sc)
    sc2, _ = call("lua_job", {"realm": "server", "async": True, "code": "job:sleep(30) return 1"})
    check("RETEX a second job in the same realm reports the first in concurrent_jobs",
          any(c.get("id") == jid for c in sc2.get("concurrent_jobs", [])), sc2)
    sc, err = call("lua_job_status", {})
    check("RETEX lua_job_status without id lists both", {jid, sc2.get("job_id")} <= {j.get("id") for j in sc.get("jobs", [])}, sc)
    sc, err = call("lua_job_status", {"id": jid, "wait_s": 10})
    check("RETEX lua_job_status waits for the end and returns the result", not err and sc.get("values") == [7], sc)
    sc, err = call("lua_job_status", {"id": jid})
    check("RETEX a collected job is forgotten", err and "not_found" in json.dumps(sc), sc)
    sc, err = call("lua_job_cancel", {"id": sc2.get("job_id")})
    check("RETEX lua_job_cancel stops a sleeping job", not err and sc.get("cancelled") is True, sc)
    sc, err = call("lua_job_status", {"id": sc2.get("job_id"), "wait_s": 5})
    check("RETEX the cancelled job ends as cancelled", err and "cancelled" in json.dumps(sc), sc)
    call("lua_job", {"realm": "server", "fresh_env": True, "code": "__RETEX_FRESH = 1 return 1"})
    sc, err = ex("return __RETEX_FRESH == nil")
    check("RETEX fresh_env keeps the job's globals out of _G", sc.get("values") == [True], sc)
    call("lua_job", {"realm": "server", "code": "__RETEX_SHARED = 1 return 1"})
    sc, err = ex("local v = __RETEX_SHARED __RETEX_SHARED = nil return v")
    check("RETEX without fresh_env the global lands in _G", sc.get("values") == [1], sc)
    # Files: absolute path, chunk name, fields.
    tmp = os.path.join(tempfile.mkdtemp(prefix="gmcp-retex-"), "verdict.lua")
    with open(tmp, "w") as f: f.write("return { pass = 3, fails = 0, big = string.rep('z', 1000), src = debug.getinfo(1, 'S').short_src }\n")
    sc, err = call("lua_exec_file", {"realm": "server", "path": tmp, "chunkname": "@addons/x/lua/verdict.lua", "fields": ["pass", "fails", "src"]})
    v = (sc.get("values") or [{}])[0]
    check("RETEX lua_exec_file: absolute path, chunkname, fields", not err and v == {"pass": 3, "fails": 0, "src": "addons/x/lua/verdict.lua"}, sc)
    # Errors of the current map only.
    ex("timer.Simple(0, function() error('retex current map error') end)")
    time.sleep(1)
    sc, err = call("errors_since", {"cursor": 0, "since_map_load": True})
    errs = sc.get("errors", [])
    check("RETEX errors_since since_map_load: current map's errors, tagged", errs and all(e.get("map_load") == sc.get("map_load") for e in errs)
          and any("retex current map error" in e.get("message", "") for e in errs), sc)
    # status fields.
    sc, err = call("status", {"fields": ["map", "fps_max", "host_timescale"]})
    check("RETEX status fields keeps only those keys", set(sc) - {"answered_by", "relayed_by"} == {"map", "fps_max", "host_timescale", "projected"}, sc)
    # console_exec: regex filter, unknown command, archived convar warning (read only: no value given).
    sc, err = call("console_exec", {"realm": "server", "cmd": "echo retex-a; echo retex-b", "regex": "retex-b"})
    check("RETEX console_exec runs both statements, regex keeps one line", not err and [l["text"].strip() for l in sc.get("output", [])] == ["retex-b"] and len(sc.get("statements", [])) == 2, sc)
    sc, err = call("console_exec", {"realm": "server", "cmd": "retex_typo_cmd 1"})
    check("RETEX unknown command is an error", err and "unknown_command" in json.dumps(sc), sc)
    sc, err = call("console_exec", {"realm": "menu", "cmd": "fov_desired"})
    c = sc.get("convars", {}).get("fov_desired", {})
    check("RETEX naming an archived convar reads it back with a warning", not err and c.get("number") is not None and "ARCHIVE" in c.get("warning", ""), sc)
    sc, err = call("convar_get", {"name": "sv_gravity", "realm": "server"})
    check("RETEX convar_get gives the number", sc.get("number") == 600, sc)
    b.close()


CASES = {"a": case_a, "f": case_f, "m": case_m, "multi": case_multi, "session": case_session, "launch": case_launch, "orphans": case_orphans, "orphan": case_orphan, "fuzz": case_fuzz, "fslow": lambda: case_f(slow=True), "b": case_b, "c": case_c, "retex": case_retex}
for c in sys.argv[2:] or ["a", "f", "m", "multi", "session", "orphans", "orphan", "fuzz"]:
    CASES[c]()
passed = sum(1 for _, ok in results if ok)
print("TOTAL %d/%d" % (passed, len(results)))
sys.exit(0 if passed == len(results) else 1)
