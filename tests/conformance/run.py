#!/usr/bin/env python3
"""Run the official MCP conformance suite against the real MCP server code of the module (no game).

What it does
  1. starts build/**/gmod-mcp-conformance-host (see tests/conformance/*) and waits for its READY line;
  2. for every run listed in tests/conformance/expected-failures.json, runs
       npx -y @modelcontextprotocol/conformance@<suite_version> server --url <url> --requirements <run> -o <dir>
     and reads the suite's machine-readable results: one <dir>/server-<scenario>-<timestamp>/checks.json per scenario
     (the suite has no JSON summary on stdout; the per-scenario checks.json files are its documented result format);
  3. compares them with the expected failures: a failing check that is not listed is a regression, a listed failure that
     now passes is a stale exclusion; both fail the run;
  4. prints "CONFORMANCE <passed>/<total> (<run>)" per run and writes build/conformance-summary.json;
  5. always stops the host.

Only what the revision requires is scored (the suite prints the scenarios it runs without scoring, such as
extensions and pending ones, in its "Not scored" section; they are reported but never fail the run).

Python standard library only; works on Windows and Linux. Exit code: 0 ok, 1 conformance failure, 2 setup error.

Options: --host PATH, --build-dir DIR, --expected FILE, --summary FILE, --port N, --timeout SECONDS (per run).
Test hook: the environment variable MCP_CONFORMANCE_SUITE_CMD (a JSON list) replaces the
["npx", "-y", "@modelcontextprotocol/conformance@<version>"] prefix of the suite command.
"""
import argparse
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOST_NAME = "gmod-mcp-conformance-host"
SUITE_PACKAGE = "@modelcontextprotocol/conformance"
READY_RE = re.compile(r"^READY\s+(http://\S+)\s*$")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
DIR_RE = re.compile(r"^server-(?P<scenario>.+)-(?P<ts>\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}-\d{3}Z)$")
RUNNING_RE = re.compile(r"^=== Running scenario: (\S+) ===\s*$")
# First line of the suite: "Running requirements 2026-07-28 (6 scenarios) against http://...".
ANNOUNCED_RE = re.compile(r"^Running requirements \S+ \((\d+) scenarios?\)")
# "  ✗ tasks-lifecycle (extension)" in the "Not scored for <revision>" section of the suite's output.
NOT_SCORED_RE = re.compile(r"^\s+\S\s+(\S+)\s+\(([a-z-]+)\)\s*$")


class SetupError(Exception):
    pass


# --------------------------------------------------------------------------------------------------------------------
# Expected failures
# --------------------------------------------------------------------------------------------------------------------

def load_expected(path):
    """Returns (suite_version, {run: [{"scenario", "check", "reason"}]})."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except OSError as e:
        raise SetupError("cannot read %s: %s" % (path, e))
    except ValueError as e:
        raise SetupError("%s is not valid JSON: %s" % (path, e))
    version = data.get("suite_version")
    runs = data.get("runs")
    if not isinstance(version, str) or not version:
        raise SetupError('%s: "suite_version" (exact npm version) is required' % path)
    if not isinstance(runs, dict) or not runs:
        raise SetupError('%s: "runs" must map a spec revision to a list of expected failures' % path)
    out = {}
    for run, entries in runs.items():
        if not isinstance(entries, list):
            raise SetupError("%s: runs.%s must be a list" % (path, run))
        norm = []
        for e in entries:
            if not isinstance(e, dict) or not isinstance(e.get("scenario"), str):
                raise SetupError('%s: runs.%s: every entry needs a "scenario"' % (path, run))
            if not e.get("reason"):
                raise SetupError('%s: runs.%s: entry %s needs a "reason"' % (path, run, e["scenario"]))
            norm.append({"scenario": e["scenario"], "check": e.get("check") or "*", "reason": e["reason"]})
        out[run] = norm
    return version, out


# --------------------------------------------------------------------------------------------------------------------
# Parsing the suite's results
# --------------------------------------------------------------------------------------------------------------------

def parse_scenarios(output_dir, stdout_text=""):
    """Reads <output_dir>/server-<scenario>-<timestamp>/checks.json.

    Returns {scenario: {"crashed": bool, "failures": {check_id: message}, "checks": n}}.
    A scenario the suite started but did not write results for (it threw) is "crashed". When a scenario appears twice,
    the latest timestamp wins. Only FAILURE counts as a failure (like the suite's own requirements exit code);
    WARNING, SKIPPED and INFO do not.
    """
    found = {}
    if os.path.isdir(output_dir):
        for name in sorted(os.listdir(output_dir)):
            m = DIR_RE.match(name)
            full = os.path.join(output_dir, name)
            if not m or not os.path.isdir(full):
                continue
            scenario, ts = m.group("scenario"), m.group("ts")
            if scenario in found and found[scenario][0] > ts:
                continue
            found[scenario] = (ts, os.path.join(full, "checks.json"))
    result = {}
    for scenario, (_, path) in found.items():
        entry = {"crashed": False, "failures": {}, "checks": 0}
        try:
            with open(path, encoding="utf-8") as f:
                checks = json.load(f)
            if not isinstance(checks, list):
                raise ValueError("not a list")
            for c in checks:
                if not isinstance(c, dict):
                    continue
                entry["checks"] += 1
                if c.get("status") == "FAILURE":
                    cid = str(c.get("id") or c.get("name") or "?")
                    entry["failures"].setdefault(cid, str(c.get("errorMessage") or c.get("description") or ""))
        except (OSError, ValueError):
            entry["crashed"] = True
            entry["failures"]["*"] = "scenario crashed: no usable checks.json (see the suite output above)"
        result[scenario] = entry
    # Scenarios announced on stdout without any result directory.
    for line in clean_lines(stdout_text):
        m = RUNNING_RE.match(line)
        if m and m.group(1) not in result:
            result[m.group(1)] = {"crashed": True, "checks": 0,
                                  "failures": {"*": "scenario crashed: no result directory"}}
    return result


def clean_lines(text):
    return [ANSI_RE.sub("", l).rstrip("\r") for l in text.splitlines()]


def parse_announced_count(stdout_text):
    """The number of scenarios the suite announced in its first line, or None if there is no such line."""
    for line in clean_lines(stdout_text):
        m = ANNOUNCED_RE.match(line)
        if m:
            return int(m.group(1))
    return None


def completeness_problem(scenarios, stdout_text):
    """None if the suite ran every scenario it announced; otherwise a message. A run cut short (the suite process died
    between two scenarios) leaves fewer result directories and would otherwise pass with a smaller N/N."""
    announced = parse_announced_count(stdout_text)
    if announced is None:
        return 'the suite did not print its "Running requirements ... (N scenarios)" line: cannot tell if it finished'
    if len(scenarios) != announced:
        return "the suite announced %d scenario(s) but %d produced results: the run was cut short" % (
            announced, len(scenarios))
    return None


def parse_not_scored(stdout_text):
    """Scenarios the suite ran but does not score, from its "Not scored for <revision>" section: {scenario: reason}."""
    out = {}
    inside = False
    for line in clean_lines(stdout_text):
        if line.startswith("Not scored for "):
            inside = True
            continue
        if inside:
            m = NOT_SCORED_RE.match(line)
            if m:
                out[m.group(1)] = m.group(2)
            elif line.strip():
                inside = False
            elif out:
                inside = False
    return out


# --------------------------------------------------------------------------------------------------------------------
# Comparison with the expected failures
# --------------------------------------------------------------------------------------------------------------------

def compare(scenarios, expected, not_scored=None):
    """Scores the required scenarios against the expected failures.

    scenarios: output of parse_scenarios; expected: the run's entries; not_scored: {scenario: reason}.
    Returns a dict: passed (no failing check), excluded (all failures expected), total (required scenarios run),
    not_scored (count), unexpected [(scenario, check, message)], stale [entry text], ok.
    """
    not_scored = not_scored or {}
    used = set()
    unexpected = []
    passed = excluded = total = 0

    def match(scenario, check):
        hit = False
        for i, e in enumerate(expected):
            if e["scenario"] == scenario and e["check"] in ("*", check):
                used.add(i)
                hit = True
        return hit

    for scenario in sorted(scenarios):
        info = scenarios[scenario]
        required = scenario not in not_scored
        if required:
            total += 1
        failures = info["failures"]
        bad = []
        for check, message in sorted(failures.items()):
            # Every matching entry is "used", also in not-scored scenarios.
            if not match(scenario, check) and required:
                bad.append((scenario, check, message))
        if not required:
            continue
        if bad:
            unexpected.extend(bad)
        elif failures:
            excluded += 1
        else:
            passed += 1
    stale = []
    for i, e in enumerate(expected):
        if i in used:
            continue
        what = e["scenario"] + ("" if e["check"] == "*" else " / " + e["check"])
        why = "scenario not run" if e["scenario"] not in scenarios else "now passes"
        stale.append("%s (%s; reason was: %s)" % (what, why, e["reason"]))
    return {"passed": passed, "excluded": excluded, "total": total, "not_scored": len(not_scored),
            "unexpected": unexpected, "stale": stale, "ok": not unexpected and not stale and total > 0}


def summary_entry(result):
    """The build/conformance-summary.json entry of a run (passed/total as in the contract, plus detail)."""
    return {"passed": result["passed"], "total": result["total"], "excluded": result["excluded"],
            "unexpected": len(result["unexpected"]), "stale": len(result["stale"]),
            "not_scored": result["not_scored"], "ok": result["ok"]}


def report(run, version, result, out):
    out("")
    out("== %s (suite %s): %d passed, %d excluded by design, %d unexpected failure(s), %d stale exclusion(s), "
        "%d not scored" % (run, version, result["passed"], result["excluded"], len(result["unexpected"]),
                           len(result["stale"]), result["not_scored"]))
    if result["total"] == 0:
        out("   no required scenario ran: the suite did not produce results")
    if result["unexpected"]:
        out("   UNEXPECTED FAILURES (not in expected-failures.json):")
        for scenario, check, message in result["unexpected"]:
            out("     - %s / %s: %s" % (scenario, check, message[:300]))
    if result["stale"]:
        out("   STALE EXCLUSIONS (listed in expected-failures.json but no longer failing: remove them):")
        for s in result["stale"]:
            out("     - " + s)
    out("CONFORMANCE %d/%d (%s)" % (result["passed"], result["total"], run))


# --------------------------------------------------------------------------------------------------------------------
# Processes
# --------------------------------------------------------------------------------------------------------------------

def find_host(build_dir):
    """Like build.yml finds the other executables: the first match under the build directory."""
    for dirpath, _, files in os.walk(build_dir):
        for f in sorted(files):
            if f in (HOST_NAME, HOST_NAME + ".exe"):
                return os.path.join(dirpath, f)
    return None


def host_command(path):
    return [sys.executable, "-u", path] if path.endswith(".py") else [path]


def _popen_flags():
    if os.name == "nt":
        return {"creationflags": getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)}
    return {"start_new_session": True}


def kill_tree(proc):
    if proc.poll() is not None:
        return
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=15)
        else:
            import signal
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except Exception:
        pass
    try:
        proc.kill()
    except Exception:
        pass


class Host:
    """The conformance host process: READY handshake, output capture, guaranteed shutdown."""

    def __init__(self, cmd):
        self.cmd = cmd
        self.lines = []
        self.url = None
        self.proc = None
        self._q = queue.Queue()

    def start(self, timeout):
        try:
            self.proc = subprocess.Popen(self.cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, **_popen_flags())
        except OSError as e:
            raise SetupError("cannot start the host %s: %s" % (self.cmd, e))
        threading.Thread(target=self._pump, daemon=True).start()
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self._q.get(timeout=0.2)
            except queue.Empty:
                if self.proc.poll() is not None and self._q.empty():
                    raise SetupError("the host exited (code %s) before READY.\n%s" % (self.proc.returncode, self.tail()))
                continue
            if line is None:
                raise SetupError("the host closed its output before READY.\n%s" % self.tail())
            m = READY_RE.match(line)
            if m:
                self.url = m.group(1)
                return self.url
        raise SetupError("the host printed no READY line within %d s.\n%s" % (timeout, self.tail()))

    def _pump(self):
        for raw in iter(self.proc.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            self.lines.append(line)
            self._q.put(line)
        self._q.put(None)

    def tail(self, n=40):
        return "host output (last %d lines):\n  %s" % (n, "\n  ".join(self.lines[-n:]) or "(none)")

    def stop(self):
        p = self.proc
        if p is None:
            return
        try:
            if p.stdin:
                p.stdin.close()  # EOF on stdin is a clean-shutdown request
        except Exception:
            pass
        for action in (lambda: None, p.terminate):
            try:
                action()
                p.wait(timeout=5)
                break
            except Exception:
                continue
        kill_tree(p)
        for stream in (p.stdin, p.stdout):
            try:
                stream.close()
            except Exception:
                pass


def run_suite(cmd, timeout, env, out):
    """Runs the suite command, echoing its output live. Returns (output_text, timed_out)."""
    try:
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=env, **_popen_flags())
    except OSError as e:
        raise SetupError("cannot start %s: %s" % (cmd[0], e))
    chunks = []

    def pump():
        for raw in iter(proc.stdout.readline, b""):
            line = raw.decode("utf-8", "replace")
            chunks.append(line)
            out(ANSI_RE.sub("", line.rstrip("\r\n")))

    t = threading.Thread(target=pump, daemon=True)
    t.start()
    timed_out = False
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
    finally:
        kill_tree(proc)
    t.join(10)
    try:
        proc.stdout.close()
    except Exception:
        pass
    return "".join(chunks), timed_out


def suite_prefix(version):
    override = os.environ.get("MCP_CONFORMANCE_SUITE_CMD")
    if override:
        try:
            prefix = json.loads(override)
            assert isinstance(prefix, list) and prefix
            return [str(x) for x in prefix]
        except Exception:
            raise SetupError("MCP_CONFORMANCE_SUITE_CMD must be a JSON list")
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if not npx:
        raise SetupError("npx not found: install Node.js (actions/setup-node in CI)")
    return [npx, "-y", "%s@%s" % (SUITE_PACKAGE, version)]


# --------------------------------------------------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", help="path of %s (default: found under --build-dir)" % HOST_NAME)
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    ap.add_argument("--expected", default=os.path.join(ROOT, "tests", "conformance", "expected-failures.json"))
    ap.add_argument("--summary", default=None, help="default: <build-dir>/conformance-summary.json")
    ap.add_argument("--port", type=int, default=27090)
    ap.add_argument("--timeout", type=int, default=900, help="seconds allowed per run (default 900)")
    args = ap.parse_args(argv)

    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    def out(line=""):
        print(line, flush=True)

    summary_path = args.summary or os.path.join(args.build_dir, "conformance-summary.json")
    host = None
    workdir = tempfile.mkdtemp(prefix="mcp-conformance-")
    try:
        version, runs = load_expected(args.expected)
        host_path = args.host or find_host(args.build_dir)
        if not host_path or not os.path.isfile(host_path):
            raise SetupError("%s not found under %s (build it first: cmake --build build)" % (HOST_NAME, args.build_dir))
        prefix = suite_prefix(version)
        host = Host(host_command(host_path) + ["--port", str(args.port)])
        url = host.start(60)
        out("host ready at %s (%s)" % (url, host_path))
        env = dict(os.environ, NO_COLOR="1", FORCE_COLOR="0", CI="1")

        summary, failed = {}, False
        for run in sorted(runs, reverse=True):
            out_dir = os.path.join(workdir, run)
            cmd = prefix + ["server", "--url", url, "--requirements", run, "--output-dir", out_dir]
            out("")
            out("running suite %s, requirements %s: %s" % (version, run, " ".join(cmd)))
            text, timed_out = run_suite(cmd, args.timeout, env, out)
            if timed_out:
                out("ERROR: the suite did not finish within %d s (killed)" % args.timeout)
                failed = True
            scenarios = parse_scenarios(out_dir, text)
            result = compare(scenarios, runs[run], parse_not_scored(text))
            incomplete = None if timed_out else completeness_problem(scenarios, text)
            if incomplete:
                out("ERROR: " + incomplete)
            if timed_out or incomplete:
                result["ok"] = False
            report(run, version, result, out)
            summary[run] = summary_entry(result)
            failed = failed or not result["ok"]
            if host.proc.poll() is not None:
                out("ERROR: the host died during the run.\n" + host.tail())
                failed = True
                break
        os.makedirs(os.path.dirname(os.path.abspath(summary_path)), exist_ok=True)
        with open(summary_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(summary, f, indent=2)
            f.write("\n")
        out("")
        out("summary written to %s" % summary_path)
        out("conformance: FAILED" if failed else "conformance: OK")
        return 1 if failed else 0
    except SetupError as e:
        out("SETUP ERROR: %s" % e)
        return 2
    finally:
        if host is not None:
            host.stop()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
