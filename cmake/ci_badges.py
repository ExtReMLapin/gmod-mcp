#!/usr/bin/env python3
"""Builds the README badges (shields.io endpoint JSON) from what the CI already prints.

Inputs (all in one or more "data" directories, written by the build job):
  unittests.log             native tests          "unittests: N passed, M failed"
  coretests.log             native core tests     "core: N passed, M failed"
  runtime.log               Lua runtime tests     "runtime_test: N passed, M failed"   (Linux leg only)
  bridge.log                bridge tests          "TOTAL N/M"
  protocol.log              protocol constants    "... : OK" (one check) or "... : N failure(s)"
  comparison.log            comparison tests      unittest "Ran N tests" + "OK" / "FAILED (failures=a, errors=b)"
  conformance-unit.log      conformance runner and fixture check tests (same unittest format)
  ci-unit.log               CI scripts tests, e.g. this script (same unittest format)
  conformance-summary.json  written by tests/conformance/run.py: {"<revision>": {"passed", "total", ...}}

Outputs, in --out:
  tests.json              {"schemaVersion": 1, "label": "tests", "message": "412 passed", "color": "brightgreen"}
  mcp-conformance.json    {"schemaVersion": 1, "label": "MCP conformance", "message": "2026-07-28 · 140/140", ...}

The first --dir is the primary one (its counts are the badge: the suites run on every platform, counting them twice
would be wrong). Every --dir is checked for failures, so a failure on any platform turns the badge red.
--build-result is the result of the build job (success, failure, cancelled...): anything but success turns the badge red
even when no log shows a failure (a step that never ran).

Python standard library only.
"""
import argparse
import json
import os
import re
import sys

# (log file, label, parser name). Order is the display order of --verbose output.
SUITES = [
    ("unittests.log", "native unit tests", "pass_fail", r"unittests: (\d+) passed, (\d+) failed"),
    ("coretests.log", "native core tests", "pass_fail", r"core: (\d+) passed, (\d+) failed"),
    ("runtime.log", "Lua runtime tests", "pass_fail", r"runtime_test: (\d+) passed, (\d+) failed"),
    ("bridge.log", "bridge tests", "total", r"TOTAL (\d+)/(\d+)"),
    ("protocol.log", "protocol constants", "protocol", None),
    ("comparison.log", "comparison tests", "unittest", None),
    ("conformance-unit.log", "conformance runner tests", "unittest", None),
    ("ci-unit.log", "CI scripts tests", "unittest", None),
]
# Logs a complete primary leg must have (the Lua runtime tests run on Linux only).
DEFAULT_EXPECTED = [s[0] for s in SUITES]


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def parse_pass_fail(text, pattern):
    hits = re.findall(pattern, text)
    if not hits:
        return None
    return sum(int(p) for p, _ in hits), sum(int(f) for _, f in hits)


def parse_total(text, pattern):
    hits = re.findall(r"^" + pattern + r"\s*$", text, re.M)
    if not hits:
        return None
    n, m = hits[-1]
    return int(n), int(m) - int(n)


def parse_protocol(text, _pattern=None):
    m = re.search(r"checked against schema [^:]*: (OK|(\d+) failure\(s\))\s*$", text, re.M)
    if not m:
        return None
    return (1, 0) if m.group(1) == "OK" else (0, int(m.group(2)))


def parse_unittest(text, _pattern=None):
    m = re.search(r"^Ran (\d+) tests? in ", text, re.M)
    if not m:
        return None
    ran = int(m.group(1))
    failed = 0
    f = re.search(r"^FAILED \((.*)\)\s*$", text, re.M)
    if f:
        failed = sum(int(x) for x in re.findall(r"(?:failures|errors)=(\d+)", f.group(1))) or 1
    elif not re.search(r"^OK\b", text, re.M):
        failed = 1
    return max(ran - failed, 0), failed


PARSERS = {"pass_fail": parse_pass_fail, "total": parse_total, "protocol": parse_protocol, "unittest": parse_unittest}


def parse_log(kind, pattern, text):
    return PARSERS[kind](text, pattern)


def collect(data_dir):
    """Returns {log name: (passed, failed) or None if the log is missing or has no result line}."""
    out = {}
    for name, _label, kind, pattern in SUITES:
        text = read(os.path.join(data_dir, name))
        out[name] = None if text is None else parse_log(kind, pattern, text)
    return out


def tests_badge(dirs, build_result="", expected=None):
    expected = expected if expected is not None else DEFAULT_EXPECTED
    primary = collect(dirs[0])
    passed = sum(r[0] for r in primary.values() if r)
    failed = sum(r[1] for r in primary.values() if r)
    for d in dirs[1:]:
        failed += sum(r[1] for r in collect(d).values() if r)
    missing = [n for n in expected if primary.get(n) is None]
    bad_build = build_result not in ("", "success")
    if failed:
        message, color = "%d passed, %d failed" % (passed, failed), "red"
    elif bad_build:
        message, color = ("%d passed, build %s" % (passed, build_result) if passed else "build " + build_result), "red"
    elif missing:
        message, color = "%d passed (incomplete)" % passed, "yellow"
    else:
        message, color = "%d passed" % passed, "brightgreen"
    return {"schemaVersion": 1, "label": "tests", "message": message, "color": color}, missing


def conformance_badge(summary, build_result=""):
    base = {"schemaVersion": 1, "label": "MCP conformance"}
    if not isinstance(summary, dict) or not summary:
        if build_result not in ("", "success"):
            return dict(base, message="not run (build %s)" % build_result, color="red")
        return dict(base, message="no data", color="lightgrey")
    parts, ok = [], True
    for run in sorted(summary, reverse=True):
        e = summary[run]
        passed, total = int(e.get("passed", 0)), int(e.get("total", 0))
        run_ok = bool(e.get("ok", passed == total and total > 0))
        ok = ok and run_ok
        text = "%s · %d/%d" % (run, passed, total)
        if e.get("excluded"):
            text += " (%d excluded)" % int(e["excluded"])
        parts.append(text)
    return dict(base, message=", ".join(parts), color="brightgreen" if ok else "red")


def write_json(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", action="append", required=True,
                    help="data directory with the logs and conformance-summary.json (repeat; the first is primary)")
    ap.add_argument("--out", required=True, help="directory for tests.json and mcp-conformance.json")
    ap.add_argument("--summary", help="conformance-summary.json (default: <primary dir>/conformance-summary.json)")
    ap.add_argument("--build-result", default="", help="result of the build job (success, failure, ...)")
    ap.add_argument("--expect", help="comma-separated log names the primary leg must have (default: all)")
    args = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    expected = args.expect.split(",") if args.expect else None
    tests, missing = tests_badge(args.dir, args.build_result, expected)
    summary = None
    text = read(args.summary or os.path.join(args.dir[0], "conformance-summary.json"))
    if text:
        try:
            summary = json.loads(text)
        except ValueError:
            summary = None
    conf = conformance_badge(summary, args.build_result)

    os.makedirs(args.out, exist_ok=True)
    write_json(os.path.join(args.out, "tests.json"), tests)
    write_json(os.path.join(args.out, "mcp-conformance.json"), conf)
    if missing:
        print("note: no result in: " + ", ".join(missing), file=sys.stderr)
    print("tests: %s (%s)" % (tests["message"], tests["color"]))
    print("MCP conformance: %s (%s)" % (conf["message"], conf["color"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
