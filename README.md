# GhettoMachine

GhettoMachine is **BongWare** custom firmware for the **M‑VAVE FM‑1**, engineered by **BongTech** at [BongCult](https://bongcult.com/). It is a four-track groovebox: one drum track and three instrument tracks.

## Engine goal

Build one efficient audio engine for the FM‑1: one BongDrums track and three BongSynth tracks feed the same mixer, with one shared BongVerb and one shared BongDelay on sends. The mixer sums dry tracks and both returns before BongMaster on the stereo output. BongMaster is being separated from the instrument engines so it can be profiled and tuned on its own, without giving up drum attack, natural tails, or playable polyphony to make a CPU number look better.

HTML5 versions are for auditioning sounds and prototyping controls. The acceptance test is the same dense four-track song on the FM‑1: 256 frames at 44.1 kHz gives **5.805 ms** per audio block. See the [engineering plan](PLAN.md#unified-engine-objective).

**GhettoComputer** is a proposed HTML5 sibling with up to eight engine tracks and one shared BongVerb and BongDelay via sends. It is not a released instrument or an FM‑1 performance benchmark.
