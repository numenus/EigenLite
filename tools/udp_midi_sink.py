#!/usr/bin/env python3
"""Open a Windows MIDI input port and discard incoming messages."""

import argparse
import ctypes
from ctypes import wintypes
import signal
import sys
import time

if hasattr(wintypes, "ULONG_PTR"):
    UINT_PTR = wintypes.ULONG_PTR
    DWORD_PTR = wintypes.ULONG_PTR
else:
    UINT_PTR = ctypes.c_size_t
    DWORD_PTR = ctypes.c_size_t


MAXPNAMELEN = 32
CALLBACK_FUNCTION = 0x00030000
MIM_DATA = 0x3C3


class MIDIINCAPSW(ctypes.Structure):
    _fields_ = [
        ("wMid", wintypes.WORD),
        ("wPid", wintypes.WORD),
        ("vDriverVersion", wintypes.UINT),
        ("szPname", wintypes.WCHAR * MAXPNAMELEN),
        ("dwSupport", wintypes.DWORD),
    ]


winmm = ctypes.WinDLL("winmm")

winmm.midiInGetNumDevs.restype = wintypes.UINT
winmm.midiInGetDevCapsW.argtypes = [UINT_PTR, ctypes.POINTER(MIDIINCAPSW), wintypes.UINT]
winmm.midiInGetDevCapsW.restype = wintypes.UINT
winmm.midiInOpen.argtypes = [ctypes.POINTER(wintypes.HANDLE), wintypes.UINT, DWORD_PTR, DWORD_PTR, wintypes.DWORD]
winmm.midiInOpen.restype = wintypes.UINT
winmm.midiInStart.argtypes = [wintypes.HANDLE]
winmm.midiInStart.restype = wintypes.UINT
winmm.midiInStop.argtypes = [wintypes.HANDLE]
winmm.midiInStop.restype = wintypes.UINT
winmm.midiInReset.argtypes = [wintypes.HANDLE]
winmm.midiInReset.restype = wintypes.UINT
winmm.midiInClose.argtypes = [wintypes.HANDLE]
winmm.midiInClose.restype = wintypes.UINT


def list_ports():
    ports = []
    count = winmm.midiInGetNumDevs()
    for idx in range(count):
        caps = MIDIINCAPSW()
        rc = winmm.midiInGetDevCapsW(idx, ctypes.byref(caps), ctypes.sizeof(caps))
        if rc == 0:
            ports.append((idx, caps.szPname))
    return ports


def find_port(name_substring):
    needle = name_substring.lower()
    for idx, name in list_ports():
        if needle in name.lower():
            return idx, name
    return None, None


def decode_midi(word):
    return word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF


def main():
    parser = argparse.ArgumentParser(description="Open a Windows MIDI input port and discard incoming messages.")
    parser.add_argument("--in", dest="port_name", required=True, help="substring of the MIDI input port name")
    parser.add_argument("--debug", action="store_true", help="print incoming MIDI packets")
    args = parser.parse_args()

    port_id, resolved_name = find_port(args.port_name)
    if port_id is None:
        print("Available MIDI input ports:", file=sys.stderr)
        for idx, name in list_ports():
            print(f"  [{idx}] {name}", file=sys.stderr)
        print(f"ERROR: no input port matched {args.port_name!r}", file=sys.stderr)
        return 1

    keep_running = True

    def stop_handler(_sig, _frame):
        nonlocal keep_running
        keep_running = False

    signal.signal(signal.SIGINT, stop_handler)

    @ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR)
    def midi_in_proc(_handle, msg, _instance, param1, _param2):
        if msg != MIM_DATA or not args.debug:
            return
        status, data1, data2 = decode_midi(param1)
        print(f"midi {status:02X} {data1:02X} {data2:02X}")

    handle = wintypes.HANDLE()
    rc = winmm.midiInOpen(ctypes.byref(handle), port_id, ctypes.cast(midi_in_proc, ctypes.c_void_p).value, 0, CALLBACK_FUNCTION)
    if rc != 0:
        print(f"ERROR: midiInOpen failed with code {rc}", file=sys.stderr)
        return 1

    try:
        rc = winmm.midiInStart(handle)
        if rc != 0:
            print(f"ERROR: midiInStart failed with code {rc}", file=sys.stderr)
            return 1

        print(f"Discarding MIDI input from [{port_id}] {resolved_name}")
        while keep_running:
            time.sleep(0.25)
        return 0
    finally:
        winmm.midiInStop(handle)
        winmm.midiInReset(handle)
        winmm.midiInClose(handle)


if __name__ == "__main__":
    raise SystemExit(main())
