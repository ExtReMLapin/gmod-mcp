"""Issue #3, live check of the outputSchemas against REAL results. Not part of CI (needs Windows, Steam and Garry's Mod).

It starts ONE test client of its own through the bridge (instances_launch, light preset, -nosound, owner "schema-check"),
loads gm_construct, calls every tool that is safe on it (read-only calls, and a spawn/remove of one prop, a few harmless
convar writes of the value it already has, a temporary file in lua/mcpagent_tmp it deletes), validates each success result
against the outputSchema the module declares in tools/list, optionally merges the results into tests/schema/results.json as
"live" cases, and closes its client (and only that one). Calls that fail on purpose (isError) are reported, not validated:
the clients do not validate them either.

  python tests/schema/live_check.py <bridge exe> [--game-dir DIR] [--out captured.json] [--merge]
  python tests/schema/live_check.py --selftest        # the whole flow against a stand-in bridge, no game

Needs `pip install jsonschema`. The module in the game must be this branch's build (older ones declare no outputSchema).
"""
import json, os, subprocess, sys, threading, queue, time

try:
    import jsonschema
except ImportError:
    jsonschema = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIXTURES = os.path.join(HERE, "results.json")
INIT = {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "schema-check", "version": "0"}}


class Client:
    """A JSON-RPC client over the stdio of a bridge process."""
    def __init__(self, argv):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.q, self.n = queue.Queue(), 0
        threading.Thread(target=lambda: [self.q.put(json.loads(l)) for l in self.p.stdout], daemon=True).start()

    def req(self, method, params=None, timeout=60):
        self.n += 1
        rid = self.n
        self.p.stdin.write((json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}) + "\n").encode())
        self.p.stdin.flush()
        end = time.time() + timeout
        while time.time() < end:
            try:
                m = self.q.get(timeout=1)
            except queue.Empty:
                continue
            if m.get("id") == rid:
                return m
        return None

    def notify(self, method):
        self.p.stdin.write((json.dumps({"jsonrpc": "2.0", "method": method}) + "\n").encode())
        self.p.stdin.flush()

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(10)
        except Exception:
            self.p.kill()


def calls(inst):
    """(tool, arguments, note) in the order they are made. Every call targets the client this script started."""
    I = {"instance": inst}
    obj = lambda **kw: dict(I, **kw)
    cls = {"class": "prop_physics"}
    return [
        ("status", obj(), ""), ("status", obj(fields=["map", "realms", "perf"]), "fields"), ("instances_list", {}, ""),
        ("lua_exec", obj(realm="server", code="print('hi') return 3, Vector(1,2,3), {a=1}"), ""),
        ("lua_exec", obj(realm="server", code="return nil"), "nil"),
        ("lua_exec", obj(realm="server", code="local t={} for i=1,100 do t[i]=i end return t", limit=10), "cut"),
        ("lua_exec", obj(realm="client", code="return ScrW(), ScrH()"), "client"),
        ("lua_exec", obj(realm="menu", code="return 1"), "menu"),
        ("lua_exec", obj(realm="server", code="return 1", fields=["x"]), "fields"),
        ("lua_exec", obj(realm="server", code="return 1/0, 0/0"), "inf nan"),
        ("lua_file_write", obj(name="schema_check_tmp.lua", content="return 42\n"), ""),
        ("lua_file_read", obj(name="schema_check_tmp.lua"), ""), ("lua_file_list", obj(), ""),
        ("lua_exec_file", obj(realm="server", path="schema_check_tmp.lua"), ""),
        ("lua_reload", obj(path="mcpagent_tmp/schema_check_tmp.lua", realms=["server"]), ""),
        ("lua_check", obj(realm="server", code="x = 1 return y"), ""),
        ("lua_check", obj(realm="server", code="local = 1"), "syntax error"),
        ("lua_check", obj(realm="server", path="schema_check_tmp.lua"), "file"),
        ("lua_inspect", obj(realm="server", expr="FindMetaTable('Player')", depth=0, limit=5), ""),
        ("lua_inspect", obj(realm="server", expr="42"), "number"),
        ("lua_function_source", obj(realm="server", expr="print"), "C function"),
        ("lua_function_source", obj(realm="server", expr="hook.Add"), "engine Lua"),
        ("lua_file_delete", obj(name="schema_check_tmp.lua"), ""),
        ("lua_mount", obj(), ""), ("map_list", obj(filter="gm_", limit=5), ""), ("wait_frames", obj(n=5), ""),
        ("wait_for", obj(realm="server", condition="true"), ""),
        ("console_exec", obj(cmd="echo schema-check"), ""), ("console_exec", obj(cmd="sv_gravity 600"), "convar"),
        ("console_wait", obj(regex="schema-check", count=1, timeout_s=3), ""), ("console_read", obj(limit=5), ""),
        ("console_buffer", obj(), ""), ("errors_since", obj(), ""), ("errors_clear", obj(), ""),
        ("convar_get", obj(name="sv_gravity", realm="server"), ""), ("convar_get", obj(name="host_timescale"), "menu"),
        ("convar_set", obj(name="sv_gravity", value=600, realm="server"), ""), ("concommand_list", obj(filter="sv_grav"), ""),
        ("ents_find", obj(realm="server", limit=5), ""), ("ents_find", obj(realm="server", near=[0, 0, 64], radius=500, limit=3), "near"),
        ("ents_find", obj(realm="client", limit=3), "client"), ("ents_find", obj(realm="server", **{"class": "no_such_class_zzz"}), "none"),
        ("ent_spawn", obj(**cls, model="models/props_c17/oildrum001.mdl", pos=[0, 0, 200], freeze=True), "SPAWN"),
        ("ent_info", obj(id="$spawned", realm="server"), ""), ("ent_fire", obj(id="$spawned", input="Wake"), ""),
        ("ent_remove", obj(id="$spawned"), ""),
        ("ent_info", obj(id=1, realm="server"), "player"), ("ent_info", obj(id=0, realm="server"), "world"),
        ("players_list", obj(realm="server"), ""), ("players_list", obj(realm="client"), "client"),
        ("player_set", obj(noclip=False, god=False), ""), ("bots_add", obj(n=1), "single player: no slot, an error"),
        ("trace", obj(realm="server", start=[0, 0, 100], end=[0, 0, -2000]), ""), ("trace", obj(realm="server", start=[0, 0, 100], end=[0, 0, 5000]), "no hit"),
        ("trace_through", obj(realm="server", start=[0, 0, 100], end=[0, 0, -2000]), ""),
        ("screenshot", obj(w=320, h=180), ""), ("screenshot_from", obj(pos=[0, 0, 200], ang=[30, 0, 0], w=320, h=180), ""),
        ("debug_draw", obj(primitives=[{"type": "line", "from": [0, 0, 64], "to": [500, 0, 64]}], duration=5), ""), ("debug_clear", obj(), ""),
        ("sound_emit", obj(name="buttons/button1.wav", pos=[0, 0, 64], level=60), ""), ("sound_trace", obj(realm="server", duration_s=1), ""),
        ("hook_list", obj(realm="server"), ""), ("hook_list", obj(realm="server", event="Think"), "event"),
        ("hook_list", obj(realm="server", event="Thinkk"), "unknown event"),
        ("hook_trace", obj(realm="server", event="Think", duration_s=1, max_calls=3), ""),
        ("perf_hook_cost", obj(realm="server", event="Think", duration_s=1), ""), ("perf_snapshot", obj(), ""),
        ("tests_run", obj(realm="server", path=os.path.join(ROOT, "tests", "selftest_pass.lua").replace("\\", "/")), ""),
        ("api_search", obj(pattern="Entity:EmitSo*"), ""), ("wiki_search", {"query": "TraceLine"}, ""), ("wiki_page", {"name": "util.TraceLine"}, ""),
        ("events_wait", obj(types=["map_loaded"], timeout_s=1), ""), ("reports_channels", obj(), ""),
        ("reports_read", obj(channel="none", cursor=0, wait_s=0), ""), ("reports_clear", obj(), ""),
        ("lua_job", obj(realm="server", code="job:frames(3) return 7", **{"async": True}), "JOB"),
        ("lua_job_status", obj(id="$job", wait_s=5), ""),
        ("lua_job", obj(realm="server", code="job:frames(2) job:report({a=1}) return 5"), "sync"),
        ("lua_job", obj(realm="server", code="job:sleep(30) return 1", **{"async": True}), "JOB to cancel"),
        ("lua_job_status", obj(), "every job"), ("lua_job_cancel", obj(id="$job"), ""),
        ("map_load", obj(map="gm_construct", timeout_s=240), ""),
    ]


def run(argv_bridge, game_dir, merge, out, selftest=False):
    c = Client(argv_bridge)
    c.req("initialize", INIT)
    c.notify("notifications/initialized")
    tl = c.req("tools/list", {}, 30)
    tools = {t["name"]: t for t in (tl or {}).get("result", {}).get("tools", [])}
    captured, bad, checked, errors = {}, [], 0, []
    inst = None
    try:
        la = {"count": 1, "map": "gm_construct", "owner": "schema-check", "sound": False, "timeout_s": 300}
        if game_dir:
            la["game_dir"] = game_dir
        r = c.req("tools/call", {"name": "instances_launch", "arguments": la}, 330)
        sc = ((r or {}).get("result") or {}).get("structuredContent") or {}
        inst = ((sc.get("instances") or [{}])[0]).get("instance")
        if not inst:
            print("no client started:", json.dumps(sc)[:300]); return 1
        ids = {}
        for tool, args, note in calls(inst):
            args = {k: (ids.get(v, v) if isinstance(v, str) and v.startswith("$") else v) for k, v in args.items()}
            if "$" in json.dumps(args) or (tool in ("ent_info", "ent_fire", "ent_remove") and args.get("id") in (None, "$spawned")):
                continue  # its id was not produced (the spawn or the job failed)
            r = c.req("tools/call", {"name": tool, "arguments": args}, 300 if tool == "map_load" else 90)
            res = ((r or {}).get("result") or {})
            sc = res.get("structuredContent")
            if res.get("isError") or sc is None:
                errors.append((tool, note, json.dumps(sc)[:120]))
                continue
            if note == "SPAWN":
                ids["$spawned"] = sc.get("id")
            if note.startswith("JOB"):
                ids["$job"] = sc.get("job_id")
            schema = (tools.get(tool) or {}).get("outputSchema")
            checked += 1
            if jsonschema and schema:
                errs = list(jsonschema.Draft202012Validator(schema).iter_errors(sc))
                if errs:
                    bad.append((tool, note, "/".join(str(p) for p in errs[0].absolute_path), errs[0].message[:160]))
            captured.setdefault(tool, []).append({"case": ("live " + time.strftime("%Y-%m-%d") + " " + note).strip(), "result": sc})
    finally:
        if inst and not selftest:
            c.req("tools/call", {"name": "instances_close", "arguments": {"all": True}}, 60)
        c.close()
    if out:
        json.dump(captured, open(out, "w", encoding="utf-8"), indent=1)
    if merge and not bad:
        fx = json.load(open(FIXTURES, encoding="utf-8"))
        for tool, cases in captured.items():
            fx.setdefault(tool, []).extend(cases[:2])  # two live cases per tool are enough
        json.dump(fx, open(FIXTURES, "w", encoding="utf-8"), indent=1)
        print("merged into", FIXTURES)
    print("checked %d results, %d do not match their outputSchema, %d calls answered an error (not validated)" % (checked, len(bad), len(errors)))
    for b in bad:
        print("MISMATCH", b)
    for e in errors:
        print("error answer", e)
    return 1 if bad else 0


def fake_bridge():
    """A stand-in bridge for --selftest: answers from tests/schema/results.json and the schemas of the snapshot."""
    snap = os.environ["SCHEMA_SNAPSHOT"]
    tl = json.load(open(snap, encoding="utf-8"))["tools_legacy"]
    tools = tl["tools"] if isinstance(tl, dict) else tl
    fx = json.load(open(FIXTURES, encoding="utf-8"))
    for line in sys.stdin:
        m = json.loads(line)
        if "id" not in m:
            continue
        meth, p = m["method"], m.get("params", {})
        if meth == "tools/list":
            res = {"tools": tools}
        elif meth == "tools/call" and p["name"] == "instances_launch":
            sc = {"instances": [{"instance": "client-1"}]}
            res = {"content": [], "structuredContent": sc, "isError": False}
        elif meth == "tools/call":
            if jsonschema:
                t = next((x for x in tools if x["name"] == p["name"]), None)
                assert t, p["name"]
                bad_args = [e.message for e in jsonschema.Draft202012Validator(t["inputSchema"]).iter_errors(p["arguments"])]
                if bad_args:
                    print("BAD ARGUMENTS for", p["name"], bad_args, file=sys.stderr)
                    res = {"content": [], "structuredContent": {"error": {"code": "invalid_params", "message": str(bad_args)}}, "isError": True}
                    sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": m["id"], "result": res}) + "\n"); sys.stdout.flush()
                    continue
            cases = fx.get(p["name"])
            sc = dict(cases[0]["result"]) if cases else {"error": {"code": "none", "message": "no fixture"}}
            if p["name"] == "ent_spawn":
                sc["id"] = 120
            res = {"content": [], "structuredContent": sc, "isError": not cases}
        else:
            res = {}
        sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": m["id"], "result": res}) + "\n")
        sys.stdout.flush()


def main():
    a = sys.argv[1:]
    if a[:1] == ["--fake"]:
        return fake_bridge()
    if a[:1] == ["--selftest"]:
        if jsonschema is None:
            print("selftest needs jsonschema"); return 2
        snap = os.path.join(ROOT, "build", "snapshot-for-selftest.json")
        exe = [os.path.join(ROOT, "build", n) for n in ("gmod-mcp-snapshot.exe", "gmod-mcp-snapshot")]
        exe = next((e for e in exe if os.path.exists(e)), None)
        if not exe:
            print("build gmod-mcp-snapshot first"); return 2
        subprocess.check_call([exe, snap])
        os.environ["SCHEMA_SNAPSHOT"] = snap
        return run([sys.executable, __file__, "--fake"], None, False, None, selftest=True)
    if not a or a[0].startswith("-"):
        print(__doc__); return 2
    opt = lambda n: a[a.index(n) + 1] if n in a else None
    if jsonschema is None:
        print("pip install jsonschema first"); return 2
    return run([os.path.abspath(a[0])], opt("--game-dir"), "--merge" in a, opt("--out"))


if __name__ == "__main__":
    sys.exit(main())
