# Official MCP Registry

gmod-mcp is listed in the [official MCP Registry](https://registry.modelcontextprotocol.io) as
`io.github.ExtReMLapin/gmod-mcp`. The registry stores metadata only; the files are the release assets of this repository.

## What is published

- **Package type: MCPB** (`registryType: "mcpb"`), the one the registry offers for a prebuilt native binary that is not on npm,
  PyPI, NuGet, crates.io or a container registry. `gmod-mcp.mcpb` is one zip for both platforms: `manifest.json`,
  `server/gmod-mcp-bridge` (Linux x86-64, mode 0755), `server/gmod-mcp-bridge.exe` (Windows x86-64), `LICENSE`,
  `THIRD_PARTY_NOTICES.md`. The manifest (`mcpb/manifest.json`) starts the bridge with `platform_overrides` for Windows.
  There is no macOS binary, so `compatibility.platforms` lists `win32` and `linux`.
- It is only the **bridge**. The module that runs inside the game is not part of the bundle: it is installed as described in
  the README (the plugin's skill, or the zip). The bundle's long description says so.
- **No OCI image**: the bridge talks to a game on the host machine through `127.0.0.1` and reads its token from the user's
  profile, so a container could not reach the game and would only be misleading.
- `server.json` (repository root) is the registry entry: name, description (100 characters at most), repository, one `mcpb`
  package (URL of the release asset, SHA-256, `stdio`). Its `version`, the package `version` and the URL are rewritten from the
  tag by CI; the SHA-256 in the file is a placeholder (64 zeros) that CI replaces with the real one.
- **Ownership**: the registry does not look inside an MCPB. It checks that you control the namespace `io.github.ExtReMLapin/`
  (the authentication) and that the URL is a GitHub release asset whose URL contains `mcp`, answers a `HEAD`, and has a
  `fileSha256`. MCP clients verify that hash before installing.

## What a release does (`.github/workflows/build.yml`)

1. `mcpb` job (every run, not only tags): packs `gmod-mcp.mcpb` from the bridges built by the `build` job, validates its
   manifest with the official `mcpb` CLI (pinned `@anthropic-ai/mcpb@2.1.2`), and runs the bridge from the unpacked bundle
   exactly as the manifest says (`tests/ci/mcpb_smoke.py`: `initialize`, `tools/list`, a call that answers `game_not_running`).
2. `release` job: attaches `gmod-mcp.mcpb` and its `.sha256` to the GitHub release with the zips.
3. `registry` job (tags only, after `release`, only in `ExtReMLapin/gmod-mcp`): downloads the released asset and compares it
   with the built one, writes `server.json` for the tag (`cmake/mcp_registry.py server-json`), installs `mcp-publisher`
   (version pinned, SHA-256 checked), runs `mcp-publisher validate`, `login github-oidc` and `publish`.
   Authentication is GitHub Actions OIDC: the job has `id-token: write` and no secret exists. The registry grants
   `io.github.<repository owner>/*` to the owner of the repository, which works for a personal account.
   The job is outside the release: if it fails, the release stays; fix the cause and re-run the job. A version can be
   published once (the registry refuses the same version twice), so a failed run that got as far as `publish` needs a new tag.

Bumping a version means `CMakeLists.txt`, `plugin/.claude-plugin/plugin.json` (as before) and now also `mcpb/manifest.json`
and `server.json`: `tests/ci/test_mcp_registry.py` fails when they differ.

## Upgrading `mcp-publisher`

`MCP_PUBLISHER_VERSION` and `MCP_PUBLISHER_SHA256` in the `registry` job. Take the new tag from
https://github.com/modelcontextprotocol/registry/releases and the line of `mcp-publisher_linux_amd64.tar.gz` in its
`registry_<version>_checksums.txt` (signed with sigstore by the registry project). An error `invalid audience` means the
pinned `mcp-publisher` is too old for the registry deployment.

## Checking by hand (no publication)

```sh
python cmake/mcp_registry.py pack --win-bridge <gmod-mcp-bridge.exe> --linux-bridge <gmod-mcp-bridge> --out gmod-mcp.mcpb
python tests/ci/mcpb_smoke.py gmod-mcp.mcpb                      # runs the bridge of this platform from the unpacked bundle
npx --yes @anthropic-ai/mcpb@2.1.2 info gmod-mcp.mcpb
python cmake/mcp_registry.py server-json --tag v0.3.7 --mcpb gmod-mcp.mcpb --out server.generated.json
mcp-publisher validate server.generated.json                     # schema and rules, nothing is published
python -B -m unittest tests.ci.test_mcp_registry
```

## Publishing a release by hand

Only needed for a release made before the `registry` job existed, or to retry. Upload `gmod-mcp.mcpb` to the release
(`gh release upload vX.Y.Z gmod-mcp.mcpb`), generate `server.json` with the command above (`--out server.json`), then
`mcp-publisher login github` (device flow in the browser) and `mcp-publisher publish`. The bundle must be the one
built from the same sources as the release's zips.
