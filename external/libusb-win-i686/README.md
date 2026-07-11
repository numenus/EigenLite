# libusb-1.0 prebuilt, Windows i686 (MinGW)

Static `libusb-1.0.a` + header for the native Windows cross build
(`cmake --preset win-native`). 32-bit to match the only Windows build of the
closed-source pico decoder (see `resources/picodecoder/windows/x86/`).

- Source: MSYS2 package `mingw-w64-i686-libusb-1.0.26-1-any.pkg.tar.zst`
  (https://mirror.msys2.org/mingw/mingw32/), extracted verbatim.
- libusb 1.0.26 is the final i686 build MSYS2 published (32-bit repo frozen).
  Good enough: isochronous transfers over WinUSB have been supported since
  libusb 1.0.24; hotplug is not needed (EigenLite polls
  `libusb_get_device_list`).
- License: LGPL-2.1 (`COPYING` alongside). Statically linked into a
  personal-use tool built from this source tree, which satisfies LGPL.
