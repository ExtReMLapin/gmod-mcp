"""Prints the release notes of one version from CHANGELOG.md (its "## <version>" section), with wrapped lines joined
(GitHub shows every newline of a release body as a line break), followed by the compare link.
Usage: python release_notes.py <version> [<previous tag>]   Exits non-zero when the section is missing."""
import re, sys

version = sys.argv[1].lstrip("v")
prev = sys.argv[2] if len(sys.argv) > 2 else ""
text = open("CHANGELOG.md", encoding="utf-8").read()
m = re.search(r"^## " + re.escape(version) + r"\s*$(.*?)(?=^## |\Z)", text, re.M | re.S)
if not m:
    sys.exit("CHANGELOG.md has no '## %s' section" % version)
out, cur = [], None
for line in m.group(1).strip("\n").splitlines():
    stripped = line.strip()
    starts_block = not stripped or stripped.startswith(("#", "- ", "* ", "|", "```")) or re.match(r"\d+\. ", stripped)
    if cur is not None and stripped and not starts_block:
        cur += " " + stripped  # continuation of the paragraph or list item
        continue
    if cur is not None:
        out.append(cur)
    cur = line.rstrip() if stripped else None
    if not stripped:
        out.append("")
if cur is not None:
    out.append(cur)
body = "\n".join(out).strip() + "\n"
if prev:
    body += "\n**Full Changelog**: https://github.com/ExtReMLapin/gmod-mcp/compare/%s...v%s\n" % (prev, version)
sys.stdout.buffer.write(body.encode("utf-8"))  # not the console code page (Windows)
