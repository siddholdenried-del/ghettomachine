# Four GhettoMachine synth banks

| Bank | IDs | Sounds |
| --- | --- | --- |
| **After Hours** | 61–81 | Deep-house basses, drawbar stabs, soft chords, and hand percussion |
| **Machine Funk** | 82–102 | Detroit electro pulse and sync, metallic keys and organs, FM rims and zaps |
| **Warehouse** | 103–123 | Pressure leads, spare techno stabs, percussion and noise |
| **Dub / Broken** | 124–144 | Clean subs, dark chord cuts, tunable hits and moving textures |

These are 84 original stock-synth programs. Each bank has seven mono basses/leads, seven poly patches, and seven pitched percussion/FX sounds. They are not samples. The [After Hours and Dub](preset-banks-house-dub.md) and [Machine Funk and Warehouse](preset-banks-electro-warehouse.md) patch sheets describe every program, its first macro, and whether it recalls Color.

## On the panel

Pick **T2**, **T3**, or **T4**. Hold **SEL** and turn **PRESETS** to jump to the first patch in a bank. The screen shows the bank and patch. Let go of SEL and turn PRESETS alone to browse the sounds. The order is Original, User Sounds (only if occupied), Motor City, After Hours, Machine Funk, Warehouse, Dub / Broken. Tap SEL with no other action for a shortcut hint. Existing **SAVE + PRESETS** still saves an explicitly chosen user slot; it takes priority over bank browsing.

Some new factory patches recall light **Color** settings on the selected track only. Off recalls bypass without erasing that track's Color knob settings. Choosing an original, Motor City or User patch does not change your current Color settings. Color is project/track state, not a setting copied into the legacy user-sound slot or part sound; it remains independent of master FX, sends, and the existing insert. Flip between dry and Color from the track's Color page to hear the difference. M4 is the preset's gain calibration on patches using MOD 6; turn the track down before removing a pad.

## Auditions / reproducibility

Four short stereo WAV reels each feature nine patches (three from each group). The reels run through the actual synth and the stored Color where enabled, with no sends, insert effects, drum layers, normalization or limiting. The separate A/B uses identical pre-Color samples for both sides: each sound dry, then the same performance with its own Color setting. A short edit fade at the end of each excerpt avoids hard tail cuts.

After building the local host app:

```sh
mkdir -p app/build/preset-banks
cc -std=c11 -O2 -Iapp/dsp -Iapp/build/gen app/tests/preset_banks_audition.c app/dsp/synth.c app/dsp/color.c -lm -o app/build/preset-banks/audition
app/build/preset-banks/audition app/build/preset-banks > app/build/preset-banks/meters.tsv
app/build/host/ghettomachine_host app/tests/preset_banks.txt app/build/preset-banks
```

The harness validates each patch's complete parameter ranges, unique name and data, real dry and colored levels, four-note audition voicings, and maximum configured polyphony at full note velocity across three registers. The test verifies single-track headroom before WAV conversion, not arbitrary stacked mixes. The bank is a private build candidate; no firmware has been flashed or tested on hardware.
