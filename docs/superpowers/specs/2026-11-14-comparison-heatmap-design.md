# Comparison heatmap (README + docs/COMPARISON.md) — design

Approved 2026-11-14.

## Intent
Show, at a glance and honestly, how gmod-mcp compares with the other Garry's Mod MCP servers, so a reader skimming the README right after "Install" gets it in seconds. Credibility over marketing: our red cells and competitors' wins stay visible.

## Goals / non-goals / anti-goals
- Goals: one dated sentence + one heatmap image in the README; a short `docs/COMPARISON.md` with the full table, per-cell evidence links, column definitions, method.
- Non-goals: star counts, tool counts, aggregated scores.
- Anti-goals: any cell turned green without evidence; exploitable security detail about other projects; image and text drifting apart.

## Approaches
1. **SVG generated from a data file (chosen).** `data.json` is the single source; a script renders light/dark SVGs and the Markdown table. Updates touch one file.
2. SVG hand-written once: faster today, but every update happens twice and drifts.
Layout chosen (mockup v7): repos as rows, horizontal 2-line headers, English labels, full `owner/repo` names, "Realms" column merged as `S C M` letters, ★ for the clear winner of a column whoever it is, hatched = not verified, "picks" line under the grid.

## Design
- `docs/comparison/data.json`: generated-at date, columns (id, label, group, definition), repos (full name, URL, pinned SHA, cells: verdict yes/partial/no/unverified + evidence URL + optional ★), picks.
- `docs/comparison/render.py`: Python stdlib only. Outputs `docs/comparison/comparison-light.svg`, `comparison-dark.svg`, and rewrites the table between `<!-- comparison:start -->` / `<!-- comparison:end -->` in `docs/COMPARISON.md`. Deterministic output.
- README: before `## Updates`, the dated sentence, `<picture>` (light/dark), a line of repo links.
- Columns. Features: Realms, Errors, Hot reload, Ents, Traces, Screenshot, Hooks, Perf, Jobs, Tests, Multi-inst., Debugger, Input. Trust & ops: Pure Lua, Auth, No inject, Dedicated, Linux, 1-step install, Long calls, MCP spec, Own CI, License, Agent feedback.
- README heatmap: top 8 repos; COMPARISON.md table: all 22.
- Data from the 2026-11-14 benchmark (`C:\Users\pierr\Documents\gmod-mcp-benchmark`); hatched cells verified on the clones at the pinned SHA before publishing, otherwise stay hatched.
- Auth column: yes/no only.

## Left to the builder
Exact SVG geometry, colours meeting contrast in both themes, Python version check, verdict definitions wording.
No commit or push without the user's go.
