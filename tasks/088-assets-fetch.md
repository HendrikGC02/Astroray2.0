# 088 Asset manifest, `tools/fetch.ps1 assets` and asset lint
Goal: showcase and benchmark assets have no checked route into the repository -> `assets/manifest.toml` records each asset with licence, size, SHA-256 and citation, `fetch.ps1 assets` downloads named assets into the git-ignored cache with checksum and licence checks, and lint enforces 0009's asset rules.
Read first: 0009 d21 (asset rules, size caps, licences, no LFS), d20 (canonical set), d6 (fetch script); docs/research/16-test-scenes-and-assets.md (Bitterli entries and licences).
Scope: assets/manifest.toml, tools/fetch.ps1 (verb `assets`), tools/lint.py (section `assets`), tests/cpu/lint_cases/assets/, tools/README.md.
Out of scope: converting assets into scene builders (090); ASSETS_NOTICE.md (M9).
Reference: none.
Do:
1. assets/manifest.toml: `[[asset]]` name, url, spdx (or `none-stated`), size, sha256, redistributable, citation, units. First entry: Bitterli's Cornell Box (CC0, from benedikt-bitterli.me/resources; record the exact archive URL, size and hash after downloading once).
2. `fetch.ps1 assets [-Name n]`: downloads into assets/_cache/<name>/, verifies SHA-256 and that `spdx` is present and not empty; a mismatch or a missing licence fails; never crawls (names only).
3. lint section `assets`: committed files under tests/data and scenes total <= 50 MB; a committed asset file must be <= 2 MB and its manifest licence one of CC0, CC-BY, MIT, Apache-2.0; share-alike, non-commercial, no-derivatives and `none-stated` assets are never committed; evidence and gallery images in the repository are <= 2 MB and <= 1600 px (PNG header width and height).
4. Lint cases: an oversized file, a CC-BY-SA file, a 2000 px PNG, and a clean case.
Acceptance: `pwsh tools/fetch.ps1 assets -Name cornell_box` ends PASS and re-running downloads nothing; `pwsh tools/check.ps1 fast` passes; `<python> tools/lint.py --self-test` includes the asset cases.
Look at: none.
Canary: tasks/088.canary.patch must turn red: `tools/lint.py --self-test` (the patch allows CC-BY-SA).
Budget: none.
Depends on: 008.
Size: S (about 200 lines); stop and report at 2x.
Progress:
