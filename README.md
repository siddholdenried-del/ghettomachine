# BongTech GhettoMachine

A four-track groovebox firmware for the **M-VAVE FM-1**: one drum track (909, 808 or 606 with editable sounds) and three
instrument tracks, each a mono synth, a poly synth or a sampler. A 128-step sequencer with 16 patterns, a scene grid
for playing live, an LFO on every track, an arpeggiator and chord mode, a reverb, a tempo delay and a master bus with a
mastering chain, a deeper synth (FM, ring, a wave shaper, a mod matrix, macros), a **303 pattern generator** whose
slides and accents play on any sound, and **stems over USB**: a computer records the master and each track on its own
pair of inputs.

It was called fm1-eight until 0.4.

- **Play it in the browser:** <https://ghettomachine.bongcult.com> (the firmware compiled to WebAssembly)
- **The manual:** [MANUAL.md](MANUAL.md)
- **How it works:** [docs/how-it-works.html](docs/how-it-works.html), the page at the site above
- **Design notes and history:** [PLAN.md](PLAN.md)

It's built on [fm1-base](https://github.com/charlesvestal/fm1-base) (the FM-1 platform, a submodule here) and runs on
an FM-1 that already has an fm1-base or Felucca firmware on it, installed over USB.

## Building

```sh
git clone --recursive https://github.com/siddholdenried-del/ghettomachine
cd ghettomachine/third_party/fm1-base
python3 -m venv ../../.venv && ../../.venv/bin/pip install pillow==13.0.0 mido==1.3.3 python-rtmidi==1.5.8
../../.venv/bin/python tools/build.py ../../app --host   # the simulator: app/build/host/ghettomachine_host
../../.venv/bin/python tools/build.py ../../app          # the FM-1 package: app/build/ghettomachine.fwsc (needs Docker)
../../.venv/bin/python tools/build.py ../../app --web    # the browser emulator: app/build/emu (needs Emscripten)
cd ../.. && sh site/publish.sh                          # the site: rebuilt, committed, pushed
```

The site at ghettomachine.bongcult.com is `site/public` as committed: Cloudflare Workers Builds deploys `main` on every
push and serves that folder (`wrangler.toml`), with no build of its own.

fm1-base's README has the details (the toolchain runs in Docker; Pillow 13 rasterises the fonts; mido and
python-rtmidi talk to the FM-1). The tests are scripts for the simulator, run from the repository's top: `app/build/host/ghettomachine_host app/tests/acid.txt
OUT_DIR` (and the other `app/tests/*.txt`), plus `app/tests/synth_unit.c`, `app/tests/mbus_unit.c` and
`app/tests/synth_same.sh`; each file says how to run it.

## Installing on an FM-1

With fm1-base's `tools/fm1_install.py app/build/ghettomachine.fwsc` over USB-MIDI, while an fm1-base or Felucca firmware is
running. **At your own risk:** save your work first; a cut-off install needs fm1-base's recovery route. See
MANUAL.md, "Installing it, and getting back".

## Licence and credits

GPL-3.0-only (see [LICENSE](LICENSE)). Built on fm1-base and felucca-bsp (MPL-2.0); the synth voice from
Felucca-Engines by Leo Kuroshita, Hügelton Instruments (MIT); the 909 and 808 engines from X0X by Charles Vestal
(GPL-3.0), after 9W9 and 8W8 by athousanddetails; the 909 cymbals from ER-99 by Matthew Cieplak (GPL-3.0); the reverb,
delay and master from X0X (GPL-3.0), after 9W9. The logo is set in Sedgwick Ave Display by The Sedgwick Ave Project
Authors (SIL Open Font License 1.1: [app/assets/fonts](app/assets/fonts)); the firmware carries only its rendered
pixels. Not affiliated with M-VAVE or Roland.
