# Pico Parity Checklist

Use this checklist when evaluating `stable` vs `parity` bridge mode in Bitwig.

## Setup

- loopMIDI ports exist:
  - `Pico In`
  - `Pico Out`
- Windows side started
- WSL side started with either:
  - `--mode stable`
  - `--mode parity`

## Notes

- repeated fast taps do not create false retriggers
- held notes do not machine-gun
- note attacks feel responsive
- note-offs are clean

## Breath

- breath activates without unreasonable effort
- small rest noise does not cause constant modulation
- musical breath produces a useful MIDI range
- Bitwig mapping responds in a controllable way

## Ribbon

- absolute ribbon mapping is stable
- ribbon spans a useful modulation range
- no obvious jitter or unusable dead zones

## Relative Ribbon (parity mode, `CC22`)

- centres at `CC 64` at touch-start with no perceived jump
- delta grows/shrinks smoothly as the finger moves away from touch-start
- resets cleanly to centre on release
- useful for vibrato/nudge-style Bitwig mappings, not just theoretically distinct from absolute

## Mode Buttons

- all 4 body buttons produce a clean Note On/Off pair (notes 44-47)
- no stuck notes after repeated presses
- Bitwig MIDI-learns each button independently

## LED Feedback

- pressing a key/button lights it orange immediately
- releasing restores the previous DAW-set colour (default off)
- a channel-16 Note On from Bitwig changes the corresponding key's base colour
- colour change while a key is held is deferred until release, not lost
- LEDs recover correctly after a Bitwig restart / bridge restart

## Bitwig Integration

- notes arrive reliably
- breath mapping is learnable/useful
- ribbon mapping is learnable/useful
- mode buttons are learnable/useful
- controller behavior remains stable after reopening Bitwig

## Stability

- no loopMIDI feedback mute
- no repeated controller startup failures
- no USB stall or dead-device behavior during normal play
- Windows/WSL restart path remains predictable
