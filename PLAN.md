# BongTech GhettoMachine: a 4-track groovebox firmware for the M-VAVE FM-1

(Called fm1-eight until 0.4, as in the history below: renamed 2026-11-20.)

Working name only (it started as 8 tracks); rename it any time. Plan drafted 19 Nov 2026, cut to
4 tracks the same day. **Version 0.2: phases 1 to 7 are built** and pass their simulator tests; none has run on the FM-1
yet. The user's guide is [MANUAL.md](MANUAL.md).

## The instrument

- **T1: the drum track.** A 909 or 808 kit, with every pad editable on the FM-1 and a MUTE page.
  (Pad 12's sampler was dropped on 19 Nov: sampling lives on the instrument tracks.)
- **T2, T3, T4: instrument tracks.** Each one picks a **TYPE**:
  - **MONO:** a mono synth, one note at a time, with legato, glide and up to 4 unison voices.
  - **POLY:** a poly synth, up to 8 voices on the track.
  - **SAMPLER:** a sample played across the keys, mono or poly.

  All three use the same voice: a **source** (the synth's oscillators, or a sample), then a filter,
  then the amp. Sampler tracks therefore get the same filter, envelopes and LFO as the synths, and
  switching TYPE keeps the filter and envelope settings.
- **One 64-step sequencer for all four tracks.**
  - Instrument steps hold up to 4 notes (chords), with velocity, gate and chance.
  - Parameter locks, swing, live recording and step recording.
  - Each track has its own length and rate, so tracks can drift against each other.
- **An LFO on every track**, including drums, where it can move any pad's control. Tempo-synced or free.
- **A mixer, reverb and delay sends, and a master bus.**
- **Undo:** 8 levels with redo (EDIT + OCT− / OCT+).

## What already exists and what we take from it

| Source | Licence | What we take |
| --- | --- | --- |
| [fm1-base](https://github.com/charlesvestal/fm1-base) + [felucca-bsp](https://github.com/hugelton/felucca-bsp) | MPL-2.0 | **The platform:** audio, keys, knobs, screen, USB-MIDI, flash storage, second core, .fwsc packaging, host simulator. In use. |
| [X0X](https://github.com/charlesvestal/fm1-x0x) | GPL-3.0 | **The drums:** the 909 (from 9W9) and the 808 (from 8W8). In use. |
| [Felucca-Engines](https://github.com/hugelton/Felucca-Engines) | MIT | **Starting point for the synth voice:** `eng_analog.c` and `dsp.c`, with PolyBLEP saw and pulse, sync, sub and a trapezoidal SVF. SLOOP and Skiff ship this engine. In use: copied into `app/dsp/felucca/` with its MIT notices, a sub level and an OSC 2 interval added. |
| X0X's FX ([fm1-x0x](https://github.com/charlesvestal/fm1-x0x) `dsp/fxbus.*`, `dsp/master.*`) | GPL-3.0 | **The reverb, the delay and the master** (after 9W9's): in use, copied unchanged but for the delay's off switch. |
| [LOOM-8](https://github.com/charlesvestal/fm1-loom) | GPL-3.0 | A reference for its voice pool and its sample bank in flash. |
| SLOOP, Skiff | GPL-3.0 | References for voice stealing, mono/legato/glide and the voice budget. |

**Licence:** the whole firmware is **GPL-3.0-only**, because the 909 and 808 models are GPL. MPL files
keep their headers. No Bluetooth.

## Hardware budget

| Resource | What there is | Plan |
| --- | --- | --- |
| **CPU** | Dual-core pi32v2 at 360 MHz with a single-precision FPU; 44.1 kHz audio in 256-frame blocks (5.8 ms). | Hot DSP runs from RAM. Voices are split across both cores, with a **single-core fallback** (some units' second core misreads RAM). |
| **Voices** | Skiff ships 16 synth voices on two cores, at about 76 % in its slowest block. | **A pool of 16 voices shared by T2–T4.** A POLY track can take up to 8; MONO takes 1, or up to 4 with unison. Released voices are stolen first. Above 85 % load, voices are shed. On one core, the pool is 8. |
| **Drums** | A dense 808 pattern took about half a core in X0X; the 909 is much cheaper. | Only one kit plays at a time. If a dense 808 and 16 voices don't fit, the pool drops to 12 while the 808 is on. |
| **RAM** | About 464 KB, no external RAM. Phase 5 uses 64 KB of the 96 KB for code data and 282 KB of the app's 344 KB pool: the project (31 KB), 8 levels of undo and redo (30 KB), the reverb (17 KB) and one 132 KB buffer that is the recording's while sampling and the delay's line (1.5 s) otherwise. | 62 KB are free. |
| **Flash for your data** | 276 KiB of the data area is the app's (0x97000–0xDBFFF; the panel calibration sits above it), plus a 92 KiB window with a rule of its own. Samples can be played **straight from flash** (fm1-base's `plat_data` reads it in place), so they need no RAM. | **The project file takes 64 KiB** (two copies of 32 KiB). **Samples get the other 212 KiB**, plus the 92 KiB window if the bank keeps to its rule. |

**Estimated CPU, as shares of one core.** These are estimates; the voice is measured once it's built.

| Part | Share |
| --- | --- |
| Usable on two cores | about 150 % (75 % each) |
| Drums | 15–25 % for the 909; up to about 50 % for a dense 808 |
| Reverb and delay | 15–22 % |
| Sequencer, mixing, master | 5–10 % |
| One synth voice (2 oscillators, sub, noise, filter, 2 envelopes) | about 3–3.5 % |
| One sampler voice | less than a synth voice |
| **16 voices** | **about 50–56 %**, which fits even with a dense 808 |

## The synth voice (MONO and POLY), as built

- **The voice:** Felucca-Engines' ANALOG, played by a host of our own (`app/dsp/synth.c`).
- **Oscillators (EDIT > OSC and TONE):** two oscillators share one WAVE: SAW, SQUARE, TRI, SINE or PWM (PWM
  sets the width), unless WAVE 2 gives OSC 2 its own (the synth update, below). OSC 2 sits DETUNE cents and
  INTERVAL semitones (−24 to +24; −12 to +12 before the synth update) from OSC 1; MIX sets the balance. Two more WAVEs: SYNC (OSC 2 hard-synced to OSC 1, DETUNE the sweep) and SUB (a saw and a square an
  octave down). Beside them, a SUB oscillator (a square an octave below OSC 1), NOISE and DRIVE.
- **Filter (EDIT > FILTER):** a trapezoidal state-variable filter, 12 dB: LP, BP or HP. CUTOFF 30 Hz to
  16 kHz, RESO, ENV AMT (either way), KEYTRACK.
- **Envelopes (ENV):** an amp ADSR and a filter ADSR, 1 ms to 5 s a stage (attack to full; decay and release
  to −40 dB).
- **MONO (EDIT > VOICE and MORE):** the newest key wins, and the keys held are a stack: let go of the top one
  and the note goes back to the one below. LEGATO: tied notes don't restart the envelopes. GLIDE up to 1.5 s.
  UNISON 1–4 voices, spread in pitch (SPREAD, up to ±50 cents) and across the stereo field.
- **POLY:** VOICES caps the track at 1–8. The same note again reuses its voice; at the cap the track's oldest
  note gives way.
- **The pool:** 16 voices for the three tracks, rendered half on each core; 8 if the second core is off or
  fails. A new note takes a free voice, else the quietest one letting go, else the oldest. While the audio
  load is over 85 % the pool shrinks a voice at a time (down to 4), and it grows back below 65 %.
- **VELOCITY** sets how much a key's velocity changes the level.
- **24 factory sounds (PRESETS):** SQR BASS, ACID, SUB BASS, FAT BASS, SAW LEAD, SUPERSAW and SYNC LEAD
  (MONO); SOFT PAD, PWM STRINGS, PLUCK, BRASS, SINE KEYS, RAVE STAB, ORGAN and WIND (POLY), after Felucca's ANALOG
  presets; then the synth update's FM BELL, FM BASS, FM KEYS, FOLD LEAD, WEST PLUCK, RING METAL, DRIFT PAD, CRUSH BASS
  and WOBBLE (MACRO 1 does something on each). A sound changed since it was loaded shows a *.
- **Not built from the first plan:** a 24 dB filter, a sine sub, and a sub two octaves down.

## The synth update (version 0.3), as built

The user chose the modulation and the ANALOG voice's character, and passed on more engines ("I like the analog synth
one"). A sound grew from 26 parameters to 64 (`dsp/synth.h` SP_*; the 26 keep their numbers).
- **OSC 2 (EDIT > OSC 2):** WAVE 2 (SAME, or its own wave), FM (OSC 2 phase-modulates OSC 1, up to 1.5 cycles,
  the amount squared, less above A5), RING (a crossfade to OSC 1 × OSC 2), DRIFT (each voice's own slow wander, up to
  ±15 cents and 3 cutoff steps, a new heading every 0.3–1 s).
- **The SHAPER (EDIT > SHAPER):** FOLD (Felucca's PHASE engine's sine folder, 0.64x to ~8x, less above A4), CLIP
  (1x–16x), RECT (a blend to the full-wave rectified wave), CRUSH (up to 13 bits off, a hold of up to 15 samples);
  BIAS; PRE (before the drive and filter) or POST (after the filter). A DC blocker after it. SAMPLER tracks too.
- **MOD ENV (ENV, third page):** a third ADSR for each voice.
- **LFO 2 (LFO, third page):** each voice's own: the seven shapes, 0.05–30 Hz, FADE IN, RESTART each note or free.
- **The matrix (LFO, fourth page):** six slots of source, destination, amount (−64..63), worked out for each voice at
  the control rate (every 32 samples): the sources LFO 1, LFO 2, MOD ENV, FILT ENV, AMP ENV, VELOCITY, NOTE, RANDOM,
  MACRO 1–4; the destinations PITCH and OSC 2 (±48 semitones, the amount squared; between 1/16 semitones by the fine
  tune), DETUNE, CUTOFF, RESO, PWM, MIX, SUB, NOISE, DRIVE, FM, RING, SHAPER, BIAS, LEVEL (×0..2), PAN, LFO 2 RATE. A
  voice the matrix moves gets its own copy of the engine's parameters for the block.
- **MACRO 1–4 (EDIT > MACRO):** sound knobs that are matrix sources: lockable per step and recorded as motion.
- **How it stays the same when unused:** the engine's old loops run unless WAVE 2, FM, RING, the SHAPER or the
  matrix's OSC 2 is in use (then `analog_render_r`, its own loop). 0.2's 15 sounds render byte for byte as before
  (`app/tests/synth_same.sh`, LFO 1 on the cutoff and the pitch, MONO, POLY and unison included).
- **Cost:** on the Mac, 16 voices with FM, FOLD and the matrix take 1.46× the time of 16 plain voices
  (`synth_unit.c`); on the FM-1, unmeasured (the shedding drops voices if it's short).
- **Two overflows fixed on the way** (the undefined-behaviour sanitizer): a loud unison voice could overflow its
  pan's multiply (0.2's too, at full level and resonance: now `mulq16`, the same result where it didn't); a sample
  read shifted a negative number left.

## The 303 generator (version 0.4), as built

The user asked for "a 303 pattern generator... one of the sequence pages", then for "the whole note or half note
settings that the 303 does" and for "this sequencer available for all synths". On T2–T4, SEQ goes STEPS → ACID → KEYS;
SELECT turns ACID's three pages: ACID (DENSITY, WHOLE, SLIDE, ACCENT), NOTES (ROOT, SCALE, NOTES, OCTAVE), LINE (SEED,
and STEPS's LENGTH, RATE, SWING; a LENGTH turn makes the line again). It writes the part's steps; they stay ordinary.
- **The line:** each step has seven numbers of its own (a hash of the seed, the step and the last MUTATE that
  re-rolled it): plays (DENSITY), which note (the root 3× as likely as each other of NOTES scale notes, ordered the
  bass's favourites first), an octave (OCTAVE; 70 % up), a slide (SLIDE: only into a step that plays), an accent
  (ACCENT), WHOLE. The knobs move thresholds only, so a turn back gives back the same line, and more DENSITY keeps the
  notes already there. Step 1 always plays.
- **A step as a 303's:** HALF gate 4/8; WHOLE held to just before the next note (8k − 1 eighths over k − 1 rests); a
  SLIDE gate 10/8 and the **NS_SLIDE** mark (the chance byte's top bit); an accent velocity 127 and the **NS_ACC** mark
  (the velocity byte's top bit), others 90. No file before 0.4 has those bits set, so the format is unchanged by them;
  every reader masks them (`ns_vel`, `ns_prob`). On STEPS a held step + OCT+ cycles HALF / WHOLE / SLIDE, and K2
  VELOCITY past 127 is ACCENT.
- **On any sound:** the sequencer passes the marks to `syn_note_on_f` (SN_SLIDE when the step before slid and sounded;
  SN_ACCENT). SN_SLIDE retargets the track's held note (MONO: its unison stack; POLY and SAMPLER: the newest held
  voice) with glide, whatever LEGATO says; GLIDE's time, or GLIDE 25's (58 ms) with GLIDE at 0. SN_ACCENT: ×1.4 level
  for the note and +20 cutoff steps falling with a 200 ms time constant. Unmarked notes take the old paths: 0.2's
  sounds render byte for byte as before.
- **NEW (OCT+)** the next seed (1..999); **MUTATE (OCT−)** re-rolls len/8 steps (at least one), again (up to eight
  tries) until the line changes. **SEED** walks the seeds; a seed's mutations start again from 0.
- **Settings** per part, `gen_t gen[NPAT][NINST]` (12 B) appended to project_t: format 3. A part never generated has
  seed 0; its first turn starts from the track's last settings (`gen_last`, RAM), or the defaults (C2 MINOR, 4 notes,
  DENSITY 75, WHOLE 20, SLIDE 25, ACCENT 30, OCTAVE 20 %). Undo snapshots and pattern copies carry the parts' settings.
- **LEGATO** gained a third value, **SLIDE**: MONO glides only into a tied note (keys played legato). The **ACID**
  preset uses it, with GLIDE 25; otherwise as 0.3's. `synth_same.sh` plays ACID with 0.2's values.
- **The page:** the line by pitch (a half note a dot, a whole note a bar as long as it's held, a slide a bar and a line
  to the next note), accents white, AC and SL rows.

## USB stems (version 0.5), as built

Asked on 20 Nov: "usb audio out from the fm-1 ... so we could record individual channels". fm1-base already had it
(stereo USB audio in and out were on in every build); `usb_audio_channels = 10` in fm1.toml makes the input ten
channels at 44.1 kHz, 16 bits: the master on 1–2 (fm1-base's tap, as before), the app's own on 3–10 (`plat_usb_ch`).

- **What a stem is:** the track as it joins the dry mix: level, pan, mutes, solo. Not the sends (the reverb and delay
  are shared), the master bus, MASTER, the metronome, or the output's perform keys (STUTTER, TAPE STOP, CRUSH; THROW
  and WASH feed the sends). Drums 3–4, T2 5–6, T3 7–8, T4 9–10.
- **Where:** the drums from `buf_l`/`buf_r` after `drum_mix`, before `synth_add` adds into them; the synth tracks from
  `syn_mix`, which already summed each track on its own and now keeps that block (`syn_track`), written in
  `synth_add` as its SYN_CTL blocks are taken. `stem_put` writes into the ring at the frame's offset in the block,
  asking again where the ring wraps. `plat_usb_ch` gives NULL while no computer records: then nothing is done.
- **Level:** `STEM_GAIN` = FX_BUS_TRIM, the level of the screen's track meters and of the master bus's input: the four
  add up to it less the FX returns. The demos peak 6–14 dB under full scale (at 4× that the drums clipped); clipped,
  not limited, past it.
- **Not possible without changing fm1-base:** keeping the metronome off the master pair (the tap copies what the app
  renders, click included).
- **MASTER out of the USB mix** (the user: "internal track levels set usb mix"): `plat_usb_audio_full(1)` in app_init,
  so app_render gets 4096, the master pair is the master bus's output at full level, and fm1-base puts MASTER on for
  the speaker after its tap (uac_mix.h). The app's pkl / pkr are now before MASTER.
- **On the FM-1** (installed 20 Nov, recorded with ffmpeg from the Mac while fm1_remote.py pressed PLAY): every stem
  carried its own track, but the shallow ring (128 frames primed) ran dry ~12 times in 9 s: all ten channels held for
  up to ~260 frames, then a jump. `FM1B_UAC_DEEP 1` in app.h keeps a whole block more (5.8 ms more latency), the ring
  in the pool: RAM .data+.bss 69,572 B, pool 298,140 of 344,064.
- **Cost:** image 445,268 B (+1.3 KB over 0.4); with the deep ring in the pool, RAM .data+.bss 69,572 of 98,304 B
  (0.4: 69,860) and the pool 298,140 of 344,064 (+15 KB).
- **Tests:** `app/tests/stems.txt` (67): every stem silent while stopped, the drums alone on 3–4, a T2 note on 5–6 only,
  MASTER 0 leaving the stems and the master pair as they were, LEVEL 50 taking the drums' stem 6 dB down, PAN hard left emptying the right side, a stem stopping with its voice while
  the master's reverb rings on, solo, the metronome on the master only, no clipped samples. `usbwav` writes the ten
  channels as a computer would get them.

## The sampler (TYPE = SAMPLER), as built

- **One sample per track**, played across the keys through the same voice as the synth: its drive, filter, both
  envelopes, velocity and pool. **C4 plays it at its own pitch** (ROOT moves that; TUNE ±50 cents).
- **Playback (EDIT > SAMPLE):** START and END, and LOOP: once through (the voice ends at the end, key held or not), or
  round from LOOP START to the end. The loop start moves to the nearest rising zero crossing, so loops click less.
  Polyphonic, VOICES capping the track at 1–8 (VOICES 1: each note restarts it).
- **Recording the FM-1's own sound:** on a SAMPLER track, hold REC and press SEL. SEL and REC blink while it waits;
  the first sound over −40 dB starts it (anything: the beat, another track, a key); it stops at 2 s, or at SEL. It
  records the whole mix, before MASTER. **RECORD AT** (EDIT > PITCH) picks 44.1 kHz (2 s) or 22.05 kHz (4 s).
  - Stopped, it goes into the bank at once. While playing, the track plays it from RAM and it goes into the bank at
    the stop; until then another track can't record (it would lose this one).
- **WAVs from the Mac:** `python3 -I app/tools/fm1_sample.py send FILE.wav --track 3 [--rate 22050]`; `get OUT.wav
  --track 3`; `rm --track 3`. Any WAV: mono, resampled, cut to the room the FM-1 reports, normalised, 12-bit. The
  track becomes a SAMPLER playing all of it.
- **The bank:** 212 KiB of flash after the project file, shared by the three tracks: **about 3.3 s at 44.1 kHz in
  all, or 6.6 s at 22.05 kHz.** A sample is kept the moment it's recorded or sent (SAVE keeps the track's TYPE,
  START, END and the rest, as for any setting).
  - A new sample is written beside the old one, which goes only once the new one is whole: a power cut keeps one of
    them. With no room for both, the old one goes first.
  - It goes in the smallest gap that fits, at the end away from the track's old sample, so the space left stays in
    one piece. If the gap is too small it's cut to fit (STORED, CUT); with no room at all a recording isn't kept
    (BANK FULL).
- **Not built yet:** the 92 KiB window (about 1.4 s more at 44.1 kHz), ping-pong loops, reverse, slices, a 4-bit
  format for longer samples.

## Modulation (phase 4), as built

- **An LFO on every track** (LFO; tap again for its second page, again to leave; SELECT turns the page).
  - Page 1: **SHAPE** (SINE, TRIANGLE, SAW UP, SAW DOWN, SQUARE, S&H, SMOOTH), **RATE** (free, 0.05–30 Hz; or with
    SYNC, 4 BARS down to 1/32, dotted and triplets too), **DEPTH** (±64), **DEST**.
  - Page 2: **SYNC** (FREE or TEMPO), **RETRIG** (FREE; NOTE: each note restarts it; BAR: each bar does), **FADE IN**
    (after a note, up to 5 s), **PHASE** (where it starts).
  - On T2–T4, DEST is one of CUTOFF, PITCH (±12 semitones at full depth), LEVEL (tremolo), PWM, RESO, DETUNE, MIX,
    DRIVE, NOISE, SUB, or START (a SAMPLER's start, as each note begins). It moves every 0.73 ms.
  - **On the drums, DEST is any knob of the pad picked** ("SD TONE", "CH DECAY"...): pick the pad, then turn DEST. It
    sets that knob at each of the pad's hits, the LFO's value at that moment added to the knob (or to the step's
    lock): an S&H on a hat's decay makes every hat different; a SQUARE over a bar on the snare's tone gives two
    snares. The knob's own value comes back at the stop, and SAVE keeps the knob's own value.
  - The LFO page shows the shape and where it is now.
- **The arpeggiator on T2–T4** (ARP; tap again for page 2, again for KEYS).
  - **MODE** (OFF, UP, DOWN, UP+DOWN, RANDOM, AS PLAYED), **RATE** (1/4 to 1/32, triplets too), **OCTAVES** (1–4),
    **GATE** (5–100 % of its step); page 2: **LATCH** (the notes stay after the keys; a new chord replaces them).
  - It plays what the keys and MIDI hold, starting 15 ms after the first key so a chord's keys all count. **It
    also plays the sequencer's chords**: a step's notes go into it for the step's gate, so a recorded chord comes
    back arpeggiated. Recording records the keys as played, not the arpeggio.
- **Locks on instrument steps**: hold a step on the steps page, tap EDIT (or ENV) and turn a knob: that value for
  that step only (the knob shows LOCK). SELECT turns the EDIT and ENV pages while the step is held, so any sound
  parameter can be locked (not VOICES or LEGATO). Up to 16 locks a part. The step's value is set as it plays, and
  the knob's own value comes back at the track's next step without that lock. Held step + OCT− clears the step's
  chance and locks. A step with locks has a mark over it.
- **Not built:** locks on LFO or arpeggiator settings, recording the arpeggio itself, an LFO per voice.

## The mixer and FX (phase 5), as built

- **The chain:** each track's level and pan, and its reverb and delay sends → the reverb and the delay (X0X's, after
  9W9's) added to the mix → the master bus (phase 7, below).
- **The master compressor (phase 6):** THRESHOLD (−48–0 dB), RATIO (1:1 off, up to ∞), ATTACK (0.1–100 ms), RELEASE
  (10 ms–1.5 s), MAKEUP (0–24 dB), MIX (parallel), PUMP (ducking at each kick), a 6 dB soft knee, and a live REDUCTION
  meter. On from the start; since 0.2 it's on the master bus's COMP pages, at 2:1 over −18 dB with +3 dB makeup.
- **GLO, the mixer:** four pages, tap GLO for the next (after the last, back): LEVEL, PAN, REVERB, DELAY. K1–K4 are
  DRUMS, T2, T3, T4 on each, with a strip a track on the screen. T2–T4's LEVEL is the same as on their VOICE page;
  the drums' is 100 for the kit as it is. Default: the drums with their sends open (their pads' own Rev and Dly
  knobs decide, at 0 until turned), the bass dry, the pluck and the pad a little left and right and wet.
- **The drum pads' Rev and Dly knobs** are on their EDIT pages now; **the kit's own knobs** (on the 909, ACCENT and
  VELOCITY) are the last EDIT page.
- **FX:** four pages, tap FX for the next: REVERB (DECAY, TONE, LOW CUT, LEVEL), DELAY (TIME in note values from
  1/32 to a dotted half, FEEDBACK, TONE, LEVEL), DELAY 2 (TYPE: DIGI or TAPE, WEAR, WIDTH: ping-pong, LOW CUT),
  FILTER (the DJ filter and its RESO; it sits on the master bus, after the compressor).
- **The delay shares its memory with sampling:** from REC + SEL until the recording is in the bank, the delay leaves
  the chain (its page says PAUSED WHILE SAMPLING); then it comes back, empty. A recording takes the reverb but not
  the delay.
- **Not built:** per-track compression or EQ, the drum track's own distortion, a send on the master.

## The look (phase 8, version 0.2), as built

**Since 0.5 (21 Nov) the screen follows Bong Cult's Dungeon BBS style guide** (github.com/siddholdenried-del/
bongcult-dungeon-bbs) at the user's request: theme() keeps its roles but on a near-black canvas (#10100F), raised
fields #1E1D18, bone text #DED9B9 and muted #BBB79F, 1-pixel rules, square corners (rrect draws a plain box);
violet #8D53ED with dark ink for what is selected (the knob being turned, the page dot, the picked pad, matrix slot,
song entry, the mixer's track, the page map's row), pale violet #C29AFC for a lock, acid green #A3ED58 only for live
meters; the section's colour for its big name, the gauges and what the page draws (hits, notes, curves, the playing
clips). The knob strip is one ruled grid. IBM Plex Mono (app/tools/gen_mono.py: FONT_MS 12 px, FONT_MB 15 px) for the
pattern, the tempo and the line under the title (the condensed face where the mono one would cut it). The boot
screen keeps the graffiti logo (the user: "it can still have the graffiti text"), bone on the canvas with BONGTECH
pale violet. The original description of 0.2's look follows.

- **Three parts:** the band (240×44: the section's colour, what and where, pattern, tempo, transport, page dots, the
  output's L/R meters before MASTER with a clip cap; a change shown large for 1.3 s), the page (240×132: its picture),
  the knob strip (240×64: four cards as K1–K4 sit; name, value, gauge; from the middle for two-way knobs, in steps
  for switches; ringed while turned; a lock's card in the track's colour; grey for readings).
- **Colours:** the tracks' own (909 orange, 808 blue, T2 green, T3 purple, T4 yellow), cream for HOME/SONG/MIX, cyan
  for FX, rose for the master bus. The user asked for it inverted after seeing the first version (dark pages, cream
  cards, a coloured band; kept in `app/attic/screen_dark.c`): now the page and the knob strip are the section's
  colour, drawn in ink with darker shades of the colour for panels, grids and labels (theme(): white highlights, or
  ink on the lightest colours, yellow and cream), and the band and the cards are dark, lettered in the colour.
- **Pictures:** the drum grid (pads lighting as they sound), the note roll, the keys with the voices in use, the drum
  pad's steps with all its knobs in rows, VOICE (type, voices, level), OSC's wave, FILTER's response (with the
  envelope's sweep), the ENV shapes, the sample, the LFO (a dot where it is), the arpeggio's order and notes, the
  pads' mutes, HOME (patterns, the tracks' meters, CPU, voices), SONG, the mixer (a live meter by each strip), the
  reverb's tail, the delay's echoes (to the sides with ping-pong), the DJ filter, and the master bus's pages.
- **Meters:** the tracks' peaks (taken where they're mixed: synth.c per track, the drums after their mixer), shown at
  the master bus's level (−14 dB); the pads' hits; the output per side. Read and let go by the screen.
- **Redraw:** the whole screen when something changes; else ~30 times a second only the band and, on a page that moves,
  the page (a full screen is ~21 ms over the LCD's SPI; the band 4 ms).
- **Cost:** the image grew 25 KB (392,660 B); RAM 64,660 of 98,304 B. A small maths unit (dsp/uimath.c) draws the
  curves, so app.c still never includes fastmath. The image is now 393,380 B.

## The master bus (phase 7, version 0.2), as built

- **The order:** the mix at −14 dB (a fixed trim) → **EQ** → **COMP** (and PUMP) → the DJ filter → **TAPE** →
  **STEREO** → the limiter's **GAIN** → the **LIMITER** under its **CEILING** → the meters → the **MASTER** knob, last.
- **Why the trim and the knob's move:** the tracks' sum peaks 11–13 dB over full scale in the demos. X0X put MASTER (a
  square law: −12 dB at half) before its limiter, which caught the rest, so the chain's sound depended on the knob.
  Now the chain sees the mix at its own level (peaks near −2 dBFS) whatever the knob, and the knob only sets how loud
  you hear the result. The cost: **at the same knob position it's quieter than X0X or 0.1**; the presets with GAIN
  (or LOUD) give the loudness back.
- **Pages** (GLO after the mixer's four, or SEL + GLO; SELECT runs along mixer and master pages as one row):
  - CHAIN: the stages as blocks, lit when at work, the compressor's and the limiter's reduction under theirs; IN and
    OUT peak meters; loudness (BS.1770 K-weighted, short-term 3 s and momentary 400 ms, LUFS); knobs PRESET, GAIN,
    CEILING, BYPASS.
  - EQ (LOW, MID, HIGH: ±12 dB in 0.5 dB steps; LOW CUT: off, 20–150 Hz, 12 dB/oct) and EQ 2 (LOW FREQ 40–300 Hz,
    MID FREQ 150 Hz–8 kHz, MID Q 0.4–3, HIGH FREQ 2–16 kHz), both with the response drawn from 20 Hz to 20 kHz.
    RBJ biquads; a band at 0 dB costs nothing.
  - COMP, COMP 2: as phase 6.
  - TAPE: DRIVE (off, up to +12 dB into a tanh whose slope at zero is 1: quiet sounds untouched, peaks rounded),
    WARMTH (asymmetry: even harmonics, DC trapped), TONE (open, or a roll-off from 20 down to 3 kHz), MIX.
  - STEREO: WIDTH (0–200 %, mid/side), BASS MONO (off, 60–200 Hz: the side high-passed), BALANCE, and the correlation.
  - LIMIT: GAIN (0–+12 dB), CEILING (0 to −6 dB; −0.3 to start), RELEASE (10–1000 ms), LIMIT on/off. The limiter
    aims 0.5 dB under the ceiling, a soft knee from 0.3 dB under it rounds what gets past.
- **Chain presets** (K1 on CHAIN; a `*` once a knob they set has moved): CLEAN (the defaults), GLUE, PUNCH, WARM, WIDE,
  LOUD, LO-FI. They set the compressor (not PUMP), EQ, TAPE, STEREO and the limiter's three knobs. Measured on the
  FX demo's groove (no PUMP): bypassed −15.1 LUFS; CLEAN −15.2 (level-matched); GLUE −13.0; PUNCH −11.7; WARM −13.2;
  WIDE −13.6 (correlation 0.79 from 0.89); LOUD −9.0; LO-FI −13.4. Peaks never past −0.7 dBFS.
- **BYPASS** takes EQ, COMP, TAPE, STEREO and GAIN out; the limiter stays, at 0 dB GAIN, to protect the output.
- **Cost:** on the Mac, every stage on plus the meters is 2.9× X0X's master alone (EQ 1.1×, meters 0.75×, TAPE 0.5×,
  STEREO 0.2×); the defaults cost what 0.1 did. The meters run only while CHAIN or STEREO is on screen. **Not
  measured on the FM-1.**
- **Saved** with the project (the FX knobs grew from 22 to 42; a 0.1 project would start fresh, but none exists).

## Presets and the song (phase 6), as built

- **User presets:** hold SAVE, turn PRESETS to a slot (EMPTY or REPLACE on screen), let go of SAVE. T2–T4 keep 16 user
  sounds after the 15 factory ones; the drums keep 8 user kits (the kit and every pad's knobs) after 909 and 808.
  PRESETS skips empty slots. They're kept in a store object of their own (OBJ_USER), apart from projects, at once.
- **The song:** HOME, then HOME again. Up to 32 entries of a pattern for 1–16 bars: the white keys set the cursor's entry
  and move on (adding at the end), K1 the pattern, K2 the bars, K4 SONG on or off, OCT+ copies, OCT− deletes. With SONG
  on, PLAY plays it from the first entry (on the SONG page, from the cursor), each entry starting on its bar, round again
  after the last. The header shows S01.. while it plays. Saved with the project.
- **Version:** 0.2 (on the HOME page; 0.1 was never installed). The project's magic is now fixed ("EGT1") with a version byte, so later formats
  can convert instead of starting fresh.

## Controls (as built, unless marked later)

- **Tracks:** **ALGORITHM picks the track** (DRUMS, T2, T3, T4), or hold **SEL** and press pad keys
  1–4. The badge at the top left takes the track's colour. Going between the drums and an instrument
  track lands on that track's own page (the drums' steps, an instrument's KEYS), except from EDIT and
  HOME.
- **The drum track:** as built before (below).
- **An instrument track's KEYS page:**
  - All 27 keys play the track; OCT− and OCT+ shift the octave (−3 to +3). A keyboard on the screen
    shows the keys held.
  - The knobs are LENGTH and RATE (this track's part), SWING and TEMPO.
- **SEQ** switches an instrument track between its steps and its keys (since 0.4: STEPS, the 303 generator, KEYS).
  - The screen shows the notes, pitch up, 16 steps at a time. The white keys are the steps; pad keys
    1–4 pick the step page.
  - Press an empty step: it takes the last note played. Press and release a lit step: it goes off.
  - Hold a step: **K1 NOTE** (the whole chord moves), **K2 VELOCITY**, **K3 GATE** (in eighths of a
    step, up to 8 steps), **SELECT** its chance; OCT− puts the chance back to 100 %.
- **REC:**
  - **While playing:** notes played go to the nearest step, and the gate is how long you held them.
    Chords recorded together land on one step.
  - **While stopped, on KEYS:** step recording. Play a note or a chord: it goes into the cursor's step,
    and when you let go the cursor moves on. SELECT moves the cursor (a rest).
- **EDIT on an instrument track:** five pages of four knobs. Tap EDIT for the next page (after the last,
  back to KEYS), or turn SELECT. The keys keep playing on the EDIT and ENV pages, so you hear what you turn.
  - VOICE: TYPE (MONO, POLY, SAMPLER), LEVEL, VOICES (UNISON on a MONO track), GLIDE
  - OSC: WAVE, DETUNE, MIX, INTERVAL
  - TONE: SUB, NOISE, PWM, DRIVE
  - FILTER: CUTOFF, RESO, ENV AMT, FILTER (LP, BP, HP)
  - MORE: KEYTRACK, VELOCITY, SPREAD, LEGATO
- **ENV on an instrument track:** the amp's ATTACK, DECAY, SUSTAIN and RELEASE; tap ENV again for the
  filter's; again for KEYS.
- **PRESETS on an instrument track:** the 15 factory sounds. Each one sets MONO or POLY.
- **A SAMPLER track's EDIT pages:** VOICE (TYPE, LEVEL, VOICES, GLIDE), SAMPLE (START, END, LOOP, LOOP START, with
  the waveform), PITCH (ROOT, TUNE, DRIVE, RECORD AT), FILTER and MORE as the synth's. ENV as the synth's.
- **SEL:** hold it and press pad keys 1–4 to pick a track; REC held + SEL samples (on a SAMPLER track); SEL stops a
  recording.
- **HOME:** the white keys pick a pattern; **a pattern holds all four tracks** and switches at the end
  of the bar, where every track starts again. **Pad keys 1–4 mute tracks** (kept with the project, not
  the pattern).
- **SAVE:** saves; hold SAVE and press a pattern to copy the whole pattern into it.
- **MIDI:** channel 10 plays the pads, channels 2–4 play T2–T4, and channel 1 plays the track picked.
  The keys send on their track's channel.
- **LFO, ARP and locks:** see Modulation above.
- **GLO and FX:** see the mixer and FX above.

## Data

- **A pattern** holds the drum part (11 rows of 64 steps, accents, mutes, 16 locks) and three
  instrument parts (64 steps of up to 4 notes, velocity, gate and chance). About 1.7 KB, so 16
  patterns take about 27 KB.
- **A project** is the 16 patterns, the three instrument tracks' settings (TYPE, LEVEL, octave), the
  drum kits' knobs, the track mutes, swing and tempo, then each pattern's sounds and kit and the follow actions, then
  the parts' 303 generators: format 3 (0.4), 32,120 of the file's 32,736 bytes, 616 left. A format-2 file (0.3's,
  31,544 bytes: all before the generators) is read as it is, the generators zero (unset); its fixture is
  `app/tests/fixtures/proj_03.img` (made by `make_v2.txt` with 0.3's build). Format 2 has 64 parameters a sound and
  7-byte steps (format 1's spare byte a step, 3 KB in all, paid for the sounds' growth). A format-1 file (0.2's,
  31,160 bytes, or the performance update's, 32,680) is read a part at a time from the flash (`project_load_v1`; its
  layout frozen as `project_v1_t`): its sounds get the new parameters' defaults, USER n moves from preset 15 + n to
  24 + n. RELOAD reads either format. User presets: "USR2" (64 a sound), 0.2's "USR1" converted in place as they
  load. It's a CRC-checked file with two
  copies (A at 0x97000, B at 0x9F000, 32 KiB each), so a power cut while saving keeps the old one.
  (fm1-base's file code doesn't keep the 92 KiB window's rule, so the project isn't there.)
- **The sample bank:** 53 sectors after the project file (0xA7000–0xDBFFF). A sample is a 32-byte record head
  (written last, CRC-checked) and its file: a 16-byte head (frames, rate) and the 12-bit frames, CRC-checked. Each
  track's newest whole record is its sample.

## How we build and test

1. **Host simulator on the Mac.** Scripted runs press keys and turn knobs, then write WAV audio and
   PNG screens. Every feature gets a scenario test before it goes near the FM-1.
2. **Device:** over USB-MIDI with `tools/fm1_install.py` while an fm1-base firmware runs (the loader writes only
   changed 4 KiB sectors); the ESP32-S3 route (backup, dry run, write) for M-VAVE's own firmware. Only the app region
   is ever written, never the boot head.

**Build:** `../../.venv/bin/python tools/build.py ../../app [--host]` from `third_party/fm1-base`, with
`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` and Docker running (the JieLi toolchain is Linux-only).
**Tests:** `app/build/host/ghettomachine_host app/tests/drums.txt OUT` and `app/tests/tracks.txt`.

## Built so far (20 Nov)

**The drum track.** Two kits, 909 and 808, switched with PRESETS; both share one pad layout.

| Pad (black key) | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 909 | BD | SD | LT | MT | HT | RS | CP | CH | OH | CR | RD |
| 808 | BD | SD | LT | MT | HT | RS | CP | CH | OH | CY | CB |

- **SEQ:** white keys are the pad's steps; hold SEQ for accents; hold a step and turn SELECT for chance.
- **EDIT:** the pad's own controls; hold a step and turn a knob to lock it.
- **ENV → MUTE page:** mute pads per pattern; mutes are saved with the pattern.
- **GLO → KIT page:** the kit's own knobs.
- **REC while playing:** pads played go to the nearest step.

**Phases 1 to 5:** everything under Controls above, and the synth, sampler, modulation, mixer and FX as described.

**Status**
- **Tests**, all passing in the simulator:
  - `app/tests/drums.txt`: 59 checks (the 14 about pad 12 went with it).
  - `app/tests/tracks.txt`: 97 checks: track select, the keys, MONO and POLY, steps and their knobs, undo,
    step and live recording, playback, polymeter, the pattern change at the bar, track mutes, TYPE and LEVEL,
    octave, MIDI in and out, copying a pattern, and the project surviving a power cycle.
  - `app/tests/synth.txt`: 82 checks: presets and TYPE, the mono stack and legato, glide, unison, the EDIT and
    ENV pages, the cutoff and a slow attack heard in the level, the POLY cap, the 16-voice pool and stealing
    across tracks, and the sound surviving a power cycle.
  - `app/tests/sampler.txt`: 74 checks: SAMPLER refused and silent without a sample; arming, the first sound
    starting it, 2 s at 44.1 kHz, stopping early, 22.05 kHz; playing it, once and looped, an octave up; the shape and
    pitch knobs; recording while playing and storing at the stop; another track refused meanwhile; the bank's
    placement, cutting to fit, BANK FULL; everything after power cycles.
  - `app/tests/sample_roundtrip.py`: a 48 kHz 24-bit stereo WAV to T3 through the companion and back byte for byte,
    a 22.05 kHz one to T4, then a delete.
  - `app/tests/mod.txt`: 87 checks: an LFO restarted by a note at a phase moving the pitch; a synced LFO restarted at
    the bar moving the cutoff; the drums' LFO giving the two snares of a bar different tones, the knob coming back at
    the stop and after SAVE; the arpeggiator UP, DOWN over two octaves, LATCH, and over a sequenced chord; a cutoff
    lock on one step, its value there and the knob's own at the next step; clearing locks.
  - `app/tests/fx.txt`: 77 checks (moved to the master bus's pages in 0.2): the mixer's pages and knobs; the drums panned hard right leave the left silent; a
    note's reverb and echoes ring on after it, and nothing does without the sends; one echo at 1/4 with no feedback,
    then silence; sampling takes the delay out and gives it back; the FX knobs; the limiter keeping a loud stack
    under the ceiling with +12 dB GAIN (and not, with LIMIT off); all of it after a power cycle.
  - `app/tests/master.txt`: 103 checks: the defaults; GLO into the master bus and SELECT back; the EQ's curve (a +6 dB
    shelf, a high cut, the low cut, the shelf's frequency); TAPE and STEREO lighting their stages; +12 dB into a −1 dB
    ceiling with everything loud and nothing over it; the correlation and the loudness measured; BYPASS; presets
    setting all their knobs and CLEAN restoring the defaults; the `*`; the FX button's four pages; a power cycle.
  - `app/tests/mbus_unit.c`: 48 checks of the DSP with sines: each EQ band against the cookbook and against its drawn
    curve (within 0.15 dB), all-flat bit for bit, TAPE's unity for quiet sounds and its harmonics, WARMTH's DC, TONE,
    WIDTH 0 and 200 %, BASS MONO, the limiter's ceiling with +12 dB in, and a 997 Hz −20 dBFS tone reading −20.0 LUFS.
  - `app/tests/master_demo.txt`: the FX demo's groove bypassed, then through each preset, 4 s each (`master_demo.wav`).
  - `app/tests/release.txt`: 69 checks: a user sound saved and browsed back to; a user kit saved, changed and reloaded;
    the song's entries (keys, bars, copy, delete), its playback changing patterns on the bar and going round; SONG off;
    all of it after a power cycle.
  - `app/tests/rec.txt`: 80 checks: the metronome, the count-in, recording to a grid, QUANTIZE, LEGATO, chord mode,
    gates past 8 steps, kept with the project.
  - `app/tests/scenes.txt`: 44 checks: clips per track (stopped: at once; playing: at the bar), a track stopped at the
    bar, scenes, LAUNCH NOW, editing the clip a track is on, mutes by a tap.
  - `app/tests/perform.txt`: 48 checks: each perform key held and let go (ROLL's replays, FILL, STUTTER, FILTER, THROW,
    WASH, CRUSH, TAPE STOP), their K1s, tap tempo, solo.
  - `app/tests/live.txt`: 120 checks: SOUNDS PER PATTERN (a T2 sound and the kit following a clip and a scene, kept
    with each pattern), follow actions (NEXT after a bar, FIRST after 2), motion recording on T2 and the drums (and none
    when not recording), RELOAD, a power cycle, solo, MIDI CLOCK sent (24 a beat) and followed (starting on the first
    clock, 100 BPM, then 125, the groove within 10 ms of the clock), tap tempo, and a save and power cycle in format 2.
  - `app/tests/convert.txt`: 65 checks: 0.2's own file (`fixtures/proj_02.img`, the layout on the user's FM-1, with a
    USER sound) and the performance update's (`proj_perf.img`), both made with the format-1 build
    (`fixtures/make_v1.txt`), load into format 2 with every value kept and the new parameters at their defaults;
    RELOAD from a format-1 file; saved as format 2 and loaded again.
  - `app/tests/synth2.txt`: 101 checks: the OSC 2, SHAPER, MACRO, MOD ENV, LFO 2 and MATRIX pages, the voice playing
    what they set (MOD ENV → PITCH, MACRO 1 → CUTOFF, LFO 2 → CUTOFF), a matrix amount locked on a step (a SOURCE
    not), a macro recorded as motion, three of the new factory sounds (sounding, not clipping), a USER sound and the
    project keeping the new parameters through a power cycle.
  - `app/tests/synth_unit.c` (plain cc, with `-fsanitize=undefined,address`): 39 checks on the synth alone: FM's
    harmonics (1:1 all, 2:1 odd only), RING's sum and difference, WAVE 2, each SHAPER type and BIAS, MOD ENV and LFO 2
    on the pitch, DRIFT, VELOCITY → LEVEL, MACRO → CUTOFF, NOTE → PAN, every feature at its maximum on every wave
    across the keys (finite, bounded, no overflow), a SAMPLER through the SHAPER; (0.4) LEGATO SLIDE gliding only into
    a tied note, ON and OFF as before; SN_SLIDE gliding on MONO and POLY with LEGATO off and GLIDE 0, SN_ACCENT louder
    and brighter, its sweep falling away.
  - `app/tests/acid.txt` (0.4): 170 checks on the 303 generator: SEQ's pages, a line from the defaults (in the scale,
    with slides and accents), DENSITY 0 and 100 and back to the same line, every step WHOLE, HALF, slid and accented
    (marks and gates), WHOLE notes held over rests, STEPS's held step + OCT+ and VELOCITY → ACCENT, a POLY sound
    playing slides on one voice and accents, NOTES 1 and
    OCTAVE 0 (the root throughout), ROOT, every scale (no note off it), SEED back to an earlier line, MUTATE, its undo
    and redo, an 8-step part, another track's own part, a pattern copy, an empty part starting from the track's last
    settings, playing while it changes, format 3 through a power cycle.
  - `app/tests/convert3.txt` (0.4): 0.3's file (`fixtures/proj_03.img`, by `make_v2.txt` with 0.3's build) loads with
    its notes, length and FM, the generators unset; a line made on it; saved as format 3 and loaded again.
  - `app/tests/synth_same.sh`: 0.2's 15 sounds rendered by the old synth (`app/attic/synth_v1`) and the new, byte for
    byte the same.
  - `app/tests/fx_demo.txt`: phase 4's groove through the mixer and FX, to `fx_demo.wav` (nothing clipped).
  - `app/tests/demo.txt`, `sampler_demo.txt` and `mod_demo.txt`: a groove built by step recording; a bar of the beat
    resampled and played back looped; the groove with the pad's cutoff on a 2-bar LFO, an arpeggio and S&H hats.
- **The FM-1 package (0.4: 0.3 and the 303 generator):** `app/build/ghettomachine.fwsc`, identity FM-1_800, image 444 KB with the
  logo (RAM 69,860 of 98,304 B; pool 282,780 of 344,064), the second core built in; every build check passes;
  installed over USB on 20 Nov (it reports GHETTOMACHINE, no crash recorded). 0.2 itself (up to chord mode) was installed over USB-MIDI twice (`fm1_install.py`) and played. **0.3
  was installed over USB on 20 Nov** (identity FM-1_800, running, no crash logged); the CPU load and redraw rate are
  still unmeasured there.

## Phases

| # | Phase | Done when |
| --- | --- | --- |
| ✓ | **Drum track** | Simulator checks pass |
| ✓ | **1. Four tracks**: track select, the project as a file, patterns holding all four tracks, note steps with chords, live and step recording, a plain test voice | Host tests pass; the drum track still passes its checks |
| ✓ | **2. The synth voice and the voice pool**: MONO (legato, glide, unison), POLY, stealing, shedding, single-core fallback, the EDIT and ENV pages, sound presets | Built and tested in the simulator. **Still to check on the device:** 16 voices plus a dense 808 with no dropouts |
| ✓ | **3. The sampler**: the sample bank in flash, SAMPLER type, loop points, recording, WAVs per track from the Mac | Done in the simulator: a WAV through the companion and a recording both play and survive power cycles. **Still to check on the device** |
| ✓ | **4. Modulation**: ENV pages, an LFO per track (drums included), the arpeggiator, locks on instrument steps | Scenario tests cover each (`app/tests/mod.txt`) |
| ✓ | **5. Mixer and FX**: levels, pans, sends, reverb, delay, master compressor and limiter | Sounds right in the simulator (`app/tests/fx.txt`, `fx_demo.wav`) and fits in memory. **Still to check on the device:** the CPU |
| ✓ | **6. Release v0.1**: presets, a song chain, a short manual, the master compressor | Built, tested in the simulator, [MANUAL.md](MANUAL.md) written |
| ✓ | **9. Recording help (v0.2)**: hold REC: a metronome (OFF/ON/REC, the bar's first beat higher), a 1–2 bar count-in (a note in its last half beat lands on step 1), recording to a grid (a step, 2, 4), QUANTIZE (OCT−) and LEGATO (OCT+) on the loop, undoable. Gates up to 31.9 steps (GATE a step a detent over 2 steps). Chord mode on ARP 2 (12 chords, inversions; recorded as chords; into the arpeggiator) | `app/tests/rec.txt` (80 checks). Settings in project_t's and track_t's reserve bytes: a saved 0.2 project still loads. **Still to try on the device** |
| ✓ | **8. The look (v0.2)**: bright colour blocks: the page in the section's colour drawn in ink, a dark band (output meters, the change shown large), a picture per page, four dark knob cards matching K1–K4; live meters (tracks, pads, voices, CPU) | Every page screenshotted in `app/tests/ui_tour.txt`; all tests still pass. **Still to check on the device:** the redraw rate (a full screen is ~21 ms over SPI; the meters redraw only the band and moving pages, ~30 a second) |
| ✓ | **7. The master bus (v0.2)**: EQ, compressor, TAPE, STEREO, limiter GAIN/CEILING/RELEASE, chain presets, BYPASS, loudness meters, MASTER last | Built, tested in the simulator (`master.txt`, `mbus_unit.c`, `master_demo.wav`), installed over USB and played. **Still to do: measure the CPU** |
| ✓ | **10. Performance (v0.2 update)**: HOME as a scene grid (a clip per track, scenes, clip stop, LAUNCH at the bar, the beat or now), seven perform keys and TAPE STOP, sounds per pattern (T2–T4's sounds and the kit), follow actions, motion recording, RELOAD (SAVE + OCT−), tap tempo, nudge, solo, MIDI clock sent and followed (timed by the audio block, tempo over 4 beats, the groove pulled to it every 16th), the SETUP page. A tempo change now stretches the bar and the steps together (before, the bar kept the old tempo until its end) | `scenes.txt` (44), `perform.txt` (48), `live.txt` (120); all suites pass (942 checks). The new parts appended to project_t (32,680 of 32,736 B): a 0.2 project still loads. **Still to try on the device** |
| ✓ | **12. The 303 generator (v0.4)**: the ACID pages on SEQ (DENSITY, WHOLE, SLIDE, ACCENT; ROOT, SCALE, NOTES, OCTAVE; SEED, LENGTH, RATE, SWING), NEW and MUTATE, HALF / WHOLE / SLIDE and accents as step marks played on any sound, per-part settings (format 3), LEGATO SLIDE | `acid.txt` (170), `convert3.txt` (28: 0.3's file), `synth_unit.c` (39: LEGATO SLIDE, SN_SLIDE, SN_ACCENT); all suites pass (1,401 checks); device build passes (image 444 KB). Installed 20 Nov |
| ✓ | **13. USB stems (v0.5)**: the USB audio input at ten channels, the master on 1–2 and each track alone on 3–10, dry, before the master bus and MASTER | `stems.txt` (67); all suites pass (1,468 checks; 1,501 with `beats.txt`, the drum steps' beat glow, 21 Nov; 1,560 with `erase.txt`, REC + GLO). Installed 20 Nov and recorded from: each stem right, but clicks (the USB ring ran dry). The deep ring, MASTER out of the USB mix and the beat glow installed 21 Nov: recorded again, no clicks, the master pair full with MASTER at 0. REC + GLO installed 21 Nov. The 808 made up to the 909's level (`kit808.txt`, 1,569 checks), not yet installed |
| ✓ | **11. The synth update (v0.3)**: OSC 2's own wave, FM, RING, DRIFT; the SHAPER (FOLD, CLIP, RECT, CRUSH, BIAS, PRE/POST); MOD ENV; LFO 2 for each voice; a six-slot matrix; MACRO 1–4; nine factory sounds. Project format 2 (64 parameters a sound, 7-byte steps), 0.2's projects and user presets converted as they load | `convert.txt` (65), `synth2.txt` (101), `synth_unit.c` (28, sanitized), `synth_same.sh` (0.2's sounds byte for byte); all suites pass (1,114 checks). **Still to try on the device**, and its CPU with FM and the SHAPER on many voices |
| ✓ | **14. CPU pass (v0.5, 21 Nov)**: the render timed by stage (SETUP, SELECT: the CPU view); the 606 clap's core at 22.05 kHz (96 taps), its hats and cymbal retuned without a sine a line; Color's WOW every 16 samples and an inaudible DIGITAL skipped; the phaser's coefficient every 16 samples; the shifter's window unwrapped; a silent track's insert and Color asleep | `cpu.txt` (24); 46 suites, 2,136 checks. Mac instructions, busy scene 6 s: 1,968 M -> 1,621 M (screen drawing included, about 290 M); stopped: 868 M -> 289 M. RAM .data+.bss 97,604 of 98,304. On the FM-1 since 21 Nov: playing, DRUMS 20–30 %, SYNTH 30 %, REVERB+DELAY 15 %, MASTER 15 % |
| ✓ | **15. CPU pass 2 (v0.5, 21 Nov)**: ROOM at 22.05 kHz (half-length prime lines, the 3 kHz damper matched to X0X's, the pairs' average lifted back by a tap: every octave band within 0.8 dB and its decay times within 20 ms); the master's loops with their state in registers and one pass less (bit for bit); the synth's render loops, ROOM, the delay and the master bus from RAM (`dsp/gm_ram.h`, fm1-base's RAM-code check: 24,360 B, `.ram_text` 29,788 of 32,768) | 46 suites, 2,136 checks; the synth's output bit for bit (12 sounds), the drums' and the master's too. Mac instructions a sample: reverb + delay 363 -> 241, master 199 -> 170, a pad's voice 152 -> 131. Not on the FM-1 yet |

## Risks

- **The master bus's cost on top** (phase 7): up to 2.9× X0X's master with every stage and the meters on. If it's
  short, the voices drop first (the shedding); then the EQ's bands as one pass, or TAPE's tanh as a table.
- **Quieter at the same MASTER position than X0X** (the knob is after the chain now): expected, and in the manual.
- **CPU with a dense 808 and 16 voices: not measured yet.** The synth's render loops run from RAM (row 15),
  half its voices on each core, and the pool shrinks when the load passes 85 %. If that isn't enough on the
  device: the drums from RAM (about 3 KB of the RAM code's 32 KB is left: the 606's metal at most), the drums
  on the second core too, a 12-voice cap with the 808, the 606's metal with 16 partials.
- **The second core** hands back a job every 0.73 ms (a synth block). Its cost per hand-off is unmeasured; if
  it's high, the render moves to a whole audio block per job, with notes timed to the block.
- **Second-core faults on some units.** The single-core fallback is required from phase 2.
- **Reading samples from flash while playing**, on both cores: fm1-base allows it; the cost of the cache misses is
  unmeasured. If it's too high, samples get a RAM cache, and the total shrinks to what RAM holds.
- **The 92 KiB window's rule** (bytes 0xF00–0xF07 of every sector must never look like an update
  record). The sample bank only gets that window if it keeps the rule, as LOOM-8's bank does;
  otherwise samples have the 212 KiB alone.
- **Installing over X0X or SLOOP** overwrites their saved data in the same flash areas. A full backup
  comes first, as always.
- **A project saved by this build won't load in a later one** whose format has changed: it starts from
  the default project instead. Fine while nothing is on the device; a version field and conversions
  come before v0.1.

## Downloads still needed (asked when we get there)

- None for now. (Felucca-Engines was downloaded for phase 2, with the user's yes.)
