#!/usr/bin/env python3
"""Render the Garry's Mod MCP comparison heatmap from data.json.

Stdlib only. Outputs two SVG heatmaps (light and dark) and a Markdown table
spliced between markers in docs/COMPARISON.md. Output is deterministic: data
order is display order, nothing is sorted or timestamped.
"""

import argparse
import html
import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

START_MARKER = "<!-- comparison:start -->"
END_MARKER = "<!-- comparison:end -->"

VERDICTS = ("yes", "partial", "no", "unverified")
GLYPHS = {"yes": "✅", "partial": "🟡", "no": "❌", "unverified": "❔"}
GROUPS = {"features": "Features", "trust": "Trust & ops"}
REALM_KEYS = ("S", "C", "M")
REALMS_ID = "realms"
README_LIMIT = 8
SHA_RE = re.compile(r"[0-9a-f]{40}")

# SVG geometry (mockup v7)
CELL_W, CELL_H, GAP = 30, 16, 3
REALMS_W = 44
GROUP_GAP = 8
PAD = 8
NAME_CHAR_W = 6.2
GROUP_Y = 13
GROUP_LINE_Y = 18
HDR_Y1, HDR_Y2 = 32, 44
GRID_TOP = 51
ROW_H = CELL_H + GAP
PICK_H = 17
FOOT_H = 15
FOOT_CHAR_W = 6.2  # estimated advance of an 11 px footer character

VERDICT_FILL = {"yes": "#2e9d5b", "partial": "#d9a520", "no": "#c94040"}

THEMES = {
    "light": {"text": "#24292f", "muted": "#57606a", "line": "#8c959f", "hatch": "#8c959f"},
    "dark": {"text": "#e6edf3", "muted": "#9198a1", "line": "#6e7681", "hatch": "#8b949e"},
}


# ---------------------------------------------------------------- data

def _check_cell(repo, column, cell, prefixes):
    where = f"{repo}: column {column}"
    if not isinstance(cell, dict):
        raise ValueError(f"{where}: cell must be an object")
    if "star" in cell:
        raise ValueError(f"{where}: 'star' is computed by render.py, remove it from the data")
    verdict = cell.get("v")
    if verdict not in VERDICTS:
        raise ValueError(f"{where}: unknown verdict {verdict!r}")
    evidence = cell.get("evidence")
    if verdict != "unverified" and not evidence:
        raise ValueError(f"{where}: verdict {verdict!r} requires evidence")
    if evidence and not str(evidence).startswith(prefixes):
        raise ValueError(
            f"{where}: evidence must be pinned to the repo and its sha "
            f"({prefixes[0]}..., {prefixes[1]}... or {prefixes[2]}...), got {evidence!r}"
        )


def _evidence_prefixes(repo):
    base = str(repo["url"]).rstrip("/")
    sha = repo["sha"]
    return (f"{base}/blob/{sha}/", f"{base}/tree/{sha}", f"{base}/releases/download/")


def load_data(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    for key in ("as_of", "columns", "repos"):
        if key not in data:
            raise ValueError(f"data: missing key {key!r}")
    data.setdefault("picks", [])

    for col in data["columns"]:
        for key in ("id", "label", "group", "definition"):
            if key not in col:
                raise ValueError(f"column {col.get('id', '?')}: missing key {key!r}")
        if col["group"] not in GROUPS:
            raise ValueError(f"column {col['id']}: unknown group {col['group']!r}")

    names = set()
    for repo in data["repos"]:
        name = repo.get("name", "?")
        for key in ("name", "url", "sha", "cells"):
            if key not in repo:
                raise ValueError(f"{name}: missing key {key!r}")
        if not isinstance(repo["sha"], str) or not SHA_RE.fullmatch(repo["sha"]):
            raise ValueError(f"{name}: sha must be 40 hex characters, got {repo['sha']!r}")
        prefixes = _evidence_prefixes(repo)
        names.add(name)
        for col in data["columns"]:
            cell = repo["cells"].get(col["id"])
            if cell is None:
                raise ValueError(f"{name}: column {col['id']}: missing cell")
            if col["id"] == REALMS_ID:
                for key in REALM_KEYS:
                    if not isinstance(cell, dict) or key not in cell:
                        raise ValueError(f"{name}: column {col['id']}: missing realm {key}")
                    _check_cell(name, f"{col['id']}.{key}", cell[key], prefixes)
            else:
                _check_cell(name, col["id"], cell, prefixes)

    for pick in data["picks"]:
        if pick.get("repo") not in names:
            raise ValueError(f"pick {pick.get('label')!r}: unknown repo {pick.get('repo')!r}")
    data["stars"] = compute_stars(data)
    return data


def compute_stars(data):
    """Return {(repo name, column id)} for every cell that is the only 'yes' in its column (realms excluded)."""
    stars = set()
    for col in data["columns"]:
        if col["id"] == REALMS_ID:
            continue
        yes = [r["name"] for r in data["repos"] if r["cells"][col["id"]]["v"] == "yes"]
        if len(yes) == 1:
            stars.add((yes[0], col["id"]))
    return stars


# --------------------------------------------------------------- table

def _md(text):
    return str(text).replace("|", "\\|")


def _url(url):
    return str(url).replace(" ", "%20").replace("(", "%28").replace(")", "%29")


def _glyph(cell):
    glyph = GLYPHS[cell["v"]]
    if cell["v"] == "unverified":
        return glyph
    return f"[{glyph}]({_url(cell['evidence'])})"


def _is_star(data, repo, col):
    stars = data["stars"] if "stars" in data else compute_stars(data)
    return (repo["name"], col["id"]) in stars


def _table_cell(data, repo, col):
    cell = repo["cells"][col["id"]]
    if col["id"] == REALMS_ID:
        return " ".join(f"{key}{_glyph(cell[key])}" for key in REALM_KEYS)
    return _glyph(cell) + (" ★" if _is_star(data, repo, col) else "")


def render_table(data):
    cols = data["columns"]
    lines = [
        "| " + " | ".join(["Repo"] + [_md(c["label"]) for c in cols]) + " |",
        "|" + "|".join(["---"] + [":-:"] * len(cols)) + "|",
    ]
    for repo in data["repos"]:
        cells = [f"[{_md(repo['name'])}]({_url(repo['url'])})"]
        cells += [_table_cell(data, repo, c) for c in cols]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def replace_between_markers(text, block):
    start = text.find(START_MARKER)
    end = text.find(END_MARKER)
    if start < 0 or end < 0:
        raise ValueError(f"markers {START_MARKER} / {END_MARKER} not found")
    if end < start:
        raise ValueError(f"{END_MARKER} comes before {START_MARKER}")
    head = text[: start + len(START_MARKER)]
    return head + "\n" + block.strip("\n") + "\n" + text[end:]


# ----------------------------------------------------------------- svg

def star_sentence(total):
    return f'★ = the only server with a verified "yes" in this column (out of {total} compared)'


def _esc(text):
    return html.escape(str(text), quote=True)


def _n(value):
    return f"{value:.1f}".rstrip("0").rstrip(".")


def _header_lines(col):
    lines = col.get("lines")
    if lines:
        return list(lines)
    label = col["label"]
    if " " in label:
        words = label.split(" ")
        best = min(range(1, len(words)), key=lambda i: abs(len(" ".join(words[:i])) - len(" ".join(words[i:]))))
        return [" ".join(words[:best]), " ".join(words[best:])]
    if "-" in label.strip("-"):
        i = label.index("-") + 1
        return [label[:i], label[i:]]
    return [label]


def _layout_columns(cols):
    """Return per-column (x, width) and per-group (start, end), left to right."""
    x = 0.0
    out = []
    spans = []
    prev = None
    for col in cols:
        if prev is not None and col["group"] != prev:
            x += GROUP_GAP
        width = REALMS_W if col["id"] == REALMS_ID else CELL_W
        out.append((x, width))
        if not spans or spans[-1][0] != col["group"]:
            spans.append([col["group"], x, x + width])
        else:
            spans[-1][2] = x + width
        x += width + GAP
        prev = col["group"]
    return out, spans, x - GAP


def render_svg(data, theme, limit=None):
    if theme not in THEMES:
        raise ValueError(f"unknown theme {theme!r}")
    colors = THEMES[theme]
    repos = data["repos"] if limit is None else data["repos"][:limit]
    cols = data["columns"]

    name_w = max((len(r["name"]) for r in repos), default=0) * NAME_CHAR_W
    x0 = PAD + math.ceil(name_w) + 8
    col_pos, spans, grid_w = _layout_columns(cols)
    grid_right = x0 + grid_w
    grid_bottom = GRID_TOP + len(repos) * ROW_H - GAP

    body = []

    # group headers
    for group, start, end in spans:
        body.append(f'<text class="grp" x="{_n(x0 + start)}" y="{GROUP_Y}">{_esc(GROUPS[group])}</text>')
        body.append(
            f'<line class="grpline" x1="{_n(x0 + start)}" y1="{GROUP_LINE_Y}" '
            f'x2="{_n(x0 + end)}" y2="{GROUP_LINE_Y}"/>'
        )

    # column headers
    for col, (cx, width) in zip(cols, col_pos):
        center = x0 + cx + width / 2
        lines = _header_lines(col)
        ys = [HDR_Y2] if len(lines) == 1 else [HDR_Y1, HDR_Y2]
        spans_xml = "".join(
            f'<tspan x="{_n(center)}" y="{y}">{_esc(line)}</tspan>' for line, y in zip(lines, ys)
        )
        body.append(f'<text class="hdr" x="{_n(center)}" text-anchor="middle">{spans_xml}</text>')

    # rows
    for ri, repo in enumerate(repos):
        y = GRID_TOP + ri * ROW_H
        body.append(f'<text class="repo" x="{PAD}" y="{y + 12}">{_esc(repo["name"])}</text>')
        for col, (cx, width) in zip(cols, col_pos):
            x = x0 + cx
            cell = repo["cells"][col["id"]]
            if col["id"] == REALMS_ID:
                body.append(
                    f'<rect class="cell realms" x="{_n(x)}" y="{y}" width="{width}" '
                    f'height="{CELL_H}" rx="3" fill="none"/>'
                )
                for key, dx in zip(REALM_KEYS, (-13, 0, 13)):
                    verdict = cell[key]["v"]
                    body.append(
                        f'<text class="realm r-{verdict}" x="{_n(x + width / 2 + dx)}" y="{y + 12}" '
                        f'text-anchor="middle">{key}</text>'
                    )
                continue
            verdict = cell["v"]
            fill = "url(#hatch)" if verdict == "unverified" else VERDICT_FILL[verdict]
            body.append(
                f'<rect class="cell v-{verdict}" x="{_n(x)}" y="{y}" width="{width}" '
                f'height="{CELL_H}" rx="3" fill="{fill}"/>'
            )
            if _is_star(data, repo, col):
                body.append(
                    f'<text class="star" x="{_n(x + width / 2)}" y="{y + 12}" '
                    f'text-anchor="middle">★</text>'
                )

    # picks
    y = grid_bottom + 20
    right = grid_right
    pick_w = max((len(p["label"]) for p in data["picks"]), default=0) * 6.6 + 12
    for pick in data["picks"]:
        body.append(f'<text class="pick" x="{PAD}" y="{y}">{_esc(pick["label"])}</text>')
        body.append(f'<text class="pickrepo" x="{_n(PAD + pick_w)}" y="{y}">{_esc(pick["repo"])}</text>')
        right = max(right, PAD + pick_w + len(pick["repo"]) * NAME_CHAR_W)
        y += PICK_H

    # legend + footer
    y += 6 if data["picks"] else 14
    lx = float(PAD)
    for verdict, word in (("yes", "yes"), ("partial", "partial"), ("no", "no"), ("unverified", "not verified")):
        fill = "url(#hatch)" if verdict == "unverified" else VERDICT_FILL[verdict]
        body.append(
            f'<rect class="sw v-{verdict}" x="{_n(lx)}" y="{y - 10}" width="11" height="11" rx="2" fill="{fill}"/>'
        )
        body.append(f'<text class="foot" x="{_n(lx + 15)}" y="{y}">{_esc(word)}</text>')
        lx += 15 + len(word) * 5.6 + 12
    rest = f"as of {data['as_of']} · pinned commits · code read, nothing executed"
    body.append(f'<text class="foot" x="{_n(lx)}" y="{y}">{_esc(rest)}</text>')
    right = max(right, lx + len(rest) * FOOT_CHAR_W)

    y += FOOT_H
    star_note = star_sentence(len(data["repos"]))
    body.append(f'<text class="foot" x="{PAD}" y="{y}">{_esc(star_note)}</text>')
    right = max(right, PAD + len(star_note) * FOOT_CHAR_W)

    width = math.ceil(right + PAD)
    height = y + PAD + 2
    label = "Comparison of Garry's Mod MCP servers"
    style = (
        f".hdr,.repo,.pick,.pickrepo,.foot,.grp,.realm,.star{{font-family:-apple-system,'Segoe UI',Helvetica,Arial,sans-serif}}"
        f".repo,.pickrepo{{font-size:11px;fill:{colors['text']}}}"
        f".pick{{font-size:11px;font-weight:700;fill:{colors['text']}}}"
        f".hdr{{font-size:10.5px;font-weight:600;fill:{colors['text']}}}"
        f".grp{{font-size:11px;letter-spacing:.05em;fill:{colors['muted']}}}"
        f".grpline{{stroke:{colors['line']};stroke-width:1}}"
        f".foot{{font-size:11px;fill:{colors['muted']}}}"
        f".star{{font-size:11px;fill:#fff}}"
        f".realm{{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:11px;font-weight:700}}"
        f".r-yes{{fill:{VERDICT_FILL['yes']}}}.r-partial{{fill:{VERDICT_FILL['partial']}}}"
        f".r-no{{fill:{VERDICT_FILL['no']};opacity:.45}}.r-unverified{{fill:{colors['muted']};opacity:.6}}"
        f".realms{{stroke:{colors['line']};stroke-width:1}}"
        f".v-no{{opacity:.85}}"
        f".v-unverified{{stroke:{colors['line']};stroke-width:1}}"
        f".hatchline{{stroke:{colors['hatch']};stroke-width:3;opacity:.27}}"
    )
    head = (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-label="{_esc(label)}">'
        f"<title>{_esc(label)}</title>"
        f"<style>{style}</style>"
        '<defs><pattern id="hatch" patternUnits="userSpaceOnUse" width="6" height="6" '
        'patternTransform="rotate(45)"><line class="hatchline" x1="0" y1="0" x2="0" y2="6"/></pattern></defs>'
    )
    return head + "\n" + "\n".join(body) + "\n</svg>\n"


# ---------------------------------------------------------------- main

def _write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--data", type=Path, default=ROOT / "docs" / "comparison" / "data.json")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "docs" / "comparison")
    parser.add_argument("--comparison-md", type=Path, default=ROOT / "docs" / "COMPARISON.md")
    parser.add_argument("--limit", type=int, default=README_LIMIT, help="repos drawn in the SVGs")
    args = parser.parse_args(argv)

    try:
        data = load_data(args.data)
        outputs = {
            args.out_dir / f"comparison-{theme}.svg": render_svg(data, theme, args.limit)
            for theme in ("light", "dark")
        }
        md = args.comparison_md.read_text(encoding="utf-8")
        outputs[args.comparison_md] = replace_between_markers(md, render_table(data))
    except (ValueError, OSError) as exc:
        print(f"render.py: {exc}", file=sys.stderr)
        return 1

    for path, text in outputs.items():
        _write(path, text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
