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

## Open

- [ ] live: Bitwig LED control path, parity mode, cold-replug auto-reconnect
      (fixed code path untested), latency vs WSL chain
- [ ] consider upstreaming the two fixes
- [ ] roadmap cross-ref + close-out once remaining live checks pass
