# Motor City: 21 new GhettoMachine synth presets

Original factory expansion for the stock two-oscillator engine. These are synth patches, not samples, chord files or wavetable presets. The bank is written to work without the track inserts, master effects, reverb, or delay. It does not change any of the original 24 patches or your 16 saved user sounds.

Pick **T2, T3, or T4** and turn **PRESETS** past **WOBBLE** and any occupied **USER** slots. **MOTOR PULSE** is the first expansion entry. The preset sets MONO/POLY automatically. To keep an edit, use **hold SAVE + PRESETS**, deliberately choose a slot, and let go of SAVE. When comparing patches, center pan and turn sends/inserts down if you want the dry sound in the audition.

## Mono bass and leads

| ID | At | Panel | Use / first move |
| ---: | :--- | :--- | :--- |
| 40 | 0:00.0 | MOTOR PULSE | Hollow moving PWM with an octave floor. M1 opens the filter. |
| 41 | 0:02.4 | SEWER ACID | Square acid with fast attacks. Use ties and accents; M1 opens, M2 sharpens resonance. |
| 42 | 0:04.8 | IRON TWIN | Detuned saw and square a fifth apart, with a ring-mod edge. Take M1 up for more metal. |
| 43 | 0:07.2 | DEEP CURRENT | Rounded sine bass with a short FM bite. M1 brings out the upper harmonics. |
| 44 | 0:09.6 | PARKING LOT | Nasal bandpass electro lead with a little fold and rise-in vibrato. M1 folds further. |
| 45 | 0:12.0 | HUSH SUB | Low, soft sine with a restrained attack. M1 adds sub; M2 gives extra attenuation. |
| 46 | 0:14.4 | CRANK SYNC | Sharp hard-sync lead. M1 stretches the sync sweep. |

## Poly keys, pads and stabs

| ID | At | Panel | Use / first move |
| ---: | :--- | :--- | :--- |
| 47 | 0:00.0 | BASEMENT ORG | Hollow organ for offbeat house chords. M1 brings up the octave wave; M2 darkens it. |
| 48 | 0:02.4 | CONCRETE DUB | Short low-pass chord chop. Try m7 or add9 voicings; M1 opens and M2 sets the edge. |
| 49 | 0:04.8 | NIGHT M9 | Swelling pulse-and-sine pad with slow movement. Play your own four-note voicings; M1 opens up the pad. |
| 50 | 0:07.2 | CHROME KEYS | Velocity-bright FM keys with a glassy attack. M1 increases FM. |
| 51 | 0:09.6 | UNDERPASS | Clipped, band-pass techno stabs. M1 opens; M2 adds drive. |
| 52 | 0:12.0 | DUST BELLS | Ring-mod mallet-like hits with a little noise. M1 changes the overtone balance. |
| 53 | 0:14.4 | RIVER GLASS | Longer, airy triangle-and-sine chord pad. M1 adds slight FM shimmer. |

## Pitched percussion and FX

| ID | At | Panel | Use / first move |
| ---: | :--- | :--- | :--- |
| 54 | 0:00.0 | DROP TOM | Syncussion-style downward sweep; sequence it at multiple pitches. M1 raises the pitch. |
| 55 | 0:02.4 | RIM LASER | Short FM rim-to-laser strike. M1 adds overtone bite. |
| 56 | 0:04.8 | SKIN DRUM | Triangle/tom body and noise click; velocity changes the hit. M1 adds noise. |
| 57 | 0:07.2 | SHEET METAL | High-passed inharmonic metal tick. M1 adds noise. |
| 58 | 0:09.6 | DUST SNARE | Tunable two-part hit: noise and a short oscillator body. M1 adds noise; M2 raises the filter. |
| 59 | 0:12.0 | SHORT ARC | Bright upward PWM zap. M1 drops the high-pass cutoff. |
| 60 | 0:14.4 | BOTTOM KICK | Deep, falling sine kick. M1 adds drive; pitch it under your bass line. |

M4 on most entries is the preset’s level trim programmed in MOD 6. Leave it at its factory position when comparing sounds. Lowering it removes attenuation on most entries, so turn the track down first if you edit it.

## Developer verification

This is a private source candidate, not a new download, live browser release, or physically tested FM-1 build. Project format remains 5: original factory IDs are 0–23, USER IDs are 24–39, new factory IDs are 40–60. Older builds do not know the new names; keep a backup before using an older editor to reopen and resave a project containing them.

After the standard host build (which generates `app/build/gen/fe_tables.h`):

```sh
mkdir -p app/build/preset-qa
cc -std=c11 -O2 -Iapp/dsp -Iapp/build/gen app/tests/preset_audition.c app/dsp/synth.c -lm -o app/build/preset-qa/audition
app/build/preset-qa/audition app/build/preset-qa > app/build/preset-qa/meters.tsv
app/build/host/ghettomachine_host app/tests/preset_expansion.txt app/build/preset-qa/slot-check
```

The three WAV reels are 44.1 kHz/16-bit stereo and each 16.76 seconds. They render the stock synth at its default track level using original riffs, actual four-note chords, and pitched hits. No drums, inserts, sends, EQ, loudness normalization or limiting are applied. The only edit is a short fade at the end of each excerpt to avoid a hard cut on sustained pads. Dry headroom is asserted before PCM conversion; all entries are also exercised at full note velocity, across three pitch areas and at each poly patch's voice cap. This is a single-track check, not a guarantee that any stacked mix stays below full scale.
