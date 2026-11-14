#!/usr/bin/env python3
"""Fails if a conformance fixture (a tool, prompt or resource that exists only for the MCP conformance suite) leaked into
what ships: the module (gmsv_mcpagent_*.dll / .so), the bridge (gmod-mcp-bridge[.exe]) or the shipped sources.

The fixtures live in tests/conformance/ and in the executable gmod-mcp-conformance-host only. The names to look for come
from three places, so the check stays in sync with the fixtures without a hand-kept second list:
  (a) KNOWN_NAMES below: the names of CONTRACT.md plus the fixtures of the pinned suite (tests/conformance/run.py);
  (b) tests/conformance/fixture-names.txt, if present: one name per line, "#" comments (a place for the host's author to
     add names that do not look like snake_case, e.g. single words such as "greet");
  (c) every snake_case string literal ("some_name") in the C/C++ sources under tests/conformance/, except the ones that
     the shipped sources (src/, loader/, plugin/) also contain: those are shipped vocabulary, not fixtures.
A new fixture in the host sources is therefore covered as soon as its name is a snake_case literal.

Checks, strongest guarantee first
  1. build graph (deterministic, the primary guarantee): the CMake File API (codemodel-v2) lists each target's sources
     and include directories, whatever the generator (Visual Studio on the Windows leg, Makefiles or Ninja on Linux).
     No source under tests/conformance/ may belong to a target other than gmod-mcp-conformance-host, and no other
     target may have tests/conformance/ on its include path. A fixture cannot be linked into the module or the bridge
     without one of those two, so this does not depend on what a compiler does with string literals. The host target must
     exist and own at least one such source (otherwise the check proves nothing). The query file is written before the
     configure step (see .github/workflows/build.yml); when the build directory has no reply yet, this script writes it
     and re-runs cmake on the build directory once;
  2. source: no name of (a) or (b) below, and no canary, appears as a string literal in src/, loader/ or plugin/;
  3. hook seam: mcp::set_request_hooks (the test-only seam of the conformance host) must not be called from src/,
     loader/ or plugin/; only its declaration (src/mcp.h) and definition (src/mcp.cpp) may contain it;
  4. binary (secondary: a compiler may build a short string with immediate stores, so a name can be invisible in a
     binary): no name from (a), (b) or (c) below appears as a string in the module or the bridge (a NUL-delimited string or a
     JSON string "name"), and neither contains the canary. The module and the bridge must exist under --build-dir,
     otherwise the check fails (it must never pass because there was nothing to scan);
  5. canary (the control of 4.): the host (tests/conformance/conformance_host.cpp) holds one long unique string,
     CANARY, that the compiler must keep (the host prints it for --version). The conformance host executable
     (gmod-mcp-conformance-host[.exe]) must exist under --build-dir and contain it, so the scan is proven able to see a
     string of the host's own. How many fixture names the host binary shows is printed for information only: short names
     may be hidden by the toolchain, which is why this is not a threshold.

The names to look for (a) to (c) come from three places, so the check stays in sync with the fixtures without a hand-kept second list:
  (a) KNOWN_NAMES below: the names of CONTRACT.md plus the fixtures of the pinned suite (tests/conformance/run.py);
  (b) tests/conformance/fixture-names.txt, if present: one name per line, "#" comments (a place for the host's author to
     add names that do not look like snake_case, e.g. single words such as "greet");
  (c) every snake_case string literal ("some_name") in the C/C++ sources under tests/conformance/, except the ones that
     the shipped sources (src/, loader/, plugin/) also contain: those are shipped vocabulary, not fixtures.

Usage: python tests/conformance/check_fixtures_absent.py [--build-dir build]
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONF = os.path.join(ROOT, "tests", "conformance")

KNOWN_NAMES = """
test_simple_text slow_compute greet failing_job confirm_delete multi_input json_schema_2020_12_tool
test_audio_content test_client_elicitation_defaults test_complete test_custom_headers test_custom_headers_null
test_elicitation test_elicitation_sep1034_defaults test_elicitation_sep1330_enums test_embedded_resource
test_error_handling test_headers test_image_content test_logging_tool test_missing_capability test_multiple_content_types
test_prompt test_prompt_with_arguments test_prompt_with_embedded_resource test_prompt_with_image test_reconnection
test_sampling test_simple_prompt test_streaming_elicitation test_tool_with_logging test_tool_with_progress
test_tool_with_task test_trigger_prompt_change test_trigger_tool_change
test_input_required_result_capabilities test_input_required_result_elicitation test_input_required_result_list_roots
test_input_required_result_multi_round test_input_required_result_multiple_inputs test_input_required_result_prompt
test_input_required_result_request_state test_input_required_result_sampling test_input_required_result_tampered_state
test_mrtr_echo_state test_mrtr_no_result_type test_mrtr_no_state test_mrtr_unrelated
protocol_error_job
""".split()

SHIPPED_DIRS = ("src", "loader", "plugin")
HOST_SOURCE_EXT = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".inc")
SHIPPED_EXT = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".inc", ".lua", ".json", ".md", ".txt", ".cmake")
LITERAL_RE = re.compile(r'"([a-z][a-z0-9]*(?:_[a-z0-9]+)+)"')
MODULE_RE = re.compile(r"^gmsv_mcpagent_.*\.(dll|so)$")
BRIDGE_NAMES = ("gmod-mcp-bridge", "gmod-mcp-bridge.exe")
HOST_NAMES = ("gmod-mcp-conformance-host", "gmod-mcp-conformance-host.exe")
CANARY = "GMOD_MCP_CONFORMANCE_FIXTURE_CANARY_7f3c9a1e"
CANARY_SOURCE = "conformance_host.cpp"  # under tests/conformance/: the one file that must define it
HOST_TARGET = "gmod-mcp-conformance-host"
HOOK_SETTER = "set_request_hooks"
HOOK_SETTER_HOME = ("mcp.h", "mcp.cpp")  # under src/: the declaration and the definition


def walk(base, exts):
    for dirpath, dirs, files in os.walk(base):
        dirs[:] = [d for d in dirs if d not in ("testdata", "__pycache__", "build", ".git")]
        for f in sorted(files):
            if f.endswith(exts):
                yield os.path.join(dirpath, f)


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def fixture_names(conf_dir=CONF):
    """Returns (fixed, auto): the declared names (known + fixture-names.txt) and the snake_case literals of the host."""
    fixed = set(KNOWN_NAMES)
    path = os.path.join(conf_dir, "fixture-names.txt")
    if os.path.isfile(path):
        for line in read_text(path).splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                fixed.add(line)
    auto = set()
    for p in walk(conf_dir, HOST_SOURCE_EXT):
        auto.update(LITERAL_RE.findall(read_text(p)))
    return fixed, auto


def shipped_text(root=ROOT):
    parts = []
    for d in SHIPPED_DIRS:
        base = os.path.join(root, d)
        if os.path.isdir(base):
            parts.extend(read_text(p) for p in walk(base, SHIPPED_EXT))
    return "\n".join(parts)


def literal_in(name, text):
    return re.search(r"""["'`]%s["'`]""" % re.escape(name), text) is not None


def in_binary(name, data):
    b = name.encode("ascii", "ignore")
    return (b"\x00" + b + b"\x00") in data or (b'"' + b + b'"') in data


def find_binaries(build_dir):
    modules, bridges, hosts = [], [], []
    for dirpath, dirs, files in os.walk(build_dir):
        dirs[:] = [d for d in dirs if d not in ("CMakeFiles", "_deps")]
        for f in files:
            full = os.path.join(dirpath, f)
            if MODULE_RE.match(f):
                modules.append(full)
            elif f in BRIDGE_NAMES:
                bridges.append(full)
            elif f in HOST_NAMES:
                hosts.append(full)
    return sorted(modules), sorted(bridges), sorted(hosts)


def hook_setter_uses(root=ROOT):
    """Lines in the shipped sources that mention the test-only hook setter, other than its declaration and definition
    in src/mcp.h and src/mcp.cpp (comments are fine). Returns ["path:line: text"]."""
    found = []
    for d in SHIPPED_DIRS:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for path in walk(base, SHIPPED_EXT):
            home = d == "src" and os.path.dirname(path) == base and os.path.basename(path) in HOOK_SETTER_HOME
            for n, line in enumerate(read_text(path).splitlines(), 1):
                if HOOK_SETTER not in line:
                    continue
                code = line.strip()
                if code.startswith(("//", "*", "/*")):
                    continue
                if home and code.startswith("void %s(" % HOOK_SETTER):
                    continue
                found.append("%s:%d: %s" % (os.path.relpath(path, root).replace(os.sep, "/"), n, code[:120]))
    return found


def canary_in(data):
    return CANARY.encode("ascii") in data


def api_dir(build_dir):
    return os.path.join(build_dir, ".cmake", "api", "v1")


def write_query(build_dir):
    """Asks CMake for the code model at its next configure (the file's name is the request, its content is unused)."""
    q = os.path.join(api_dir(build_dir), "query")
    os.makedirs(q, exist_ok=True)
    open(os.path.join(q, "codemodel-v2"), "a").close()


def latest_codemodel(build_dir):
    """Returns (codemodel dict, reply dir) of the newest File API reply, or None."""
    reply = os.path.join(api_dir(build_dir), "reply")
    for index in sorted(glob.glob(os.path.join(reply, "index-*.json")), reverse=True):
        with open(index, encoding="utf-8") as f:
            objects = json.load(f).get("objects", [])
        for o in objects:
            if o.get("kind") == "codemodel" and o.get("version", {}).get("major") == 2:
                with open(os.path.join(reply, o["jsonFile"]), encoding="utf-8") as f:
                    return json.load(f), reply
    return None


def load_targets(build_dir):
    """Reads the targets of the CMake File API reply. Returns (source_dir, {name: {"sources": set, "includes": set}})
    with absolute normalised paths, or raises RuntimeError. A multi-config generator (Visual Studio) lists the same target
    once per configuration: they are merged."""
    found = latest_codemodel(build_dir)
    if found is None:
        # No reply: a build directory configured before the query existed. Write it and configure once more.
        if not os.path.isfile(os.path.join(build_dir, "CMakeCache.txt")):
            raise RuntimeError("no CMake File API reply and no CMakeCache.txt under %s: configure the project first"
                               % build_dir)
        write_query(build_dir)
        r = subprocess.run(["cmake", build_dir], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0:
            raise RuntimeError("cmake could not re-configure %s to produce the File API reply:\n%s"
                               % (build_dir, r.stdout[-2000:]))
        found = latest_codemodel(build_dir)
        if found is None:
            raise RuntimeError("cmake produced no codemodel-v2 reply under %s" % api_dir(build_dir))
    model, reply = found
    source_dir = model["paths"]["source"]
    targets = {}
    for config in model.get("configurations", []):
        for t in config.get("targets", []):
            with open(os.path.join(reply, t["jsonFile"]), encoding="utf-8") as f:
                j = json.load(f)
            entry = targets.setdefault(j["name"], {"sources": set(), "includes": set()})
            for s in j.get("sources", []):
                entry["sources"].add(norm_path(os.path.join(source_dir, s["path"])))
            for g in j.get("compileGroups", []):
                for inc in g.get("includes", []):
                    entry["includes"].add(norm_path(inc["path"]))
    return norm_path(source_dir), targets


def norm_path(p):
    return os.path.normcase(os.path.normpath(p)).replace("\\", "/")


def under(path, directory):
    return path == directory or path.startswith(directory.rstrip("/") + "/")


def build_graph_problems(source_dir, targets):
    """Fixture sources belong to the conformance host target and to no other; the host target exists."""
    conf = norm_path(os.path.join(source_dir, "tests", "conformance"))
    problems = []
    host = targets.get(HOST_TARGET)
    if host is None or not any(under(s, conf) for s in host["sources"]):
        problems.append("build graph: target %s not found or without a source under tests/conformance/: the check "
                        "would prove nothing" % HOST_TARGET)
    for name in sorted(targets):
        if name == HOST_TARGET:
            continue
        for s in sorted(targets[name]["sources"]):
            if under(s, conf):
                problems.append("build graph: source %s under tests/conformance/ belongs to target %r, not to %s"
                                % (os.path.relpath(s, source_dir).replace("\\", "/"), name, HOST_TARGET))
        for inc in sorted(targets[name]["includes"]):
            if under(inc, conf):
                problems.append("build graph: target %r has tests/conformance/ on its include path (%s)" % (name, inc))
    return problems


def rel(path):
    try:
        return os.path.relpath(path, ROOT)
    except ValueError:  # another drive
        return path


def main(argv=None, root=ROOT):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    args = ap.parse_args(argv)

    fixed, auto = fixture_names(os.path.join(root, "tests", "conformance"))
    shipped = shipped_text(root)
    auto_only = {n for n in auto if n not in fixed and not literal_in(n, shipped)}
    names = sorted(fixed | auto_only)
    print("fixture names checked: %d (%d known or listed, %d found in tests/conformance sources)"
          % (len(names), len(fixed), len(auto_only)))

    problems = []

    # 1. The build graph: the deterministic guarantee.
    try:
        source_dir, targets = load_targets(args.build_dir)
    except (RuntimeError, OSError, ValueError, KeyError) as e:
        problems.append("build graph: cannot read the CMake File API reply: %s" % e)
    else:
        graph = build_graph_problems(source_dir, targets)
        print("build graph: %d targets; %s" % (len(targets), "conformance sources only in %s" % HOST_TARGET
                                               if not graph else "VIOLATION"))
        problems.extend(graph)

    # 2. Sources.
    for n in sorted(fixed) + [CANARY]:
        if literal_in(n, shipped):
            problems.append("source: fixture name %r appears as a literal in src/, loader/ or plugin/" % n)
    host_source = os.path.join(root, "tests", "conformance", CANARY_SOURCE)
    if not os.path.isfile(host_source) or CANARY not in read_text(host_source):
        problems.append("source: tests/conformance/%s must define the canary %s" % (CANARY_SOURCE, CANARY))

    # 3. Hook seam.
    for use in hook_setter_uses(root):
        problems.append("source: %s is a test-only seam of the conformance host, not for shipped code: %s"
                        % (HOOK_SETTER, use))

    # 4. Binaries (secondary).
    modules, bridges, hosts = find_binaries(args.build_dir)
    if not modules:
        problems.append("no module (gmsv_mcpagent_*.dll/.so) found under %s: nothing to scan" % args.build_dir)
    if not bridges:
        problems.append("no bridge (gmod-mcp-bridge[.exe]) found under %s: nothing to scan" % args.build_dir)
    for path in modules + bridges:
        with open(path, "rb") as f:
            data = f.read()
        hits = [n for n in names if in_binary(n, data)]
        if canary_in(data):
            hits.append("the host canary")
        print("scanned %s (%d bytes): %s" % (rel(path), len(data),
                                             "clean" if not hits else "LEAK " + ", ".join(hits)))
        for n in hits:
            problems.append("binary: fixture name %r found in %s" % (n, rel(path)))

    # 5. The canary in the host.
    if not hosts:
        problems.append("no conformance host (gmod-mcp-conformance-host[.exe]) found under %s: no canary to find"
                        % args.build_dir)
    for path in hosts:
        with open(path, "rb") as f:
            data = f.read()
        seen = sum(1 for n in names if in_binary(n, data))
        has = canary_in(data)
        print("canary %s in %s; fixture names visible in it: %d of %d (informational: short names may be hidden by "
              "the toolchain)" % ("found" if has else "MISSING", rel(path), seen, len(names)))
        if not has:
            problems.append("canary: %s not found in %s: the scan cannot see the host's own strings, so a clean "
                            "result would mean nothing" % (CANARY, rel(path)))

    if problems:
        print("\nFIXTURE LEAK: conformance fixtures must exist only in tests/conformance/ and "
              "gmod-mcp-conformance-host:")
        for p in problems:
            print("  - " + p)
        return 1
    print("OK: no conformance fixture in the shipped module, bridge or sources (build graph and canary checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
