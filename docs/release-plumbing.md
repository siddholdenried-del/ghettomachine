# GhettoMachine: release from one repository

The established GhettoMachine GitHub repository is authoritative. Edit firmware under `app/`, the guide under `site/guide/` and the technical tour under `docs/how-it-works.html`. `site/public/` is generated output, not an independent website source. The existing Cloudflare Worker Builds project serves that directory on `main`. This worktree does not change hosting, DNS, or any current release.

## What this baseline actually is

- Website badge: **V0.4** (existing presentation, generated from the release profile).
- Manual: **0.4 and 0.5**, with explicit 0.5 hardware-only USB-stem instructions. Technical tour's **V0.5** is historical documentation.
- Browser instrument: archived `GHETTOMACHINE DEV` WebAssembly built from source `60b577ea722e7e089b8f620ec010af3200d3f0bf`, provenance from tracked site-build commit `47e52f19c0e5b5ec0dcbfb9f3a5731492b0ab5be`. It is not represented as a fresh build, hardware firmware, or a semver-tagged device binary.
- Device: no device download in this release profile; no hardware test claim for this web release. Preserve historical device evidence in the manual without treating it as approval for a new package.
- Theme: vendor exact Dungeon BBS tokens and wordmark from private theme source at `ac8decf93dd011966de00a151d1ff654c7f801bc`; local style uses the shared border token without repainting existing pages. Browser imports only the served copy, not GitHub. Never export the theme repository's private config or browse it at runtime.

## Verification and a new release

1. Identify the reviewed source commit. Build the emulator from that clean commit with pinned fm1-base and Emscripten; don't mix an older emulator with a newer manual. Compare boot/UI controls, lock and save behavior and other changed features during host/real-browser QA. The release profile stores reviewed artifact SHA-256/source provenance, not a guess from the date or archive filename.
2. Update `release/config.json` explicitly and review visible claims: browser/landing, manual versions and hardware-only features, technical-tour history, any actual device evidence. A generated hash does not independently certify the provenance of a supplier binary. A checked-in release config change is a human review boundary.
3. Build the site to a fresh directory using `python3 -I site/build.py PATH`. `python3 release/verify_release.py seal PATH` writes the public `release.json` only if the approved payload pins, asset inventory, themes and routes match. Run `python3 release/verify_release.py verify PATH`; the repository's read-only default is `sh site/publish.sh --check`. The separate explicit `--publish` path requires a clean reviewed `main` and checks a temporary candidate before the existing Git push/automatic deploy. No push was done for this baseline.
4. Run `python3 -B -m unittest discover -s release -v` and `node release/browser_smoke.mjs ghettomachine PATH` (Node 22+, installed Chrome, optional `CHROME_PATH`). At 390px and desktop it checks the local theme, manual and main actions, then switches on the actual browser audio worklet and toggles PLAY via the emulator's keyboard shortcut. Repeat on an actual phone before calling out physical-device support.
5. After explicitly approved publishing, compare the served release manifest/hashes and repeat real browser smoke. Reject drift rather than rewriting a manifest to match an unexplained deployment. Roll back through the known prior Worker version if needed. The local script, PR check and workflow dispatch are not substitutes for main branch protection and the hosting build gate. Coordinate any enforcement with the owner before changing remote settings.

Don't include wavetable or other staged features unless approved in the integration scope. Do not package or flash from this worktree.
