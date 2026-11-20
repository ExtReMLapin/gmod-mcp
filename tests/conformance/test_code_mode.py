"""Tests of the Tasks answers for Claude Code's "Code Mode" (Issue #3), against the conformance host: the module's real
MCP server code and real tool registry, no game (the tools answer realm_unavailable at once).

In Code Mode a script awaits `tools[...]({...})`. When the server answers a call with a task (2026-07-28 Tasks
extension), the script only gets a stub string and the real result goes to the model, so `await map_load(...)` followed
by more code breaks. Claude Code marks the calls made by a script with `_meta["claudecode/toolUseId"]` starting with
"repl_" (a normal call has "toolu_..."). The server must then answer in place. GMOD_MCP_TASKS=off turns the
server-chosen tasks off for every client.

Run: python -B -m unittest discover -s tests/conformance   (skipped when the host is not built)
"""
import json
import os
import socket
import sys
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import run  # noqa: E402

MODERN = "2026-07-28"
LEGACY = "2025-11-25"
TASKS_EXT = "io.modelcontextprotocol/tasks"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    """One host process with the given GMOD_MCP_TASKS (None: unset)."""

    def __init__(self, tasks_env):
        self.tasks_env = tasks_env
        self.host = None

    def __enter__(self):
        path = run.find_host(os.path.join(ROOT, "build"))
        if not path:
            raise unittest.SkipTest("gmod-mcp-conformance-host is not built")
        saved = os.environ.get("GMOD_MCP_TASKS")
        if self.tasks_env is None:
            os.environ.pop("GMOD_MCP_TASKS", None)
        else:
            os.environ["GMOD_MCP_TASKS"] = self.tasks_env
        try:
            self.host = run.Host(run.host_command(path) + ["--port", str(free_port())])
            self.url = self.host.start(30)
        finally:
            if saved is None:
                os.environ.pop("GMOD_MCP_TASKS", None)
            else:
                os.environ["GMOD_MCP_TASKS"] = saved
        return self

    def __exit__(self, *exc):
        self.host.stop()
        run.kill_tree(self.host.proc)

    def call(self, tool, args, tool_use_id=None, modern=True, task=None, tasks_ext=True, wait=2):
        params = {"name": tool, "arguments": args}
        if modern:
            meta = {"io.modelcontextprotocol/protocolVersion": MODERN,
                    "io.modelcontextprotocol/clientInfo": {"name": "claude-code", "version": "2.1.329"},
                    "io.modelcontextprotocol/clientCapabilities": {"extensions": {TASKS_EXT: {}}} if tasks_ext else {}}
            if tool_use_id is not None:
                meta["claudecode/toolUseId"] = tool_use_id
            params["_meta"] = meta
        if task is not None:
            params["task"] = task
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": params}).encode()
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                   "MCP-Protocol-Version": MODERN if modern else LEGACY, "Mcp-Method": "tools/call", "Mcp-Name": tool}
        req = urllib.request.Request(self.url, data=body, headers=headers)
        # A task answer comes back at once. A call run in place may take long (the host has no game: console_wait really waits
        # its timeout_s): after `wait` seconds without an answer, the call is known to run in place.
        try:
            with urllib.request.urlopen(req, timeout=wait) as r:
                raw = r.read().decode()
                if r.headers.get("Content-Type", "").startswith("text/event-stream"):
                    # The last `data:` event of an SSE answer is the response.
                    datas = [l[5:].strip() for l in raw.splitlines() if l.startswith("data:")]
                    raw = datas[-1]
                return json.loads(raw)
        except (socket.timeout, TimeoutError):
            return {"result": {"running_in_place": True}}


def is_task(resp):
    res = resp.get("result", {})
    return res.get("resultType") == "task" or "task" in res


class CodeModeTaskTests(unittest.TestCase):
    # Issue #3: a call made by a Code Mode script must be answered in place.
    def test_map_load_is_a_task_for_a_normal_call(self):
        with Server(None) as s:
            self.assertTrue(is_task(s.call("map_load", {"map": "gm_construct"}, "toolu_01abc")))

    def test_map_load_is_answered_in_place_for_a_repl_call(self):
        with Server(None) as s:
            resp = s.call("map_load", {"map": "gm_construct"}, "repl_toolu_01abc")
            self.assertFalse(is_task(resp), resp)
            self.assertTrue("content" in resp["result"] or resp["result"].get("running_in_place"), resp)  # the tool result itself, or still running

    def test_long_wait_is_answered_in_place_for_a_repl_call(self):
        with Server(None) as s:
            self.assertTrue(is_task(s.call("console_wait", {"regex": "x", "timeout_s": 30}, "toolu_01abc")))
            self.assertFalse(is_task(s.call("console_wait", {"regex": "x", "timeout_s": 30}, "repl_1")))

    def test_off_never_answers_with_a_task(self):
        with Server("off") as s:
            self.assertFalse(is_task(s.call("map_load", {"map": "gm_construct"}, "toolu_01abc")))
            self.assertFalse(is_task(s.call("console_wait", {"regex": "x", "timeout_s": 30}, "toolu_01abc")))
            self.assertFalse(is_task(s.call("map_load", {"map": "gm_construct"})))  # no tool use id at all

    def test_auto_and_unknown_values_keep_the_default(self):
        for value in ("auto", "AUTO", "bogus", ""):
            with Server(value) as s:
                self.assertTrue(is_task(s.call("map_load", {"map": "gm_construct"}, "toolu_01abc")), value)

    def test_off_keeps_an_explicit_legacy_client_task_request(self):
        # The client asked for a task itself (2025-11-25 `params.task`): that is not a server choice.
        with Server("off") as s:
            resp = s.call("map_load", {"map": "gm_construct"}, modern=False, task={"ttl": 60000})
            self.assertTrue(is_task(resp), resp)


if __name__ == "__main__":
    unittest.main()
