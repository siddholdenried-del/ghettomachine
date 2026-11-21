# GhettoMachine development preview

This is a browser release candidate, not a new numbered firmware release. No firmware download is published for it.

- Up to 128 steps per track. Gate can span 128 steps; chord stamping and per-step note editing are on instrument STEPS.
- 909, 808 and synthesized 606 kits. Retains the current live build's 808 level matching.
- Mutes cut an active drum or instrument immediately. An instrument's insert tail is cleared with its track; already-ringing shared delay/reverb can continue.
- On T2–T4, hold **SEL** and tap **FX** for a single insert, then use SELECT to choose a mode/OFF and K4 for wet mix. It is independent of the two shared sends and the master.
- Dark Dungeon BBS display and supplied boot art, on the same sequencer and audio paths.
- This preview reads project formats 1–5 and 7, and saves format 7. Format 6 from private development builds is not supported. Earlier firmware cannot open a format 7 save; keep a backup for downgrades. If an existing project opens blank or shows the default pattern, don’t save over it.

Browser engine and site compiler provenance, hashes and device artifact status are listed separately in `site/public/release.json`. The how-it-works tour explicitly describes the historical 0.5 build.
