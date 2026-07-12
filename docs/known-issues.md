# Known Issues and Technical Debt

Active limitations and deferred work. Sourced from code analysis and `docs/archive/devnotes.md`.

---

## Native Bridge: Unplug Crashes the Process (Recovered by Relaunch Loop)

Live-observed 2026-07-12 (native Windows): unplugging the Pico while the
bridge runs eventually crashes the process — a teardown race in the
EigenD-era USB code that survived cleanup reordering and flood throttling.
Accepted rather than fixed: `pico-native.ps1` relaunches the bridge on
abnormal exit (2s delay, 3-strike fast-failure abort), so replug =
crash -> auto relaunch -> firmware load -> reconnected, no manual steps.
**Validated live 2026-07-12.** In-process teardown race remains open if
anyone ever wants true crash-free unplug.

## WSL-Flow Features Not Live-Tested (now legacy path)

The WSL chain is superseded by the native bridge
(`docs/reference/pico-native-windows.md`); these were never live-tested and
remain so: `pico-online.ps1 -StartWsl`, `-Watch`, and WSL-targeted LED
forwarding. The LED control channel itself IS validated (natively,
2026-07-12): Bitwig HW Instrument ch16 -> `udp_midi_receiver.py` sink
forward -> bridge -> key LEDs.

---

## Normalisation Violations

### Pico Breath Not Clamped After Gain (FINDING-1) — Fixed
- Spec (`technical-requirements.md`): breath must be clamped to [-1, 1].
- Root cause was not the 1.4x gain itself — `breathToFloat()` in `ef_harp.h`
  already `clip()`s that. It was the warm-up zero-offset subtraction in
  `ef_pico.cpp`'s `kbd_breath` (`fv - breathZero_`), applied *after* that clip,
  which could push the result back outside [-1, 1].
- Observed in captures: min -1.195.
- Fix applied: wrap the subtraction in `clip(...)` too (`ef_pico.cpp`).
- The checked-in `PICO_*.elcf` fixture still contains pre-fix captured values
  (the replay harness replays stored floats verbatim, doesn't recompute them),
  so `ApiContractTest.cpp`'s `PicoSession1.BreathNormalisedValues` keeps a
  [-1.3, 1.3] tolerance for that fixture specifically (same pattern as
  FINDING-2). Regenerate the PICO capture on real hardware to tighten it to
  [-1, 1].

---

## Unimplemented Hardware Features

### Headphone Support (Alpha/Tau)
- The `alpha2::active_t` API has full headphone control: `headphone_enable`, `headphone_gain`, `headphone_limit`.
- EigenLite never calls these. Not exposed in public API.
- Deferred because EigenLite was originally designed for non-audio-rate usage.
- Risk: unknown whether audio clock initialisation is correct; untested.

### Microphone Support (Alpha)
- `alpha2::active_t` has mic API: `mic_enable`, `mic_gain`, `mic_type`, `kbd_mic` delegate callback.
- Not exposed. No test hardware (Alpha microphone) available.
- `EF_BaseDelegate::kbd_mic` is a no-op stub.

---

## Simplified Hysteresis

EigenD has configurable windows for axis rounding, stepping, and gain per sensor. EigenLite uses fixed constants:
- Breath: ±0.01
- Strip: ±0.01
- Pedal: ±0.01

These values were chosen based on practical observation, not a strict port of EigenD logic. May need tuning for specific use cases.

---

## Strip: Absolute Values Only

EigenLite core exposes only the absolute strip position. EigenD (at module level) also exposes relative (delta from touch origin). Computing relative is left to the application.

`tools/pico_udp_midi_bridge.cpp` now does this at the application level for the Bitwig bridge (`parity` mode, `CC22`) — see `docs/reference/pico-bitwig-midi.md#relative-ribbon-parity-mode-only`. Other EigenLite consumers still have to compute it themselves.

---

## Pico Key-Press LEDs

On Tau/Alpha, the basestation firmware automatically lights a key orange when pressed (non-configurable at the firmware level). The Pico does not do this. EigenLite core does not compensate.

EigenD's pico module implements this in software. EigenLite's `eigenapi/` could do the same but has not — `tools/pico_udp_midi_bridge.cpp` implements it at the bridge-tool level instead (`LedState`, see `docs/reference/pico-bitwig-midi.md#led-feedback`), so it's specific to the Bitwig bridge, not available to other EigenLite consumers.

---

## Auto-Connect Model

EigenLite automatically connects to any discovered Eigenharp (subject to filter). There is no "inform app of available devices, let app decide to connect" API.

This means:
- The filter (`setDeviceFilter`) is the only control mechanism.
- Multi-device scenarios are handled by enumeration order, not application choice.

A future inversion (app-initiated connection) would require careful handling of the basestation's firmware loading and reconnection logic.

---

## EigenD Source Drift

The three directories copied from EigenD (`lib_alpha2/`, `lib_pico/`, `picross/`) can drift from the EigenD 3.0 branch over time. There is no automated tooling to compare or sync them.

Known historical policy: changes to shared code should be made in EigenD first, then copied here. No patches have been applied that would block this.

---

## Compiler Warnings in Driver Code

The EigenD-derived driver code produces compiler warnings (deprecated declarations, etc.). These are suppressed with `-Wno-deprecated-declarations` on macOS rather than fixed, to minimise divergence from EigenD source.

---

## discoverProcessRun Global

`discoverProcessRun` in `eigenlite.cpp` is a `volatile bool` global, not a member of `EigenLite`. This means creating multiple `EigenLite` instances simultaneously would be unsafe (last writer wins on create/destroy).

Not a current concern — typical usage is one instance. The global was introduced in the July 2019 "new reconnecting api" commit (28b4d20) as the simplest way to control the background discovery thread when reconnection was first added. It has not been revisited since. — see `ai/work/260411-discover-thread-cleanup.md`
