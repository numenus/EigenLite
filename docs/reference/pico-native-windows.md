# Pico -> Bitwig, Native Windows 11 (no WSL)

Status: **live-validated 2026-07-11** — enumeration, firmware self-load, iso
input, and key play into loopMIDI all work natively (over the libusb-win32
driver; WinUSB/libusbK untested but expected fine). Still pending live checks:
LED control path from Bitwig, parity mode feel, cold-replug auto-reconnect,
latency comparison. The WSL chain (`pico-wsl-bitwig.md`) remains as fallback.

Two cross-platform EigenLite bugs were found and fixed during bring-up (both
latent on Linux, fatal on Windows): an uninitialised `std::atomic_flag`
deadlocking USB discovery, and post-firmware rediscovery matching the stale
pre-load device name. Candidates for upstreaming to TheTechnobear/EigenLite.

## Architecture

```
Pico USB (WinUSB or libusbK driver, installed once via Zadig)
  -> pico-udp-midi-bridge.exe    (native i686; EigenLite + libusb-1.0 + pico decoder DLL)
  -> UDP 127.0.0.1:5005
  -> udp_midi_receiver.py        (unchanged from the WSL flow)
  -> loopMIDI "Pico In" -> Bitwig
Bitwig -> loopMIDI "Pico Out" -> receiver sink -> UDP 127.0.0.1:5006 -> exe -> LEDs
```

Same MIDI mapping, modes, and LED protocol as the WSL bridge
(`pico-bitwig-midi.md`) — identical code (`tools/pico_midi_bridge_core.h`),
different transport underneath. What disappears vs WSL: usbipd bind/attach
(and its admin shell), WSL boot ordering, the Windows-side `pico_loader.py`
firmware step (EigenLite loads embedded firmware itself over libusb), and the
USB-over-IP jitter behind most `poll_pipe frame out of order` spam.

## Why 32-bit

The only Windows build of the closed-source pico decoder is
`pico_decoder_1_0_0.dll` (PE32 i386, from `resources/picodecoder/unsupported/`,
staged at `resources/picodecoder/windows/x86/`). The exe must match, so the
whole build targets i686. Runs fine under WOW64 on Windows 11 x64.

## Build (from WSL/Linux)

One-time: `sudo apt install g++-mingw-w64-i686-posix mingw-w64-tools`

```
cmake --preset win-native
cmake --build build-win-native -j8
```

Output: `build-win-native/release/bin/pico-udp-midi-bridge.exe` (statically
linked except the decoder DLL). Vendored deps: `external/libusb-win-i686/`
(static libusb 1.0.26 for MinGW i686, from MSYS2), decoder import lib generated
at configure time from `resources/picodecoder/windows/x86/pico_decoder.def`.

## One-time Windows setup

1. **Driver (Zadig)**: install WinUSB on BOTH Pico USB identities:
   - `2139:0001` (pre-firmware; plug in cold to see it)
   - `2139:0101` (post-firmware)
   If isochronous input is broken/stuttery with WinUSB, switch the driver to
   **libusbK** in Zadig — libusb picks the right backend automatically.
2. **Unbind usbipd** (if previously bound for WSL): `usbipd unbind --busid <id>`
   — usbipd's stub driver and Zadig's driver fight over the device.
3. **VC++ 2008 SP1 x86 redistributable** — the decoder DLL links MSVCR90.dll.
4. loopMIDI ports "Pico In" / "Pico Out" as before.
5. Copy to a Windows folder: `pico-udp-midi-bridge.exe`,
   `resources/picodecoder/windows/x86/pico_decoder_1_0_0.dll`,
   `tools/pico-native.ps1`, `tools/udp_midi_receiver.py`.

## Run

```
powershell -ExecutionPolicy Bypass -File pico-native.ps1 `
    -BitwigInputPort "Pico In" -BitwigOutputPort "Pico Out"
```

Flags: `-Mode parity`, `-DeviceFilter pico|base|all` (default `pico` -- pass
`all` if you also have an Alpha/Tau basestation; pico-only skips the slow
basestation USB scans), `-DebugBridge`, `-DebugScope gates|controls|all`,
`-DebugReceiver`, `-SinkDebug`, `-SkipSink`, `-SkipLedForward`,
`-BridgeExe <path>`, `-UdpPort`/`-LedPort`. Receiver opens in its own window; the bridge runs in
the invoking window (Ctrl+C to stop).

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| exe won't start: missing MSVCR90.dll | install VC++ 2008 SP1 x86 redist |
| exe won't start: missing pico_decoder_1_0_0.dll | copy DLL next to exe |
| no device found | wrong driver on active PID — check both PIDs in Zadig; unbind usbipd |
| device found, no data / iso errors | switch WinUSB -> libusbK in Zadig |
| firmware never loads from cold plug | pre-load PID lacks WinUSB driver; or fall back to `pico_loader.py` then rerun |
| notes work, LEDs don't | receiver started without `--forward-host/--forward-port` (check `-SkipLedForward` not set) |

## Validation log

- 2026-07-11: first full native session. Confirmed working: enumeration
  (libusb-win32/libusb0 driver), firmware self-load from pre-load state
  (`2139:0001` -> `2139:0101`, no `pico_loader.py`), iso input pipes, key
  events to loopMIDI. Not yet exercised live: Bitwig LED control path, parity
  mode, cold-replug auto-reconnect (code path fixed but untested), latency
  vs the WSL chain.
- 2026-07-12..14: LED control path (Bitwig HW Instrument ch16), replug
  self-heal via relaunch loop, parity velocity + relative roll, startup 16s
  (shared libusb context + pico-only device filter) -- all validated live.
- 2026-07-15: stuck-note fix validated (watchdog + bridge-local monotonic
  timestamps; lost key-ups from decoder resyncs self-heal in ~250ms). Rapid
  sustained play: no stuck notes, no watchdog fires. Only remaining item:
  subjective latency comparison vs the WSL chain. (Struck by user 2026-07-15: responsiveness validated in play; task closed, octave switching added.)
- 2026-07-16: overnight device drop exposed missing death detection — bridge
  hot-spun resubmitting failed URBs all night (8.5M `LIBUSB_TRANSFER_ERROR`),
  degrading the whole machine. Fixed: 100 consecutive failed completions now
  declare the device dead and route through normal teardown + rescan; ps1
  relaunch loop recovers. If the pico's LEDs cycle on their own after such an
  event, the device itself is wedged — power-cycle its USB port.
