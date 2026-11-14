"""Tests of cmake/ci_badges.py on log lines as the CI prints them."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "cmake"))
import ci_badges  # noqa: E402

LOGS = {
    "unittests.log": "FAIL-free output\nunittests: 40 passed, 0 failed\n",
    "coretests.log": "core: 300 passed, 0 failed\n",
    "runtime.log": "runtime_test: 55 passed, 0 failed\n",
    "bridge.log": "PASS a\nPASS b\n   [all 2 cases in parallel: 1.0 s]\nTOTAL 2/2\n",
    "protocol.log": "56 error codes, 6 _meta keys checked against schema 0123456789ab: OK\n",
    "comparison.log": "..........\n----------------------------------------------------------------------\nRan 41 tests in 0.068s\n\nOK\n",
    "conformance-unit.log": "Ran 23 tests in 0.5s\n\nOK\n",
    "ci-unit.log": "Ran 7 tests in 0.1s\n\nOK\n",
}
SUMMARY = {"2026-07-28": {"passed": 140, "total": 140, "ok": True}, "2025-11-25": {"passed": 120, "total": 120}}


def make_dir(logs=None, summary=SUMMARY):
    d = tempfile.mkdtemp()
    for name, text in (logs or LOGS).items():
        with open(os.path.join(d, name), "w") as f:
            f.write(text)
    if summary is not None:
        with open(os.path.join(d, "conformance-summary.json"), "w") as f:
            json.dump(summary, f)
    return d


class BadgeTests(unittest.TestCase):
    def test_parsers(self):
        self.assertEqual(ci_badges.collect(make_dir()), {
            "unittests.log": (40, 0), "coretests.log": (300, 0), "runtime.log": (55, 0), "bridge.log": (2, 0),
            "protocol.log": (1, 0), "comparison.log": (41, 0), "conformance-unit.log": (23, 0),
            "ci-unit.log": (7, 0)})

    def test_failures(self):
        self.assertEqual(ci_badges.parse_log("pass_fail", r"core: (\d+) passed, (\d+) failed", "core: 3 passed, 2 failed"), (3, 2))
        self.assertEqual(ci_badges.parse_log("total", r"TOTAL (\d+)/(\d+)", "TOTAL 5/8\n"), (5, 3))
        self.assertEqual(ci_badges.parse_unittest("Ran 10 tests in 1s\n\nFAILED (failures=2, errors=1)\n"), (7, 3))
        self.assertEqual(ci_badges.parse_unittest("Traceback...\n"), None)
        self.assertEqual(ci_badges.parse_protocol("2 error codes, 1 _meta keys checked against schema abc: 3 failure(s)\n"), (0, 3))

    def test_green(self):
        badge, missing = ci_badges.tests_badge([make_dir()], "success")
        self.assertEqual((badge["message"], badge["color"]), ("%d passed" % (40 + 300 + 55 + 2 + 1 + 41 + 23 + 7), "brightgreen"))
        self.assertEqual(missing, [])

    def test_failure_on_secondary_leg_turns_red(self):
        win = dict(LOGS, **{"coretests.log": "core: 290 passed, 4 failed\n"})
        win.pop("runtime.log")
        badge, _ = ci_badges.tests_badge([make_dir(), make_dir(win)], "failure")
        self.assertEqual(badge["color"], "red")
        self.assertIn("4 failed", badge["message"])

    def test_missing_logs_and_failed_build(self):
        partial = {k: v for k, v in LOGS.items() if k != "bridge.log"}
        badge, missing = ci_badges.tests_badge([make_dir(partial)], "success")
        self.assertEqual((badge["color"], missing), ("yellow", ["bridge.log"]))
        badge, _ = ci_badges.tests_badge([make_dir(partial)], "failure")
        self.assertEqual(badge["color"], "red")

    def test_conformance(self):
        b = ci_badges.conformance_badge(SUMMARY)
        self.assertEqual(b["message"], "2026-07-28 \u00b7 140/140, 2025-11-25 \u00b7 120/120")
        self.assertEqual(b["color"], "brightgreen")
        red = ci_badges.conformance_badge({"2026-07-28": {"passed": 138, "total": 140, "unexpected": 2, "ok": False}})
        self.assertEqual((red["message"], red["color"]), ("2026-07-28 \u00b7 138/140", "red"))
        excl = ci_badges.conformance_badge({"2026-07-28": {"passed": 138, "total": 140, "excluded": 2, "ok": True}})
        self.assertEqual(excl["message"], "2026-07-28 \u00b7 138/140 (2 excluded)")
        self.assertEqual(ci_badges.conformance_badge(None)["color"], "lightgrey")
        self.assertEqual(ci_badges.conformance_badge(None, "failure")["color"], "red")

    def test_main_writes_endpoint_files(self):
        out = tempfile.mkdtemp()
        self.assertEqual(ci_badges.main(["--dir", make_dir(), "--out", out, "--build-result", "success"]), 0)
        for name, label in (("tests.json", "tests"), ("mcp-conformance.json", "MCP conformance")):
            with open(os.path.join(out, name), encoding="utf-8") as f:
                j = json.load(f)
            self.assertEqual((j["schemaVersion"], j["label"]), (1, label))
            self.assertTrue({"message", "color"} <= set(j))


if __name__ == "__main__":
    unittest.main()
