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
- if parity relative ribbon is added later, evaluate it separately

## Bitwig Integration

- notes arrive reliably
- breath mapping is learnable/useful
- ribbon mapping is learnable/useful
- controller behavior remains stable after reopening Bitwig

## Stability

- no loopMIDI feedback mute
- no repeated controller startup failures
- no USB stall or dead-device behavior during normal play
- Windows/WSL restart path remains predictable
