"""Offline eval for issue #3: does a machine-usable `next_call` field next to the prose hints help or hurt?

No game, not part of CI. For each case it shows a model the result a gmod-mcp tool returned, in two variants, and
asks which tool call it would make next:

  current   the payload as the module / bridge sends it today
  proposed  the same payload plus a structured `next_call` {tool, arguments?, after_s?}, exactly what the module and the
            bridge now produce (src/next_call.*, tests/native/test_core.cpp test_next_call); the prose is kept

The model sees what Claude Code shows it: on success the compact JSON of structuredContent, on an error the JSON text.
It answers with ONE JSON object {"tool": ..., "arguments": {...}}; a per-case predicate grades it (tool and the
arguments that matter, not the exact wording). The payloads are taken from the code (see `src` reference per case).

Usage (needs the `claude` CLI, uses your login; about 80 short calls per model, cents):
  python tests/eval/hint_eval.py [--models haiku sonnet] [--runs 3] [--cases a,b] [--workers 4] [--out results.json]
  python tests/eval/hint_eval.py --sizes        # payload sizes only, no model call
  python tests/eval/hint_eval.py --selftest     # grader and payload checks, no model call
"""
import argparse, concurrent.futures, json, os, re, shutil, subprocess, sys, tempfile, time

CATALOG = """\
status {instance?, fields?}: instance status: map, gamemode, realms, fps, main_thread_stalled. Call it first.
instances_list {}: the running game instances (id, owner, pid, state).
instances_launch {count, map?, owner?, lua_mount?, timeout_s?}: start light test clients, optionally load a map in each.
instances_close {instances?, owner?, all?}: close test clients this session started.
instance_select {instance}: make an instance the default target of later calls.
map_list {filter?}: maps on disk.
map_load {map, timeout_s?, instance?}: load a map (creates the server and client realms).
lua_exec {code, realm, budget?, timeout_s?, depth?, limit?, max_string?, instance?}: run Lua in a realm and return its values.
lua_job {code, realm, async?, instance?}: run long Lua as a job; async:true returns a job id at once.
lua_job_status {id, wait_s?, instance?}: poll a job, optionally waiting for its end.
lua_mount {path, instance?}: make the game load Lua from this folder first (takes effect at the next map_load).
players_list {realm?, instance?}: list the players.
tests_run {path, realm?, instance?}: run a Lua test file.
console_exec {cmd, realm?, wait_frames?, regex?, instance?}: run a console command.
game_terminate {instance?}: kill the game process. Last resort.
wait_for {realm, condition, timeout_s?, instance?}: wait until a Lua condition is true.
"""

SYSTEM = ("You are a coding agent that drives Garry's Mod through the gmod-mcp tools below. You see the result of your "
          "last tool call and decide the next one. Reply with ONLY one JSON object {\"tool\": \"<name>\", "
          "\"arguments\": {...}}: the single next tool call you would make. No prose.\n\nTools:\n" + CATALOG)


def D(o):
    # Compact, keys sorted like the module's nlohmann::json writes them on the wire.
    return json.dumps(o, separators=(",", ":"), sort_keys=True)


def err(code, message, details=None, extra=None):
    e = {"code": code, "message": message}
    if details is not None:
        e["details"] = details
    r = {"error": e}
    r.update(extra or {})
    return r


def with_next(payload, nxt, in_error=False):
    """The proposed variant: the same payload plus next_call (inside `error` for an error result)."""
    p = json.loads(json.dumps(payload))
    (p["error"] if in_error else p)["next_call"] = nxt
    return p


def tool_of(c): return (c or {}).get("tool")
def args_of(c): return ((c or {}).get("arguments")) or {}


# Each case: id, task, call (what the model just called), is_error, current payload, proposed payload, grader, src.
CASES = []


def case(cid, task, call, is_error, cur, new, grade, src):
    CASES.append(dict(id=cid, task=task, call=call, is_error=is_error, cur=cur, new=new, grade=grade, src=src))


# 1. Control: identical payloads, measures the noise. The right next step is starting the game (not a gmod tool),
#    so anything reasonable passes; the payload is the same in both variants.
nr = err("game_not_running",
         "Garry's Mod is not reachable on 127.0.0.1:27080 (connection refused). Start the game with the -mcp launch option "
         "(the gmod-mcp skill knows how); the tools come back by themselves once it answers, no reconnection needed.",
         {"host": "127.0.0.1", "port": 27080, "reason": "connection refused"})
case("not_running", "Tell me which map is loaded in Garry's Mod.", 'status {}', True, nr, nr,
     lambda c: tool_of(c) in ("instances_launch", "status", "instances_list") or tool_of(c) in ("Bash", "Skill"),
     "bridge.cpp not_running_error (control: identical in both variants)")

# 2. realm_unavailable: the message names the missing realm but not what to do. next_call: map_load (no map chosen).
ru = err("realm_unavailable", "realm server unavailable", None, {"answered_by": "client-1"})
case("realm_unavailable", "List the players currently on the server.", 'players_list {"realm":"server"}', True, ru,
     with_next(ru, {"tool": "map_load"}, True),
     lambda c: tool_of(c) in ("map_load", "map_list"),
     "tools_session.cpp:160 / agent.cpp:573 (message only, no hint today); next_call.cpp")

# 3. map_load_in_progress (a launch refused while another game loads a map): the same call, later, more room to wait.
mp = {"error": {"code": "map_load_in_progress",
                "message": "a map is loading in another game (client-2): starting a game now would make it release its graphics "
                           "resources and could crash it (Garry's Mod issue 7137). Nothing was started: waited 20 s and the call's "
                           "timeout would be exceeded; retry later (or set GMOD_MCP_FP7137_WORKAROUND=0 to skip this wait)."},
      "waited_s": 20.0, "waited_reason": "a map is loading in another game (client-2)"}
case("map_load_in_progress", "Start 2 test clients with gm_construct.",
     'instances_launch {"count":2,"map":"gm_construct","timeout_s":30}', True, mp,
     with_next(mp, {"tool": "instances_launch", "arguments": {"count": 2, "map": "gm_construct", "timeout_s": 60}, "after_s": 15}, True),
     lambda c: tool_of(c) == "instances_launch" and args_of(c).get("count") == 2 and (args_of(c).get("timeout_s") or 0) >= 30,
     "fp7137.cpp Gate::error_result; bridge.cpp retry_later")

# 4. launch_in_progress (a map_load refused while a launch is in progress).
lp = {"error": {"code": "launch_in_progress",
                "message": "a game process was started 4 s ago: the other games release and restore their graphics resources for "
                           "several seconds after a start, and a map load ending meanwhile can crash (Garry's Mod issue 7137); "
                           "waiting 15 s after the last start. Nothing was started: waited 6 s and the call's timeout would be "
                           "exceeded; retry later (or set GMOD_MCP_FP7137_WORKAROUND=0 to skip this wait)."},
      "waited_s": 6.0, "waited_reason": "a game process was started 4 s ago"}
case("launch_in_progress", "Load gm_flatgrass on client-1.", 'map_load {"map":"gm_flatgrass","instance":"client-1","timeout_s":10}',
     True, lp,
     with_next(lp, {"tool": "map_load", "arguments": {"map": "gm_flatgrass", "instance": "client-1", "timeout_s": 60}, "after_s": 15}, True),
     lambda c: tool_of(c) == "map_load" and args_of(c).get("map") == "gm_flatgrass" and (args_of(c).get("timeout_s") or 0) > 10,
     "fp7137.cpp Gate::error_result (map_load)")

# 5. main_thread_timeout.
mt = err("main_thread_timeout", "the main thread did not respond in time (map loading, freeze or infinite loop?)",
         {"timeout_s": 60, "job_started": True, "seconds_since_last_frame": 61.4}, {"answered_by": "client-1"})
case("main_thread_timeout", "Run the heavy scan script on the server and give me the result.",
     'lua_exec {"code":"for i=1,1e12 do end","realm":"server","timeout_s":60}', True, mt,
     with_next(mt, {"tool": "status"}, True),
     lambda c: tool_of(c) in ("status", "wait_for"),
     "agent.cpp rt() main_thread_timeout")

# 6. client_crashed on a targeted call.
cc = err("client_crashed", "instance client-2 (pid 41212) crashed: access violation 0xC0000005, minidump "
         "gmod_win64_20261119_101212_1.mdmp. Relaunch a client with instances_launch and rerun what it was doing.",
         {"instance": "client-2", "pid": 41212, "dump": "gmod_win64_20261119_101212_1.mdmp", "exit_code": "0xC0000005",
          "evidence": "dump written 1 s before exit"})
case("client_crashed", "Run tests/my_suite.lua on client-2 and report the failures.",
     'tests_run {"path":"tests/my_suite.lua","instance":"client-2"}', True, cc,
     with_next(cc, {"tool": "instances_launch", "arguments": {"count": 1}}, True),
     lambda c: tool_of(c) == "instances_launch",
     "coordinator.cpp ended_error / guide 'A game crashed'")

# 7. lua_job started async: `next` is prose today.
jid = "server-g3-1"
jb = {"job_id": jid, "state": "running", "realm": "server", "generation": 3, "answered_by": "client-1",
      "next": 'lua_job_status {id: "' + jid + '"} polls it (wait_s to wait for the end) and returns the result once finished; '
              'lua_job_cancel stops it. An uncollected result is dropped after 1 h; the job dies with its realm (map change).'}
jb2 = dict(jb); jb2["next_call"] = {"tool": "lua_job_status", "arguments": {"id": jid, "wait_s": 30}}
case("job_started", "Scan every entity on the server with a job and tell me how many are props.", 'lua_job {"code":"...","realm":"server","async":true}',
     False, jb, jb2, lambda c: tool_of(c) == "lua_job_status" and args_of(c).get("id") == jid,
     "tools_jobs.cpp:146")

# 8. truncated values: next_call = the same call without the limits that cut.
tr = {"values": [[{"__type": "truncated", "length": 1812}]], "n": 1, "duration_ms": 3, "generation": 3, "chunkname": "=mcpagent:lua_exec",
      "truncated": 1,
      "truncated_note": "some values were cut by the depth / limit / max_string you passed: marked __type = \"truncated\" "
                        "or truncated = true. Never read a cut value as data; call again without these limits to get it whole.",
      "answered_by": "client-1"}
tr2 = with_next(tr, {"tool": "lua_exec", "arguments": {"code": "local t={} for _,e in ipairs(ents.GetAll()) do t[#t+1]=e:GetClass() end return t", "realm": "server"}})
case("truncated", "Give me the full list of entity classes currently on the server.",
     'lua_exec {"code":"local t={} for _,e in ipairs(ents.GetAll()) do t[#t+1]=e:GetClass() end return t","realm":"server","limit":50}',
     False, tr, tr2,
     lambda c: tool_of(c) == "lua_exec" and (args_of(c).get("limit") is None or args_of(c).get("limit") > 50),
     "lua/10_core.lua serialize_values truncated_note")

# 9. lua_mount not active on a client (instances_launch result with isError).
lm = {"instances": [{"instance": "client-3", "pid": 5120, "port": 27081, "owner": "wt", "lua_mount": {"active": False, "error": "folder not found"},
                     "map": "gm_construct"}],
      "game_dir": "D:/SteamLibrary/steamapps/common/GarrysMod/garrysmod", "found_by": "port", "preset": "light", "keep": False, "fps_max": 20,
      "lua_mount": "D:/dev/wt-mybranch",
      "fps_note": "these clients run at most 20 fps (fps_max, also when unfocused): per-frame costs and frame-to-frame behaviour differ "
                  "from a normal game; raise it with convar_set fps_max for timing work",
      "next": "Pass `instance` to target one, or instance_select to make one the default; instances_close when done.",
      "warning": "lua_mount D:/dev/wt-mybranch is NOT active on every client: client-3: folder not found. Tests on these clients would run "
                 "the installed addon's code (or no usable Lua): fix it with lua_mount {path, instance} then map_load, or close them (instances_close)."}
lm2 = with_next(lm, {"tool": "lua_mount", "arguments": {"path": "D:/dev/wt-mybranch", "instance": "client-3"}})
case("lua_mount_inactive", "Test my addon branch in D:/dev/wt-mybranch on a fresh client.",
     'instances_launch {"count":1,"map":"gm_construct","lua_mount":"D:/dev/wt-mybranch","owner":"wt"}', True, lm, lm2,
     lambda c: (tool_of(c) == "lua_mount" and "wt-mybranch" in str(args_of(c).get("path", ""))) or tool_of(c) == "instances_close",
     "bridge.cpp instances_launch warning")

# 10. invalid arguments: the message already names the right argument; next_call is the corrected call.
ia = err("invalid_params", "invalid arguments: [json.exception.out_of_range.403] required property 'cmd' not found in object. "
         "Unknown argument 'command' (did you mean 'cmd'?). Missing required argument 'cmd'. "
         "Accepted arguments: cmd (string), realm (string), wait_frames (integer), regex (string), instance (string).")
ia2 = with_next(ia, {"tool": "console_exec", "arguments": {"cmd": "echo hi"}}, True)
case("bad_argument", "Run the console command `echo hi`.", 'console_exec {"command":"echo hi"}', True, ia, ia2,
     lambda c: tool_of(c) == "console_exec" and args_of(c).get("cmd") == "echo hi",
     "mcp.cpp argument_hint / corrected_call")

# 11. ambiguous instance: several games share the endpoint.
am = err("ambiguous_instance", "several game instances share this endpoint: pass `instance` (see instances_list)",
         {"instances": ["client-1 (this one)", "client-2", "client-3"]})
am2 = with_next(am, {"tool": "instances_list"}, True)
case("ambiguous_instance", "Run `return #player.GetAll()` on the server of client-2.",
     'lua_exec {"code":"return #player.GetAll()","realm":"server"}', True, am, am2,
     lambda c: (tool_of(c) == "lua_exec" and args_of(c).get("instance") == "client-2") or tool_of(c) in ("instances_list",),
     "mcp.cpp ambiguous_instance")

# 12. instance_unreachable right after a launch: wait and retry, do not kill.
iu = err("instance_unreachable", "instance client-2 (pid 41212) stopped answering 17 s ago but its process is still running: frozen "
         "(infinite loop, long load) or busy. Check it with status {instance}; game_terminate {instance} ends it.")
iu2 = with_next(iu, {"tool": "status", "arguments": {"instance": "client-2"}, "after_s": 10}, True)
case("instance_unreachable", "Get the status of client-2 (a client was started a few seconds ago).", 'status {"instance":"client-2"}', True, iu, iu2,
     lambda c: tool_of(c) in ("status", "instances_list", "wait_for"),
     "coordinator.cpp gone_message (alive)")

# 13. budget_exceeded: the same code as a job.
bcode = "local n={} for _,e in ipairs(ents.GetAll()) do local c=e:GetClass() n[c]=(n[c] or 0)+1 end return n"
be = err("budget_exceeded", "the code used more than its 0.5 s budget (loops written in this code itself are stopped)",
         {"budget": 0.5}, {"answered_by": "client-1"})
be2 = with_next(be, {"tool": "lua_job", "arguments": {"code": bcode, "realm": "server", "async": True}}, True)
case("budget_exceeded", "Count the entities of each class on the server (it may take a few seconds).",
     'lua_exec {"code":"%s","realm":"server","budget":0.5}' % bcode, True, be, be2,
     lambda c: (tool_of(c) == "lua_exec" and (args_of(c).get("budget") is None or args_of(c).get("budget") > 0.5)) or tool_of(c) == "lua_job",
     "lua/20_lua.lua budget_exceeded")

# 14. realm_unavailable for a screenshot at the menu: message already says what to do (hint in prose).
sh = err("realm_unavailable", "screenshots need the client realm: load a map first (the main menu cannot be captured)", None, {"answered_by": "client-1"})
sh2 = with_next(sh, {"tool": "map_load"}, True)
case("screenshot_menu", "Take a screenshot of the game.", 'screenshot {}', True, sh, sh2,
     lambda c: tool_of(c) in ("map_load", "map_list"), "tools_world.cpp:56 (prose already actionable)")


def render(c, variant):
    payload = c["cur"] if variant == "current" else c["new"]
    return payload


def prompt(c, variant):
    p = render(c, variant)
    return ("Task: %s\n\nYou just called: %s\nTool result%s:\n%s\n\nWhat is the single next tool call?" %
            (c["task"], c["call"], " (error)" if c["is_error"] else "", D(p)))


def parse_call(text):
    """The model's answer as {tool, arguments}. Strict JSON first; when the model wrote invalid JSON (typically bad escapes
    inside a long Lua string) the tool name and the plain scalar arguments are read with a regex, so a formatting slip is
    not graded as a wrong decision."""
    i = text.find("{")
    if i < 0:
        return None
    try:
        o, _ = json.JSONDecoder().raw_decode(text[i:])
        if isinstance(o, dict):
            return o
    except ValueError:
        pass
    m = re.search(r'"tool"\s*:\s*"([A-Za-z_]+)"', text)
    if not m:
        return None
    args = {}
    for k, v in re.findall(r'"([A-Za-z_]+)"\s*:\s*("(?:[^"\\]|\\.)*"|-?\d+(?:\.\d+)?|true|false|null)', text):
        if k not in ("tool", "arguments", "code"):
            try:
                args.setdefault(k, json.loads(v))
            except ValueError:
                pass
    return {"tool": m.group(1), "arguments": args, "lenient": True}


def find_claude():
    """The claude executable. On Windows the npm shim (claude.cmd) mangles quotes in arguments: use the real binary."""
    exe = shutil.which("claude.exe") or shutil.which("claude")
    if exe and os.name == "nt":
        real = os.path.join(os.path.dirname(exe), "node_modules", "@anthropic-ai", "claude-code", "bin", "claude.exe")
        if os.path.exists(real):
            return real
    return exe


_SYSTEM_FILE = []


def system_file():
    if not _SYSTEM_FILE:
        f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        f.write(SYSTEM); f.close()
        _SYSTEM_FILE.append(f.name)
    return _SYSTEM_FILE[0]


def ask(model, user, timeout=120):
    # The prompt goes through stdin (no quoting problems), the system prompt through a file.
    cmd = [find_claude(), "-p", "--model", model, "--tools", "", "--system-prompt-file", system_file(), "--no-session-persistence",
           "--disable-slash-commands", "--setting-sources", "", "--strict-mcp-config", "--output-format", "json"]
    last = None
    for attempt in range(3):
        try:
            r = subprocess.run(cmd, input=user, capture_output=True, text=True, timeout=timeout, encoding="utf-8")
            o = json.loads(r.stdout)
            return o.get("result", ""), o.get("total_cost_usd", 0.0), o.get("usage", {})
        except Exception as e:  # noqa: BLE001 - retry any CLI hiccup
            last = e
            time.sleep(2)
    return "ERROR " + repr(last), 0.0, {}


def sizes():
    rows = []
    for c in CASES:
        a, b = len(D(c["cur"])), len(D(c["new"]))
        rows.append((c["id"], a, b))
    return rows


def selftest():
    ok = True
    for c in CASES:
        for v in ("current", "proposed"):
            p = c["cur"] if v == "current" else c["new"]
            json.loads(D(p))
        # the proposed payload keeps every key of the current one (prose kept)
        def keys(o, pre=""):
            out = set()
            if isinstance(o, dict):
                for k, v in o.items():
                    out.add(pre + k); out |= keys(v, pre + k + ".")
            return out
        if not keys(c["cur"]) <= keys(c["new"]):
            print("FAIL proposed drops keys:", c["id"]); ok = False
    assert parse_call('```json\n{"tool":"status","arguments":{}}\n```') == {"tool": "status", "arguments": {}}
    assert parse_call("no json") is None
    bad = r'{"tool": "lua_job", "arguments": {"code": "return \"a\x\"", "realm": "server", "async": true}} trailing }'
    assert parse_call(bad)["tool"] == "lua_job"
    lenient = '{"tool": "lua_exec", "arguments": {"code": "x = 1", "realm": "server", "limit": 500,}}'
    assert parse_call(lenient)["tool"] == "lua_exec" and parse_call(lenient)["arguments"].get("limit") == 500
    # graders reject an obviously wrong call and accept the right one
    g = {c["id"]: c["grade"] for c in CASES}
    assert not g["client_crashed"]({"tool": "game_terminate", "arguments": {}})
    assert g["client_crashed"]({"tool": "instances_launch", "arguments": {"count": 1}})
    assert not g["map_load_in_progress"]({"tool": "instances_launch", "arguments": {"count": 2, "timeout_s": 10}})
    assert g["truncated"]({"tool": "lua_exec", "arguments": {"limit": 100000}})
    assert not g["truncated"]({"tool": "lua_exec", "arguments": {"limit": 50}})
    print("selftest", "ok" if ok else "FAILED")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", nargs="+", default=["haiku", "sonnet"])
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--cases", default="")
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--out", default="")
    ap.add_argument("--sizes", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    cases = [c for c in CASES if not a.cases or c["id"] in a.cases.split(",")]
    tot = {"current": 0, "proposed": 0}
    print("%-22s %8s %8s" % ("case", "current", "proposed"))
    for c in cases:
        x, y = len(D(c["cur"])), len(D(c["new"]))
        tot["current"] += x; tot["proposed"] += y
        print("%-22s %8d %8d  (chars of the JSON the model reads)" % (c["id"], x, y))
    print("%-22s %8d %8d  (~%d / ~%d tokens at 3.5 chars per token)" % ("TOTAL", tot["current"], tot["proposed"], tot["current"] / 3.5, tot["proposed"] / 3.5))
    if a.sizes:
        return
    jobs = [(m, c, v, r) for m in a.models for c in cases for v in ("current", "proposed") for r in range(a.runs)]
    results = []

    def run(j):
        m, c, v, r = j
        text, cost, usage = ask(m, prompt(c, v))
        call = parse_call(text)
        passed = bool(call) and bool(c["grade"](call))
        return dict(model=m, case=c["id"], variant=v, run=r, call=call, raw=text[:300], passed=passed, cost=cost)

    with concurrent.futures.ThreadPoolExecutor(a.workers) as ex:
        for res in ex.map(run, jobs):
            results.append(res)
    if a.out:
        json.dump(results, open(a.out, "w", encoding="utf-8"), indent=1)
    print()
    for m in a.models:
        print("== %s: passed / runs (per case, current vs proposed)" % m)
        sumv = {"current": [0, 0], "proposed": [0, 0]}
        for c in cases:
            line = "%-22s" % c["id"]
            for v in ("current", "proposed"):
                rs = [x for x in results if x["model"] == m and x["case"] == c["id"] and x["variant"] == v]
                p = sum(x["passed"] for x in rs)
                sumv[v][0] += p; sumv[v][1] += len(rs)
                line += "  %s %d/%d" % (v[:4], p, len(rs))
            print(line)
        print("%-22s  curr %d/%d (%.0f%%)  prop %d/%d (%.0f%%)  cost $%.3f" % (
            "TOTAL", sumv["current"][0], sumv["current"][1], 100 * sumv["current"][0] / max(1, sumv["current"][1]),
            sumv["proposed"][0], sumv["proposed"][1], 100 * sumv["proposed"][0] / max(1, sumv["proposed"][1]),
            sum(x["cost"] for x in results if x["model"] == m)))


if __name__ == "__main__":
    main()
