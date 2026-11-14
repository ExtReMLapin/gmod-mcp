"""Tests of the conformance runner's parser and comparison logic, on a recorded sample of the suite's output.

Run: python -B -m unittest discover -s tests/conformance

testdata/sample-run/ was recorded from @modelcontextprotocol/conformance@0.2.0-alpha.12
(`server --url <dead url> --requirements 2026-07-28 --output-dir ...`, trimmed to six scenarios); the scenario
completion-complete was edited by hand to pass, so that the sample has both outcomes.
"""
import json
import os
import sys
import tempfile
import textwrap
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run  # noqa: E402

SAMPLE = os.path.join(HERE, "testdata", "sample-run")


def sample_stdout():
    with open(os.path.join(SAMPLE, "stdout.txt"), encoding="utf-8") as f:
        return f.read()


class ParseTests(unittest.TestCase):
    def test_scenarios(self):
        sc = run.parse_scenarios(SAMPLE, sample_stdout())
        self.assertEqual(sorted(sc), ["completion-complete", "json-schema-2020-12", "server-stateless",
                                      "tasks-capability-negotiation", "tasks-status-notifications", "tools-list"])
        self.assertFalse(sc["completion-complete"]["failures"])
        self.assertEqual(sc["completion-complete"]["checks"], 2)
        self.assertIn("tools-list", sc["tools-list"]["failures"])
        self.assertNotIn("wire-schema-valid", sc["tools-list"]["failures"])  # a SUCCESS check
        self.assertTrue(sc["tasks-capability-negotiation"]["crashed"])  # directory without checks.json
        self.assertFalse(sc["tasks-status-notifications"]["failures"])

    def test_not_scored(self):
        self.assertEqual(run.parse_not_scored(sample_stdout()),
                         {"tasks-capability-negotiation": "extension", "tasks-status-notifications": "extension",
                          "json-schema-2020-12": "pending"})

    def test_not_scored_ignores_ansi_and_absence(self):
        self.assertEqual(run.parse_not_scored("Total: 1 passed, 0 failed\n"), {})
        text = "\x1b[31mNot scored for 2026-07-28: 1 scenario(s) run, 1 failing.\x1b[0m\n  x a-b (extension)\n\nDone\n"
        self.assertEqual(run.parse_not_scored(text), {"a-b": "extension"})

    def test_announced_scenario_without_directory_is_a_crash(self):
        with tempfile.TemporaryDirectory() as d:
            sc = run.parse_scenarios(d, "=== Running scenario: foo ===\n")
            self.assertTrue(sc["foo"]["crashed"])

    def test_latest_timestamp_wins(self):
        with tempfile.TemporaryDirectory() as d:
            for ts, status in (("2026-01-01T00-00-00-000Z", "FAILURE"), ("2026-01-01T00-00-01-000Z", "SUCCESS")):
                p = os.path.join(d, "server-foo-" + ts)
                os.makedirs(p)
                with open(os.path.join(p, "checks.json"), "w") as f:
                    json.dump([{"id": "c", "status": status}], f)
            self.assertEqual(run.parse_scenarios(d)["foo"]["failures"], {})

    def test_warning_is_not_a_failure(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "server-foo-2026-01-01T00-00-00-000Z")
            os.makedirs(p)
            with open(os.path.join(p, "checks.json"), "w") as f:
                json.dump([{"id": "a", "status": "WARNING"}, {"id": "b", "status": "SKIPPED"}], f)
            self.assertEqual(run.parse_scenarios(d)["foo"]["failures"], {})


def entry(scenario, check="*", reason="by design"):
    return {"scenario": scenario, "check": check, "reason": reason}


class CompareTests(unittest.TestCase):
    def setUp(self):
        self.sc = run.parse_scenarios(SAMPLE, sample_stdout())
        self.ns = run.parse_not_scored(sample_stdout())

    def test_unexpected_failures_are_reported(self):
        r = run.compare(self.sc, [], self.ns)
        # required: server-stateless, completion-complete, tools-list
        self.assertEqual(r["total"], 3)
        self.assertEqual(r["passed"], 1)
        self.assertEqual(r["not_scored"], 3)
        self.assertFalse(r["ok"])
        self.assertIn("tools-list", {u[0] for u in r["unexpected"]})
        self.assertNotIn("json-schema-2020-12", {u[0] for u in r["unexpected"]})  # not scored

    def test_all_failures_listed_is_ok(self):
        listed = [entry("tools-list"), entry("server-stateless")]
        r = run.compare(self.sc, listed, self.ns)
        self.assertTrue(r["ok"], r)
        self.assertEqual((r["passed"], r["excluded"], r["total"]), (1, 2, 3))

    def test_specific_check_entry(self):
        self.assertTrue(self.sc["tools-list"]["failures"])
        ids = list(self.sc["tools-list"]["failures"])
        listed = [entry("tools-list", c) for c in ids] + [entry("server-stateless")]
        self.assertTrue(run.compare(self.sc, listed, self.ns)["ok"])
        r = run.compare(self.sc, [entry("tools-list", "no-such-check"), entry("server-stateless")], self.ns)
        self.assertFalse(r["ok"])
        self.assertEqual(len(r["stale"]), 1)

    def test_stale_exclusion(self):
        listed = [entry("tools-list"), entry("server-stateless"), entry("completion-complete")]
        r = run.compare(self.sc, listed, self.ns)
        self.assertFalse(r["ok"])
        self.assertEqual(len(r["stale"]), 1)
        self.assertIn("completion-complete (now passes", r["stale"][0])

    def test_entry_for_scenario_not_run_is_stale(self):
        r = run.compare(self.sc, [entry("tools-list"), entry("server-stateless"), entry("ghost")], self.ns)
        self.assertIn("ghost (scenario not run", r["stale"][0])

    def test_crashed_required_scenario_fails(self):
        sc = {"foo": {"crashed": True, "checks": 0, "failures": {"*": "crash"}}}
        self.assertFalse(run.compare(sc, [], {})["ok"])
        self.assertTrue(run.compare(sc, [entry("foo")], {})["ok"])

    def test_no_result_is_not_ok(self):
        self.assertFalse(run.compare({}, [], {})["ok"])

    def test_summary_entry_and_report(self):
        r = run.compare(self.sc, [entry("tools-list"), entry("server-stateless")], self.ns)
        self.assertEqual(run.summary_entry(r), {"passed": 1, "total": 3, "excluded": 2, "unexpected": 0, "stale": 0,
                                                "not_scored": 3, "ok": True})
        lines = []
        run.report("2026-07-28", "0.2.0-alpha.12", r, lines.append)
        self.assertIn("CONFORMANCE 1/3 (2026-07-28)", lines)


class ExpectedFileTests(unittest.TestCase):
    def write(self, data):
        d = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        p = os.path.join(d, "expected.json")
        with open(p, "w") as f:
            json.dump(data, f)
        return p

    def test_valid(self):
        v, runs = run.load_expected(self.write({"suite_version": "1.2.3", "runs": {"x": [{"scenario": "a", "reason": "r"}]}}))
        self.assertEqual(v, "1.2.3")
        self.assertEqual(runs["x"][0]["check"], "*")

    def test_invalid(self):
        for bad in ({"runs": {"x": []}}, {"suite_version": "1"}, {"suite_version": "1", "runs": {"x": [{"scenario": "a"}]}}):
            with self.assertRaises(run.SetupError):
                run.load_expected(self.write(bad))


FAKE_HOST = textwrap.dedent("""
    import sys, time
    print("READY http://127.0.0.1:%s/mcp" % sys.argv[sys.argv.index("--port") + 1], flush=True)
    sys.stdin.read()          # exits on stdin EOF, like the real host
""")

FAKE_SUITE = textwrap.dedent("""
    import os, shutil, sys
    sample = sys.argv[1]
    out = sys.argv[sys.argv.index("--output-dir") + 1]
    for d in os.listdir(sample):
        if os.path.isdir(os.path.join(sample, d)):
            shutil.copytree(os.path.join(sample, d), os.path.join(out, d))
    sys.stdout.buffer.write(open(os.path.join(sample, "stdout.txt"), "rb").read())
    sys.exit(1)
""")


class EndToEndTests(unittest.TestCase):
    """run.main() with a fake host and a fake suite replaying the recorded sample (no Node, no game)."""

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(self.d, ignore_errors=True))
        self.host = os.path.join(self.d, "fake_host.py")
        self.suite = os.path.join(self.d, "fake_suite.py")
        with open(self.host, "w") as f:
            f.write(FAKE_HOST)
        with open(self.suite, "w") as f:
            f.write(FAKE_SUITE)
        os.environ["MCP_CONFORMANCE_SUITE_CMD"] = json.dumps([sys.executable, self.suite, SAMPLE])
        self.addCleanup(lambda: os.environ.pop("MCP_CONFORMANCE_SUITE_CMD", None))
        self.summary = os.path.join(self.d, "summary.json")

    def expected(self, entries):
        p = os.path.join(self.d, "expected.json")
        with open(p, "w") as f:
            json.dump({"suite_version": "0.0.0", "runs": {"2026-07-28": entries}}, f)
        return p

    def go(self, entries, host=None):
        argv = ["--host", host or self.host, "--expected", self.expected(entries), "--summary", self.summary,
                "--timeout", "60", "--port", "27999"]
        saved, sys.stdout = sys.stdout, open(os.devnull, "w")
        try:
            return run.main(argv)
        finally:
            sys.stdout.close()
            sys.stdout = saved

    def test_pass_and_summary(self):
        self.assertEqual(self.go([entry("tools-list"), entry("server-stateless")]), 0)
        with open(self.summary) as f:
            s = json.load(f)
        self.assertEqual((s["2026-07-28"]["passed"], s["2026-07-28"]["total"]), (1, 3))

    def test_regression_fails_but_summary_is_written(self):
        self.assertEqual(self.go([entry("tools-list")]), 1)
        with open(self.summary) as f:
            self.assertFalse(json.load(f)["2026-07-28"]["ok"])

    def test_host_without_ready_is_a_setup_error(self):
        bad = os.path.join(self.d, "bad_host.py")
        with open(bad, "w") as f:
            f.write("import sys\nprint('boom')\nsys.exit(3)\n")
        self.assertEqual(self.go([], host=bad), 2)

    def test_missing_host(self):
        self.assertEqual(self.go([], host=os.path.join(self.d, "nope.py")), 2)


if __name__ == "__main__":
    unittest.main()
