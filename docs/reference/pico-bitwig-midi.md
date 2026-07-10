# Pico Bitwig MIDI Mapping

Authoritative mapping for the current EigenLite Pico-to-Bitwig bridge.

This document describes the current `stable` bridge mode.

This reflects the intended stable controller behavior for using an Eigenharp Pico
as a MIDI controller in Bitwig through:

- WSL `pico-udp-midi-bridge`
- Windows `udp_midi_receiver.py`
- loopMIDI virtual ports

## Goals

- one note-on per key press
- one note-off per key release
- no repeated note retrigger while a key is held
- no overlapping controller assignments
- one clear ribbon CC
- behavior suitable for Bitwig MIDI learn and instrument control
- preserve a known-good bridge mode while refined parity work is developed separately

## Event Mapping

### Keys

- key press -> MIDI Note On, channel 1
- key release -> MIDI Note Off, channel 1
- note number = `key + 48`

### Key Pressure

- per-key pressure -> Poly Aftertouch, channel 1
- sent only when the MIDI 7-bit pressure value changes

### Breath

- breath -> `CC2`
- sent only when the MIDI 7-bit value changes

### Ribbon

- Pico ribbon -> `CC21`
- any Pico strip event is treated as the single user-facing ribbon control
- sent only when the MIDI 7-bit value changes
- when ribbon touch becomes inactive, value is sent as `0`

### Relative Ribbon (parity mode only)

- Pico ribbon delta from touch origin -> `CC22`
- origin is captured on touch-start, held for the duration of the touch
- centred at `CC 64` (no displacement); increases/decreases from there as the
  finger moves away from the touch-start point
- resets to centre (`CC 64`) when the touch ends
- only sent in `parity` mode; absolute `CC21` above still works in both modes
- good for vibrato/nudge-style gestures where "how far have I moved" matters
  more than "where am I on the strip"

### Mode Buttons

- the 4 Pico body buttons -> MIDI Note On/Off, channel 1
- note number = `44 + button` (44-47), velocity 127, momentary
- sits just below the main-key note range (48-65) so it never collides
- same mapping in both `stable` and `parity`
- Bitwig-mappable via ordinary MIDI learn, like any other controller button

### Roll (parity mode only)

- last-touched main key's roll (tilt) -> `CC74`
- only sent in `parity` mode; `stable` mode does not send it
- diagnostic/expressive signal, not gated on note-on state
- sent only when the MIDI 7-bit value changes

## LED Feedback

Two independent behaviors, layered: a DAW-set base colour per key, with a
transient press overlay on top.

### Auto key-press light

- pressing any main key or mode button lights it orange
- on release, it reverts to that key's current base colour (default off)
- local to the WSL bridge; no DAW involvement; same in `stable` and `parity`

### Bitwig -> Pico LED control

- dedicated MIDI channel 16, Note On (status `0x9F`)
- note = key/button index: `0-17` main keys, `18-21` mode buttons
- velocity = colour: `0`=off, `1`=green, `2`=red, `3`=orange
- sets that key's base colour; applied immediately unless the key is currently
  held, in which case it's applied on release

### Transport

```text
Bitwig -> loopMIDI "Pico Out" -> Windows sink (udp_midi_receiver.py --sink-in)
-> UDP 5006 -> WSL pico-udp-midi-bridge -> harp.setLED()
```

- Windows resolves the WSL IP automatically (`wsl hostname -I`) in
  `pico-online.ps1`; override with `-WslDistro` if you run multiple distros
- WSL side listens on UDP `5006` by default; override with
  `pico-online-wsl.sh --led-port PORT`, or disable with `--no-led`
- disable on the Windows side with `pico-online.ps1 -SkipLedForward`
- use `tools/pico-led-test.cpp` (`tools/pico-led-test.sh`) as a standalone
  sanity check of `setLED()` independent of the MIDI/UDP plumbing

## Transport Path

```text
Pico
-> EigenLite in WSL
-> pico-udp-midi-bridge
-> UDP to Windows
-> udp_midi_receiver.py
-> loopMIDI input port
-> Bitwig
```

Recommended loopMIDI layout:

- `Pico In` for Pico -> Bitwig MIDI input
- `Pico Out` for Bitwig output/controller return path if needed

## Bitwig Notes

- Use the loopMIDI input port as the Pico MIDI source.
- Use `CC21` for ribbon mapping.
- Use `CC2` for breath mapping.
- If Bitwig wants a separate controller output port, use a second loopMIDI port rather than reusing the input port.

## Runtime Mode

The WSL launcher currently supports:

- `stable` — the current working Bitwig-oriented mapping documented here
- `parity` — refined EigenD-oriented controller behavior under active development

Current `parity` mode work-in-progress:

- breath uses an EigenD-style deadband/hold interpretation instead of the blunt stable-mode gain boost
- key note-on is no longer immediate; it uses a debounce / short estimation / gating path modeled on EigenD's Pico key layer
- ribbon currently remains mapped like `stable` while controller-layer parity for relative vs absolute strip behavior is designed

Example:

```bash
/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh --mode stable
```

## Debug Expectations

### WSL bridge debug

Expected event types:

- `key ...`
- `button ...`
- `breath ...`
- `strip ...`
- `gate note_on ... [parity]`
- `gate reject ... [parity]`
- `gate note_off ... [parity]`

Focused examples:

```bash
/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh --debug --debug-scope gates --mode parity
/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh --debug --debug-scope controls --mode parity
```

### Windows receiver debug

Expected MIDI forms:

- notes: `midi 9x .. ..` and `midi 8x .. ..`
- pressure: `midi Ax .. ..`
- breath: `midi B0 02 ..`
- ribbon: `midi B0 15 ..`

Focused examples:

```powershell
py "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\udp_midi_receiver.py" --out "Pico In" --sink-in "Pico Out" --port 5005 --debug --debug-filter notes
py "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\udp_midi_receiver.py" --out "Pico In" --sink-in "Pico Out" --port 5005 --debug --debug-filter breath
py "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\udp_midi_receiver.py" --out "Pico In" --sink-in "Pico Out" --port 5005 --debug --debug-filter ribbon
```

## Rationale

Earlier bridge revisions had three problems:

- repeated note-on while a key was held
- controller spam caused by comparing raw floats instead of MIDI-byte values
- ambiguous ribbon mapping caused by treating Pico strip indices as two separate user-facing CC controls

The current mapping removes those failure modes and is the intended baseline.
