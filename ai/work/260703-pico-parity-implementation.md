<!-- created: 2026-07-03  refs: docs/reference/pico-parity-roadmap.md, tools/pico_udp_midi_bridge.cpp -->

# Pico Parity Implementation

## Goal

Move `pico-udp-midi-bridge` from a purely pragmatic Bitwig controller mapping toward a runtime-selectable faithful controller layer, while preserving the existing working `stable` mode.

The target is not raw hardware parity at any cost. The target is controller-behavior parity that is:

- recognisably closer to original EigenD Pico feel
- useful in Bitwig
- testable phase by phase

## Runtime Modes

### `stable`

- current known-good operational bridge
- remains usable throughout parity work

### `parity`

- reconstructed EigenD-style controller behavior
- evolves incrementally

## Phase Plan

### Phase 1: Breath Parity

Status: baseline achieved, continue validation

Current state:

- parity mode now uses a normalized-space deadband/gain/hold model tuned from observed EigenLite breath output
- current tuning:
  - deadband `0.015`
  - gain `4.0`
  - hold `100` ticks
- live Bitwig test indicates this is now materially more usable than the earlier parity attempt
- Windows debug now reaches a healthy `CC2` range (observed up to roughly `0x63`) and decays cleanly to `0`

Tasks:

- compare `stable` vs `parity` using the same breath gesture in Windows debug (`B0 02 xx`) and in real Bitwig mappings
- confirm no oversensitivity or harsh onset in longer play sessions
- document the parity breath model once it stabilizes

Acceptance:

- parity breath is musically usable in Bitwig
- parity breath feels more instrument-like than stable mode
- parity breath no longer requires absurd effort or excessive dead range
- baseline acceptance reached; continue validating under longer use

### Phase 2: Key Parity

Status: started, needs live validation

Current state:

- parity mode has a first-pass debounce / estimation / gating path
- not yet validated against real playing
- breath is now good enough that key feel can be evaluated without breath being the dominant blocker

Tasks:

- verify parity key behavior under fast repeated playing
- compare note attacks and false retriggers against stable mode
- confirm no sluggish onset regression
- decide whether key thresholds need normalized-space retuning
- test both soft and hard attacks with the same Bitwig instrument chain used for stable-mode evaluation

Acceptance:

- parity keys do not retrigger spuriously
- parity keys remain responsive
- parity note-on behavior is meaningfully closer to original Pico feel

### Phase 3: Ribbon Parity

Status: not started

Current state:

- parity ribbon currently aliases stable behavior

Tasks:

- design MIDI exposure for both absolute and relative ribbon semantics
- test whether Bitwig can use relative ribbon cleanly
- keep absolute ribbon stable and predictable
- decide whether relative ribbon is default parity behavior or optional extra mapping

Acceptance:

- parity ribbon adds expressive value beyond stable mode
- absolute ribbon remains easy to map
- relative ribbon is useful, not just theoretically faithful

### Phase 4: Default Decision

Status: pending

Tasks:

- compare `stable` and `parity` in real musical use
- document tradeoffs clearly
- decide whether `parity` should replace `stable` as default or remain opt-in

Acceptance:

- decision is based on playability and robustness, not theory alone

## Open Questions

1. Should strict parity be pursued from EigenLite normalized callbacks only?
2. Do we need raw Pico sensor access in a lower layer to achieve real breath parity?
3. Should relative ribbon output use a dedicated CC or a different MIDI representation?
4. Do Bitwig controller-learn limitations argue for a Bitwig-specific controller script later?

## Next Concrete Work

1. Validate current parity key behavior in live playing.
2. Compare stable vs parity note onset feel on the same Bitwig instruments.
3. Decide whether parity key thresholds should stay close to EigenD-derived intent or be retuned to EigenLite normalized output.
4. Write a parity validation checklist covering:
   - notes
   - breath
   - ribbon
   - Bitwig mapping behavior
