# USB MIDI Host bench candidate

This app uses fm1-base's existing USB Host port, with the Melodee-derived
felucca-bsp implementation credited in its NOTICE. No board driver, clock,
USB port power, update protocol or storage format was changed.

- Boot always starts in **USB Computer** mode. Leave it there for USB audio,
  MIDI from your computer and companion functions.
- For a computer-free session, stop playback and turn record off. Disconnect
  the Mac before changing role. Open SETUP with HOME, HOME, HOME from the
  normal sequencer screen. Turn K3 right to **MIDI HOST**.
- Use a suitable **powered** hub or adapter with a class-compliant,
  full-speed USB MIDI 1.0 controller. The FM-1 does **not** supply 5 V VBUS.
  Turn K3 left in SETUP to return to **COMPUTER**. The choice is not saved to
  flash; restarting also restores Computer mode.
- On a default channel 1 controller, select a synth track first (T2–T4).
  Channels 2–4 directly play those tracks; channel 10 follows the existing
  GM-to-pad map. SETUP K1 remains the existing MIDI CLOCK control.
- USB Host is exclusive with the computer-facing port. USB audio, computer
  MIDI and the Mac companion are unavailable while hosting. This does not
  add simultaneous Host + Computer, bus power, MIDI 2.0, multi-device merge,
  or third-party SysEx forwarding.

## Acceptance before a hardware rollout

1. Retain the exact known-good AC84F7D2 program-only rollback and confirm a
   restorable project backup without sending any writes. No automatic flash.
2. With a separately approved bench install, verify boot in Computer mode,
   project format/save, MIDI velocity, ten-channel USB audio, and companion
   handshakes. In Host, qualify a powered Akai model (record exact model and
   hub); notes and velocity, channel map, stopped-only role change, clock
   Stop, unplug/replug while notes are held, same-note overlap with TRS and
   panel, and a USB arpeggio latch. Verify return to the Mac with no stuck
   notes and without power cycling. Reboot once and verify Computer again.
3. Run the documented heavy synth/drum play case under ordinary playing,
   then with rapid controller events and an unplug during a held chord.
   Compare LATE counters and maximum callback/USB service times against the
   matched computer-mode control. A build and simulator are not proof of
   FM-1 USB power negotiation, enumeration, or real-time headroom.
4. Qualify combinations where the same note is held by two different sources
   through a hard sound/arp/mute reset, and USB synths as separate, later use
   cases. A same-source synth retrigger still attacks after a UI cut, but no
   physical proof exists for mixed-input overlaps across those transitions
   or for output-note ACK/reconnect cleanup on a downstream synth.

The application regression is `app/tests/usb_midi_host.txt`; the BSP USB
host model is `third_party/fm1-base/bsp/tests/run_usb_host_tests.sh`.
