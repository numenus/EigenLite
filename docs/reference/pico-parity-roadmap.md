# Pico Parity Roadmap

Roadmap for moving from the current practical EigenLite-to-MIDI bridge toward a
more faithful reconstruction of the original EigenD Pico controller behavior.

This document is not about preserving raw hardware values for their own sake. It
is about recovering the expressive controller semantics that made the Pico feel
like an instrument rather than a bundle of generic sensors.

See also:

- [pico-bitwig-midi.md](/home/hotpo/repos/EigenLite/docs/reference/pico-bitwig-midi.md:1)
- [pico-wsl-bitwig.md](/home/hotpo/repos/EigenLite/docs/reference/pico-wsl-bitwig.md:1)
- [eigend-sync.md](/home/hotpo/repos/EigenLite/docs/reference/eigend-sync.md:1)

## Why Parity Matters

EigenLite intentionally chose a simpler abstraction boundary than EigenD:

- device data is normalized early into floats
- some richer controller semantics are simplified or omitted
- some behavior is explicitly left to the application

That design is defensible for a lightweight embeddable API.

It is not ideal if the goal is:

- recover the original Pico playing feel
- preserve the original controller behavior
- make Bitwig feel like it is hosting an expressive instrument rather than a raw MIDI adapter

## Current State

The current `stable` bridge mode is a working Bitwig-oriented controller mapping:

- note on/off
- poly aftertouch
- breath on `CC2`
- ribbon on `CC21`
- practical fixes for note retrigger, controller spam, and ambiguous strip mapping

This is useful and should remain available.

It is not full parity with EigenD's Pico module.

## What EigenD Adds Beyond Current EigenLite Bridging

### 1. Breath Controller Semantics

EigenD Pico breath logic operates in raw sensor space and includes:

- warm-up zero calibration
- positive and negative thresholds around the zero point
- separate positive and negative gain calculations
- activity gating and hold behavior
- blocked breath-pipe detection

#### Practical Bitwig Benefit

- easier expressive control with less accidental movement
- better dynamic feel
- less need for crude gain boosting
- more stable modulation around rest

#### Proposed MIDI Exposure

- keep the main musical breath path on `CC2`
- optionally expose raw/diagnostic breath on a separate CC in `parity` debug mode only

#### Risk

- EigenLite callbacks already provide normalized values, not raw ADC values
- strict parity requires either:
  - reconstructing an equivalent controller-space model from normalized values, or
  - exposing raw Pico breath data through a lower layer

### 2. Key Gesture Semantics

EigenD Pico key handling includes:

- debounce
- short attack estimation window
- pressure-based gating thresholds
- staged activation behavior

#### Practical Bitwig Benefit

- more intentional attacks
- fewer false retriggers
- more instrument-like response under fast or messy technique
- velocity that better reflects the original Pico playing model

#### Proposed MIDI Exposure

- notes remain standard MIDI note on/off
- velocity derived from parity attack estimation
- poly aftertouch remains the ongoing pressure path

#### Risk

- some EigenD semantics were designed around its own event/routing model, not plain MIDI
- parity work here must preserve musical intent without making note triggering feel sluggish

### 3. Ribbon / Strip Semantics

EigenD Pico strip logic exposes both:

- relative movement from touch origin
- absolute strip position

EigenLite currently exposes only absolute strip position.

#### Practical Bitwig Benefit

- absolute ribbon is useful for position-like control
- relative ribbon is better for vibrato, nudging, pitch gestures, and motion around a touch point
- both together make the ribbon more expressive and less “one generic slider”

#### Proposed MIDI Exposure — Implemented

- `CC21` = absolute ribbon (both modes)
- `CC22` = relative ribbon delta, `parity` mode only

`CC21` stays the stable-compatible default; `CC22` is additive and only
present in `parity`. See `docs/reference/pico-bitwig-midi.md#relative-ribbon-parity-mode-only`.
Live-feel validation (does it actually help with vibrato/nudge gestures in
Bitwig) is still open — see Phase 3 below.

#### Risk

- relative strip is not a universal MIDI convention
- Bitwig mapping behavior may be better for absolute than relative values unless routed through modulators or script logic

### 4. Instrument-Like Statefulness

The original EigenD Pico layer is not simply sensor-to-value conversion. It has
stateful gesture interpretation.

That includes:

- onset vs sustain distinctions
- calibrated rest state
- touch origin
- activity hold/release behavior

#### Practical Bitwig Benefit

- more coherent feel across breath, keys, and ribbon
- less compensation needed in Bitwig mappings
- better alignment with the Pico’s original design intent

## Proposed Runtime Modes

### `stable`

Purpose:

- known-good Bitwig controller mode
- pragmatic mapping
- low risk

### `parity`

Purpose:

- reconstruct EigenD-style controller semantics
- recover the original expressive feel as much as possible
- may expose additional controller data where useful

## Recommended Implementation Order

### Phase 1: Breath Parity

Goal:

- replace blunt gain shaping with EigenD-inspired threshold/gain/hold behavior

Why first:

- breath is central to expressiveness
- easy to evaluate in Bitwig immediately
- isolated enough to iterate quickly

Success criteria:

- breath is easier to control than raw EigenLite output
- no large dead zones that make normal playing feel unresponsive
- Bitwig modulation feels instrument-like, not hacked

Current status:

- first normalized-space parity tuning is working well in live Bitwig testing
- current parity breath tuning is no longer blocked on raw-EigenD threshold literalism
- remaining work is validation and possible fine tuning, not basic rescue

### Phase 2: Key Parity

Goal:

- port debounce / estimation / gating behavior in a musically defensible way

Why second:

- key onset quality strongly affects playability
- it interacts with all instruments, not just mappings

Success criteria:

- no repeated accidental attacks
- no sluggish feel
- attack/velocity feel more intentional than `stable`

### Phase 3: Ribbon Parity

Status: implemented, needs live validation

Goal:

- add relative ribbon semantics while preserving absolute ribbon utility

Why third:

- ribbon already works acceptably in `stable`
- parity here is about expressive expansion, not basic recovery

Current state:

- `CC22` relative delta implemented in `ParityMidiBridgeImplementation::on_strip`,
  origin captured on touch-start, centred at `CC 64`
- not yet evaluated in real Bitwig play (vibrato/nudge gestures)

Success criteria:

- absolute ribbon still maps cleanly
- relative ribbon enables useful gestural control in Bitwig

### Phase 4: Evaluation and Default Decision

Goal:

- compare `stable` vs `parity` in real playing

Decision questions:

- Is `parity` more expressive?
- Is it still stable?
- Does it require special Bitwig handling?
- Should `parity` become default, or remain an advanced mode?

## When the Lift Is Worth It

Parity work is worth it if the goal is:

- recover the Pico as an expressive instrument
- preserve the original design intent
- leverage Bitwig as a host for instrument-like control, not just raw MIDI

Parity work is probably not worth it if the goal is only:

- basic note input
- a generic ribbon + breath controller
- minimal maintenance burden

## Current Recommendation

- keep `stable` available as the operational baseline
- continue building `parity` deliberately
- evaluate each parity phase in Bitwig before proceeding to the next

This avoids replacing a working bridge with theory while still moving toward a
more faithful and expressive controller model.
