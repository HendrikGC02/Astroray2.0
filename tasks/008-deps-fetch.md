# 008 deps.toml, `tools/fetch.ps1 deps` and THIRD_PARTY.md
Goal: dependencies have no machine-readable list, no checksummed fetch and no generated notice file -> `third_party/deps.toml` lists every dependency with licence and SHA-256, `fetch.ps1 deps` fills the git-ignored cache and verifies it, THIRD_PARTY.md is generated, and lint rejects anything off 0001's closed list.
Read first: 0001 D9.1-D9.2, D9.6 (closed list, how obtained), 0001 D7.2 (borrowing rules, THIRD_PARTY.md), 0009 d6 (deps.toml fields, no network at configure), R20, R34, R55.
Scope: third_party/deps.toml, tools/fetch.ps1, tools/thirdparty.py, THIRD_PARTY.md, tools/lint.py (section `deps`), tests/cpu/lint_cases/deps/, cmake/deps.cmake, tools/README.md.
Out of scope: OIDN and NGX entries (added by the tasks that need them: 098, and the DLSS work after spike F); stb_image (060); Embree linking (037).
Reference: none.
Do:
1. third_party/deps.toml: `[[dep]]` with `name`, `version`, `url`, `sha256`, `spdx`, `kind` (`vendored` | `fetched-binary` | `fetched-not-redistributable` | `tool-python` | `tool-binary`, the last for build tools fetched only by hosted CI and never shipped), `purpose`, `record` (the decision that needs it). Entries now: doctest (vendored, from task 003); embree (fetched-binary, the release asset from task 007: URL `<the lead writes it here before dispatch>`, SHA-256 `<the lead writes it here>`); optix-headers (fetched-not-redistributable: the `NVIDIA/optix-dev` GitHub archive at the commit of its v9.1.0 tag, pinned by commit SHA and archive SHA-256); numpy for hosted CI (tool-python, version 2.3.4 with its wheel hash).
2. tools/fetch.ps1 `deps`: for each fetched entry download into third_party/_cache/<name>-<version>/ (skip when present and verified), verify SHA-256 (mismatch: delete and fail), extract; fail on a missing `spdx`. Any other verb fails until a later task adds it.
3. tools/thirdparty.py: renders THIRD_PARTY.md from deps.toml plus a `[[borrowed]]` table (source URL, commit, licence, our files) that later tasks append to; also records 0001 D7.4's NVIDIA licence risk paragraph.
4. lint section `deps`: every entry has every field; every `vendored` or `fetched-binary` entry's name is on the closed list (a constant `D9_1_ALLOWED` in lint.py mirroring 0001 D9.1 with a comment citing it); THIRD_PARTY.md equals thirdparty.py's output; no vendored directory under third_party/ lacks an entry.
5. cmake/deps.cmake: resolves `ASTRORAY_DEPS_CACHE=third_party/_cache` paths for later find logic; configure fails with "run tools/fetch.ps1 deps" when a needed entry is missing; never touches the network.
Acceptance: `pwsh tools/fetch.ps1 deps` ends `PASS deps` and a second run downloads nothing; corrupting one cached archive byte makes the next run re-download or fail with the entry name; `pwsh tools/check.ps1 fast` passes; adding a fake vendored entry `libfoo` to deps.toml makes lint fail naming it.
Look at: none.
Canary: tasks/008.canary.patch must turn red: `tools/lint.py --self-test` (the patch empties `D9_1_ALLOWED` checks).
Budget: none on `fast` beyond the new lint section.
Depends on: 006, 007.
Size: M (about 300 lines); stop and report at 2x.
Progress:
