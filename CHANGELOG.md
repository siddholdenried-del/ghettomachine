# GhettoMachine: build history

Moved out of MANUAL.md on 24 Nov 2026. The newest notes are in the manual's sections and the commit log.

**24 Nov 2026, later (project format 12):** mix per pattern (SETUP K4); MIDI out of the sequencer (SEQ OUT), MIDI
Continue, and Bluetooth MIDI on fm1-base's own stack (SETUP, SELECT: MIDI); the drums' beat generator (SEQ again on
their steps); the 303 generator's KEEP page. The stock 808 / 909 engines' state left the FM-1's RAM (15 KB: they
have not played there since the DrumEngine took over), which is what made room for the radio.

**24 Nov 2026, later still:** Bluetooth audio out (SETUP, SELECT: BT AUDIO; its buffers lent from the undo history
while it is on); the keys' pressure as a matrix source and as channel pressure; the battery in the band. The delay's
line 63000 -> 59400 samples (0.71 -> 0.67 s) for the pool.
Then Bluetooth keyboards (the FM-1 as the central: SETUP > MIDI, K3 / K4) and the headphone jack read and said.

**24 Nov 2026, the last batch:** cut: the 0.2-0.4 save converters and the stock 808 / 909's knobs, pages, locks and
Color on the FM-1 (24 KB of flash, 1.5 KB of RAM). Added: microtiming (a held step + PRESETS); copy and paste (SAVE /
REC with a step, a lane's key or SEQ held); STEP LOCKS (LFO 1 rate and depth, arp rate and gate, a step); BEAT's
lane KEEP and hat ROLLS; LFO 2 synced to the tempo; the arpeggio recorded; Song Position, CC 1 / 20-23 to the
macros, per-key aftertouch out; Bluetooth audio back at power-on.

**24 Nov 2026, after:** SWING is each part's own (a track's part of a pattern); SEL + SWING sets them all.

**25 Nov 2026:** STUTTER 1/16, 1/8T, 1/8, 1/4 (1/32 out); the QUICK MIXER's STUTTER 1/16, 1/8, 1/4; ROLL's triplets
1/4T and 1/8T. The percs: models G ZAP, H SYNC, I RING, RESO and FILTER on every model, eight electro presets. Levels:
the drums' trims written again for every recipe (tests/drumengine_levels.c), and a trim for every synth factory
sound for the first time (tests/synth_levels.c, dsp/synth_levels.inc).
LEVEL reads in dB (100 = 0.0 dB) and goes to 200 (+6.0 dB).
Project format 13: with SOUNDS PER PATTERN each pattern keeps every drum's own settings. LFO 2's page draws and
scrolls as LFO 1's. The delay's line 59400 -> 58200 samples for the pool.

**Historical hardware test: 0.5** (first installed over USB on 20 Nov). The first recording from it clicked now and then, so
0.5 now keeps a deeper USB buffer, MASTER no longer changes what a computer records, and the drum steps' beats glow
(1, 5, 9, 13): that build went on the FM-1 on 21 Nov, and a recording from it had no clicks. REC + GLO (erase the
track's part, to record it again) went on it the same day, and later the 808 brought up to the 909's level with the
Dungeon BBS screen. The CPU pass (SETUP's CPU view; a cheaper 606, Color and phaser; effects that sleep on silence) went
on it the same day, and a second pass (the reverb at half the rate; the synth's voices, the reverb, the delay and the
master bus run from RAM) later that day. A third (the reverb, the delay and the master bus on the second core, a block
behind the first; the CPU view's peaks) went on it that evening. The sections' budgets went on it that night at 20 %
for the drums, 30 % for the synth and 25 % for the effects: played, it had no late blocks, but the budgets cut 138 drum
sounds in a minute and held the synth to 4 or 5 voices with a third of the first core unused. Loosened to 30 %, 40 %
and 25 %, that went on it the same night (7 to 9 voices, few drum cuts; the first core at about 70 %). The 606 and
the 707 were then cut (their code and memory given to the processor's budget) and FILL made to leave the drum clip
where it would have been: on the FM-1 since that night. The synth made cheaper without changing its sound (below)
went on it later that night (the synth's mix from 450 to about 250 us a block; steady playing at about 60 % of the
first core, the only late blocks at kit changes). Then, asked for: the budgets back at 20 %, 30 % and 25 % with a
guard for the block that takes too long, the LFO made each clip's own (project format 8), and no spike as clips
change (a kit that leaves stops sounding): on the FM-1 since 22 Nov. The tracks' mix, with their inserts and Color,
shared between the two cores: in the source, not on the FM-1 yet. 0.5 is 0.4 plus the USB stems. 0.4 is 0.3 plus the 303 generator, the new name and the boot screen's logo; 0.3 is 0.2 plus the performance
update (the scene grid onwards) and the synth update. 0.5 passes its simulator tests (2,194 checks, 39 more on the
synth itself and 48 on the master bus's sound). It reads a project saved by 0.2, 0.3 or 0.4, and by 0.5 before each clip had its own LFO.
