"""Checks src/mcp_protocol.h against the official MCP schema, so a typo in an error code or a _meta key cannot go
unnoticed. The schema is pinned to a commit of the specification repository and verified by its SHA-256.

Usage: python check_constants.py [schema.json]   (downloads the pinned schema when no file is given)
Exits non-zero on a mismatch."""
import hashlib, json, os, re, sys, urllib.request

COMMIT = "200d5339bbc7649ee92d6445dc17368e2b5b11ba"
URL = "https://raw.githubusercontent.com/modelcontextprotocol/modelcontextprotocol/%s/schema/2026-07-28/schema.json" % COMMIT
SHA256 = "edbb197ed0f92e90d658866e23cf76f107df0caac5ce25c2b9e64e1ca9a6314a"
HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "mcp_protocol.h")

# Constant in mcp_protocol.h -> error definition in the schema.
ERRORS = {
    "kParseError": "ParseError",
    "kInvalidRequest": "InvalidRequestError",
    "kMethodNotFound": "MethodNotFoundError",
    "kInvalidParams": "InvalidParamsError",
    "kInternalError": "InternalError",
    "kHeaderMismatch": "HeaderMismatchError",
    "kMissingRequiredClientCapability": "MissingRequiredClientCapabilityError",
    "kUnsupportedProtocolVersion": "UnsupportedProtocolVersionError",
}
# kResourceNotFoundLegacy (-32002) is only described in the text of the 2025 revisions, not as a schema constant.
NOT_IN_SCHEMA = {"kResourceNotFoundLegacy"}


def load_schema():
    if len(sys.argv) > 1:
        data = open(sys.argv[1], "rb").read()
    else:
        data = urllib.request.urlopen(URL, timeout=30).read()
    got = hashlib.sha256(data).hexdigest()
    if got != SHA256:
        sys.exit("schema SHA-256 %s, expected %s" % (got, SHA256))
    return json.loads(data)


def main():
    schema = load_schema()
    defs = schema.get("$defs") or schema.get("definitions") or {}
    text = json.dumps(schema)
    header = open(HEADER, encoding="utf-8").read()
    codes = dict(re.findall(r"inline constexpr int (k\w+) = (-?\d+);", header))
    strings = dict(re.findall(r'inline constexpr const char \*(k\w+) = "([^"]+)";', header))
    failures = []

    for name, value in codes.items():
        if name in NOT_IN_SCHEMA:
            continue
        d = ERRORS.get(name)
        if d is None:
            failures.append("%s: no schema definition mapped (add it to ERRORS)" % name)
            continue
        consts = re.findall(r'"const": (-?\d+)', json.dumps(defs.get(d, {})))
        if consts != [value]:
            failures.append("%s = %s, schema %s says %s" % (name, value, d, consts or "nothing"))
    for name, d in ERRORS.items():
        if name not in codes:
            failures.append("%s (%s) missing from mcp_protocol.h" % (name, d))

    for name, value in strings.items():
        if name.startswith("kMeta") and value not in text:
            failures.append('%s = "%s" does not appear in the schema' % (name, value))

    for f in failures:
        print("FAIL " + f)
    print("%d error codes, %d _meta keys checked against schema %s: %s" % (
        len(codes) - len(NOT_IN_SCHEMA), sum(1 for n in strings if n.startswith("kMeta")), COMMIT[:12],
        "OK" if not failures else "%d failure(s)" % len(failures)))
    sys.exit(1 if failures else 0)


main()
