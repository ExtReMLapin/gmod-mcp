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
  lua_mount_verify instances_launch {lua_mount}: the result reports the mount each engine really has (fake game server)
  launch_options instances_launch {fps_max, fps_max_nofocus, commands, launch_args} (Issue #10): command line, commands in
         order before the map, convars to restore listed for the module, bad options refused before anything starts
  handover Issue #6: the coordinator role moves to another game: calls wait for it and retry coordinator_changing, but
         never wait when no other instance is known to live (fake server)
  launch_mount_first Issue #6: instances_launch checks (and re-applies) the lua_mount before any map loads, no map for a client
         without it, coordinator_changing waited out (fake server, stand-in game)
  close_coordinator Issue #6: instances_close of the coordinator says the role moved and to whom; instances_list warns beforehand
  chaos_health Issue #8 (chaos tests G1): instances_launch refuses (peer_unhealthy) while a running client is unresponsive or
         hung, unless ignore_unhealthy, and names the unhealthy peers when it times out (fake server, stand-in game)
  chaos_launch Issue #8 (chaos test G2): a launched client dies at startup while its sibling keeps running: the error names
         the survivor and warns it is not closed (stand-in game, fake game server)
  lowmem instances_launch refuses under low free memory (low_memory), ignore_low_memory, free memory in a crash report
  crash  a client of the session crashed or quit (stand-in game with a crash mode and a fake dump folder): client_crashed
         with the dump name and exit code, instances_list shows it as crashed, a clean exit is not a crash, a map_load
         in flight when the client dies reports the crash, an old dump is not taken for its own (Windows)
  fp7137 launch / map_load lock shared by the bridges (FP7137, docs/FP7137.md): waits, expiry, dead bridge, kill switch
  unseen map loads the bridge does not see (issue #7, FP7137): a launch waits for a game that reports loading, fails at its
         timeout, obeys the kill switch; map_load lists the other games that were loading
  lua_mount instances_launch {lua_mount}: -mcp_lua_mount passed to the client (stand-in game), bad folders refused
  win64_layout instances_launch finds bin/win64/gmod.exe (no gmod_win64.exe at the root), starts it in the game folder
         (Issue #4)
  orphan the process that started the bridge dies without closing the pipe: the bridge exits (and stays with
         --no-parent-watch)
  fuzz   400 random messages (random JSON shapes, ids, params, invalid UTF-8): the bridge survives and still answers
  fslow  the fake server keeps a stream silent for 65 s (a client request has no idle limit)
  launch live: instances_launch of 2 light clients with a map, default instance, instances_close (Windows, Steam,
         no other game; the game saves their window size as its video setting)
  portconflict  a program that is not gmod-mcp holds the port (issue #5): port_conflict, names the port and the fix
  b      live game on 27080, read-only calls (needs the game and the user's token)
  c      the live game appears mid-session through a local HTTP relay (needs the game)
Cases a, f and fslow use a temporary config folder: the user's token and cache are not touched.

Usage: python test_bridge.py <path to gmod-mcp-bridge> [case ...]   (default: a f m multi session orphans lua_mount win64_layout lua_mount_verify crash lowmem fp7137 orphan portconflict unseen handover launch_mount_first close_coordinator fuzz)
Prints PASS/FAIL per check and exits non-zero on a failure."""
import base64, http.server, json, os, queue, random, select, socket, socketserver, subprocess, sys, tempfile, threading, time
import urllib.error, urllib.request

try: sys.stdout.reconfigure(errors="replace")  # failure details may hold non-ASCII paths
except Exception: pass
BRIDGE = os.path.abspath(sys.argv[1])
results = []
TOKEN = "test-token-0123456789abcdef0123456789abcdef"
MODERN = "2026-07-28"


def check(name, cond, info=""):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  -> " + str(info)[:400]), flush=True)


def json_equal(text, value):
    """True when `text` is JSON that equals `value` (the text block of a result is the JSON of its structuredContent)."""
    try: return json.loads(text) == value
    except (ValueError, TypeError): return False


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
            return self.reply_json({"error": {"code": "unauthorized", "message": "missing or invalid bearer token"}}, 401)
        if "id" not in body:
            self.send_response(202); self.send_header("Content-Length", "0"); self.end_headers(); return
        rid, method, params = body["id"], body.get("method"), body.get("params", {})
        res = lambda r: {"jsonrpc": "2.0", "id": rid, "result": r}
        if method == "initialize":
            return self.reply_json(res({"protocolVersion": params.get("protocolVersion", "2025-06-18"),
                                        "capabilities": {"tools": {"listChanged": False}},
                                        "serverInfo": {"name": "gmod-mcp", "version": srv.version},
                                        "instructions": srv.instructions}))
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
        if name == "echo" and srv.changing > 0:  # Issue #6: the coordinator role is moving
            srv.changing -= 1
            sc = {"error": {"code": "coordinator_changing", "message": "the coordinator role just moved", "details": {"retry_after_s": 0.3}}}
            return self.reply_json(res({"content": [{"type": "text", "text": json.dumps(sc)}], "structuredContent": sc, "isError": True}))
        if name == "game_terminate" and srv.terminate_hook:  # Issue #6
            srv.terminate_hook(args)
            return self.reply_json(text("terminating"))
        if name == "instances_list" and srv.instances_answer:  # Issue #6: the test decides what the directory says
            sc = srv.instances_answer()
            return self.reply_json(res({"content": [{"type": "text", "text": "instances"}], "structuredContent": sc}))
        if name == "echo":
            h = {k.lower(): v for k, v in self.headers.items() if k.lower().startswith("mcp")}
            sc = {"args": args, "headers": h}  # like the module: the text is the JSON of structuredContent
            return self.reply_json(res({"content": [{"type": "text", "text": json.dumps(sc, separators=(",", ":"))}],
                                        "structuredContent": sc}))
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
        if name == "instances_list" and srv.pid_file:
            # The stand-in game (GMCP_FAKE_PID) wrote its pid: the fake module lists it as a registered instance.
            try: pid = int(open(srv.pid_file).read().strip() or 0)
            except (OSError, ValueError): pid = 0
            sc = {"instances": [{"id": "fake-%d" % pid, "pid": pid, "port": 1, "mode": "client"}] if pid else []}
            return self.reply_json(res({"content": [{"type": "text", "text": "instances"}], "structuredContent": sc}))
        if name == "map_load" and srv.map_load_hook:
            srv.map_load_hook(args)  # blocks until the test lets the client die: then the connection is cut, no answer
            self.close_connection = True
            return
        if name == "instances_list" and srv.pid_dir:  # FP7137 tests: one pid file per stand-in game
            pids = []
            for f in os.listdir(srv.pid_dir):
                try: pids.append(int(open(os.path.join(srv.pid_dir, f)).read().strip()))
                except (OSError, ValueError): pass
            sc = {"instances": [{"id": "fake-%d" % p, "pid": p, "port": 1, "mode": "client"} for p in pids] + [dict(e) for e in srv.extra_instances]}
            return self.reply_json(res({"content": [{"type": "text", "text": "instances"}], "structuredContent": sc}))
        if name == "map_load" and srv.map_hold is not None:  # FP7137 tests: a map load that takes srv.map_hold[map] s
            t0 = time.time()
            with srv.lock:
                srv.order.append(("map_load", args.get("map")))
                srv.map_active += 1; srv.map_max = max(srv.map_max, srv.map_active); srv.map_log.append([args.get("map"), t0, None])
                entry = srv.map_log[-1]
            time.sleep(srv.map_hold.get(args.get("map"), srv.map_hold.get("*", 0.5)))
            with srv.lock:
                srv.map_active -= 1; entry[2] = time.time()
            sc = {"map": args.get("map"), "duration_s": time.time() - t0}
            return self.reply_json(res({"content": [{"type": "text", "text": json.dumps(sc)}], "structuredContent": sc, "isError": False}))
        if name == "console_exec" and srv.exec_answer:  # Issue #10: instances_launch {commands}
            with srv.lock:
                srv.exec_calls.append(args); srv.order.append(("console_exec", args.get("cmd")))
            sc = srv.exec_answer(args)
            return self.reply_json(res({"content": [{"type": "text", "text": json.dumps(sc)}], "structuredContent": sc,
                                        "isError": "error" in sc}))
        if name == "lua_mount" and srv.lua_mount_answer:
            with srv.lock: srv.lua_mount_calls.append(args)
            sc = srv.lua_mount_answer(args)
            return self.reply_json(res({"content": [{"type": "text", "text": json.dumps(sc)}], "structuredContent": sc,
                                        "isError": "error" in sc}))
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
        self.instructions = "fake instructions"
        self.pid_file, self.lua_mount_answer, self.lua_mount_calls = None, None, []
        self.changing, self.terminate_hook, self.instances_answer = 0, None, None  # Issue #6
        self.map_load_hook = None
        self.exec_answer, self.exec_calls, self.order = None, [], []  # Issue #10: console_exec answers and the order of console_exec / map_load calls
        self.pid_dir, self.map_hold, self.map_log, self.map_active, self.map_max = None, None, [], 0, 0  # FP7137 tests
        self.extra_instances = []  # FP7137 tests (issue #7): games the bridge never started, as instances_list shows them (with "loading")
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
    # Issue #3: the same JSON error shape as the module (what Claude Code shows the model on isError is the text only).
    texts = [c.get("text", "") for c in res.get("content", []) if c.get("type") == "text"]
    err = (res.get("structuredContent") or {}).get("error", {})
    check("A (issue 3) game_not_running: the only text block is the JSON of structuredContent", len(texts) == 1 and json_equal(texts[0], res.get("structuredContent")), texts)
    check("A (issue 3) game_not_running error carries code, message with what to do, and details naming the port and why",
          err.get("code") == "game_not_running" and "-mcp" in err.get("message", "") and
          err.get("details", {}).get("port") and err.get("details", {}).get("reason"), err)
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


class Foreign(socketserver.ThreadingMixIn, http.server.HTTPServer):
    """A program that is not gmod-mcp holding the port: answers every POST with a fixed status and body."""
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, port, status=404, body=b"<html>not found</html>", ctype="text/html", headers=None):
        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def log_message(self, *a): pass
            def do_POST(self):
                self.rfile.read(int(self.headers.get("Content-Length") or 0))
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                for k, v in (headers or {}).items(): self.send_header(k, v)
                self.end_headers()
                self.wfile.write(body)
        super().__init__(("127.0.0.1", port), H)
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def stop(self):
        self.shutdown(); self.server_close()


def two_free_ports():
    while True:
        p = free_port()
        s = socket.socket()
        try: s.bind(("127.0.0.1", p + 1))
        except OSError: continue
        finally: s.close()
        return p


def case_portconflict():
    # Issue #5: a foreign program on the base port was reported as "the game is not running" / "rejected the token".
    print("== PORT_CONFLICT: a program that is not gmod-mcp holds the port", flush=True)
    for label, kw in (("404 html", dict(status=404)),
                      ("403 empty", dict(status=403, body=b"", ctype="text/plain")),
                      ("401 of another program", dict(status=401, body=b'{"error":"unauthorized"}', ctype="application/json"))):
        port, env = free_port(), isolated_config(with_token=True)
        foreign = Foreign(port, **kw)
        b = Bridge(port, env)
        r = b.req("initialize", INIT)
        ins = (r or {}).get("result", {}).get("instructions", "")
        check("PORT_CONFLICT [%s] instructions do not say the game is not running" % label, ins.startswith("port_conflict") and "not running right now" not in ins and "GMOD_MCP_PORT" in ins, ins[:300])
        r = b.req("tools/call", {"name": "status", "arguments": {}})
        res = (r or {}).get("result", {})
        err = res.get("structuredContent", {}).get("error", {})
        msg = err.get("message", "")
        check("PORT_CONFLICT [%s] tool error port_conflict (not game_not_running, not the token)" % label,
              res.get("isError") and err.get("code") == "port_conflict" and "rejected the token" not in json.dumps(res), r)
        check("PORT_CONFLICT [%s] the message names the port, the fix and how to find the program" % label,
              str(port) in msg and "GMOD_MCP_PORT" in msg and "Get-NetTCPConnection" in msg and "ss -ltnp" in msg and "another program" in msg, msg)
        r = mreq(b, "tools/call", {"name": "status", "arguments": {}})
        check("PORT_CONFLICT [%s] same error for a 2026-07-28 client" % label, "port_conflict" in json.dumps(r), r)
        r = b.req("tools/call", {"name": "instances_list", "arguments": {}})
        check("PORT_CONFLICT [%s] instances_list note names the conflict" % label, "port_conflict" in json.dumps(r) or "another program" in json.dumps(r), r)
        b.close(); foreign.stop()

    # A foreign 401 and the module's own 401 (0.3.6 body, no signature header) stay apart.
    port, env = free_port(), isolated_config(with_token=True)
    old = Foreign(port, status=401, ctype="application/json", body=json.dumps({"error": {"code": "unauthorized", "message": "x"}}).encode())
    b = Bridge(port, env); b.req("initialize", INIT)
    r = b.req("tools/call", {"name": "status", "arguments": {}})
    check("PORT_CONFLICT a gmod-mcp 0.3.6 401 (no signature header) is still 'rejected the token'", "rejected the token" in json.dumps(r) and "port_conflict" not in json.dumps(r), r)
    b.close(); old.stop()

    # A real gmod-mcp on the next port: the message names it.
    port, env = two_free_ports(), isolated_config(with_token=True)
    foreign = Foreign(port); game = FakeServer(port + 1, version=bridge_version())
    b = Bridge(port, env); b.req("initialize", INIT)
    msg = ""
    for _ in range(12):  # the next ports are probed in the background (a refused loopback connection is slow on Windows)
        r = b.req("tools/call", {"name": "status", "arguments": {}})
        msg = (r or {}).get("result", {}).get("structuredContent", {}).get("error", {}).get("message", "")
        if "GMOD_MCP_PORT=%d" % (port + 1) in msg: break
        time.sleep(1)
    check("PORT_CONFLICT the message names the gmod-mcp instance found on the next port", "port_conflict" in json.dumps(r) and "GMOD_MCP_PORT=%d" % (port + 1) in msg, msg)
    # The foreign program leaves, the game takes the port: back to normal with no restart.
    foreign.stop(); game.stop(); game = FakeServer(port, version=bridge_version())
    time.sleep(2.5)
    r = b.req("tools/call", {"name": "echo", "arguments": {}})
    check("PORT_CONFLICT the port is freed and taken by the game: calls work again", "port_conflict" not in json.dumps(r) and "error" not in (r or {}).get("result", {}).get("structuredContent", {"error": 1}) and not (r or {}).get("result", {}).get("isError"), r)
    b.close(); game.stop()


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


def case_win64_layout():
    """Issue #4: the current x86-64 install has bin/win64/gmod.exe and no gmod_win64.exe (its root gmod.exe is 32-bit).
    instances_launch must find the game there, start it with the game folder as working directory, and say which two
    paths it looked for when there is none."""
    print("== WIN64_LAYOUT: instances_launch finds bin/win64/gmod.exe (Issue #4)", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("WIN64_LAYOUT stand-in game built next to the bridge", False, fake)
        return
    import shutil
    work = tempfile.mkdtemp(prefix="gmcp-layout-")

    def pe32(path):  # just a PE header saying "32-bit x86": the game's root gmod.exe
        b = bytearray(0x100)
        b[0:2] = b"MZ"
        b[0x3C] = 0x80
        b[0x80:0x84] = b"PE\0\0"
        b[0x84], b[0x85] = 0x4C, 0x01
        with open(path, "wb") as f: f.write(bytes(b))

    game = os.path.join(work, "cur game")
    os.makedirs(os.path.join(game, "bin", "win64"))
    os.makedirs(os.path.join(game, "garrysmod"))
    pe32(os.path.join(game, "gmod.exe"))
    shutil.copy(fake, os.path.join(game, "bin", "win64", "gmod.exe"))
    only32 = os.path.join(work, "only32")
    os.makedirs(os.path.join(only32, "garrysmod"))
    pe32(os.path.join(only32, "gmod.exe"))
    argv_file, cwd_file = os.path.join(work, "argv.txt"), os.path.join(work, "cwd.txt")
    port, env = free_port(), isolated_config(with_token=True)
    env["GMCP_FAKE_ARGV"] = argv_file
    env["GMCP_FAKE_CWD"] = cwd_file
    b = Bridge(port, env)

    def launch(game_dir, timeout=40):
        r = b.req("tools/call", {"name": "instances_launch", "arguments": {"count": 1, "owner": "lay", "timeout_s": 10, "game_dir": game_dir}}, timeout)
        return (r or {}).get("result", {})

    res = launch(only32)
    sc = res.get("structuredContent", {})
    msg = json.dumps(sc)
    check("WIN64_LAYOUT a folder with only the 32-bit gmod.exe is refused, naming both paths",
          res.get("isError") and "game_not_found" in msg and "gmod_win64.exe" in msg and "win64" in msg and "gmod.exe" in msg and not os.path.exists(argv_file), sc)
    res = launch(game)
    sc = res.get("structuredContent", {})
    pids = sc.get("silent_pids", [])
    check("WIN64_LAYOUT bin/win64/gmod.exe is found and started (no gmod_win64.exe at the root)", len(pids) == 1 and all(alive(p) for p in pids) and os.path.exists(argv_file), sc)
    line = open(argv_file, encoding="utf-8", errors="replace").read() if os.path.exists(argv_file) else ""
    exe_in_line = line.lower().replace("/", "\\").lstrip().startswith('"' + os.path.join(game, "bin", "win64", "gmod.exe").lower().replace("/", "\\") + '"')
    check("WIN64_LAYOUT the started file is bin/win64/gmod.exe of the game folder", exe_in_line, line)
    cwd = open(cwd_file, encoding="utf-8", errors="replace").read().strip() if os.path.exists(cwd_file) else ""
    check("WIN64_LAYOUT the game starts with the game folder as working directory", cwd != "" and os.path.normcase(os.path.realpath(cwd)) == os.path.normcase(os.path.realpath(game)), cwd)
    b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "lay"}}, 20)
    time.sleep(1)
    check("WIN64_LAYOUT stand-in closed", not any(alive(p) for p in pids), pids)
    # game_dir may also be the garrysmod folder inside the game folder (what the module reports).
    for f in (argv_file, cwd_file):
        if os.path.exists(f): os.remove(f)
    res = launch(os.path.join(game, "garrysmod"))
    sc = res.get("structuredContent", {})
    pids = sc.get("silent_pids", [])
    check("WIN64_LAYOUT game_dir = <game>/garrysmod works with the new layout too", len(pids) == 1 and os.path.exists(argv_file), sc)
    b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "lay"}}, 20)
    b.close()
    shutil.rmtree(work, ignore_errors=True)


def case_lua_mount():
    """instances_launch {lua_mount} (B1): the client starts with -mcp_lua_mount "<folder>", a path with spaces stays one
    argument; a folder without lua/, a relative path or a quote is refused before anything starts."""
    print("== LUA_MOUNT: instances_launch passes the mounted folder to the client", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("LUA_MOUNT stand-in game built next to the bridge", False, fake)
        return
    import shutil
    game = tempfile.mkdtemp(prefix="gmcp-fake-game-")
    shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
    work = tempfile.mkdtemp(prefix="gmcp-mount-")
    wt = os.path.join(work, "my addon wt")
    os.makedirs(os.path.join(wt, "lua", "autorun"))
    os.makedirs(os.path.join(work, "nolua"))
    wt_u = os.path.join(work, "ov A \u00e9 \u0436\u4e2d")  # accent, Cyrillic and CJK: not in any single ANSI code page
    os.makedirs(os.path.join(wt_u, "lua"))
    argv_file = os.path.join(work, "argv.txt")
    port, env = free_port(), isolated_config(with_token=True)
    env["GMCP_FAKE_ARGV"] = argv_file
    b = Bridge(port, env)

    def launch(mount_dir, timeout=40):
        r = b.req("tools/call", {"name": "instances_launch", "arguments": {"count": 1, "owner": "ov", "timeout_s": 10, "game_dir": game, "lua_mount": mount_dir}}, timeout)
        return (r or {}).get("result", {})

    for bad, word in ((os.path.join(work, "nolua"), "lua"), ("relative/dir", "absolute"), (wt + '"', "character"), (os.path.join(work, "missing"), "not found")):
        res = launch(bad)
        sc = res.get("structuredContent", {})
        check("LUA_MOUNT refused before starting anything: " + word, res.get("isError") and word in json.dumps(sc) and not sc.get("silent_pids") and not os.path.exists(argv_file), sc)
    res = launch(os.path.join(wt, "lua"))
    sc = res.get("structuredContent", {})
    pids = sc.get("silent_pids", [])
    line = open(argv_file, encoding="utf-8", errors="replace").read() if os.path.exists(argv_file) else ""
    check("LUA_MOUNT the client got -mcp_lua_mount with the addon root, quoted (spaces kept)", ('-mcp_lua_mount "%s"' % wt) in line, line)
    check("LUA_MOUNT the timeout answer names the mounted folder", sc.get("lua_mount") == wt, sc)
    check("LAUNCH_VIDEO the client got -dxlevel 95 (the engine then neither reads nor writes the user's video settings)", " -dxlevel 95" in line and " -w 640 -h 360" in line, line)
    check("TEST_CLIENT the client got -mcp_test_client (the module then restores the archived convars before a clean quit)", " -mcp_test_client " in line, line)
    b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "ov"}}, 20)
    time.sleep(1)
    check("LUA_MOUNT stand-in closed", not any(alive(p) for p in pids), pids)
    # A non-ASCII folder: the game's file system lists it but loads none of its files (seen live 2026-11-14), so it is
    # refused before anything starts, with the way out (an ASCII junction).
    os.remove(argv_file)
    res = launch(wt_u)
    sc = res.get("structuredContent", {})
    pids = sc.get("silent_pids", [])
    check("LUA_MOUNT non-ASCII folder refused before launching, with the way out",
          res.get("isError") and "ASCII" in json.dumps(sc) and "mklink" in json.dumps(sc) and not os.path.exists(argv_file), sc)
    b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "ov"}}, 20)
    b.close()
    shutil.rmtree(game, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)


def case_lua_mount_verify():
    """instances_launch {lua_mount} (B2): the launch result says what each client's engine really has, from the game's
    own lua_mount answer. A mount that is not active (folder missing from the answer, error, base Lua missing) is a
    top-level warning naming the instances and isError, never a silent success."""
    print("== LUA_MOUNT_VERIFY: the launch reports the mount the engine has, not the one requested", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("LUA_MOUNT_VERIFY stand-in game built next to the bridge", False, fake)
        return
    import shutil
    game = tempfile.mkdtemp(prefix="gmcp-fake-game-")
    shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
    work = tempfile.mkdtemp(prefix="gmcp-mount-")
    wt = os.path.join(work, "ov A wt")
    os.makedirs(os.path.join(wt, "lua"))
    pid_file = os.path.join(work, "pid.txt")
    port, env = free_port(), isolated_config(with_token=True)
    env["GMCP_FAKE_PID"] = pid_file
    srv = FakeServer(port, version=bridge_version())
    srv.pid_file = pid_file
    b = Bridge(port, env)
    answers = {}
    srv.lua_mount_answer = lambda a: answers["now"](a)

    def launch(mount):
        if os.path.exists(pid_file): os.remove(pid_file)
        args = {"count": 1, "owner": "ovv", "timeout_s": 20, "game_dir": game}
        if mount: args["lua_mount"] = mount
        r = b.req("tools/call", {"name": "instances_launch", "arguments": args}, 60)
        res = (r or {}).get("result", {})
        b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "ovv"}}, 20)
        return res, res.get("structuredContent", {})

    # 1. the engine has the folder: active, no warning.
    answers["now"] = lambda a: {"lua_mount": wt.replace("\\", "/"), "base_lua_mounted": True}
    res, sc = launch(wt)
    inst = (sc.get("instances") or [{}])[0]
    check("LUA_MOUNT_VERIFY the launch asked the game with lua_mount and no path", srv.lua_mount_calls and "path" not in srv.lua_mount_calls[-1] and srv.lua_mount_calls[-1].get("instance") == inst.get("instance"), srv.lua_mount_calls)
    m = inst.get("lua_mount") or {}
    check("LUA_MOUNT_VERIFY active only because the game says so (requested, active, base_lua_mounted)",
          not res.get("isError") and m.get("active") is True and m.get("requested") == wt and m.get("base_lua_mounted") is True and "warning" not in sc, sc)
    # 2. the game answers without the folder (module ignored it): clear warning, isError.
    answers["now"] = lambda a: {"lua_mount": None, "base_lua_mounted": True}
    res, sc = launch(wt)
    inst = (sc.get("instances") or [{}])[0]
    m = inst.get("lua_mount") or {}
    check("LUA_MOUNT_VERIFY the game has no mount: not active, isError, warning names the instance",
          res.get("isError") and m.get("active") is False and m.get("requested") == wt and inst.get("instance") in sc.get("warning", "") and "NOT" in sc.get("warning", ""), sc)
    # Issue #3: the fix named in the warning is also data.
    check("LUA_MOUNT_VERIFY (issue 3) not active: next_call is lua_mount {path, instance} for the client that lacks it",
          sc.get("next_call") == {"tool": "lua_mount", "arguments": {"path": wt, "instance": inst.get("instance")}}
          and json_equal(res["content"][0]["text"], sc), sc.get("next_call"))
    # 3. another folder mounted: not active.
    answers["now"] = lambda a: {"lua_mount": "D:\\other", "base_lua_mounted": True}
    res, sc = launch(wt)
    m = ((sc.get("instances") or [{}])[0]).get("lua_mount") or {}
    check("LUA_MOUNT_VERIFY a different folder mounted: not active", res.get("isError") and m.get("active") is False, sc)
    # 4. the game errors.
    answers["now"] = lambda a: {"error": {"code": "unsupported_platform", "message": "nope"}}
    res, sc = launch(wt)
    m = ((sc.get("instances") or [{}])[0]).get("lua_mount") or {}
    check("LUA_MOUNT_VERIFY the game answers an error: not active, the error is reported",
          res.get("isError") and m.get("active") is False and "nope" in json.dumps(m.get("error")), sc)
    # 5. folder mounted but the base Lua is missing: unusable client, said clearly.
    answers["now"] = lambda a: {"lua_mount": wt, "base_lua_mounted": False}
    res, sc = launch(wt)
    m = ((sc.get("instances") or [{}])[0]).get("lua_mount") or {}
    check("LUA_MOUNT_VERIFY base Lua missing: isError and warning", res.get("isError") and m.get("base_lua_mounted") is False and "base" in sc.get("warning", "").lower(), sc)
    check("LUA_MOUNT_VERIFY (issue 3) base Lua missing: next_call removes the mount (path null) from that client",
          sc.get("next_call") == {"tool": "lua_mount", "arguments": {"path": None, "instance": ((sc.get("instances") or [{}])[0]).get("instance")}}, sc.get("next_call"))
    # 6. no mount requested: the game is not asked.
    n = len(srv.lua_mount_calls)
    answers["now"] = lambda a: {"lua_mount": None, "base_lua_mounted": True}
    res, sc = launch(None)
    check("LUA_MOUNT_VERIFY no lua_mount requested: no check, no warning", not res.get("isError") and len(srv.lua_mount_calls) == n and "warning" not in sc, sc)
    b.close(); srv.stop()
    shutil.rmtree(game, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)


def case_launch_options():
    """Issue #10: instances_launch {fps_max, fps_max_nofocus, commands, launch_args}. The stand-in game records its command
    line, the fake game server records the console_exec and map_load calls. Bad options are refused before anything starts."""
    print("== LAUNCH_OPTIONS: instances_launch passes fps_max, extra options and startup commands (Issue #10)", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("LAUNCH_OPTIONS stand-in game built next to the bridge", False, fake)
        return
    import shutil
    game = tempfile.mkdtemp(prefix="gmcp-fake-game-")
    shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
    work = tempfile.mkdtemp(prefix="gmcp-opts-")
    pid_file, argv_file = os.path.join(work, "pid.txt"), os.path.join(work, "argv.txt")
    port, env = free_port(), isolated_config(with_token=True)
    env.update(GMCP_FAKE_PID=pid_file, GMCP_FAKE_ARGV=argv_file, GMOD_MCP_FP7137_WORKAROUND="0")  # no 15 s settle delay before the map
    srv = FakeServer(port, version=bridge_version())
    srv.pid_file = pid_file
    srv.map_hold = {"*": 0.1}
    srv.exec_answer = lambda a: {"cmd": a.get("cmd"), "realm": "menu", "via": "menu"}
    b = Bridge(port, env)

    def launch(extra, timeout=60):
        for f in (pid_file, argv_file):
            if os.path.exists(f): os.remove(f)
        args = {"count": 1, "owner": "opts", "timeout_s": 20, "game_dir": game}
        args.update(extra)
        r = b.req("tools/call", {"name": "instances_launch", "arguments": args}, timeout)
        res = (r or {}).get("result", {})
        line = open(argv_file, encoding="utf-8", errors="replace").read() if os.path.exists(argv_file) else ""
        b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "opts"}}, 20)
        return res, res.get("structuredContent", {}), line

    # 1. no option: the preset's frame cap, nothing else added, the game is not asked for anything.
    res, sc, line = launch({})
    check("LAUNCH_OPTIONS default: +fps_max 20 +fps_max_nofocus 20, no restore list, no command", " +fps_max 20 +fps_max_nofocus 20" in line and "-mcp_restore_convars" not in line and not srv.exec_calls and sc.get("fps_max") == 20, (line, sc))
    # 2. fps_max alone: the unfocused cap follows it; the result says what the clients run at.
    res, sc, line = launch({"preset": "normal", "fps_max": 120})
    check("LAUNCH_OPTIONS fps_max 120 overrides the preset, fps_max_nofocus follows", " +fps_max 120 +fps_max_nofocus 120" in line and "+fps_max 30" not in line and sc.get("fps_max") == 120 and sc.get("fps_max_nofocus") == 120, (line, sc))
    check("LAUNCH_OPTIONS the fps_note names the value", "120" in sc.get("fps_note", ""), sc.get("fps_note"))
    # 3. both, 0 = no cap.
    res, sc, line = launch({"fps_max": 300, "fps_max_nofocus": 0})
    check("LAUNCH_OPTIONS fps_max 300 and fps_max_nofocus 0 (no cap when unfocused)", " +fps_max 300 +fps_max_nofocus 0" in line and sc.get("fps_max") == 300 and sc.get("fps_max_nofocus") == 0, (line, sc))
    res, sc, line = launch({"fps_max": 0})
    check("LAUNCH_OPTIONS fps_max 0 = no cap, reported as such", " +fps_max 0 +fps_max_nofocus 0" in line and sc.get("fps_max") == 0 and "no cap" in sc.get("fps_note", "").lower(), (line, sc))
    # 4. launch_args and commands: argv, restore list, commands in order, before the map.
    srv.exec_calls.clear(); srv.order.clear()
    res, sc, line = launch({"launch_args": ["-noborder", "+cl_showfps", "1"], "commands": ["fps_max 240", "toggle snd_mute_losefocus; echo hi"], "map": "gm_construct"}, 90)
    check("LAUNCH_OPTIONS launch_args come after the fixed options, one token each", " -dxlevel 95 -noborder +cl_showfps 1" in line.replace(" -nosound", ""), line)
    check("LAUNCH_OPTIONS the convars the options may change are listed for the module (-mcp_restore_convars)", " -mcp_restore_convars fps_max,snd_mute_losefocus,echo,cl_showfps" in line + " ", line)
    inst = (sc.get("instances") or [{}])[0]
    check("LAUNCH_OPTIONS the commands ran on the client, one statement each, in order", [c.get("cmd") for c in srv.exec_calls] == ["fps_max 240", "toggle snd_mute_losefocus", "echo hi"] and all(c.get("instance") == inst.get("instance") for c in srv.exec_calls), srv.exec_calls)
    check("LAUNCH_OPTIONS the commands ran before the map load", [o[0] for o in srv.order] == ["console_exec"] * 3 + ["map_load"], srv.order)
    check("LAUNCH_OPTIONS the result lists the commands and the arguments", [c.get("cmd") for c in inst.get("commands", [])] == ["fps_max 240", "toggle snd_mute_losefocus", "echo hi"] and all(c.get("ok") for c in inst.get("commands", [])) and sc.get("launch_args") == ["-noborder", "+cl_showfps", "1"] and not res.get("isError"), sc)
    # 5. a command the game rejects: reported on the instance, the others still run, isError.
    srv.exec_calls.clear()
    srv.exec_answer = lambda a: {"error": {"code": "not_found", "message": "unknown"}} if a.get("cmd") == "nope 1" else {"cmd": a.get("cmd"), "realm": "menu"}
    res, sc, line = launch({"commands": ["nope 1", "echo after"]})
    inst = (sc.get("instances") or [{}])[0]
    cmds = inst.get("commands", [])
    check("LAUNCH_OPTIONS a failed command is reported, the next one still runs, isError", res.get("isError") and len(cmds) == 2 and cmds[0].get("ok") is False and "unknown" in json.dumps(cmds[0]) and cmds[1].get("ok") is True and len(srv.exec_calls) == 2, sc)
    srv.exec_answer = lambda a: {"cmd": a.get("cmd"), "realm": "menu"}
    # 6. refused before anything starts: invalid_params naming the option.
    bad = [({"fps_max": 5}, "fps_max"), ({"fps_max_nofocus": 5000}, "fps_max_nofocus"), ({"fps_max": "fast"}, "fps_max"),
           ({"commands": ["quit"]}, 'quit'), ({"commands": ["echo ok", "connect 8.8.8.8"]}, 'connect'), ({"commands": ["map gm_flatgrass"]}, "map option"),
           ({"launch_args": ["-multirun"]}, "-multirun"), ({"launch_args": ["-mcpport", "1"]}, "-mcpport"), ({"launch_args": ["-w", "1920"]}, '-w'),
           ({"launch_args": ["-dxlevel", "81"]}, "-dxlevel"), ({"launch_args": ["+connect", "1.2.3.4"]}, "+connect"), ({"launch_args": ["+map", "gm_x"]}, "map option"),
           ({"launch_args": ["-mcp_test_client"]}, "-mcp_test_client"), ({"launch_args": ["-windowed"]}, "-windowed"), ({"launch_args": ["-a b"]}, "launch_args"),
           ({"launch_args": ["-condebug"]}, "-condebug")]
    for extra, word in bad:
        res, sc, line = launch(extra)
        err = sc.get("error", {})
        check("LAUNCH_OPTIONS refused before starting anything: %s" % json.dumps(extra), res.get("isError") and (err.get("code") == "invalid_params" or "invalid" in json.dumps(res).lower()) and word in json.dumps(sc) and not line, sc)
    check("LAUNCH_OPTIONS the refused launches never reached the game", not srv.exec_calls[2:], srv.exec_calls)
    # 7. the tool description says where the options are listed.
    r = b.req("tools/list", {})
    tool = next((t for t in (r or {}).get("result", {}).get("tools", []) if t["name"] == "instances_launch"), {})
    d = tool.get("description", "")
    props = tool.get("inputSchema", {}).get("properties", {})
    check("LAUNCH_OPTIONS the schema offers fps_max, fps_max_nofocus, commands, launch_args", all(k in props for k in ("fps_max", "fps_max_nofocus", "commands", "launch_args")), list(props))
    check("LAUNCH_OPTIONS the description points at the option and command lists", "developer.valvesoftware.com/wiki/Command_line_options" in d and "concommand_list" in d and "api_search" in d, d[-600:])
    b.close(); srv.stop()
    shutil.rmtree(game, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)


def case_crash():
    """Crash report (B6): a client this session launched that crashed is reported as client_crashed (dump name, exit code),
    shown by instances_list as crashed, and told apart from a clean exit. The stand-in game ends on a trigger file with a
    chosen exit code and writes a fake .mdmp into <game>/crashes first; the fake game server plays the module."""
    print("== CRASH: client_crashed, dump match, clean exit is not a crash", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("CRASH stand-in game built next to the bridge", False, fake)
        return
    import shutil

    def run(tag, exit_code, dump, old_dump=False, map_name=None, cut_first=False, inflight=False):
        game = tempfile.mkdtemp(prefix="gmcp-crash-game-")
        shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
        os.makedirs(os.path.join(game, "garrysmod")); crashes = os.path.join(game, "crashes"); os.makedirs(crashes)
        if old_dump:  # another game's dump, from before this one even started
            old = os.path.join(crashes, "gmod_win64_20200101_000000_1_crash.mdmp")
            open(old, "wb").write(b"MDMP"); os.utime(old, (time.time() - 3600, time.time() - 3600))
        work = tempfile.mkdtemp(prefix="gmcp-crash-"); pid_file, trigger = os.path.join(work, "pid.txt"), os.path.join(work, "go")
        port, env = free_port(), isolated_config(with_token=True)
        env.update(GMCP_FAKE_PID=pid_file, GMCP_FAKE_TRIGGER=trigger, GMCP_FAKE_CRASH_DIR=crashes,
                   GMCP_FAKE_EXIT=str(exit_code), GMCP_FAKE_DUMP="1" if dump else "0")
        if cut_first: env["GMCP_FAKE_EXIT_DELAY_MS"] = "800"  # Issue #8: the connection breaks, the process object is signalled later
        srv = FakeServer(port, version=bridge_version()); srv.pid_file = pid_file
        b = Bridge(port, env)
        out = {"game": game}
        def pid_now():
            try: return int(open(pid_file).read().strip() or 0)
            except (OSError, ValueError): return 0
        def go(): open(trigger, "w").close()
        args = {"count": 1, "owner": tag, "timeout_s": 30, "game_dir": game}
        if map_name:
            started = threading.Event()
            def hook(a):
                started.set(); go()
                if cut_first: return  # Issue #8: the answer is cut at once, the process is still there for a moment
                for _ in range(100):
                    if not alive(pid_now()): break
                    time.sleep(0.1)
            srv.map_load_hook = hook
            args["map"] = map_name
            r = b.req("tools/call", {"name": "instances_launch", "arguments": args}, 90)
            out["launch"] = (r or {}).get("result", {}).get("structuredContent", {})
            out["pid"] = pid_now()
        else:
            r = b.req("tools/call", {"name": "instances_launch", "arguments": args}, 60)
            sc = (r or {}).get("result", {}).get("structuredContent", {})
            inst = (sc.get("instances") or [{}])[0]
            out["launch"], out["pid"], out["id"] = sc, pid_now(), inst.get("instance")
            if inflight:
                # Issue #8: a call waits on the client (the fake game server holds it open, like a game that does not answer) when
                # the client dies: the answer must come at once, not when the call times out.
                box = {}
                def call(): box["r"] = b.req("tools/call", {"name": "silent_hang", "arguments": {"instance": out["id"]}}, 60)
                th = threading.Thread(target=call); th.start()
                time.sleep(1.0)
                t0 = time.time(); go()
                th.join(45)
                out["inflight"] = (box.get("r") or {}).get("result", {}).get("structuredContent", {})
                out["inflight_s"] = time.time() - t0
            else:
                go()
            for _ in range(100):
                if not alive(out["pid"]): break
                time.sleep(0.1)
        srv.stop()  # the module is gone with the game: nothing answers any more
        time.sleep(0.3)
        r = b.req("tools/call", {"name": "instances_list", "arguments": {}}, 20)
        out["list"] = (r or {}).get("result", {}).get("structuredContent", {})
        r = b.req("tools/call", {"name": "status", "arguments": {"instance": out.get("id") or "x"}}, 20)
        out["status"] = (r or {}).get("result", {}).get("structuredContent", {})
        b.req("tools/call", {"name": "instances_close", "arguments": {"owner": tag}}, 20)
        b.close()
        shutil.rmtree(game, ignore_errors=True); shutil.rmtree(work, ignore_errors=True)
        return out

    DUMP = "gmod_win64_20261114_120000_1_accessviolation.mdmp"
    # 1. an access violation with a dump written just before.
    o = run("c1", 0xC0000005, True)
    e = o["status"].get("error", {})
    check("CRASH access violation with a dump: client_crashed", e.get("code") == "client_crashed", o["status"])
    check("CRASH the answer names the dump file, the exit code, the pid and says to relaunch",
          DUMP in e.get("message", "") and "0xC0000005" in e.get("message", "") and str(o["pid"]) in e.get("message", "") and "instances_launch" in e.get("message", ""), e)
    check("CRASH details: instance, pid, dump, exit_code, evidence", e.get("details", {}).get("dump") == DUMP and e["details"].get("exit_code") == 0xC0000005
          and e["details"].get("pid") == o["pid"] and e["details"].get("instance") == o["id"] and e["details"].get("evidence"), e)
    crashed = [i for i in o["list"].get("instances", []) if i.get("state") == "crashed"]
    check("CRASH instances_list shows it as crashed with the dump, the owner and the pid",
          len(crashed) == 1 and crashed[0].get("pid") == o["pid"] and crashed[0].get("crash", {}).get("dump") == DUMP and crashed[0].get("owner") == "c1"
          and crashed[0]["crash"].get("exit_code") == 0xC0000005, o["list"])
    check("CRASH it is no longer 'launched_not_answering'", not o["list"].get("launched_not_answering"), o["list"])
    # 2. a crash that wrote no dump, with an old dump of another game in the folder: the old one is not taken.
    o = run("c2", 0xC0000005, False, old_dump=True)
    e = o["status"].get("error", {})
    check("CRASH access violation without a dump (an hour-old dump in the folder): client_crashed, dump null",
          e.get("code") == "client_crashed" and e.get("details", {}).get("dump") is None and "No crash dump" in e.get("message", "") and "gmod_win64_2020" not in e.get("message", ""), e)
    # 3. abnormal exit without any dump.
    o = run("c3", 1, False)
    e = o["status"].get("error", {})
    check("CRASH exit code 1 without a dump: client_crashed, says it may have been killed from outside",
          e.get("code") == "client_crashed" and "killed" in e.get("message", ""), e)
    # 4. a clean exit.
    o = run("c4", 0, False)
    e = o["status"].get("error", {})
    check("CRASH exit code 0: instance_gone, not a crash", e.get("code") == "instance_gone" and "crash" not in e.get("message", "").lower(), e)
    check("CRASH a clean exit is not listed as crashed", not [i for i in o["list"].get("instances", []) if i.get("state") == "crashed"], o["list"])
    # 5. a map_load in flight when the client dies.
    o = run("c5", 0xC0000005, True, map_name="gm_construct")
    inst = (o["launch"].get("instances") or [{}])[0]
    me = inst.get("map_error", {})
    check("CRASH map_load in flight when the client crashed: map_error is client_crashed with the dump",
          me.get("code") == "client_crashed" and DUMP in me.get("message", "") and me.get("details", {}).get("dump") == DUMP, o["launch"])
    # 6. Issue #8: the map_load connection breaks first and the process object is signalled 0.8 s later (a real crash closes the
    # sockets before the process is signalled): map_error was game_not_running, with no crash entry.
    o = run("c6", 0xC0000005, True, map_name="gm_construct", cut_first=True)
    inst = (o["launch"].get("instances") or [{}])[0]
    me = inst.get("map_error", {})
    check("CRASH Issue #8: a client that dies during its map load (connection cut before the process ends): map_error is client_crashed with the dump, not game_not_running",
          me.get("code") == "client_crashed" and DUMP in me.get("message", "") and me.get("details", {}).get("dump") == DUMP, o["launch"])
    # 7. Issue #8: a call in flight on a client that dies is answered at once.
    o = run("c7", 0xC0000005, True, inflight=True)
    fl = o.get("inflight", {}).get("error", {})
    check("CRASH Issue #8: a call in flight when the client crashes answers client_crashed with the dump",
          fl.get("code") == "client_crashed" and DUMP in fl.get("message", "") and fl.get("details", {}).get("dump") == DUMP, o.get("inflight"))
    check("CRASH Issue #8: ... within a few seconds (the call itself would have waited 30 s)", o.get("inflight_s", 99) < 6, o.get("inflight_s"))
    # 8. the same with a clean exit: not a crash, still answered at once.
    o = run("c8", 0, False, inflight=True)
    fl = o.get("inflight", {}).get("error", {})
    check("CRASH Issue #8: a call in flight when the client quits cleanly answers instance_gone, not client_crashed",
          fl.get("code") == "instance_gone" and o.get("inflight_s", 99) < 6, o.get("inflight"))
def case_lowmem():
    """Issue #8: instances_launch refuses to start games when the free memory does not cover them (low_memory, with the figures),
    unless ignore_low_memory; a crash report carries the free memory. The free memory is given by GMOD_MCP_FAKE_FREE_COMMIT_MB /
    GMOD_MCP_FAKE_FREE_RAM_MB (the bridge measures the machine otherwise); stand-in game, fake game server, Windows."""
    print("== LOWMEM: low_memory before any game starts, ignore_low_memory, free memory in a crash report", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("LOWMEM stand-in game built next to the bridge", False, fake)
        return
    import shutil

    def session(commit, ram, args, crash=False, others=0):
        game = tempfile.mkdtemp(prefix="gmcp-mem-game-")
        shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
        os.makedirs(os.path.join(game, "garrysmod")); crashes = os.path.join(game, "crashes"); os.makedirs(crashes)
        work = tempfile.mkdtemp(prefix="gmcp-mem-"); pid_dir, trigger = os.path.join(work, "pids"), os.path.join(work, "go")
        os.makedirs(pid_dir)
        port, env = free_port(), isolated_config(with_token=True)
        env.update(GMCP_FAKE_PID_DIR=pid_dir, GMCP_FAKE_TRIGGER=trigger, GMCP_FAKE_CRASH_DIR=crashes, GMCP_FAKE_EXIT="3221225477",
                   GMOD_MCP_FAKE_FREE_COMMIT_MB=str(commit), GMOD_MCP_FAKE_FREE_RAM_MB=str(ram))
        srv = FakeServer(port, version=bridge_version()); srv.pid_dir = pid_dir
        srv.extra_instances = [{"id": "other-%d" % i, "pid": 4000000 + i, "port": 1, "mode": "client", "state": "running"} for i in range(others)]
        b = Bridge(port, env)
        a = {"count": 1, "owner": "mem", "timeout_s": 30, "game_dir": game}
        a.update(args)
        r = b.req("tools/call", {"name": "instances_launch", "arguments": a}, 90)
        out = {"launch": (r or {}).get("result", {}).get("structuredContent", {}), "is_error": (r or {}).get("result", {}).get("isError")}
        out["started"] = len(os.listdir(pid_dir))
        if crash:
            open(trigger, "w").close()
            for _ in range(100):
                if not any(alive(int(open(os.path.join(pid_dir, f)).read() or 0)) for f in os.listdir(pid_dir)): break
                time.sleep(0.1)
            inst = (out["launch"].get("instances") or [{}])[0]
            srv.stop(); time.sleep(0.3)
            r = b.req("tools/call", {"name": "status", "arguments": {"instance": inst.get("instance", "x")}}, 20)
            out["status"] = (r or {}).get("result", {}).get("structuredContent", {})
            r = b.req("tools/call", {"name": "instances_list", "arguments": {}}, 20)
            out["list"] = (r or {}).get("result", {}).get("structuredContent", {})
        else:
            srv.stop()
        b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "mem"}}, 20)
        b.close()
        shutil.rmtree(game, ignore_errors=True); shutil.rmtree(work, ignore_errors=True)
        return out

    # 1. The report of the issue: 11 clients run, 1.8 GB of commit is free, one more client is asked for.
    o = session(1780, 6000, {}, others=11)
    e = o["launch"].get("error", {})
    d = e.get("details", {})
    check("LOWMEM free commit 1780 MB, 11 clients running: low_memory, isError, nothing started", e.get("code") == "low_memory" and o["is_error"] is True and o["started"] == 0, o)
    check("LOWMEM details: free_commit_mb, free_ram_mb, running_clients, per_client_mb, count, needed_commit_mb",
          d.get("free_commit_mb") == 1780 and d.get("free_ram_mb") == 6000 and d.get("running_clients") == 11 and d.get("per_client_mb") == 1600
          and d.get("count") == 1 and d.get("needed_commit_mb", 0) > 1780, e)
    check("LOWMEM the message says how to go on (instances_close, ignore_low_memory)", "instances_close" in e.get("message", "") and "ignore_low_memory" in e.get("message", "") and "Nothing was started" in e.get("message", ""), e)
    # 2. ignore_low_memory starts anyway.
    o = session(1780, 6000, {"ignore_low_memory": True}, others=11)
    check("LOWMEM ignore_low_memory: the client starts", o["started"] == 1 and not o["launch"].get("error"), o)
    # 3. plenty of memory.
    o = session(100000, 60000, {})
    check("LOWMEM plenty of memory: the client starts", o["started"] == 1 and not o["launch"].get("error"), o)
    # 4. free RAM alone is too low.
    o = session(100000, 500, {})
    check("LOWMEM free RAM 500 MB: low_memory", o["launch"].get("error", {}).get("code") == "low_memory" and o["started"] == 0, o)
    # 5. the need grows with the count: 3 clients fit in 6000 MB of commit, 4 do not.
    o = session(6000, 60000, {"count": 3})
    check("LOWMEM 3 clients with 6000 MB of commit: they start", o["started"] == 3, o)
    o = session(6000, 60000, {"count": 4})
    check("LOWMEM 4 clients with 6000 MB of commit: low_memory, none started", o["launch"].get("error", {}).get("code") == "low_memory" and o["started"] == 0, o)
    # 6. a crash report names the free memory.
    o = session(1800, 900, {"ignore_low_memory": True}, crash=True)
    e = o.get("status", {}).get("error", {})
    check("LOWMEM client_crashed carries the free memory in details.memory and says memory is low",
          e.get("code") == "client_crashed" and e.get("details", {}).get("memory", {}).get("free_commit_mb") == 1800
          and e["details"]["memory"].get("free_ram_mb") == 900 and "Memory is low" in e.get("message", ""), e)
    crashed = [i for i in o.get("list", {}).get("instances", []) if i.get("state") == "crashed"]
    check("LOWMEM instances_list crash entry carries the memory too", len(crashed) == 1 and crashed[0].get("crash", {}).get("memory", {}).get("free_commit_mb") == 1800, o.get("list"))
    # 7. plenty of memory at the crash: the figures, no hint.
    o = session(60000, 30000, {}, crash=True)
    e = o.get("status", {}).get("error", {})
    check("LOWMEM a crash with plenty of memory: the figures without the low-memory sentence",
          e.get("code") == "client_crashed" and e.get("details", {}).get("memory", {}).get("free_commit_mb") == 60000 and "Memory is low" not in e.get("message", ""), e)


# --- FP7137: the launch / map_load lock shared by every bridge (docs/FP7137.md). Removed with the workaround. ---
def fp_setup(settle_ms=2500, workaround=None, hold=None):
    """A fake game server, a config folder shared by the bridges of the scenario, a folder where each stand-in game
    drops its pid file."""
    env = isolated_config(with_token=True)
    env["GMOD_MCP_FP7137_SETTLE_MS"] = str(settle_ms)
    if workaround is not None: env["GMOD_MCP_FP7137_WORKAROUND"] = workaround
    port = free_port()
    srv = FakeServer(port, version=bridge_version())
    srv.map_hold = hold or {"*": 0.5}
    srv.pid_dir = tempfile.mkdtemp(prefix="gmcp-fp-pids-")
    env["GMCP_FAKE_PID_DIR"] = srv.pid_dir
    return port, env, srv


def fp_state_file(env):
    return os.path.join(env["_TEST_CONFIG_DIR"], "gmod-mcp", "fp7137.state")


def fp_seed(env, entries, last_start=0):
    """entries: (kind, pid, seconds until expiry, tag). The bridge's own state file format (src/bridge/fp7137.h)."""
    lines = ["S %d" % last_start] + ["E %s %d 0 %d %s" % (k, pid, int(time.time() * 1000) + int(sec * 1000), tag) for k, pid, sec, tag in entries]
    with open(fp_state_file(env), "w") as f: f.write("\n".join(lines) + "\n")


def fp_call(b, tool, args, timeout, token=None):
    params = {"name": tool, "arguments": args}
    if token: params["_meta"] = {"progressToken": token}
    return b.start("tools/call", params)


def fp_result(b, rid, timeout):
    r = b.wait(rid, timeout)
    res = (r or {}).get("result", {})
    return res, res.get("structuredContent", {})


def fp_dead_pid():
    p = subprocess.Popen([sys.executable, "-c", "pass"]); p.wait()
    return p.pid


def fp_progress(b, token):
    with b.lock: return [m["params"].get("message", "") for m in b.notes if m.get("method") == "notifications/progress" and m["params"].get("progressToken") == token]


def case_fp7137():
    """FP7137 (B4): launch and map_load wait for each other through a state shared by every bridge of the user. Pieces
    that need no game run everywhere (seeded state file); the real launches use the stand-in game (Windows)."""
    print("== FP7137: launch / map_load lock shared by the bridges", flush=True)
    import shutil
    me = os.getpid()
    # 1. A launch in progress (seeded, owner alive): map_load waits, says so, goes ahead when it ends.
    port, env, srv = fp_setup()
    b = Bridge(port, env)
    fp_seed(env, [("L", me, 60, "launch")])
    t0 = time.time()
    rid = fp_call(b, "map_load", {"map": "gm_a", "timeout_s": 30}, 30, token="P1")
    time.sleep(1.5)
    with b.lock: early = json.dumps(rid) in b.by_id
    check("FP7137 map_load waits while a launch is in progress (nothing sent to the game)", not early and not srv.map_log, srv.map_log)
    check("FP7137 the wait is reported as progress notifications", any("waiting" in m and "launch" in m for m in fp_progress(b, "P1")), fp_progress(b, "P1"))
    os.remove(fp_state_file(env))
    res, sc = fp_result(b, rid, 10)
    check("FP7137 once the launch ends, map_load goes ahead and reports waited_s and the reason",
          not res.get("isError") and sc.get("map") == "gm_a" and sc.get("waited_s", 0) >= 1.0 and "launch" in sc.get("waited_reason", "")
          and json.loads(res["content"][0]["text"]).get("waited_s") == sc.get("waited_s"), res)
    check("FP7137 the game saw the load only after the wait", srv.map_log and srv.map_log[0][1] - t0 >= 1.3, srv.map_log)
    # 2. The wait would outlast the call's timeout: clean error, the game is never asked.
    fp_seed(env, [("L", me, 60, "launch")])
    t1 = time.time()
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_never", "timeout_s": 5}, 5), 30)
    check("FP7137 a wait longer than timeout_s fails with launch_in_progress and waited_s",
          res.get("isError") and sc.get("error", {}).get("code") == "launch_in_progress" and 4 <= sc.get("waited_s", 0) <= 9 and time.time() - t1 < 12, sc)
    check("FP7137 ... and the game was never asked", all(m[0] != "gm_never" for m in srv.map_log), srv.map_log)
    # Issue #3: the retry is data too: the same call a little later, with more room to wait (and the text stays the JSON).
    nc = sc.get("error", {}).get("next_call", {})
    check("FP7137 (issue 3) launch_in_progress carries next_call: the same map_load, after_s, a larger timeout_s",
          nc.get("tool") == "map_load" and nc.get("arguments", {}).get("map") == "gm_never" and nc.get("arguments", {}).get("timeout_s", 0) > 5
          and nc.get("after_s", 0) > 0 and json_equal(res["content"][0]["text"], sc), nc)
    # 3. The bridge that held the lock is gone (pid does not exist), expiry far away: no deadlock, no wait.
    fp_seed(env, [("L", fp_dead_pid(), 3600, "launch"), ("M", fp_dead_pid(), 3600, "map:ghost")])
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_b", "timeout_s": 30}, 30), 10)
    check("FP7137 an entry of a dead bridge is ignored: no wait, no waited_s", not res.get("isError") and sc.get("map") == "gm_b" and "waited_s" not in sc, sc)
    check("FP7137 ... and it is removed from the shared state", "ghost" not in open(fp_state_file(env)).read(), open(fp_state_file(env)).read())
    # 4. An entry of a live process past its expiry does not count.
    fp_seed(env, [("L", me, -5, "launch")])
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_c", "timeout_s": 30}, 30), 10)
    check("FP7137 an expired entry is ignored", not res.get("isError") and "waited_s" not in sc, sc)
    b.close(); srv.stop()
    # 5. Settle delay longer than timeout_s: refused at once instead of waiting in vain.
    port, env, srv = fp_setup(settle_ms=8000)
    b = Bridge(port, env)
    fp_seed(env, [], last_start=int(time.time() * 1000))
    t1 = time.time()
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_d", "timeout_s": 5}, 5), 20)
    check("FP7137 a game started 0 s ago and settle 8 s > timeout_s 5: refused at once", res.get("isError") and sc.get("error", {}).get("code") == "launch_in_progress" and time.time() - t1 < 3, sc)
    b.close(); srv.stop()
    # 6. Settle delay shorter than the timeout: waits for it, then loads.
    port, env, srv = fp_setup(settle_ms=2500)
    b = Bridge(port, env)
    fp_seed(env, [], last_start=int(time.time() * 1000))
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_e", "timeout_s": 30}, 30), 20)
    check("FP7137 map_load waits for the settle delay after the last start", not res.get("isError") and 1.5 <= sc.get("waited_s", 0) <= 4, sc)
    b.close(); srv.stop()
    # 7. Kill switch: GMOD_MCP_FP7137_WORKAROUND=0 -> no wait whatever the state says.
    port, env, srv = fp_setup(workaround="0")
    b = Bridge(port, env)
    fp_seed(env, [("L", me, 60, "launch")], last_start=int(time.time() * 1000))
    t1 = time.time()
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_f", "timeout_s": 30}, 30), 10)
    check("FP7137 kill switch off: no wait, no waited_s", not res.get("isError") and "waited_s" not in sc and time.time() - t1 < 2, sc)
    b.close(); srv.stop()

    if os.name != "nt":
        print("   (the launch scenarios are Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("FP7137 stand-in game built next to the bridge", False, fake)
        return
    game = tempfile.mkdtemp(prefix="gmcp-fake-game-")
    shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
    launch_args = lambda **kw: dict({"count": 1, "owner": "fp", "timeout_s": 40, "game_dir": game}, **kw)

    def pid_times(srv):
        return sorted(os.path.getmtime(os.path.join(srv.pid_dir, f)) for f in os.listdir(srv.pid_dir))

    # 8. Two bridges: B loads a map, A launches meanwhile -> the launch waits for the load; then B loads again right
    # after the launch -> that load waits for the settle delay.
    port, env, srv = fp_setup(hold={"*": 3.0})
    a, b = Bridge(port, env), Bridge(port, env)
    rid_b = fp_call(b, "map_load", {"map": "gm_g", "timeout_s": 60}, 60)
    end = time.time() + 10
    while not srv.map_log and time.time() < end: time.sleep(0.05)
    check("FP7137 the first load reached the game", bool(srv.map_log))
    rid_a = fp_call(a, "instances_launch", launch_args(), 60, token="L1")
    res_a, sc_a = fp_result(a, rid_a, 60)
    res_b, sc_b = fp_result(b, rid_b, 10)
    load_end = srv.map_log[0][2] or 0
    first_pid = (pid_times(srv) or [0])[0]
    check("FP7137 a launch waits for a map load running in another bridge", not res_a.get("isError") and first_pid >= load_end - 0.05 and sc_a.get("waited_s", 0) >= 1.5 and "map" in sc_a.get("waited_reason", ""),
          {"first_pid": first_pid, "load_end": load_end, "sc": sc_a})
    check("FP7137 the wait was reported as progress", any("waiting" in m for m in fp_progress(a, "L1")), fp_progress(a, "L1"))
    check("FP7137 the load itself was not delayed", not res_b.get("isError") and "waited_s" not in sc_b, sc_b)
    res_b, sc_b = fp_result(b, fp_call(b, "map_load", {"map": "gm_h", "timeout_s": 60}, 60), 30)
    last_pid = (pid_times(srv) or [0])[-1]
    start_h = [m for m in srv.map_log if m[0] == "gm_h"]
    check("FP7137 a map_load right after a launch waits for the settle delay (2.5 s here)", not res_b.get("isError") and start_h and start_h[0][1] - last_pid >= 2.2 and sc_b.get("waited_s", 0) > 0.3 and "started" in sc_b.get("waited_reason", ""),
          {"last_pid": last_pid, "h": start_h, "sc": sc_b})
    a.req("tools/call", {"name": "instances_close", "arguments": {"all": True}}, 20)
    a.close(); b.close(); srv.stop()
    # 9. A bridge dies while its map load runs: the other bridge's launch is not blocked.
    port, env, srv = fp_setup(hold={"*": 30.0})
    a, b = Bridge(port, env), Bridge(port, env)
    fp_call(a, "map_load", {"map": "gm_i", "timeout_s": 60}, 60)
    end = time.time() + 10
    while not srv.map_log and time.time() < end: time.sleep(0.05)
    a.p.kill()  # no clean exit: nothing releases its slot
    t1 = time.time()
    res_b, sc_b = fp_result(b, fp_call(b, "instances_launch", launch_args(timeout_s=20), 30), 30)
    check("FP7137 a dead bridge's map load never blocks a launch", not res_b.get("isError") and time.time() - t1 < 15 and "waited_s" not in sc_b, {"took": time.time() - t1, "sc": sc_b})
    b.req("tools/call", {"name": "instances_close", "arguments": {"all": True}}, 20)
    b.close(); srv.stop()
    # 10. A launch that would wait longer than its timeout fails cleanly, nothing is started.
    port, env, srv = fp_setup(hold={"*": 40.0})
    a, b = Bridge(port, env), Bridge(port, env)
    fp_call(b, "map_load", {"map": "gm_j", "timeout_s": 60}, 60)
    end = time.time() + 10
    while not srv.map_log and time.time() < end: time.sleep(0.05)
    t1 = time.time()
    res_a, sc_a = fp_result(a, fp_call(a, "instances_launch", launch_args(timeout_s=10), 30), 30)
    check("FP7137 a launch blocked past timeout_s fails with map_load_in_progress and waited_s", res_a.get("isError") and sc_a.get("error", {}).get("code") == "map_load_in_progress" and 8 <= sc_a.get("waited_s", 0) <= 14, sc_a)
    check("FP7137 ... and no game was started", not os.listdir(srv.pid_dir), os.listdir(srv.pid_dir))
    nc = sc_a.get("error", {}).get("next_call", {})
    check("FP7137 (issue 3) map_load_in_progress carries next_call: the same instances_launch, after_s, a larger timeout_s",
          nc.get("tool") == "instances_launch" and nc.get("arguments", {}).get("count") == launch_args().get("count") and nc.get("arguments", {}).get("timeout_s", 0) > 10
          and nc.get("after_s", 0) > 0 and json_equal(res_a["content"][0]["text"], sc_a), nc)
    a.close(); b.close(); srv.stop()
    # 11. Kill switch: the same two bridges, no wait either way.
    port, env, srv = fp_setup(workaround="0", hold={"*": 3.0})
    a, b = Bridge(port, env), Bridge(port, env)
    rid_b = fp_call(b, "map_load", {"map": "gm_k", "timeout_s": 60}, 60)
    end = time.time() + 10
    while not srv.map_log and time.time() < end: time.sleep(0.05)
    res_a, sc_a = fp_result(a, fp_call(a, "instances_launch", launch_args(), 60), 60)
    done_b = srv.map_log[0][2]
    check("FP7137 kill switch off: the launch does not wait for the load", not res_a.get("isError") and "waited_s" not in sc_a and (done_b is None or (pid_times(srv) or [0])[0] < done_b), {"sc": sc_a, "done_b": done_b})
    res_b, sc_b = fp_result(b, rid_b, 10)
    res_b, sc_b = fp_result(b, fp_call(b, "map_load", {"map": "gm_l", "timeout_s": 60}, 60), 20)
    check("FP7137 kill switch off: map_load right after a launch does not wait", "waited_s" not in sc_b and not res_b.get("isError"), sc_b)
    a.req("tools/call", {"name": "instances_close", "arguments": {"all": True}}, 20)
    a.close(); b.close(); srv.stop()
    # 12. FP7137 (B5): instances_launch {map} loads after the settle delay and at most 2 maps at a time; with the kill
    # switch off all 3 at once and no wait.
    for off in (False, True):
        port, env, srv = fp_setup(workaround="0" if off else None, hold={"*": 1.2})
        a = Bridge(port, env)
        res_a, sc_a = fp_result(a, fp_call(a, "instances_launch", launch_args(count=3, map="gm_m", timeout_s=60), 90), 90)
        ins = sc_a.get("instances", [])
        last_pid = (pid_times(srv) or [0])[-1]
        first_load = min([m[1] for m in srv.map_log] or [0])
        tag = "FP7137 kill switch off: " if off else "FP7137 "
        check(tag + "instances_launch {map, count 3}: all three loaded", not res_a.get("isError") and len(ins) == 3 and all(i.get("map") == "gm_m" for i in ins) and len(srv.map_log) == 3, sc_a)
        if off:
            check(tag + "the three loads overlap (3 at once) and nobody waited", srv.map_max == 3 and not any("waited_s" in i for i in ins), {"max": srv.map_max, "ins": ins})
        else:
            check(tag + "at most 2 maps load at the same time", srv.map_max == 2, srv.map_max)
            check(tag + "no load starts before the settle delay (2.5 s here) after the last process start", first_load - last_pid >= 2.0, {"gap": first_load - last_pid})
        a.req("tools/call", {"name": "instances_close", "arguments": {"all": True}}, 20)
        a.close(); srv.stop()
    shutil.rmtree(game, ignore_errors=True)



def case_unseen_loads():
    """Issue #7 (FP7137): a map load the bridge does not see (console `map`, Lua, a script talking HTTP to the game) is
    known from the games' heartbeat (`loading` in instances_list). instances_launch waits for it (reason
    map_load_outside_bridge, waited_s), fails cleanly at its timeout, obeys the kill switch; map_load lists the other
    games that were loading. The fake game server plays the games; the launches use the stand-in game (Windows)."""
    print("== UNSEEN: map loads the bridge does not see (issue #7)", flush=True)
    import shutil
    hand = lambda loading: {"id": "by-hand", "pid": 1, "port": 1, "mode": "client", "state": "running", "loading": loading}
    # Issue #7: map_load says which other games were loading (it does not wait for them: only launches matter).
    port, env, srv = fp_setup()
    b = Bridge(port, env)
    srv.extra_instances = [hand(True)]
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_a", "timeout_s": 30}, 30), 15)
    check("UNSEEN map_load lists the other game that was loading, and does not wait for it",
          not res.get("isError") and sc.get("other_games_loading") == ["by-hand"] and "waited_s" not in sc
          and json.loads(res["content"][0]["text"]).get("other_games_loading") == ["by-hand"], res)
    srv.extra_instances = [hand(False)]
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_b", "timeout_s": 30}, 30), 15)
    check("UNSEEN map_load: no other game loading, nothing reported", not res.get("isError") and "other_games_loading" not in sc, sc)
    srv.extra_instances = [hand(True)]
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_c", "instance": "by-hand", "timeout_s": 30}, 30), 15)
    check("UNSEEN map_load targeting the game that loads does not list it as another game", not res.get("isError") and "other_games_loading" not in sc, sc)
    b.close(); srv.stop()
    port, env, srv = fp_setup(workaround="0")
    b = Bridge(port, env)
    srv.extra_instances = [hand(True)]
    res, sc = fp_result(b, fp_call(b, "map_load", {"map": "gm_d", "timeout_s": 30}, 30), 15)
    check("UNSEEN kill switch off: map_load reports nothing", not res.get("isError") and "other_games_loading" not in sc, sc)
    b.close(); srv.stop()

    if os.name != "nt":
        print("   (the launch scenarios are Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("UNSEEN stand-in game built next to the bridge", False, fake)
        return
    game = tempfile.mkdtemp(prefix="gmcp-fake-game-")
    shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
    launch_args = lambda **kw: dict({"count": 1, "owner": "unseen", "timeout_s": 40, "game_dir": game}, **kw)
    pid_times = lambda srv: sorted(os.path.getmtime(os.path.join(srv.pid_dir, f)) for f in os.listdir(srv.pid_dir))

    # Issue #7: a game loads a map the bridge never saw: the launch waits until it is over, then starts.
    port, env, srv = fp_setup()
    a = Bridge(port, env)
    srv.extra_instances = [hand(True)]
    t0 = time.time()
    rid = fp_call(a, "instances_launch", launch_args(), 60, token="U1")
    time.sleep(2.0)
    check("UNSEEN the launch waits while a game loads outside the bridge (no game started yet)", not os.listdir(srv.pid_dir), os.listdir(srv.pid_dir))
    check("UNSEEN the wait is reported as progress", any("map_load_outside_bridge" in m for m in fp_progress(a, "U1")), fp_progress(a, "U1"))
    srv.extra_instances = [hand(False)]
    flipped = time.time()
    res, sc = fp_result(a, rid, 60)
    first_pid = (pid_times(srv) or [0])[0]
    check("UNSEEN once the load is over the launch goes ahead, with waited_s and waited_reason",
          not res.get("isError") and first_pid >= flipped - 0.1 and sc.get("waited_s", 0) >= 1.5 and "map_load_outside_bridge" in sc.get("waited_reason", "") and "by-hand" in sc.get("waited_reason", ""),
          {"first_pid": first_pid, "flipped": flipped, "sc": sc})
    # No game loading: no wait.
    a.req("tools/call", {"name": "instances_close", "arguments": {"all": True}}, 20)
    res, sc = fp_result(a, fp_call(a, "instances_launch", launch_args(), 60), 60)
    check("UNSEEN no game loading: no wait", not res.get("isError") and "waited_s" not in sc, sc)
    a.req("tools/call", {"name": "instances_close", "arguments": {"all": True}}, 20)
    a.close(); srv.stop()
    # Issue #7: a load that never ends: the launch fails at its timeout, nothing is started.
    port, env, srv = fp_setup()
    a = Bridge(port, env)
    srv.extra_instances = [hand(True)]
    t1 = time.time()
    res, sc = fp_result(a, fp_call(a, "instances_launch", launch_args(timeout_s=10), 30), 30)
    check("UNSEEN a launch blocked past timeout_s fails with map_load_in_progress and waited_s",
          res.get("isError") and sc.get("error", {}).get("code") == "map_load_in_progress" and 8 <= sc.get("waited_s", 0) <= 14
          and "map_load_outside_bridge" in sc.get("waited_reason", "") and time.time() - t1 < 18, sc)
    check("UNSEEN ... and no game was started", not os.listdir(srv.pid_dir), os.listdir(srv.pid_dir))
    a.close(); srv.stop()
    # Issue #7: the game that answers instances_list is the bridge's own client loading through the bridge: that load
    # is the bridge's (a seeded entry), the reason is the registered one, not "outside".
    port, env, srv = fp_setup()
    a = Bridge(port, env)
    srv.extra_instances = [hand(True)]
    fp_seed(env, [("M", os.getpid(), 60, "map:gm_mine")])
    res, sc = fp_result(a, fp_call(a, "instances_launch", launch_args(timeout_s=10), 30), 30)
    check("UNSEEN a load registered by a bridge keeps the registered reason", res.get("isError") and "map:gm_mine" in sc.get("waited_reason", "") and "map_load_outside_bridge" not in sc.get("waited_reason", ""), sc)
    a.close(); srv.stop()
    # Issue #7: kill switch.
    port, env, srv = fp_setup(workaround="0")
    a = Bridge(port, env)
    srv.extra_instances = [hand(True)]
    res, sc = fp_result(a, fp_call(a, "instances_launch", launch_args(), 60), 60)
    check("UNSEEN kill switch off: the launch does not wait", not res.get("isError") and "waited_s" not in sc, sc)
    a.req("tools/call", {"name": "instances_close", "arguments": {"all": True}}, 20)
    a.close(); srv.stop()
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

    srcds = call("status", {"fields": ["mode"]})[0].get("mode") == "srcds"

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
    # (echo is blocked from server Lua, and srcds has no engine path: use two convars.)
    sc, err = call("console_exec", {"realm": "server", "cmd": "sv_gravity 600; sv_friction 8", "regex": "sv_friction"})
    check("RETEX console_exec runs both statements, regex keeps one line",
          not err and len(sc.get("statements", [])) == 2 and len(sc.get("output", [])) <= 1
          and all("sv_friction" in l["text"] for l in sc.get("output", [])) and set(sc.get("convars", {})) == {"sv_gravity", "sv_friction"}, sc)
    sc, err = call("console_exec", {"realm": "server", "cmd": "retex_typo_cmd 1"})
    check("RETEX unknown command is an error", err and "unknown_command" in json.dumps(sc), sc)
    if not srcds:  # an archived client convar, read only (no value given)
        sc, err = call("console_exec", {"realm": "menu", "cmd": "fov_desired"})
        c = sc.get("convars", {}).get("fov_desired", {})
        check("RETEX naming an archived convar reads it back with a warning", not err and c.get("number") is not None and "ARCHIVE" in c.get("warning", ""), sc)
    sc, err = call("convar_get", {"name": "sv_gravity", "realm": "server"})
    check("RETEX convar_get gives the number", sc.get("number") == 600, sc)
    b.close()


def case_lua_mount_live():
    """Live (Windows, Steam running, no other game): O-6 two clients launched with two different mounted folders and one
    without; O-8 same file name and time in the game and the mounted folder, with and without empty_cache. Folders come from
    LUA_MOUNT_A / LUA_MOUNT_B (each holds lua/mcpagent_tmp/ovtest_shared.lua returning its own tag) and the game's
    lua/mcpagent_tmp/ovtest_shared.lua returns "main"."""
    print("== LUA_MOUNT LIVE: per-instance mounted folders (O-6, O-8)", flush=True)
    a, bdir = os.environ["LUA_MOUNT_A"], os.environ["LUA_MOUNT_B"]
    b = Bridge(27080)

    def call(tool, args, timeout=300):
        r = b.req("tools/call", {"name": tool, "arguments": args}, timeout)
        res = (r or {}).get("result", {})
        return res.get("structuredContent", {}), bool(res.get("isError"))

    ids = {}
    for tag, ov in (("A", a), ("B", bdir), ("none", None)):
        args = {"count": 1, "map": "gm_construct", "owner": "mount-live", "sound": False}
        if ov: args["lua_mount"] = ov
        sc, err = call("instances_launch", args)
        ins = sc.get("instances", [])
        check("MOUNTLIVE client %s launched with map" % tag, not err and len(ins) == 1 and ins[0].get("map") == "gm_construct", sc)
        if ins: ids[tag] = ins[0]["instance"]
    expect = {"A": "branch", "B": "branch2", "none": "main"}
    for tag, inst in ids.items():
        sc, err = call("lua_exec", {"instance": inst, "realm": "server", "code": "return include('mcpagent_tmp/ovtest_shared.lua'), OVTEST_AUTORUN"})
        check("MOUNTLIVE client %s runs its own copy (%s)" % (tag, expect[tag]), sc.get("values", [None])[0] == expect[tag], sc)
        sc, err = call("lua_exec", {"instance": inst, "realm": "client", "code": "return OVTEST_AUTORUN"})
        check("MOUNTLIVE client %s client realm" % tag, (sc.get("values") or [None])[0] == ({"A": "client from mounted folder", "B": "client from mounted folder 2"}.get(tag)), sc)
    # O-8: same name and time in the game and mounted folder A: which copy does include() return?
    if "A" in ids:
        game_copy = os.path.join(os.environ["GMOD_LUA_TMP"], "ovtest_shared.lua")
        ov_copy = os.path.join(a, "lua", "mcpagent_tmp", "ovtest_shared.lua")
        st = os.stat(game_copy)
        os.utime(ov_copy, (st.st_atime, st.st_mtime))
        for ec in (False, True):
            call("lua_mount", {"instance": ids["A"], "path": a, "empty_cache": ec})
            sc, err = call("lua_exec", {"instance": ids["A"], "realm": "server", "code": "return include('mcpagent_tmp/ovtest_shared.lua')"})
            print("   O-8 same mtime, empty_cache=%s -> include returns %r" % (ec, sc.get("values")), flush=True)
            check("MOUNTLIVE O-8 same name and time, empty_cache=%s: the mounted folder copy wins" % ec, sc.get("values") == ["branch"], sc)
    call("instances_close", {"owner": "mount-live"}, 60)
    b.close()


def case_edge_live():
    """Live edge cases (Windows, Steam running, no other game): async jobs and map loads, cancel twice, yield inside
    pcall, 100 async jobs, mounted folder switch / removal / deleted folder / accented path, closing a client while it loads."""
    print("== EDGE LIVE: corner cases against a real client", flush=True)
    import shutil
    b = Bridge(27080)

    def call(tool, args, timeout=300):
        r = b.req("tools/call", {"name": tool, "arguments": args}, timeout)
        res = (r or {}).get("result", {})
        return res.get("structuredContent", {}), bool(res.get("isError"))

    sc, err = call("instances_launch", {"count": 1, "map": "gm_construct", "owner": "edge-live", "sound": False})
    inst = (sc.get("instances") or [{}])[0].get("instance")
    check("EDGE client launched", not err and inst, sc)
    if not inst:
        b.close(); return
    I = {"instance": inst}

    # Jobs.
    sc, err = call("lua_job", dict(I, realm="server", code="local ok, v = pcall(function() job:frames(3) return 7 end) return ok, v"))
    check("EDGE job:frames inside pcall resumes and returns", not err and sc.get("values") == [True, 7], sc)
    sc, err = call("lua_job", dict(I, realm="server", **{"async": True}, code="job:sleep(60) return 1"))
    jid = sc.get("job_id")
    sc1, err1 = call("lua_job_cancel", dict(I, id=jid))
    sc2, err2 = call("lua_job_cancel", dict(I, id=jid))
    check("EDGE cancel twice: both answer, no crash", not err1 and not err2, [sc1, sc2])
    ids = []
    for k in range(100):
        sc, err = call("lua_job", dict(I, realm="server", **{"async": True}, code="job:frames(1 + args.k % 5) return args.k", args={"k": k}), 30)
        ids.append(sc.get("job_id"))
    vals = []
    for jid2 in ids:
        sc, err = call("lua_job_status", dict(I, id=jid2, wait_s=20), 40)
        vals.append((sc.get("values") or [None])[0])
    check("EDGE 100 async jobs all return their own value", vals == list(range(100)), vals[:10])
    sc, err = call("lua_job", dict(I, realm="server", **{"async": True}, code="job:sleep(120) return 1"))
    jid3 = sc.get("job_id")
    call("map_load", dict(I, map="gm_construct", timeout_s=120), 180)
    sc, err = call("lua_job_status", dict(I, id=jid3))
    check("EDGE async job of a realm destroyed by map_load: a clear error", err and ("not_found" in json.dumps(sc) or "realm" in json.dumps(sc)), sc)

    # Mounted folder corner cases.
    base = tempfile.mkdtemp(prefix="gmcp-edge-")
    ova = os.path.join(base, "ov A")
    ovb = os.path.join(base, "ovB")
    for d, tag in ((ova, "edgeA"), (ovb, "edgeB")):
        os.makedirs(os.path.join(d, "lua", "mcpagent_tmp"))
        with open(os.path.join(d, "lua", "mcpagent_tmp", "edge_ov.lua"), "w", encoding="utf-8") as f:
            f.write("return '%s'\n" % tag)
    sc, err = call("lua_mount", dict(I, path=None))
    check("EDGE removing a mounted folder when none is set is fine", not err, sc)
    sc, err = call("lua_mount", dict(I, path=ova))
    check("EDGE mounted folder with spaces and an accent in the path", not err and sc.get("mount_hook") is True, sc)
    sc, err = call("lua_mount", dict(I, path=ovb))
    check("EDGE switching A -> B without a map load: A removed", not err and sc.get("removed", {}).get("lsv:lua") is True, sc)
    call("map_load", dict(I, map="gm_construct", timeout_s=120), 180)
    sc, err = call("lua_exec", dict(I, realm="server", code="return include('mcpagent_tmp/edge_ov.lua')"))
    check("EDGE after the map load B is served, not A", sc.get("values") == ["edgeB"], sc)
    shutil.rmtree(ovb, ignore_errors=True)
    call("map_load", dict(I, map="gm_construct", timeout_s=120), 180)
    sc, err = call("lua_exec", dict(I, realm="server", code="return GAMEMODE ~= nil, file.Exists('mcpagent_tmp/edge_ov.lua', 'LUA')"))
    check("EDGE mounted folder deleted while set: the map still loads, the file is just gone", sc.get("values") == [True, False], sc)
    sc, err = call("lua_mount", dict(I))
    check("EDGE mounted folder status still answers (base Lua mounted)", not err and sc.get("base_lua_mounted") is True, sc)
    call("lua_mount", dict(I, path=None))
    shutil.rmtree(base, ignore_errors=True)

    # Close a client while it is still loading its map.
    rid = b.start("tools/call", {"name": "instances_launch", "arguments": {"count": 1, "map": "gm_construct", "owner": "edge-loading", "sound": False}})
    time.sleep(8)
    sc, err = call("instances_close", {"owner": "edge-loading"}, 60)
    r = b.wait(rid, 300)
    check("EDGE closing a client by owner while instances_launch waits: the launch call ends (no hang)", r is not None, r)
    time.sleep(2)
    sc, err = call("instances_list", {"owner": "edge-loading"})
    check("EDGE the closed loading client is gone", not sc.get("instances") and not sc.get("launched_not_answering"), sc)

    call("instances_close", {"owner": "edge-live"}, 60)
    b.close()


def case_lua_mount_ecosystem():
    """Live (Windows, Steam running, no other game): every way Lua is loaded, with a mounted Lua folder holding an
    addon that exists nowhere else (autorun + shared + server + client files)."""
    print("== LUA_MOUNT ECOSYSTEM: the Lua tools and engine paths with a mounted folder", flush=True)
    base = tempfile.mkdtemp(prefix="gmcp-ovx-")
    ov = os.path.join(base, "my addon wt")
    files = {
        "lua/autorun/ovx_init.lua": 'AddCSLuaFile()\nAddCSLuaFile("ovx/sh.lua")\nAddCSLuaFile("ovx/cl.lua")\ninclude("ovx/sh.lua")\n'
                                    'if SERVER then include("ovx/sv.lua") else include("ovx/cl.lua") end\n',
        "lua/ovx/sh.lua": 'OVX = OVX or {}\nfunction OVX.Shared() return "sh v1" end\n',
        "lua/ovx/sv.lua": 'function OVX.Server() return "sv v1" end\n',
        "lua/ovx/cl.lua": 'function OVX.Client() return "cl v1" end\n',
        "gamemodes/ovxgm/gamemode/shared.lua": 'return "gm from the mounted folder"\n',
    }
    for rel, text in files.items():
        os.makedirs(os.path.dirname(os.path.join(ov, rel)), exist_ok=True)
        with open(os.path.join(ov, rel), "w", encoding="utf-8") as f: f.write(text)
    b = Bridge(27080)

    def call(tool, args, timeout=300):
        r = b.req("tools/call", {"name": tool, "arguments": args}, timeout)
        res = (r or {}).get("result", {})
        return res.get("structuredContent", {}), bool(res.get("isError"))

    sc, err = call("instances_launch", {"count": 1, "map": "gm_construct", "owner": "ovx", "sound": False, "lua_mount": ov})
    inst = (sc.get("instances") or [{}])[0].get("instance")
    check("OVX client launched with the folder from its first map", not err and inst and sc.get("lua_mount"), sc)
    if not inst:
        b.close(); return
    I = {"instance": inst}
    sc, err = call("lua_exec", dict(I, realm="server", code="return OVX.Shared(), OVX.Server(), debug.getinfo(OVX.Shared, 'S').source"))
    check("OVX server: autorun, include, engine name @lua/...", sc.get("values") == ["sh v1", "sv v1", "@lua/ovx/sh.lua"], sc)
    sc, err = call("lua_exec", dict(I, realm="client", code="return OVX.Shared(), OVX.Client(), OVX.Server == nil"))
    check("OVX client realm: AddCSLuaFile copies from the folder, server file not sent", sc.get("values") == ["sh v1", "cl v1", True], sc)
    sc, err = call("lua_exec", dict(I, realm="server", code="local f = CompileFile('ovxgm/gamemode/shared.lua') return file.Exists('ovxgm/gamemode/shared.lua', 'LUA'), f and f()"))
    check("OVX <folder>/gamemodes/ is searched too (gamemode files by their gamemodes-relative path)",
          sc.get("values") == [True, "gm from the mounted folder"], sc)
    sc, err = call("lua_function_source", dict(I, realm="server", expr="OVX.Shared"))
    check("OVX lua_function_source finds the folder's file, matches", sc.get("matches_disk") is True and "my addon wt" in sc.get("file", ""), sc)
    sc, err = call("lua_check", dict(I, realm="server", path="ovx"))
    check("OVX lua_check of a folder that only exists in the mounted folder", not err and sc.get("ok") is True, sc)
    sc, err = call("lua_exec_file", dict(I, realm="server", path="ovx/sv.lua"))
    check("OVX lua_exec_file finds it and names it like the engine", not err and sc.get("chunkname") == "@lua/ovx/sv.lua", sc)
    sc, err = call("lua_job", dict(I, realm="server", path="ovx/sh.lua"))
    check("OVX lua_job {path} finds it", not err, sc)
    sc, err = call("console_exec", dict(I, realm="server", cmd="lua_openscript ovx/sv.lua"))
    check("OVX console lua_openscript finds it", not err and any("ovx/sv.lua" in l["text"] for l in sc.get("output", [])), sc)
    # Edit: no autorefresh; lua_function_source says the running code is stale; lua_reload runs the folder's copy.
    with open(os.path.join(ov, "lua/ovx/sh.lua"), "w", encoding="utf-8") as f:
        f.write('OVX = OVX or {}\nfunction OVX.Shared() return "sh v2" end\n')
    time.sleep(3)
    sc, err = call("lua_exec", dict(I, realm="server", code="return OVX.Shared()"))
    check("OVX no autorefresh: still the old code after an edit", sc.get("values") == ["sh v1"], sc)
    sc, err = call("lua_function_source", dict(I, realm="server", expr="OVX.Shared"))
    check("OVX lua_function_source reports the edit (matches_disk false)", sc.get("matches_disk") is False, sc)
    sc, err = call("lua_reload", dict(I, path="ovx/sh.lua"))
    check("OVX lua_reload runs the folder's copy in server and client", not err and sc.get("ok") is True and "my addon wt" in sc.get("file", ""), sc)
    sc, err = call("lua_exec", dict(I, realm="server", code="return OVX.Shared()"))
    sc2, err2 = call("lua_exec", dict(I, realm="client", code="return OVX.Shared()"))
    check("OVX after lua_reload both realms run v2", sc.get("values") == ["sh v2"] and sc2.get("values") == ["sh v2"], [sc, sc2])
    sc, err = call("lua_function_source", dict(I, realm="server", expr="OVX.Shared"))
    check("OVX lua_function_source matches again", sc.get("matches_disk") is True, sc)
    # Removal: from the next map load the addon is gone (it exists nowhere else).
    call("lua_mount", dict(I, path=None))
    call("map_load", dict(I, map="gm_construct", timeout_s=120), 180)
    sc, err = call("lua_exec", dict(I, realm="server", code="return OVX == nil"))
    check("OVX removed: the next map load runs without it", sc.get("values") == [True], sc)
    sc, err = call("lua_reload", dict(I, path="ovx/sh.lua"))
    check("OVX removed: lua_reload no longer finds the folder's files", err and "not_found" in json.dumps(sc), sc)
    call("instances_close", {"owner": "ovx"}, 60)
    b.close()
    import shutil
    shutil.rmtree(base, ignore_errors=True)


def case_edge():
    """Edge cases of the bridge's own tools, no game (2026-11-14): nothing launched, unicode owners, wrong argument
    types, a mounted folder inside the game folder."""
    print("== EDGE: bridge tools with odd input (no game)", flush=True)
    port, env = free_port(), isolated_config(with_token=True)
    b = Bridge(port, env)

    def call(tool, args, timeout=20):
        r = b.req("tools/call", {"name": tool, "arguments": args}, timeout)
        if r is None: return None, None
        if "error" in r: return r["error"], True  # JSON-RPC level error (schema)
        res = r.get("result", {})
        return res.get("structuredContent", {}), bool(res.get("isError"))

    sc, err = call("instances_close", {"owner": "nobody"})
    check("EDGE close by owner with nothing launched: an error, nothing closed", err and not (sc or {}).get("closed"), sc)
    sc, err = call("instances_list", {"owner": "équipe-中文"})
    check("EDGE instances_list with a unicode owner and no game: empty, no error", err is False and sc.get("instances") == [] and sc.get("owner_filter") == "équipe-中文", sc)
    # game_dir points nowhere: even if a check regressed, no real game could start from this test.
    nowhere = os.path.join(tempfile.gettempdir(), "gmcp-no-game-here")
    for tool, args, key in (("instances_launch", {"count": "two"}, "count"), ("instances_launch", {"count": 0}, "count"),
                            ("instances_launch", {"count": 99}, "count"), ("instances_launch", {"lua_mount": 5}, "lua_mount"),
                            ("instances_launch", {"preset": "turbo"}, "preset"), ("instances_launch", {"sound": "no"}, "sound"),
                            ("instances_launch", {"map": "../../etc"}, "map"), ("instances_launch", {"bogus": 1}, "bogus"),
                            ("instances_close", {"pids": "abc"}, "pids"), ("instances_close", {"pids": [-1]}, None),
                            ("instances_close", {"owner": 12}, "owner"), ("instances_close", {"instances": "client-1"}, "instances"),
                            ("instance_select", {"instance": 5}, "instance")):
        if tool == "instances_launch": args = dict(args, game_dir=nowhere)
        sc, err = call(tool, args)
        check("EDGE %s %s refused, naming the argument" % (tool, json.dumps(args)),
              err is True and (key is None or key in json.dumps(sc)), sc)
    sc, err = call("instances_launch", {"lua_mount": "relative/path", "game_dir": nowhere})
    check("EDGE lua_mount relative path refused before launching",
          err and ("absolute" in json.dumps(sc) or (os.name != "nt" and "unsupported" in json.dumps(sc))), sc)
    if os.name == "nt":
        import shutil
        game = tempfile.mkdtemp(prefix="gmcp-fake-game-")
        fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
        if os.path.exists(fake): shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
        inside = os.path.join(game, "garrysmod", "addons", "myaddon")
        os.makedirs(os.path.join(inside, "lua"))
        sc, err = call("instances_launch", {"lua_mount": inside, "game_dir": game, "timeout_s": 10}, 30)
        check("EDGE lua_mount inside the game folder refused before launching", err and "inside the game folder" in json.dumps(sc), sc)
        shutil.rmtree(game, ignore_errors=True)
    r = b.req("ping", {}, 5)
    check("EDGE bridge still answers after the odd calls", r is not None and "result" in r, r)
    b.close()


def case_version():
    """Version mismatch between the bridge (the plugin, updated by Claude Code) and the module in the game (not
    updated): the agent must be told on every tool result, even when the game starts after Claude Code connected."""
    print("== VERSION: module in the game older than the bridge", flush=True)
    port, env = free_port(), isolated_config(with_token=True)
    b = Bridge(port, env)
    b.req("initialize", INIT)
    b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    fake = FakeServer(port, version="0.0.1")   # the game starts AFTER Claude Code connected
    time.sleep(5)                               # the bridge's probe sees it (every 2 s)
    r = b.req("tools/call", {"name": "echo", "arguments": {}})
    res = (r or {}).get("result", {})
    sc = res.get("structuredContent", {})
    texts = [c.get("text", "") for c in res.get("content", []) if c.get("type") == "text"]
    first = texts[0] if texts else ""
    bv = bridge_version()
    check("VERSION every tool result carries version_warning naming both versions",
          "0.0.1" in sc.get("version_warning", "") and bv in sc.get("version_warning", ""), sc)
    # Issue #3: no prose block next to the JSON; the warning is a field of the JSON and the text stays the JSON of
    # structuredContent (code-mode hosts parse it), with the warning as its first key so it is read first.
    check("VERSION (issue 3) one text block, the JSON of structuredContent, version_warning first, with what to do",
          len(texts) == 1 and first.startswith('{"version_warning":"') and json_equal(first, sc) and "0.0.1" in first and "skill" in first, texts)
    check("VERSION the tool's own answer is still there", sc.get("args") == {} and "args" in first, res)
    # The bridge's own tools too (seen in a real update, 2026-11-14: "launch an instance" went through instances_launch
    # and the agent never saw the warning).
    for name, args in (("instance_select", {"instance": None}), ("instances_close", {"owner": "nobody"})):
        r = b.req("tools/call", {"name": name, "arguments": args})
        res = (r or {}).get("result", {})
        t0 = (res.get("content") or [{}])[0].get("text", "")
        check("VERSION the bridge's own %s carries the warning too" % name,
              t0.startswith('{"version_warning":"') and json_equal(t0, res.get("structuredContent")) and "0.0.1" in res.get("structuredContent", {}).get("version_warning", ""), res)
    check("VERSION the warning explains tools missing from the list", "listed" in first or "list" in first, first[:400])
    fake.stop(); b.close()
    # Same version: no warning.
    port, env = free_port(), isolated_config(with_token=True)
    fake = FakeServer(port, version=bv)
    b = Bridge(port, env)
    b.req("initialize", INIT)
    time.sleep(3)
    r = b.req("tools/call", {"name": "echo", "arguments": {}})
    res = (r or {}).get("result", {})
    check("VERSION same version: no warning", "version_warning" not in res.get("structuredContent", {}) and
          "version_warning" not in (res.get("content") or [{}])[0].get("text", ""), res)
    fake.stop(); b.close()


def case_version_live():
    """Live (Windows, Steam running, no other game; the game's module must be OLDER than this bridge, e.g. the previous
    release's DLL put back in garrysmod/lua/bin): a client launched by this bridge, then any tool result warns."""
    print("== VERSION LIVE: an older module in the game", flush=True)
    b = Bridge(27080)
    r = b.req("tools/call", {"name": "instances_launch", "arguments": {"count": 1, "owner": "version-live", "sound": False}}, 300)
    ins = ((r or {}).get("result", {}).get("structuredContent", {}).get("instances") or [{}])
    inst = ins[0].get("instance")
    check("VERSION LIVE client launched", inst, r)
    lres = (r or {}).get("result", {})
    check("VERSION LIVE the instances_launch answer itself starts with the warning",
          (lres.get("content") or [{}])[0].get("text", "").startswith('{"version_warning":"'), str(lres)[:300])
    time.sleep(4)
    r = b.req("tools/call", {"name": "status", "arguments": {"instance": inst} if inst else {}}, 30)
    res = (r or {}).get("result", {})
    first = (res.get("content") or [{}])[0].get("text", "")
    sc = res.get("structuredContent", {})
    print("   module", sc.get("agent_version"), "| bridge", bridge_version(), flush=True)
    check("VERSION LIVE the result starts with the warning naming both versions",
          first.startswith('{"version_warning":"') and str(sc.get("agent_version")) in first and bridge_version() in first, first[:300])
    b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "version-live"}}, 60)
    b.close()


def case_observe():
    """Live observations (no pass/fail on behaviour, printed for the docs): client include() of an edited file in
    singleplayer and listen server, convar_set of a server convar from the default realm, what sound_trace sees, an
    unknown ent_fire input, bots across a map load, door states."""
    print("== OBSERVE: facts for the tool descriptions", flush=True)
    b = Bridge(27080)

    def call(tool, args, timeout=300):
        r = b.req("tools/call", {"name": tool, "arguments": args}, timeout)
        res = (r or {}).get("result", {})
        return res.get("structuredContent", {}), bool(res.get("isError"))

    def show(label, v): print("   %s: %s" % (label, json.dumps(v, ensure_ascii=False)[:600]), flush=True)
    sc, err = call("instances_launch", {"count": 1, "map": "gm_construct", "owner": "observe", "sound": True})
    inst = (sc.get("instances") or [{}])[0].get("instance")
    check("OBSERVE client launched", inst, sc)
    if not inst: b.close(); return
    I = {"instance": inst}
    tmp = os.path.join(os.environ["GMOD_LUA_TMP"], "incq.lua")
    def put(v):
        with open(tmp, "w") as f: f.write("return '%s'\n" % v)
    for mode, mp in (("singleplayer", 1), ("listen", 2)):
        put("v1")
        call("map_load", dict(I, map="gm_construct", maxplayers=mp, timeout_s=120), 180)
        c1, _ = call("lua_exec", dict(I, realm="client", code="return include('mcpagent_tmp/incq.lua')"))
        put("v2"); time.sleep(1)
        c2, _ = call("lua_exec", dict(I, realm="client", code="return include('mcpagent_tmp/incq.lua')"))
        s2, _ = call("lua_exec", dict(I, realm="server", code="return include('mcpagent_tmp/incq.lua')"))
        show("Q1 %s client include before/after edit, server after" % mode, [c1.get("values") or c1, c2.get("values") or c2, s2.get("values") or s2])
    # Q5 bots across a map load (listen server is up now).
    sc, err = call("bots_add", dict(I, n=1)); show("Q5 bots_add", sc)
    sc, _ = call("players_list", I); show("Q5 players before map_load", [p.get("name") for p in sc.get("players", [])])
    call("map_load", dict(I, map="gm_construct", maxplayers=2, timeout_s=120), 180)
    sc, _ = call("players_list", I); show("Q5 players after map_load", [p.get("name") for p in sc.get("players", [])])
    # Q2 convar_set of a server convar from the default realm.
    sc, err = call("convar_set", dict(I, name="sv_gravity", value=555)); show("Q2 convar_set sv_gravity default realm", [err, sc])
    sc, _ = call("lua_exec", dict(I, realm="server", code="return GetConVar('sv_gravity'):GetString()")); show("Q2 server sees", sc.get("values"))
    call("convar_set", dict(I, name="sv_gravity", value=600, realm="server"))
    # Q3 what sound_trace sees.
    rid = b.start("tools/call", {"name": "sound_trace", "arguments": dict(I, duration_s=4, realm="server")})
    time.sleep(1)
    call("lua_exec", dict(I, realm="server", code="sound.Play('buttons/button14.wav', Vector(0,0,0)) Entity(1):EmitSound('buttons/button15.wav') return true"))
    call("lua_exec", dict(I, realm="client", code="surface.PlaySound('buttons/button16.wav') LocalPlayer():EmitSound('buttons/button17.wav') return true"))
    r = b.wait(rid, 30)
    calls = (r or {}).get("result", {}).get("structuredContent", {}).get("calls", [])
    show("Q3 server sound_trace saw", [c.get("sound", {}).get("name") for c in calls])
    rid = b.start("tools/call", {"name": "sound_trace", "arguments": dict(I, duration_s=4, realm="client")})
    time.sleep(1)
    call("lua_exec", dict(I, realm="server", code="sound.Play('buttons/button14.wav', Vector(0,0,0)) Entity(1):EmitSound('buttons/button15.wav') return true"))
    call("lua_exec", dict(I, realm="client", code="surface.PlaySound('buttons/button16.wav') LocalPlayer():EmitSound('buttons/button17.wav') return true"))
    r = b.wait(rid, 30)
    calls = (r or {}).get("result", {}).get("structuredContent", {}).get("calls", [])
    show("Q3 client sound_trace saw", [c.get("sound", {}).get("name") for c in calls])
    # Q4 unknown ent_fire input.
    sc, err = call("ent_fire", dict(I, id=1, input="BogusInputXyz")); show("Q4 ent_fire unknown input", [err, sc])
    time.sleep(1)
    sc, _ = call("console_read", dict(I, regex="BogusInputXyz|unhandled input|Bogus", limit=5)); show("Q4 console", [l.get("text") for l in sc.get("lines", [])])
    # Q6 door states.
    sc, _ = call("ent_spawn", dict(I, **{"class": "prop_door_rotating", "pos": [0, 0, 100], "model": "models/props_c17/door01_left.mdl",
                                   "keyvalues": {"hardware": "1", "distance": "90", "speed": "100"}}))
    did = sc.get("id") or (sc.get("entity") or {}).get("id")
    show("Q6 spawned door", sc)
    if did:
        sc, _ = call("ent_info", dict(I, id=did)); show("Q6 door closed", sc.get("door"))
        call("ent_fire", dict(I, id=did, input="Open")); time.sleep(0.3)
        sc, _ = call("ent_info", dict(I, id=did)); show("Q6 door opening", sc.get("door"))
        time.sleep(2)
        sc, _ = call("ent_info", dict(I, id=did)); show("Q6 door open", sc.get("door"))
    sc, _ = call("ents_find", dict(I, **{"class": "func_door*", "limit": 3})); show("Q6 func_door on the map", sc)
    call("instances_close", {"owner": "observe"}, 60)
    os.remove(tmp)
    b.close()


def case_feedback():
    """The plugin option feedback_offers (Claude Code userConfig, passed to the bridge as GMOD_MCP_FEEDBACK_OFFERS):
    on by default, the instructions ask agents to offer friction reports and say how to turn that off; off, the
    paragraph is gone and agents are told not to offer."""
    print("== FEEDBACK: the option that turns friction-report offers off", flush=True)
    def instructions(env_value, live):
        port, env = free_port(), isolated_config(with_token=True)
        if env_value is not None: env["GMOD_MCP_FEEDBACK_OFFERS"] = env_value
        fake = None
        if live:
            fake = FakeServer(port, version=bridge_version())
            fake.instructions = "fake instructions\nFeedback: while you work, note the frictions you meet with gmod-mcp itself."
        b = Bridge(port, env)
        r = b.req("initialize", INIT)
        b.close()
        if fake: fake.stop()
        return (r or {}).get("result", {}).get("instructions", "")
    for live in (False, True):
        where = "live" if live else "offline"
        on = instructions(None, live)
        check("FEEDBACK %s default: agents are asked to offer reports" % where, "Feedback: while you work" in on, on[-500:])
        check("FEEDBACK %s default: they are told how to turn it off" % where, "feedback_offers" in on and "plugin configure" in on, on[-500:])
        for v in ("false", "0"):
            off = instructions(v, live)
            check("FEEDBACK %s option %s: no offer paragraph, a clear 'turned off'" % (where, v),
                  "Feedback: while you work" not in off and "turned off" in off, off[-500:])
        on2 = instructions("true", live)
        check("FEEDBACK %s option true: same as default" % where, "Feedback: while you work" in on2, on2[-300:])


def case_convar_crash():
    """Live regression (crash seen in 0.3.2, RETEX 2026-11-14): console_exec / convar_get from the MENU realm on
    convars that an addon creates at every map load. The menu's GetConVar cache kept the old ConVar object, freed at
    the map change, and the next call crashed the game (call to address 0)."""
    print("== CONVAR CRASH: menu-side convar queries across map loads", flush=True)
    base = tempfile.mkdtemp(prefix="gmcp-cvcrash-")
    for rel, text in (("lua/autorun/client/mcp_regress_cv.lua", 'CreateClientConVar("mcp_regress_cl", "0", true)\n'),
                      ("lua/autorun/server/mcp_regress_cv.lua", 'CreateConVar("mcp_regress_sv", "0")\n')):
        os.makedirs(os.path.dirname(os.path.join(base, rel)), exist_ok=True)
        with open(os.path.join(base, rel), "w") as f: f.write(text)
    b = Bridge(27080)

    def call(tool, args, timeout=300):
        r = b.req("tools/call", {"name": tool, "arguments": args}, timeout)
        res = (r or {}).get("result", {})
        return res.get("structuredContent", {}), bool(res.get("isError")), r

    sc, err, _ = call("instances_launch", {"count": 1, "map": "gm_construct", "owner": "cvcrash", "sound": False, "lua_mount": base})
    inst = (sc.get("instances") or [{}])[0].get("instance")
    pid = (sc.get("instances") or [{}])[0].get("pid")
    check("CVCRASH client launched", inst, sc)
    if not inst: b.close(); return
    I = {"instance": inst}
    alive_all = True
    for k in range(3):
        for cmd in ("mcp_regress_cl %d" % (k + 1), "mcp_regress_sv %d" % (k + 1)):
            call("console_exec", dict(I, realm="menu", cmd=cmd), 60)
            call("convar_get", dict(I, realm="menu", name=cmd.split()[0]), 60)
        call("map_load", dict(I, map="gm_construct", timeout_s=120), 180)
        time.sleep(1)
        if pid and not alive(pid):
            alive_all = False
            break
    sc, err, _ = call("console_exec", dict(I, realm="menu", cmd="mcp_regress_cl 9"), 60)
    check("CVCRASH the game survives menu-side queries of addon convars across 3 map loads",
          alive_all and pid and alive(pid), {"alive": alive(pid) if pid else None})
    check("CVCRASH and the value still reads back", sc.get("convars", {}).get("mcp_regress_cl", {}).get("value") == "9", sc)
    call("instances_close", {"owner": "cvcrash"}, 60)
    b.close()
    import shutil
    shutil.rmtree(base, ignore_errors=True)


def case_fixes_live():
    """Live checks of the review fixes (Windows, Steam running, no other game): connect not alone refused; Lua errors
    raised by autorun files while a map loads belong to that map (since_map_load); async job ids after a map change;
    a lua_mount folder with non-ASCII characters is really active."""
    print("== FIXES LIVE: review fixes in a real game", flush=True)
    import shutil
    base = tempfile.mkdtemp(prefix="gmcp-fixes-")
    mount = os.path.join(base, "wt fixes")
    for rel, text in (("lua/autorun/zz_mcp_err.lua", 'error("mcp shared autorun error")\n'),
                      ("lua/autorun/server/zz_mcp_err.lua", 'error("mcp server autorun error")\n'),
                      ("lua/autorun/server/aaa_mcp_err.lua", 'error("mcp early server autorun error")\n')):
        os.makedirs(os.path.dirname(os.path.join(mount, rel)), exist_ok=True)
        with open(os.path.join(mount, rel), "w") as f: f.write(text)
    b = Bridge(27080)

    def call(tool, args, timeout=300):
        r = b.req("tools/call", {"name": tool, "arguments": args}, timeout)
        res = (r or {}).get("result", {})
        return res.get("structuredContent", {}), bool(res.get("isError"))

    # A non-ASCII folder: the engine lists it but loads none of its files (seen live), so it is refused up front.
    odd = os.path.join(base, "wt é ж中")
    os.makedirs(os.path.join(odd, "lua"))
    sc, err = call("instances_launch", {"count": 1, "owner": "fixes", "lua_mount": odd})
    check("FIXES a non-ASCII lua_mount folder is refused before launching, with the way out",
          err and "ASCII" in json.dumps(sc) and not sc.get("instances"), sc)
    sc, err = call("instances_launch", {"count": 1, "map": "gm_construct", "owner": "fixes", "sound": False, "lua_mount": mount})
    ins = (sc.get("instances") or [{}])[0]
    inst = ins.get("instance")
    check("FIXES client launched with the lua_mount folder, reported active", inst and (ins.get("lua_mount") or {}).get("active") is True, sc)
    if not inst: b.close(); return
    I = {"instance": inst}
    sc, err = call("console_exec", dict(I, cmd="echo x; connect 127.0.0.1:27016"))
    check("FIXES 'echo x; connect ...' refused, nothing sent", err and "connect must be alone" in json.dumps(sc), sc)
    sc, err = call("status", dict(I, fields=["map"]))
    check("FIXES still on its map after the refused connect", sc.get("map") == "gm_construct", sc)
    # Errors of the first map load, then a second load: since_map_load keeps the autorun errors of the current map.
    call("map_load", dict(I, map="gm_construct", timeout_s=120), 180)
    con, _ = call("console_read", dict(I, regex="mcp .*autorun|not loaded|mcpagent", limit=40))
    print("   console:", [l.get("text") for l in con.get("lines", [])][-12:], flush=True)
    sc, err = call("errors_since", dict(I, cursor=0, since_map_load=True))
    msgs = [e.get("message", "") for e in sc.get("errors", [])]
    cur = sc.get("map_load")
    check("FIXES since_map_load lists the autorun errors raised while this map loaded",
          all(any(t in m for m in msgs) for t in ("mcp shared autorun error", "mcp server autorun error", "mcp early server autorun error"))
          and all(e.get("map_load") == cur for e in sc.get("errors", [])), {"map_load": cur, "messages": msgs})
    sc_all, _ = call("errors_since", dict(I, cursor=0))
    n_all = len([e for e in sc_all.get("errors", []) if "mcp server autorun error" in e.get("message", "")])
    n_cur = len([m for m in msgs if "mcp server autorun error" in m])
    check("FIXES the previous map's copy of the error is not in since_map_load", n_all >= 2 and n_cur == 1, {"all": n_all, "current": n_cur})
    # Async job ids across a map change.
    sc, err = call("lua_job", dict(I, realm="server", code="job:sleep(600) return 1", **{"async": True}))
    old = sc.get("job_id", "")
    check("FIXES async job id carries the generation", "-g" in old, sc)
    call("map_load", dict(I, map="gm_construct", timeout_s=120), 180)
    sc, err = call("lua_job", dict(I, realm="server", code="return 2", **{"async": True}))
    new = sc.get("job_id", "")
    check("FIXES the new map's first job has another id", new and new != old, [old, new])
    sc, err = call("lua_job_status", dict(I, id=old))
    check("FIXES the old id answers realm_reset", err and "realm_reset" in json.dumps(sc), sc)
    sc, err = call("lua_job_cancel", dict(I, id=old))
    check("FIXES cancelling the old id answers realm_reset too", err and "realm_reset" in json.dumps(sc), sc)
    sc, err = call("lua_job_status", dict(I, id=new, wait_s=10))
    check("FIXES the new job is untouched and returns", sc.get("values") == [2], sc)
    call("instances_close", {"owner": "fixes"}, 60)
    b.close()
    shutil.rmtree(base, ignore_errors=True)


def tool_names(srv):
    with srv.lock: return [b["params"]["name"] for b, _ in srv.seen if b.get("method") == "tools/call"]


def case_handover():
    """Issue #6: the coordinator role moves (the game on the base port ends, another takes over a few seconds later).
    The bridge rides it out instead of answering game_not_running / the raw coordinator_changing, but only when other
    instances are known to live (it never makes a call wait when the only game is closed)."""
    print("== HANDOVER: a call survives the coordinator role moving to another game", flush=True)
    cluster = lambda: {"instances": [{"id": "client-1", "pid": 1, "port": 27081, "mode": "client", "state": "running", "role": "coordinator", "self": True},
                                     {"id": "client-2", "pid": 2, "port": 27082, "mode": "client", "state": "running", "role": "member"}],
                       "coordinator_id": "client-1"}
    env0 = isolated_config(with_token=True)
    env0["GMOD_MCP_HANDOVER_WAIT_S"] = "4"  # the real wait is 10 s: the tests that give up do not need that long
    # 1. Nothing else known to live: the game closes, the next call says game_not_running at once (no wait added).
    port = free_port()
    srv = FakeServer(port, version=bridge_version())
    b = Bridge(port, env0)
    r = b.req("tools/call", {"name": "echo", "arguments": {}})
    check("HANDOVER a call is answered while the game runs", r and not r["result"].get("isError"), r)
    srv.stop()
    t0 = time.time()
    r = b.req("tools/call", {"name": "echo", "arguments": {}}, 20)
    check("HANDOVER the only game closed: game_not_running at once, no wait",
          r and "game_not_running" in json.dumps(r) and time.time() - t0 < 2.5, (time.time() - t0, r))
    b.close()
    # 2. Several instances live (the bridge saw them in instances_list): the coordinator ends, another takes the port 2 s later.
    port = free_port()
    srv = FakeServer(port, version=bridge_version()); srv.instances_answer = cluster
    b = Bridge(port, env0)
    r = b.req("tools/call", {"name": "instances_list", "arguments": {}})
    check("HANDOVER instances_list answers with the coordinator named", r and r["result"]["structuredContent"].get("coordinator_id") == "client-1", r)
    srv.stop()
    new = {}
    threading.Thread(target=lambda: (time.sleep(2.0), new.setdefault("srv", FakeServer(port, version=bridge_version()))), daemon=True).start()
    t0 = time.time()
    r = b.req("tools/call", {"name": "echo", "arguments": {"instance": "client-2"}}, 25)
    took = time.time() - t0
    check("HANDOVER a call for another instance waits for the new coordinator and succeeds",
          r and not r["result"].get("isError") and r["result"]["structuredContent"]["args"].get("instance") == "client-2" and 1.5 < took < 4, (took, r))
    # 3. The new coordinator says coordinator_changing twice (members not registered yet): retried, the answer is the real one.
    if "srv" not in new:  # the bridge answered before the new coordinator existed (red run): bring it up anyway
        time.sleep(2.5); new["srv"] = new.get("srv") or FakeServer(port, version=bridge_version())
    new["srv"].changing = 2
    t0 = time.time()
    r = b.req("tools/call", {"name": "echo", "arguments": {"instance": "client-2"}}, 25)
    check("HANDOVER coordinator_changing is retried, the caller gets the real answer",
          r and not r["result"].get("isError") and r["result"]["structuredContent"]["args"].get("instance") == "client-2" and new["srv"].changing == 0, (time.time() - t0, r))
    # 4. It never recovers: the caller gets the retryable coordinator_changing (with a delay), not game_not_running.
    new["srv"].changing = 10 ** 6
    t0 = time.time()
    r = b.req("tools/call", {"name": "echo", "arguments": {"instance": "client-2"}}, 25)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    check("HANDOVER still changing after the wait: the retryable coordinator_changing, relayed",
          r and r["result"].get("isError") and sc.get("error", {}).get("code") == "coordinator_changing" and 3 < time.time() - t0 < 12, (time.time() - t0, r))
    new["srv"].stop()
    t0 = time.time()
    r = b.req("tools/call", {"name": "echo", "arguments": {"instance": "client-2"}}, 25)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    check("HANDOVER nobody takes the role over: coordinator_changing with retry_after_s after the wait (not game_not_running)",
          r and r["result"].get("isError") and sc.get("error", {}).get("code") == "coordinator_changing" and sc["error"].get("details", {}).get("retry_after_s", 0) > 0
          and 3 < time.time() - t0 < 12, (time.time() - t0, r))
    b.close()


def case_launch_mount_first():
    """Issue #6: instances_launch never leaves a client without the requested lua_mount. The mount is checked (and
    re-applied when it is missing) BEFORE any map loads; a client that still has none gets no map and a clear error;
    a hand-over answer while checking is waited out."""
    print("== LAUNCH_MOUNT_FIRST: the mount is checked before the map loads", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("LAUNCH_MOUNT_FIRST stand-in game built next to the bridge", False, fake)
        return
    import shutil
    game = tempfile.mkdtemp(prefix="gmcp-fake-game-")
    shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
    work = tempfile.mkdtemp(prefix="gmcp-mount-")
    wt = os.path.join(work, "wt")
    os.makedirs(os.path.join(wt, "lua"))
    pid_file = os.path.join(work, "pid.txt")
    port, env = free_port(), isolated_config(with_token=True)
    env["GMCP_FAKE_PID"] = pid_file
    srv = FakeServer(port, version=bridge_version())
    srv.pid_file = pid_file
    srv.map_hold = {"*": 0.1}
    b = Bridge(port, env)
    state, answers = {"m": None, "seq": 0}, {}
    srv.lua_mount_answer = lambda a: answers["now"](a)

    def launch():
        if os.path.exists(pid_file): os.remove(pid_file)
        with srv.lock: del srv.seen[:]
        state["seq"] = 0
        r = b.req("tools/call", {"name": "instances_launch", "arguments": {"count": 1, "owner": "lmf", "timeout_s": 20, "game_dir": game,
                                                                          "lua_mount": wt, "map": "gm_test"}}, 60)
        res = (r or {}).get("result", {})
        b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "lmf"}}, 20)
        sc = res.get("structuredContent", {})
        return res, sc, (sc.get("instances") or [{}])[0], tool_names(srv)

    # 1. The client has no mount (the module did not apply it): re-applied with lua_mount {path}, then the map loads.
    def missing_then_ok(a):
        if "path" in a: state["m"] = a["path"]
        return {"lua_mount": state["m"], "base_lua_mounted": True}
    answers["now"] = missing_then_ok
    res, sc, inst, names = launch()
    check("LAUNCH_MOUNT_FIRST a missing mount is re-applied and verified before the map loads",
          not res.get("isError") and inst.get("lua_mount", {}).get("active") is True and inst.get("lua_mount", {}).get("reapplied") is True
          and inst.get("map") == "gm_test" and names.index("lua_mount") < names.index("map_load"), (names, sc))
    # 2. Every check says "no mount" and the re-apply fails: no map is loaded, the client says why.
    state["m"] = None
    answers["now"] = lambda a: ({"error": {"code": "unsupported_platform", "message": "cannot mount"}} if "path" in a else {"lua_mount": None, "base_lua_mounted": True})
    res, sc, inst, names = launch()
    check("LAUNCH_MOUNT_FIRST a mount that cannot be applied: no map_load at all, clear error, isError",
          res.get("isError") and "map_load" not in names and inst.get("lua_mount", {}).get("active") is False and inst.get("map") is None
          and inst.get("map_error", {}).get("code") == "lua_mount_not_active" and wt in inst.get("map_error", {}).get("message", ""), (names, sc))
    # 3. The coordinator is changing while the mount is checked: waited out, the mount counts as active, nothing re-applied.
    state["m"] = wt
    seq = {"n": 0}
    def changing_twice(a):
        seq["n"] += 1
        if seq["n"] <= 2: return {"error": {"code": "coordinator_changing", "message": "moving", "details": {"retry_after_s": 0.2}}}
        return {"lua_mount": state["m"], "base_lua_mounted": True}
    answers["now"] = changing_twice
    res, sc, inst, names = launch()
    paths = [c for c in srv.lua_mount_calls if "path" in c]
    check("LAUNCH_MOUNT_FIRST coordinator_changing while checking: retried, mount active, map loaded, nothing re-applied",
          not res.get("isError") and inst.get("lua_mount", {}).get("active") is True and inst.get("map") == "gm_test" and not any(c.get("path") == wt and c.get("instance") for c in paths[2:]), (names, sc, paths))
    b.close(); srv.stop()
    shutil.rmtree(game, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)


def case_close_coordinator():
    """Issue #6: instances_close on a client that is the coordinator while other instances live says so and reports the
    instance that took the role; instances_list warns about it beforehand."""
    print("== CLOSE_COORDINATOR: closing the coordinator says the role moves", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    if not os.path.exists(fake):
        check("CLOSE_COORDINATOR stand-in game built next to the bridge", False, fake)
        return
    import shutil
    game = tempfile.mkdtemp(prefix="gmcp-fake-game-")
    shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
    work = tempfile.mkdtemp(prefix="gmcp-cc-")
    pid_file = os.path.join(work, "pid.txt")
    port, env = free_port(), isolated_config(with_token=True)
    env["GMCP_FAKE_PID"] = pid_file
    srv = FakeServer(port, version=bridge_version())
    srv.pid_file = pid_file
    st = {"closed": False, "others": True}

    def directory():
        pid = int(open(pid_file).read().strip() or 0)
        me = "fake-%d" % pid
        other = {"id": "other-1", "pid": 4242, "port": 27082, "mode": "client", "state": "running", "role": "member", "self": False}
        if not st["closed"]:
            ents = [{"id": me, "pid": pid, "port": 27081, "mode": "client", "state": "running", "role": "coordinator", "self": True}]
            ents += [other] if st["others"] else []
            return {"instances": ents, "coordinator_id": me}
        other.update(role="coordinator", self=True)
        return {"instances": [other], "coordinator_id": "other-1"}
    srv.instances_answer = directory
    srv.terminate_hook = lambda a: st.update(closed=True)
    b = Bridge(port, env)
    r = b.req("tools/call", {"name": "instances_launch", "arguments": {"count": 1, "owner": "cc", "timeout_s": 20, "game_dir": game}}, 60)
    check("CLOSE_COORDINATOR launched", r and not r["result"].get("isError"), r)
    r = b.req("tools/call", {"name": "instances_list", "arguments": {}}, 20)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    check("CLOSE_COORDINATOR instances_list warns that a client of this session (owner cc) is the coordinator",
          "coordinator" in sc.get("coordinator_warning", "") and "cc" in sc.get("coordinator_warning", "") and "instances_close" in sc.get("coordinator_warning", ""), sc)
    r = b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "cc"}}, 40)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    c = (sc.get("closed") or [{}])[0]
    check("CLOSE_COORDINATOR instances_close names the role move and the instance that took it over",
          c.get("was_coordinator") is True and c.get("coordinator_moved_to") == "other-1" and "coordinator" in sc.get("warning", ""), sc)
    # Alone: closing the coordinator says nothing (nothing to hand over).
    st.update(closed=False, others=False)
    r = b.req("tools/call", {"name": "instances_launch", "arguments": {"count": 1, "owner": "cc2", "timeout_s": 20, "game_dir": game}}, 60)
    r = b.req("tools/call", {"name": "instances_list", "arguments": {}}, 20)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    check("CLOSE_COORDINATOR alone: no coordinator warning", "coordinator_warning" not in sc, sc)
    r = b.req("tools/call", {"name": "instances_close", "arguments": {"owner": "cc2"}}, 40)
    sc = (r or {}).get("result", {}).get("structuredContent", {})
    c = (sc.get("closed") or [{}])[0]
    check("CLOSE_COORDINATOR alone: closing it says nothing about the role", "was_coordinator" not in c and "warning" not in sc, sc)
    b.close(); srv.stop()
    shutil.rmtree(game, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)


# --- Issue #8 (chaos tests): instances_launch and instances_close around unhealthy, crashed and foreign clients. ---
def chaos_session(extra_env=None, extra_instances=None, with_pid_dir=True):
    """A bridge, a fake game server and a temporary game folder holding the stand-in game. Returns a dict with call(tool, args)."""
    import shutil
    fake = os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")
    game = tempfile.mkdtemp(prefix="gmcp-chaos-game-")
    shutil.copy(fake, os.path.join(game, "gmod_win64.exe"))
    os.makedirs(os.path.join(game, "garrysmod")); crashes = os.path.join(game, "crashes"); os.makedirs(crashes)
    work = tempfile.mkdtemp(prefix="gmcp-chaos-"); pid_dir = os.path.join(work, "pids"); os.makedirs(pid_dir)
    port, env = free_port(), isolated_config(with_token=True)
    env.update(GMCP_FAKE_CRASH_DIR=crashes, GMCP_FAKE_ARGV=os.path.join(work, "argv.txt"), GMOD_MCP_FP7137_WORKAROUND="0",
               GMOD_MCP_FAKE_FREE_COMMIT_MB="100000", GMOD_MCP_FAKE_FREE_RAM_MB="100000")
    if with_pid_dir: env["GMCP_FAKE_PID_DIR"] = pid_dir
    env.update(extra_env or {})
    srv = FakeServer(port, version=bridge_version()); srv.pid_dir = pid_dir
    srv.extra_instances = [dict(e) for e in (extra_instances or [])]
    b = Bridge(port, env)
    S = {"game": game, "work": work, "pid_dir": pid_dir, "srv": srv, "b": b, "crashes": crashes}
    def call(tool, args, timeout=90):
        r = b.req("tools/call", {"name": tool, "arguments": args}, timeout)
        res = (r or {}).get("result", {})
        return res.get("structuredContent", {}), res.get("isError")
    S["call"] = call
    def started(): return [int(open(os.path.join(pid_dir, f)).read() or 0) for f in os.listdir(pid_dir)]
    S["started"] = started
    def close():
        try: call("instances_close", {"all": True}, 30)
        except Exception: pass
        b.close(); srv.stop()
        shutil.rmtree(game, ignore_errors=True); shutil.rmtree(work, ignore_errors=True)
    S["close"] = close
    return S


def case_chaos_health():
    """Issue #8 (chaos test G1): a launch while a running client is frozen (unresponsive or hung) would time out with `0 of 1
    clients answered` (a frozen client blocks a new one from registering, FP7137 handshake): it is refused up front with
    peer_unhealthy unless ignore_unhealthy, and a timeout names the unhealthy peers seen meanwhile."""
    print("== CHAOS_HEALTH: peer_unhealthy before a launch, unhealthy peers named at a timeout", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    if not os.path.exists(os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")):
        check("CHAOS_HEALTH stand-in game built next to the bridge", False)
        return
    me = os.getpid()
    # 1. A frozen client (unresponsive) is running: the launch is refused, nothing started, the error names it and the way out.
    S = chaos_session(extra_instances=[{"id": "frozen-1", "pid": me, "port": 1, "mode": "client", "state": "unresponsive", "stalled_s": 25.0}])
    sc, err = S["call"]("instances_launch", {"count": 1, "owner": "g1", "timeout_s": 30, "game_dir": S["game"]})
    e, d = sc.get("error", {}), sc.get("error", {}).get("details", {})
    check("CHAOS_HEALTH an unresponsive client running: peer_unhealthy, isError, nothing started", e.get("code") == "peer_unhealthy" and err is True and S["started"]() == [], sc)
    check("CHAOS_HEALTH the message names the instance, its pid and state, and the way out (instances_close, game_terminate, ignore_unhealthy)",
          all(w in e.get("message", "") for w in ("frozen-1", str(me), "unresponsive", "instances_close", "game_terminate", "ignore_unhealthy")), e)
    check("CHAOS_HEALTH details.unhealthy lists {instance, pid, state}", d.get("unhealthy") == [{"instance": "frozen-1", "pid": me, "state": "unresponsive"}], d)
    # 2. ignore_unhealthy: true starts anyway.
    sc, err = S["call"]("instances_launch", {"count": 1, "owner": "g1", "timeout_s": 30, "game_dir": S["game"], "ignore_unhealthy": True})
    check("CHAOS_HEALTH ignore_unhealthy: the client starts and answers", len(S["started"]()) == 1 and not sc.get("error"), sc)
    S["close"]()
    # 3. A hung client counts too; a healthy one (running) and a crashed one do not.
    S = chaos_session(extra_instances=[{"id": "hung-1", "pid": me, "port": 1, "mode": "client", "state": "hung", "stalled_s": 100.0},
                                       {"id": "ok-1", "pid": me + 1, "port": 1, "mode": "client", "state": "running"},
                                       {"id": "dead-1", "pid": me + 2, "port": 1, "mode": "client", "state": "crashed"}])
    sc, err = S["call"]("instances_launch", {"count": 1, "owner": "g1", "timeout_s": 30, "game_dir": S["game"]})
    d = sc.get("error", {}).get("details", {})
    check("CHAOS_HEALTH a hung client is refused too; only it is named (not the running or the crashed one)", sc.get("error", {}).get("code") == "peer_unhealthy" and [u["instance"] for u in d.get("unhealthy", [])] == ["hung-1"], sc)
    S["close"]()
    S = chaos_session(extra_instances=[{"id": "ok-1", "pid": me + 1, "port": 1, "mode": "client", "state": "running"},
                                       {"id": "dead-1", "pid": me + 2, "port": 1, "mode": "client", "state": "crashed"}])
    sc, err = S["call"]("instances_launch", {"count": 1, "owner": "g1", "timeout_s": 30, "game_dir": S["game"]})
    check("CHAOS_HEALTH only healthy and crashed peers: the launch goes ahead", len(S["started"]()) == 1 and not sc.get("error"), sc)
    S["close"]()
    # 4. A peer freezes while the launch waits for its client (which never answers): the timeout names it.
    S = chaos_session(extra_instances=[{"id": "soon-frozen", "pid": me, "port": 1, "mode": "client", "state": "running"}], with_pid_dir=False)
    def freeze():
        time.sleep(3)
        S["srv"].extra_instances[0].update(state="unresponsive", stalled_s=12.0)
    threading.Thread(target=freeze, daemon=True).start()
    sc, err = S["call"]("instances_launch", {"count": 1, "owner": "g1", "timeout_s": 10, "game_dir": S["game"]}, 60)
    e = sc.get("error", {})
    check("CHAOS_HEALTH timeout: 0 of 1 clients answered, and the unhealthy peer seen meanwhile is named in details and message",
          e.get("code") == "timeout" and e.get("details", {}).get("unhealthy_peers") == [{"instance": "soon-frozen", "pid": me, "state": "unresponsive"}]
          and "soon-frozen" in e.get("message", "") and "unresponsive" in e.get("message", ""), sc)
    S["close"]()


def case_chaos_launch():
    """Issue #8 (chaos test G2): instances_launch {count: 2} where the first process dies ~1 s after it was spawned (before it
    registered) while the second keeps starting: the error must name the sibling that still runs (pid, instance if known) and
    carry a warning, so the caller does not leak it (stand-in game with GMCP_FAKE_DIE_ONCE)."""
    print("== CHAOS_LAUNCH: a client dies during startup while its sibling keeps running", flush=True)
    if os.name != "nt":
        print("   (Windows only: instances_launch is Windows only)", flush=True)
        return
    if not os.path.exists(os.path.join(os.path.dirname(BRIDGE), "gmod-mcp-fakegame.exe")):
        check("CHAOS_LAUNCH stand-in game built next to the bridge", False)
        return
    work = tempfile.mkdtemp(prefix="gmcp-chaos-once-")
    S = chaos_session(extra_env={"GMCP_FAKE_DIE_ONCE": os.path.join(work, "claimed"), "GMCP_FAKE_EXIT": "3221225477"})
    sc, err = S["call"]("instances_launch", {"count": 2, "owner": "g2", "timeout_s": 40, "game_dir": S["game"]})
    e = sc.get("error", {})
    alive_pids = [p for p in S["started"]() if alive(p)]
    check("CHAOS_LAUNCH the dead first client is reported (client_crashed, isError)", e.get("code") == "client_crashed" and err is True, sc)
    sib = (e.get("details") or {}).get("still_running") or []
    check("CHAOS_LAUNCH details.still_running names the sibling that keeps running (pid, state, instance when it answered)",
          len(alive_pids) == 1 and [x.get("pid") for x in sib] == alive_pids and all(x.get("state") in ("answering", "still starting") for x in sib)
          and (sib[0].get("state") != "answering" or sib[0].get("instance") == "fake-%d" % alive_pids[0]), {"alive": alive_pids, "sc": sc})
    w = sc.get("warning", "")
    check("CHAOS_LAUNCH a warning says the sibling is NOT closed and how (pid, instances_close)",
          bool(alive_pids) and str(alive_pids[0]) in w and "instances_close" in w and "g2" in w and "not closed" in w.lower(), sc)
    sc2, _ = S["call"]("instances_close", {"owner": "g2"}, 30)
    check("CHAOS_LAUNCH the sibling is still closable by owner", bool(alive_pids) and alive_pids[0] in [c.get("pid") for c in sc2.get("closed", [])], sc2)
    S["close"]()
    import shutil; shutil.rmtree(work, ignore_errors=True)


CASES = {"a": case_a, "f": case_f, "m": case_m, "multi": case_multi, "session": case_session, "launch": case_launch, "orphans": case_orphans, "lua_mount": case_lua_mount, "win64_layout": case_win64_layout, "lua_mount_verify": case_lua_mount_verify, "launch_options": case_launch_options, "crash": case_crash, "lowmem": case_lowmem, "fp7137": case_fp7137, "orphan": case_orphan, "fuzz": case_fuzz, "fslow": lambda: case_f(slow=True), "b": case_b, "c": case_c, "retex": case_retex, "lua_mount_live": case_lua_mount_live, "edge_live": case_edge_live, "lua_mount_ecosystem": case_lua_mount_ecosystem, "edge": case_edge, "version": case_version, "version_live": case_version_live, "observe": case_observe, "feedback": case_feedback, "convar_crash": case_convar_crash, "fixes_live": case_fixes_live, "portconflict": case_portconflict, "unseen": case_unseen_loads, "handover": case_handover, "launch_mount_first": case_launch_mount_first, "close_coordinator": case_close_coordinator, "chaos_health": case_chaos_health, "chaos_launch": case_chaos_launch}
DEFAULT = ["a", "f", "m", "multi", "session", "orphans", "lua_mount", "win64_layout", "lua_mount_verify", "launch_options", "crash", "lowmem", "fp7137", "orphan", "edge", "version", "feedback", "portconflict", "unseen", "handover", "launch_mount_first", "close_coordinator", "chaos_health", "chaos_launch", "fuzz"]
args = sys.argv[2:]
if args[:1] == ["--serial"] or len(args) == 1:
    # One case at a time in this process (also what each parallel worker runs).
    for c in [a for a in args if a != "--serial"] or DEFAULT:
        t_case = time.time()
        CASES[c]()
        print("   [%s: %.1f s]" % (c, time.time() - t_case), flush=True)
else:
    # Several cases: each in its own process, all at once (they use their own ports, configs and stand-in games), so
    # the whole default suite takes about as long as its slowest case. Output is printed per case, in order.
    t0 = time.time()
    names = args or DEFAULT
    procs = [(c, subprocess.Popen([sys.executable, "-u", __file__, BRIDGE, "--serial", c], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, env=dict(os.environ, PYTHONIOENCODING="utf-8"))) for c in names]
    for c, pr in procs:
        out = pr.communicate()[0].decode("utf-8", "replace")
        for line in out.splitlines():
            if line.startswith("TOTAL "):
                continue
            print(line, flush=True)
            if line.startswith("PASS "): results.append((line[5:], True))
            elif line.startswith("FAIL "): results.append((line[5:], False))
        if pr.returncode != 0 and not any(l.startswith(("PASS ", "FAIL ")) for l in out.splitlines()):
            results.append(("case %s crashed (exit %d)" % (c, pr.returncode), False))
    print("   [all %d cases in parallel: %.1f s]" % (len(names), time.time() - t0), flush=True)
passed = sum(1 for _, ok in results if ok)
print("TOTAL %d/%d" % (passed, len(results)))
sys.exit(0 if passed == len(results) else 1)
