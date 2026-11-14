# Comparison heatmap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A dated, evidence-backed heatmap comparing gmod-mcp with the other Garry's Mod MCP servers, shown in the README and detailed in `docs/COMPARISON.md`, both generated from one data file.

**Architecture:** `docs/comparison/data.json` is the single source. `docs/comparison/render.py` (stdlib only) validates it and writes two SVGs (light/dark, top 8 repos) plus the Markdown table between markers in `docs/COMPARISON.md`. The README embeds the SVGs with `<picture>`.

**Tech Stack:** Python 3.12 stdlib (`json`, `html`, `pathlib`, `unittest`), SVG, GitHub Markdown.

**Spec:** `docs/superpowers/specs/2026-11-14-comparison-heatmap-design.md`

## Global Constraints

- Python stdlib only; no new dependency.
- All visible text in English.
- Verdicts: `yes`, `partial`, `no`, `unverified`. A `yes`/`partial`/`no` cell needs an `evidence` URL pinned to a commit SHA (`/blob/<sha>/path#Lx`) or to a release asset; otherwise it is `unverified`.
- ★ only on a `yes` cell, and only when that repo is the clear winner of the column (any repo, including gmod-mcp's competitors).
- Auth column: verdict only, no exploit detail in notes or evidence text.
- No stars, tool counts or aggregated scores anywhere.
- Dated: `as_of` = `2026-11-14`, shown in the README sentence, the SVG footer and COMPARISON.md.
- Work on a branch `docs/comparison-heatmap`; no commit to `main`, no push without the user's go.

## Review Focus

- A repo name containing `&`, `<` or `"` must render as valid SVG (escaped), not a broken image → test in Task 1.
- A label containing `|` must not break the Markdown table → test in Task 1.
- `docs/COMPARISON.md` without markers must fail loudly, not silently append → test in Task 1.
- Running the generator twice must produce byte-identical files (no churn in diffs) → test in Task 1.
- The longest repo name (`Pollux12/vscode-gmod-glua-ls`) must not overlap the first column of cells → label column width computed from the longest name; test in Task 1.

---

### Task 1: Generator (`render.py`) with tests

**Files:**
- Create: `docs/comparison/render.py`
- Create: `tests/comparison/test_render.py`
- Create: `tests/comparison/fixture.json` (3 repos, all columns, one name with `&`, one label with `|`)

**Interfaces:**
- Produces:
  - `load_data(path: pathlib.Path) -> dict` — raises `ValueError` naming repo and column for: unknown verdict; missing cell for a declared column; `yes`/`partial`/`no` without `evidence`; ★ on a non-`yes` cell.
  - Data shape: `{"as_of": str, "columns": [{"id", "label", "group": "features"|"trust", "definition"}], "repos": [{"name": "owner/repo", "url", "sha", "cells": {col_id: {"v": verdict, "evidence"?: url, "star"?: bool}}}], "picks": [{"label", "repo"}]}`. The `realms` cell is `{"S": cell, "C": cell, "M": cell}`.
  - `render_table(data: dict) -> str` — Markdown table, one row per repo in data order, first cell `[owner/repo](url)`, verdict glyphs ✅ 🟡 ❌ ❔, each non-unverified glyph linked to its evidence, ` ★` appended when starred; realms cell rendered `S✅ C✅ M❌`.
  - `render_svg(data: dict, theme: str, limit: int | None) -> str` — `theme` in `{"light", "dark"}`; `limit` = number of repos drawn (8 for README).
  - `replace_between_markers(text: str, block: str) -> str` — markers `<!-- comparison:start -->` / `<!-- comparison:end -->`; raises `ValueError` if either is missing.
  - `main(argv: list[str]) -> int` — default paths: `docs/comparison/data.json` → `docs/comparison/comparison-light.svg`, `comparison-dark.svg` (limit 8), and the table into `docs/COMPARISON.md`.

- [ ] **Step 1: Write the failing tests** in `tests/comparison/test_render.py` (`unittest`):
  - `test_rejects_unknown_verdict` / `test_rejects_missing_cell` / `test_rejects_yes_without_evidence` / `test_rejects_star_on_partial`: `assertRaisesRegex(ValueError, "<repo>.*<column>")`.
  - `test_table_links_repo_and_evidence`: output contains `[a/b](https://github.com/a/b)` and `[✅](<evidence url>)`.
  - `test_table_escapes_pipe_in_label`: label `a|b` appears as `a\|b`.
  - `test_svg_is_valid_xml_with_ampersand_name`: `xml.etree.ElementTree.fromstring(render_svg(...))` does not raise; text contains `&amp;`.
  - `test_svg_limit`: with `limit=2`, exactly 2 repo labels.
  - `test_svg_label_column_fits_longest_name`: first cell `x` ≥ `len("Pollux12/vscode-gmod-glua-ls") * 6.2 + 8`.
  - `test_dark_and_light_differ_only_in_colors`: same element count.
  - `test_replace_between_markers_requires_markers`: `assertRaises(ValueError)`.
  - `test_idempotent`: `main` run twice on a temp copy → identical bytes.

- [ ] **Step 2: Run tests to verify they fail**
  Run: `python -m unittest discover -s tests/comparison -v`
  Expected: errors, `ModuleNotFoundError` / missing functions.

- [ ] **Step 3: Implement `render.py`** with the interfaces above. SVG geometry from mockup v7: cells 28×16 px, gap 3 px, realms cell 44 px wide with three letters, two-line headers (`<tspan>`), group headers "Features" / "Trust & ops" with an 8 px gap, legend + footer `yes · partial · no · not verified · as of {as_of} · pinned commits · code read, nothing executed`, picks lines under the grid. Colors: yes `#2e9d5b`, partial `#d9a520`, no `#c94040`, unverified = diagonal hatch pattern; text `#24292f` (light) / `#e6edf3` (dark), transparent background. Escape all text with `html.escape(..., quote=True)`. Sort nothing: data order is display order. Write files with `newline="\n"`.

- [ ] **Step 4: Run tests to verify they pass**
  Run: `python -m unittest discover -s tests/comparison -v`
  Expected: all OK.

- [ ] **Step 5: Commit**
  ```bash
  git switch -c docs/comparison-heatmap
  git add docs/comparison/render.py tests/comparison
  git commit -m "docs: comparison heatmap generator"
  ```

### Task 2: Data file, with every cell verified or hatched

**Files:**
- Create: `docs/comparison/data.json`

**Interfaces:**
- Consumes: data shape and `load_data` from Task 1.
- Produces: `data.json` for the 22 repos, gmod-mcp first, then the order of mockup v7 (AmyJeanes/GMod-MCP-Server, Sycatle/gmod-mcp, TomDotBat/gmcl_mcp, Pollux12/vscode-gmod-glua-ls, Celant/GmodMCP, Earu/GTerm, mathis1M/gmod-mcp), then the other 14 from the benchmark matrix.

Inputs: `C:\Users\pierr\Documents\gmod-mcp-benchmark\research_notes\Benchmark des serveurs MCP gmod\*.md`, report `...\reports\Benchmark des serveurs MCP gmod.md` (tables A/B/C), clones in `...\repos\<owner>__<repo>` (read-only, never execute anything there). gmod-mcp itself: this repo at HEAD (v0.3.4).

Columns (id → label): `realms` Realms, `errors` Errors, `reload` Hot reload, `ents` Ents, `traces` Traces, `screenshot` Screenshot, `hooks` Hooks, `perf` Perf, `jobs` Jobs, `tests` Tests, `multi` Multi-inst., `debugger` Debugger, `input` Input | `pure_lua` Pure Lua, `auth` Auth, `no_inject` No inject, `dedicated` Dedicated, `linux` Linux, `install` 1-step install, `long_calls` Long calls, `mcp_spec` MCP spec, `ci` Own CI, `license` License, `feedback` Agent feedback.

Definitions to put in `definition` (and to apply identically to every repo):
- `long_calls`: yes = MCP Tasks, or progress notifications + cancellation with no blocking server-side timeout ≤ 30 s on game work; no = a blocking timeout ≤ 30 s cuts the call; partial = progress without cancellation or vice versa.
- `mcp_spec`: yes = declares protocol version ≥ `2025-06-18`, handles `notifications/cancelled`, returns `structuredContent` with `outputSchema`; partial = 1–2 of 3.
- `install`: yes = one command (plugin install, `npx`, extension install) plus at most copying files into the game; partial = several manual steps or an injector.
- `feedback`: yes = the tool itself invites the agent to report friction and that report is collected (gmod-mcp: `docs/RETEX.md`, plugin option `feedback_offers`).
- `auth`: yes = token/secret required, or no network listener at all.
- `license`: yes = LICENSE file present.

- [ ] **Step 1:** Fill every cell from the benchmark matrix (report tables A/B/C), with the evidence `file:line` translated into a SHA-pinned GitHub URL (`git -C <clone> rev-parse HEAD` gives the SHA).
- [ ] **Step 2:** For each `unverified` cell, read the clone at its SHA; set the verdict only with a SHA-pinned evidence line, otherwise leave `unverified`. `long_calls` and `mcp_spec`: grep for `taskSupport`, `progressToken`, `notifications/cancelled`, `protocolVersion`, `outputSchema`, timeouts (`30000`, `timeout`) and the SDK version in lockfiles; record the line that decides.
- [ ] **Step 3:** Stars: `multi` gmod-mcp, `debugger` Pollux12, `input` AmyJeanes, `pure_lua` AmyJeanes; add others only where one repo is alone in green. Picks: Most capabilities → gmod-mcp; No binary, pure Lua → AmyJeanes/GMod-MCP-Server; Breakpoints / step debug → Pollux12/vscode-gmod-glua-ls (server only); Dedicated-server workflow → Sycatle/gmod-mcp.
- [ ] **Step 4:** Validate: `python -c "import sys; sys.path.insert(0,'docs/comparison'); import render, pathlib; render.load_data(pathlib.Path('docs/comparison/data.json')); print('ok')"` → `ok`.
- [ ] **Step 5:** Report to the user the list of cells that changed versus mockup v7 and the cells still `unverified`, then commit `git add docs/comparison/data.json && git commit -m "docs: comparison data (2026-11-14)"`.

### Task 3: COMPARISON.md, README, generated images

**Files:**
- Create: `docs/COMPARISON.md`
- Modify: `README.md` (insert before `## Updates`, currently line 143)
- Create (generated): `docs/comparison/comparison-light.svg`, `docs/comparison/comparison-dark.svg`

**Interfaces:**
- Consumes: `main` from Task 1, `data.json` from Task 2.

- [ ] **Step 1:** Write `docs/COMPARISON.md`: title, one line `As of 2026-11-14, pinned commits, code read, nothing executed.`, the `<picture>` (same as README), `<!-- comparison:start -->`/`<!-- comparison:end -->`, then column definitions (from `data.json`), the 3-line method, and `Maintain one of these projects and a cell is wrong? Open an issue.`
- [ ] **Step 2:** Insert in `README.md` before `## Updates`:
  ```markdown
  **As of 2026-11-14, gmod-mcp covers the most capabilities of the 22 Garry's Mod MCP servers we compared.** Code read, nothing executed · [method & full table](docs/COMPARISON.md)

  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/comparison/comparison-dark.svg">
    <img alt="Capability heatmap of Garry's Mod MCP servers; full table in docs/COMPARISON.md" src="docs/comparison/comparison-light.svg">
  </picture>

  Repos: [ExtReMLapin/gmod-mcp](…) · … (the 8 shown) · [14 more](docs/COMPARISON.md)
  ```
  Add `Comparison` to the `## Contents` list if that list links sections.
- [ ] **Step 3:** Run `python docs/comparison/render.py` → exit 0; run it again → `git status --short` shows no further change.
- [ ] **Step 4:** Visual check: open both SVGs in a browser on light and dark backgrounds; text readable, no overlap, hatched cells distinguishable. Show the user both renders.
- [ ] **Step 5:** Commit `git add README.md docs/COMPARISON.md docs/comparison/*.svg && git commit -m "docs: comparison heatmap in README"`. Do not push; ask the user.
