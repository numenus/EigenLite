# Eigenharp Pico on Windows, WSL, and Bitwig

> **Legacy/fallback path.** As of 2026-07-11 the native Windows bridge
> (`pico-native-windows.md`) runs the Pico without WSL, usbipd, or an admin
> shell, and is the preferred daily flow. Keep this runbook for fallback and
> for the usbipd-specific troubleshooting knowledge.

Practical runbook for using an Eigenharp Pico that:

- powers up in pre-load mode on Windows
- requires manual firmware loading
- is readable via EigenLite in WSL
- is bridged into Bitwig through a Windows MIDI loopback port

This document is based on a real recovery/debugging session and is intended to be followed by a tired human later.

---

## TL;DR

If this worked before and you just want the least-pain repeatable path:

1. Plug in the Pico.
2. In Windows PowerShell, run:

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\pico-online.ps1" -BitwigInputPort "Pico In" -BitwigOutputPort "Pico Out" -StartWsl -Watch
```

`-StartWsl` launches the WSL bridge (`pico-online-wsl.sh`) itself in a second
window, so this one command replaces the old two-step (Windows script + WSL
script) bring-up. `-Watch` keeps this window running afterward and
automatically re-arms the Pico (firmware reload + `usbipd` re-attach) if it
detects a real unplug/replug, without needing the whole sequence re-run by
hand -- the already-running WSL bridge reconnects on its own once EigenLite's
discovery thread sees the device again.

Without `-StartWsl`/`-Watch` (the original step-by-step flow still works):

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\pico-online.ps1" -BitwigInputPort "Pico In" -BitwigOutputPort "Pico Out"
```

This loads firmware, tries to attach the operational device to WSL, and starts one Windows MIDI host process that:

- forwards Pico UDP MIDI into the Bitwig input loopMIDI port
- optionally opens the separate Bitwig output loopMIDI port and forwards Bitwig's outbound MIDI to WSL as the LED control channel (see `docs/reference/pico-bitwig-midi.md#led-feedback`)
- automatically stops stale `udp_midi_receiver.py` processes before relaunching
- tears both handles down together when that PowerShell window exits

3. In WSL, run:

```bash
/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh
```

This auto-detects the Windows-side WSL host IP, builds the bridge if needed, and starts `pico-udp-midi-bridge`.

4. In Bitwig, use your chosen input loopMIDI port on an armed instrument track, and if Bitwig wants a controller output port, point it at the separate output loopMIDI port.

If `usbipd attach` needs elevation, rerun the Windows PowerShell as Administrator or pass `-BusId <BUSID>` explicitly.

**Not yet live-tested end to end** (`-StartWsl`, `-Watch`, and the LED
forwarding path were written and code-reviewed but need a real Windows +
Bitwig + Pico session to confirm) -- see `docs/known-issues.md`.

---

## ELI5

The Pico is not showing up to Bitwig by itself.

The working chain is:

```text
Pico
-> Windows firmware loader
-> WSL EigenLite
-> UDP packets from WSL to Windows
-> Windows MIDI output port (loopMIDI)
-> Bitwig
```

Bitwig only sees the last piece: the loopMIDI port.

---

## What Was Learned

### Device states

The Pico was observed in these states:

- pre-load / bootloader: `VID_2139&PID_0001`
- operational after firmware load: `VID_2139&PID_0101`

The generic `USB Audio Device` seen in Windows was unrelated.

### Firmware persistence

For this device/setup, firmware did **not** survive a true unplug/replug.

That means:

- you must load firmware after each real power cycle / reconnect
- software should assume the Pico comes back as `2139:0001`

### Windows-native host status

The local `MEC` and `EigenLite` trees were **not** usable as-is for a straightforward native Windows Eigenharp pipeline:

- top-level Windows builds disable Eigenharp/libusb support
- the only Windows Pico decoder binary in-tree is an old unsupported 32-bit DLL

So the practical route was:

- Windows for firmware loading and final MIDI destination
- WSL/Linux for actual Pico USB event handling

---

## One-Time Setup

### Windows

Install/prepare:

- Python 3 for Windows
- Zadig/libusb driver on the Pico pre-load device
- `usbipd-win`
- `loopMIDI`

Optional but useful:

- firewall rule for UDP port `5005`

Example:

```powershell
New-NetFirewallRule -DisplayName "Pico UDP 5005" -Direction Inbound -Protocol UDP -LocalPort 5005 -Action Allow
```

### WSL / Ubuntu

Install:

```bash
sudo apt update
sudo apt install -y build-essential cmake libusb-1.0-0-dev usbutils
```

### Build EigenLite

From the `EigenLite` repo:

```bash
cd /home/hotpo/repos/EigenLite
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DYNAMIC=ON
cmake --build build -j
```

Important: `USE_DYNAMIC=ON` is needed because the repo contains `libpicodecoder.so` but not the Linux static archive.

---

## Files Added or Changed During Recovery

### Firmware loader

- [pico_loader.py](/home/hotpo/repos/EigenLite/tools/pico_loader.py:1)
- [pico_loader.README.md](/home/hotpo/repos/EigenLite/tools/pico_loader.README.md:1)

Purpose:

- load Pico firmware over USB from Windows
- detect pre-load and post-load USB IDs

### Bitwig bridge

- [pico_udp_midi_bridge.cpp](/home/hotpo/repos/EigenLite/tools/pico_udp_midi_bridge.cpp:1)
- [udp_midi_receiver.py](/home/hotpo/repos/EigenLite/tools/udp_midi_receiver.py:1)
- [udp_midi_sink.py](/home/hotpo/repos/EigenLite/tools/udp_midi_sink.py:1)
- [pico-online.ps1](/home/hotpo/repos/EigenLite/tools/pico-online.ps1:1)
- [pico-reset.ps1](/home/hotpo/repos/EigenLite/tools/pico-reset.ps1:1)
- [pico-online-wsl.sh](/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh:1)

Purpose:

- WSL side: read Pico events from EigenLite and emit UDP MIDI packets
- Windows side: receive UDP MIDI packets and send them to a Windows MIDI output port
- Windows side: open a second MIDI input port and discard Bitwig's outbound stream so input and output can use separate loopMIDI ports
- Windows reset helper: stop stale receiver/loader processes and optionally restart loopMIDI / shut down WSL
- launcher scripts: compress the normal bring-up sequence into one Windows step and one WSL step

### Build fixes made in EigenLite

- [replay_harness.h](/home/hotpo/repos/EigenLite/eigenapi/src/replay_harness.h:1): added `<cstdint>`
- [fwr_embedded.cpp](/home/hotpo/repos/EigenLite/eigenapi/src/fwr_embedded.cpp:1): added `<cstring>`
- [eigenapi/CMakeLists.txt](/home/hotpo/repos/EigenLite/eigenapi/CMakeLists.txt:133): fixed `USE_DYNAMIC` handling for `libpicodecoder.so`
- [CMakeLists.txt](/home/hotpo/repos/EigenLite/CMakeLists.txt:141): added `pico-udp-midi-bridge` target

---

## Windows Firmware Loading

### Copy the loader and firmware to Windows

If the repo lives only in WSL, copy the files you need:

```powershell
Copy-Item "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\pico_loader.py" "C:\Users\hotpo\Desktop\pico_loader.py" -Force
Copy-Item "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenD\resources\pico.ihx" "C:\Users\hotpo\Desktop\pico.ihx" -Force
```

Adjust the distro name if needed.

### Verify pre-load device visibility

```powershell
python "C:\Users\hotpo\Desktop\pico_loader.py" --list
```

Expected before firmware load:

```text
Pre-load devices:
  VID_2139&PID_0001 ...
```

### Load firmware

```powershell
python "C:\Users\hotpo\Desktop\pico_loader.py" --firmware "C:\Users\hotpo\Desktop\pico.ihx"
```

Expected after a successful load:

- pre-load device disappears
- operational Pico appears as `VID_2139&PID_0101`

If needed, verify again:

```powershell
python "C:\Users\hotpo\Desktop\pico_loader.py" --list
```

### Driver notes

The Windows firmware loader used `pyusb`, so the pre-load device must be bound to a libusb-compatible driver via Zadig.

---

## WSL USB Attach

### Find the device in Windows

In PowerShell:

```powershell
usbipd list
```

Identify the Pico bus ID for the operational device.

### Bind and attach to WSL

As needed:

```powershell
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

### Verify in WSL

```bash
lsusb
```

Expected:

```text
2139:0101
```

---

## Test the Pico in WSL

### Hardware test

```bash
sudo env LD_LIBRARY_PATH=/home/hotpo/repos/EigenLite/resources/picodecoder/linux/x86_64 ./build/release/bin/eigenapitest
```

This should:

- discover the Pico
- open it successfully
- print key/breath/strip activity when touched

### Common failure: permissions

If you see `LIBUSB_ERROR_ACCESS`, run as `sudo`.

### Common failure: busy interface

If you see `LIBUSB_ERROR_BUSY`, some other process already owns the Pico.

Kill anything using it:

```bash
pkill -f eigenapitest
pkill -f picodump
pkill -f pico-udp-midi-bridge
```

Only one of those may run at a time.

---

## WSL to Windows MIDI Bridge

### Fastest repeatable launch

Normal daily bring-up is now:

1. Plug in Pico.
2. Run [pico-online.ps1](/home/hotpo/repos/EigenLite/tools/pico-online.ps1:1) in Windows PowerShell.
3. Run [pico-online-wsl.sh](/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh:1) in WSL.
4. Open Bitwig and use your chosen input loopMIDI port.

Windows launcher examples:

```powershell
.\pico-online.ps1 -BitwigInputPort "Pico In" -BitwigOutputPort "Pico Out"
.\pico-online.ps1 -BitwigInputPort "Pico In" -BitwigOutputPort "Pico Out" -DebugReceiver
.\pico-online.ps1 -BusId 4-4
.\pico-online.ps1 -SkipFirmware
```

Reset helper examples:

```powershell
.\pico-reset.ps1
.\pico-reset.ps1 -ShutdownWsl
.\pico-reset.ps1 -ShutdownWsl -RestartLoopMidi
```

Recommended loopMIDI layout:

- create one port for `Pico In` and point Bitwig's MIDI input at it
- create a second port for `Pico Out` and point Bitwig's MIDI output at it
- let `pico-online.ps1` start the receiver on `Pico In` and the sink on `Pico Out`

WSL launcher examples:

```bash
./tools/pico-online-wsl.sh
./tools/pico-online-wsl.sh --debug
./tools/pico-online-wsl.sh --mode stable
./tools/pico-online-wsl.sh --mode parity
./tools/pico-online-wsl.sh --host 172.17.80.1
```

### Windows receiver

Create the loopMIDI input port you actually want to use, for example `Pico In`.

### Standalone Windows folder

If you run the bridge from a copied standalone Windows folder instead of directly
from the repo UNC path, that folder must contain:

- `pico-online.ps1`
- `pico-reset.ps1`
- `pico_loader.py`
- `udp_midi_receiver.py`
- `pico.ihx`

Copy the receiver if needed:

```powershell
Copy-Item "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\udp_midi_receiver.py" "C:\Users\hotpo\Desktop\udp_midi_receiver.py" -Force
```

Start it:

```powershell
python "C:\Users\hotpo\Desktop\udp_midi_receiver.py" --out "Pico In" --sink-in "Pico Out"
```

Debug mode:

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\pico-reset.ps1"
py "C:\Users\hotpo\Desktop\udp_midi_receiver.py" --out "Pico In" --sink-in "Pico Out" --port 5005 --debug
```

### Determine the correct Windows-side WSL IP

The bridge must send UDP to the Windows `vEthernet (WSL ...)` adapter, not to `127.0.0.1` and not usually to the normal LAN adapter.

On the machine used during debugging, `ipconfig` showed:

```text
Carte Ethernet vEthernet (WSL (Hyper-V firewall))
Adresse IPv4: 172.17.80.1
```

That address worked.

### Start the WSL sender

```bash
sudo env LD_LIBRARY_PATH=/home/hotpo/repos/EigenLite/resources/picodecoder/linux/x86_64 ./build/release/bin/pico-udp-midi-bridge 172.17.80.1 5005
```

Debug mode:

```bash
sudo env LD_LIBRARY_PATH=/home/hotpo/repos/EigenLite/resources/picodecoder/linux/x86_64 ./build/release/bin/pico-udp-midi-bridge 172.17.80.1 5005 1
```

Debug mode prints local Pico events in WSL, which is useful to distinguish:

- Pico callback path working
- UDP path to Windows working or failing

### Meaning of success

When it works:

- WSL bridge sees Pico events
- Windows receiver prints `midi ..` in debug mode
- Bitwig can receive data from your chosen input loopMIDI port

---

## Bitwig Setup

### Minimal working setup

1. Create an instrument track.
2. Add `Polysynth`.
3. Arm the track.
4. Make sure the track input accepts your chosen input loopMIDI port.
5. Track input channel should be `All` or channel `1`.

The current authoritative Pico-to-MIDI mapping is documented in
[pico-bitwig-midi.md](/home/hotpo/repos/EigenLite/docs/reference/pico-bitwig-midi.md:1).

In short:

- notes on MIDI channel 1
- note pressure as poly aftertouch
- breath as `CC2`
- ribbon as `CC21`

### Things that were not the right path

Not the right place:

- automation lanes
- parameter mapping / MIDI learn for automation

The first target is simply:

- get note data from your chosen input loopMIDI port into an armed instrument track

---

## Shutdown / Restart Sequence

### Quick daily use

1. Plug in the Pico.
2. Run `pico-online.ps1` with `-BitwigInputPort` and `-BitwigOutputPort`.
3. Run `pico-online-wsl.sh`.
4. Open Bitwig and use your chosen input loopMIDI port.

### After unplug/replug

Repeat from step 1.

The Pico should be assumed to fall back to pre-load mode after a real disconnect.

### Exact post-reboot startup

After a full system reboot, use this sequence:

1. Plug in the Pico.
2. Confirm the two loopMIDI ports exist in Windows:
   - `Pico In`
   - `Pico Out`
3. Optional but useful if the last session ended badly: reset the Windows side first:

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\pico-reset.ps1" -ShutdownWsl -RestartLoopMidi
```

If you use a copied standalone Windows folder instead, run the local copy of
`pico-reset.ps1`.

4. In Windows PowerShell, start the Windows side:

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\pico-online.ps1" -BitwigInputPort "Pico In" -BitwigOutputPort "Pico Out"
```

5. Wait for the script to finish firmware load and `usbipd` attach.
6. In WSL, start the bridge:

```bash
/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh
```

7. Open Bitwig.
8. Use `Pico In` as the Pico MIDI input port.
9. If Bitwig asks for a separate controller output port, use `Pico Out`.

If the Windows script fails to attach USB:

- rerun the Windows PowerShell as Administrator
- or pass `-BusId <BUSID>` manually after checking `usbipd list`

If you need debug after reboot:

1. Stop the WSL bridge:

```bash
pkill -f pico-udp-midi-bridge
```

2. Restart it in WSL debug mode:

```bash
/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh --debug
```

For focused parity key debugging instead of raw sensor flood:

```bash
/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh --debug --debug-scope gates --mode parity
```

For breath/ribbon only:

```bash
/home/hotpo/repos/EigenLite/tools/pico-online-wsl.sh --debug --debug-scope controls --mode parity
```

3. Stop the Windows receiver:

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\pico-reset.ps1"
```

4. Run the Windows receiver manually in debug:

```powershell
py "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\udp_midi_receiver.py" --out "Pico In" --sink-in "Pico Out" --port 5005 --debug
```

For filtered MIDI debug instead of full packet spam:

```powershell
py "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\udp_midi_receiver.py" --out "Pico In" --sink-in "Pico Out" --port 5005 --debug --debug-filter notes
py "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\udp_midi_receiver.py" --out "Pico In" --sink-in "Pico Out" --port 5005 --debug --debug-filter breath
py "\\wsl.localhost\Ubuntu-26.04\home\hotpo\repos\EigenLite\tools\udp_midi_receiver.py" --out "Pico In" --sink-in "Pico Out" --port 5005 --debug --debug-filter ribbon
```

---

## Troubleshooting

### `python-rtmidi` failed to install on Windows

That dependency was removed from the receiver.

The receiver now uses the built-in Windows WinMM API through `ctypes`.

### `Ctrl-C` would not stop the Windows receiver

The receiver was updated to poll the UDP socket with a timeout instead of blocking forever in `recvfrom()`.

Closing the PowerShell window is also acceptable.

### WSL bridge shows Pico events, but Windows receiver prints nothing

Suspects:

- wrong Windows target IP
- Windows firewall blocking UDP 5005

Check:

- receiver started with `--debug`
- bridge target IP is the `vEthernet (WSL ...)` IPv4 address
- inbound UDP rule exists for port `5005`

### Windows receiver prints MIDI, but Bitwig does nothing

Then the problem is Bitwig-side only.

Check:

- your chosen input loopMIDI port exists and is selected
- track is armed
- track monitor is on if needed
- input channel is `All` or `1`
- test with a simple instrument like `Polysynth`

### `LIBUSB_ERROR_BUSY`

Another Pico process owns the interface.

Kill other tools:

```bash
pkill -f eigenapitest
pkill -f picodump
pkill -f pico-udp-midi-bridge
```

### `LIBUSB_ERROR_ACCESS`

WSL permission issue. Run the Pico-side tool with `sudo`.

### No `2139:0101` after firmware load

Possible causes:

- firmware load failed
- wrong Zadig driver on the pre-load device
- the device re-enumerated but Windows bound it differently and the loader cannot see it

Start with:

```powershell
python "C:\Users\hotpo\Desktop\pico_loader.py" --list
```

---

## Suggested Future Cleanup

If this setup becomes permanent, worthwhile next steps are:

- reduce or tune the MIDI mapping
- move from simple note/CC output to a proper MPE mapping if desired
- make Bitwig-specific setup notes or presets
