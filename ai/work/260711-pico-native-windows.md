<!-- created: 2026-07-11 refs: docs/reference/pico-native-windows.md, docs/reference/pico-wsl-bitwig.md, ai/work/260710-pico-full-integration.md -->

# Native Windows 11 Pico Bridge (no WSL)

## Goal

Replace the WSL half of the Pico->Bitwig chain with a native 32-bit Windows
exe. Windows-side Python receiver + loopMIDI stay unchanged. Branch:
`feature/pico-bitwig-bridge`.

## Key findings (what made this feasible)

- `picross/src/pic_usb_linux.cpp` is pure libusb-1.0 -> compiles for Windows
  as-is over WinUSB/libusbK (Zadig). Old `pic_usb_win32.cpp` targets EigenD's
  dead kernel driver; not used.
- Only Windows decoder binary is 32-bit (`pico_decoder_1_0_0.dll`, MSVCR90
  dep). Exports exactly the 4 symbols lib_pico needs, cdecl. Whole build i686.
- picross `PI_WINDOWS` branches still alive; `pic_mlock` already stubs
  non-macOS; `pic_tool_*`/`pic_winloop` unused; `eigenapi/src` platform-clean;
  firmware embedded (FWR_Embedded) so the exe self-loads firmware.

## Done

- vendored `external/libusb-win-i686/` (MSYS2 1.0.26 static, i686) + README
- `resources/picodecoder/windows/x86/`: DLL + header + `.def`; implib
  generated at configure time via dlltool
- `cmake/toolchain-mingw-i686.cmake` + `win-native` preset (`-static`,
  posix-flavour MinGW)
- root/eigenapi/external CMake: Windows native path (i686-gated), win source
  list = `pic_thread_win32.cpp` + `pic_usb_linux.cpp`, links
  ws2_32/shell32/ole32; tests + capture TUI excluded on Windows
- `pic_windows.h`: `Ws2tcpip.h` -> lowercase (case-sensitive cross builds)
- `tools/pico_udp_midi_bridge.cpp`: winsock port (socket_t/FIONBIO/WSAStartup
  shim; POSIX path byte-identical behaviour)
- `tools/pico-native.ps1`: receiver + exe bring-up, no usbipd/WSL/admin
- `docs/reference/pico-native-windows.md`
- Linux regression: build + full test suite green after all changes
- cross-compile pass: 5 portability fixes (pic_stdint/pic_config MSVC-era
  guards, LIBUSB_CALL callbacks, pic_microsleep, ef_pico 32-bit abs widen,
  -msse2); exe imports = system DLLs + decoder only
- deployed exe + decoder DLL + pico-native.ps1 to C:\fucking-windows

## Live bring-up findings (2026-07-11)

Two latent cross-platform EigenLite bugs, fatal on Windows, fixed here —
**upstream PR candidates** (TheTechnobear/EigenLite):

1. `eigenlite_impl.h`: `std::atomic_flag usbDevCheckSpinLock` uninitialised;
   pre-C++20 initial state unspecified, starts SET under MinGW -> USB
   discovery deadlocked forever (clear() only runs after successful acquire).
2. `EF_Pico::checkFirmware`: post-firmware re-find matched the pre-load
   device name; libusb names embed product+address, both change on
   re-enumeration -> never matched. Now accepts any post-load pico and
   returns the new name to create().

Diagnosis trail: standalone usb_diag.exe proved libusb+threads fine ->
raw-fprintf instrumentation proved discover thread ran but spinlock never
acquired -> flag init. Validated natively: enumeration, firmware self-load
(no pico_loader.py), iso input over libusb-win32 driver, keys -> loopMIDI.

## Stuck-note round (2026-07-15)

Symptom: notes randomly stuck sounding forever; loopMIDI shows zero traffic
while stuck; one bad session held every note played.

Root cause chain (validated live):
1. USB reader occasionally loses iso frames (`pop_free_queue() stealing
   buffers`) -> forced decoder resync.
2. Closed decoder drops key tracking on resync without emitting a key-up ->
   bridge never sees `active=false` -> stuck MIDI note.
3. Amplifier: heavy console output (debug dumps, later the watchdog's own
   spam) blocks the same loop that drains the USB pipe -> more steals ->
   more lost releases -> feedback loop. Explains the all-notes-stuck session.

Fixes (all in `tools/pico_midi_bridge_core.h` / `pico_udp_midi_bridge.cpp`):
- stuck-note watchdog, both modes: a sounding note whose key is silent
  >`kStuckNoteTimeoutUs` (250ms) while other events flow (breath streams
  constantly) is force-released + key re-armed; `release_all()` flushes
  notes on device disconnect. 5 new unit tests.
- watchdog v1 trusted device timestamps -> endless release/retrigger loop:
  decoder emits garbage ts `0x8000000000000000` (x86 failed float->int64
  sentinel) during resyncs, which latched the max. Now all impl-facing
  events are stamped with bridge-local monotonic time (`mono_now_us()`);
  device ts no longer used for arithmetic (also de-poisons parity debounce).
  **This garbage-timestamp behaviour is a third upstream-relevant finding.**
- `stealing buffers` log throttled (first 3 + every 250th, with count).

Validated 2026-07-15: rapid sustained play across rolls/breath/ribbon -- no
stuck notes, no watchdog fires, steals only a 3-count startup burst (never
reached x250). Considered closed; a `watchdog:` line with sane silent_ms in
normal play is the signal the USB reader needs attention (more read URBs /
more aggressive pipe draining).

## Live validation status (2026-07-12)

- [x] notes/keys/press-lights native (07-11)
- [x] LED control path end to end: Bitwig HW Instrument ch16 -> "Pico Out"
      -> receiver sink -> UDP 5006 -> LEDs. Protocol reworked DAW-native
      (velocity thirds = colour, note-off clears). Gotchas found: Bitwig
      needs restart to bind ports created/freed mid-session; LED track must
      NOT be armed/monitoring (idle pico CC drift loops back out -> loopMIDI
      feedback mute); clip notes must be MIDI 0-17 (3 octaves below
      Bitwig's default draw area)
- [x] replug recovery: in-process teardown race crashes bridge (accepted);
      pico-native.ps1 relaunch loop makes replug self-heal -- validated
- [x] parity feel: velocity range-mapped (attack max pressure), relative
      gated roll (CC74), tuning cheat-sheet in pico-bitwig-midi.md
- [x] stuck notes: watchdog + monotonic timestamps (see 2026-07-15 section)
- [ ] latency vs WSL chain (play-testing only)
- [ ] consider upstreaming: atomic_flag init, checkFirmware rename,
      dead-device cleanup ordering, garbage decoder timestamps on resync
- [ ] roadmap cross-ref + close-out
