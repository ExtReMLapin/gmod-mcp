#!/usr/bin/env python3
"""Fails if a conformance fixture (a tool, prompt or resource that exists only for the MCP conformance suite) leaked into
what ships: the module (gmsv_mcpagent_*.dll / .so), the bridge (gmod-mcp-bridge[.exe]) or the shipped sources.

The fixtures live in tests/conformance/ and in the executable gmod-mcp-conformance-host only. The names to look for come
from three places, so the check stays in sync with the fixtures without a hand-kept second list:
  1. KNOWN_NAMES below: the names of CONTRACT.md plus the fixtures of the pinned suite (tests/conformance/run.py);
  2. tests/conformance/fixture-names.txt, if present: one name per line, "#" comments (a place for the host's author to
     add names that do not look like snake_case, e.g. single words such as "greet");
  3. every snake_case string literal ("some_name") in the C/C++ sources under tests/conformance/, except the ones that
     the shipped sources (src/, loader/, plugin/) also contain: those are shipped vocabulary, not fixtures.
A new fixture in the host sources is therefore covered as soon as its name is a snake_case literal.

Checks
  - source: no name of 1. or 2. appears as a string literal in src/, loader/ or plugin/;
  - binary: no name of 1., 2. or 3. appears as a string in the module or the bridge (a NUL-delimited string or a
    JSON string "name"). The module and the bridge must exist under --build-dir, otherwise the check fails (it must
    never pass because there was nothing to scan);
  - positive control: the scanner must see the fixtures where they are. The conformance host executable
    (gmod-mcp-conformance-host[.exe]) must exist under --build-dir and contain at least MIN_HOST_FRACTION (80%) of the
    names of 1., 2. and 3. If it does not (a toolchain change, e.g. string pooling or UTF-16 literals, that hides the
    strings from this scan), "clean" would mean nothing, so the check fails;
  - hook seam: mcp::set_request_hooks (the test-only seam of the conformance host) must not be called from src/,
    loader/ or plugin/; only its declaration (src/mcp.h) and definition (src/mcp.cpp) may contain it.

Usage: python tests/conformance/check_fixtures_absent.py [--build-dir build]
"""
import argparse
import os
import re
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
MIN_HOST_FRACTION = 0.8
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
    for n in sorted(fixed):
        if literal_in(n, shipped):
            problems.append("source: fixture name %r appears as a literal in src/, loader/ or plugin/" % n)

    for use in hook_setter_uses(root):
        problems.append("source: %s is a test-only seam of the conformance host, not for shipped code: %s"
                        % (HOOK_SETTER, use))

    modules, bridges, hosts = find_binaries(args.build_dir)
    if not modules:
        problems.append("no module (gmsv_mcpagent_*.dll/.so) found under %s: nothing to scan" % args.build_dir)
    if not bridges:
        problems.append("no bridge (gmod-mcp-bridge[.exe]) found under %s: nothing to scan" % args.build_dir)
    for path in modules + bridges:
        with open(path, "rb") as f:
            data = f.read()
        hits = [n for n in names if in_binary(n, data)]
        print("scanned %s (%d bytes): %s" % (rel(path), len(data),
                                             "clean" if not hits else "LEAK " + ", ".join(hits)))
        for n in hits:
            problems.append("binary: fixture name %r found in %s" % (n, rel(path)))

    if not hosts:
        problems.append("no conformance host (gmod-mcp-conformance-host[.exe]) found under %s: no positive control"
                        % args.build_dir)
    for path in hosts:
        with open(path, "rb") as f:
            data = f.read()
        found = [n for n in names if in_binary(n, data)]
        need = int(MIN_HOST_FRACTION * len(names) + 0.999999)
        print("positive control %s: %d of %d fixture names found (at least %d needed)"
              % (rel(path), len(found), len(names), need))
        if len(found) < need:
            problems.append("positive control: only %d of %d fixture names found in %s (need %d): the scan cannot see "
                            "fixture strings in this toolchain's output, so a clean result would mean nothing"
                            % (len(found), len(names), rel(path), need))

    if problems:
        print("\nFIXTURE LEAK: conformance fixtures must exist only in tests/conformance/ and "
              "gmod-mcp-conformance-host:")
        for p in problems:
            print("  - " + p)
        return 1
    print("OK: no conformance fixture in the shipped module, bridge or sources (positive control passed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
