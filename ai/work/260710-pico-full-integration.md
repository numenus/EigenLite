<!-- created: 2026-07-10 refs: docs/reference/pico-bitwig-midi.md, docs/reference/pico-parity-roadmap.md, ai/work/260703-pico-parity-implementation.md -->

# Pico Full Integration & Polish

## Goal

Turn `pico-udp-midi-bridge` from a partially-mapped, manual-bring-up bridge
into a functionally complete, resilient Pico-to-Bitwig (and generic MIDI)
integration. Branch: `feature/pico-bitwig-bridge`.

## Phase 1: correctness + mapping completeness — done

- fixed FINDING-1 breath clamp bug (root cause: zero-offset subtraction after
  the gain clip, in `ef_pico.cpp`, not the gain itself)
- mapped the 4 mode buttons (were producing zero MIDI): notes 44-47, ch1
- added parity-mode-only roll CC74

## Phase 2: bidirectional LED feedback — done, not live-tested

- auto orange-on-press with base-colour restore on release (`LedState`)
- Bitwig -> Pico LED control channel: ch16 Note On, note=index, vel=colour
- Windows sink forwards to WSL UDP 5006 instead of discarding
- `pico-online.ps1` auto-resolves WSL IP, wires forwarding by default

## Phase 3: parity mode — implemented, live validation still open

- relative ribbon (`CC22`, parity only) implemented and unit-tested
- key-gating live-feel validation and the stable-vs-parity default decision
  still need real Bitwig play sessions — not something codeable/testable here

## Phase 4: resilience + one-command bring-up — done, not live-tested

- `pico-online.ps1 -StartWsl` launches the WSL bridge from the same invocation
- `-Watch` polls USB state, auto re-arms (firmware reload + usbipd attach) on
  a detected unplug/replug; running WSL bridge reconnects on its own
- decided against a bridge-binary config file: `pico-online-wsl.sh` /
  `pico-online.ps1` already provide the named-flag ergonomics; nothing calls
  the raw positional argv except those scripts

## Phase 5: bridge unit tests + docs cleanup — done

- extracted `MidiBridgeImplementation`/`LedState`/`MidiSink` into
  `tools/pico_midi_bridge_core.h`, decoupled from live sockets/hardware
- `tests/PicoMidiBridgeTest.cpp`: 27 new tests, byte-exact MIDI assertions for
  stable/parity notes, buttons, breath, ribbon (abs+relative), roll CC, and
  LED state transitions (press overlay, deferred base-colour, protocol parsing)
- `docs/known-issues.md`, `docs/roadmap.md`, `docs/reference/pico-parity-*.md`
  updated to reflect what's implemented, what's bridge-tool-only vs
  `eigenapi/`-level, and what still needs a live session

## What Still Needs a Human With Hardware

Everything below requires actually plugging in the Pico and playing —
nothing further to do here without that:

- Phase 2/4 Windows-only code paths (LED forwarding, `-StartWsl`, `-Watch`)
  are unit-testable up to the MIDI-mapping/LED-state layer but the
  Windows-specific glue (`ctypes`/`winmm`, `usbipd`, WMI-adjacent polling)
  cannot run in this environment
- parity key-gating feel (Phase 3) and the stable-vs-parity default decision
- relative ribbon (CC22) usefulness for vibrato/nudge gestures in Bitwig
- `docs/reference/pico-parity-checklist.md` end to end, including its new
  mode-button and LED sections

## Cross-references

- `docs/reference/pico-bitwig-midi.md` — authoritative mapping (now includes
  buttons, roll, LED, relative ribbon)
- `docs/reference/pico-wsl-bitwig.md` — runbook, now with the one-command flow
- `docs/reference/pico-parity-roadmap.md` / `pico-parity-checklist.md` —
  parity-specific detail
- `ai/work/260703-pico-parity-implementation.md` — prior parity-only task,
  superseded in scope by this one
