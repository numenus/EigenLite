#!/usr/bin/env python3
"""Receive UDP MIDI, forward it to a Windows MIDI output, and optionally sink a MIDI input port."""

import argparse
import ctypes
from ctypes import wintypes
import socket
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


class MIDIOUTCAPSW(ctypes.Structure):
    _fields_ = [
        ("wMid", wintypes.WORD),
        ("wPid", wintypes.WORD),
        ("vDriverVersion", wintypes.UINT),
        ("szPname", wintypes.WCHAR * MAXPNAMELEN),
        ("wTechnology", wintypes.WORD),
        ("wVoices", wintypes.WORD),
        ("wNotes", wintypes.WORD),
        ("wChannelMask", wintypes.WORD),
        ("dwSupport", wintypes.DWORD),
    ]


class MIDIINCAPSW(ctypes.Structure):
    _fields_ = [
        ("wMid", wintypes.WORD),
        ("wPid", wintypes.WORD),
        ("vDriverVersion", wintypes.UINT),
        ("szPname", wintypes.WCHAR * MAXPNAMELEN),
        ("dwSupport", wintypes.DWORD),
    ]


winmm = ctypes.WinDLL("winmm")

winmm.midiOutGetNumDevs.restype = wintypes.UINT
winmm.midiOutGetDevCapsW.argtypes = [UINT_PTR, ctypes.POINTER(MIDIOUTCAPSW), wintypes.UINT]
winmm.midiOutGetDevCapsW.restype = wintypes.UINT
winmm.midiOutOpen.argtypes = [ctypes.POINTER(wintypes.HANDLE), wintypes.UINT, DWORD_PTR, DWORD_PTR, wintypes.DWORD]
winmm.midiOutOpen.restype = wintypes.UINT
winmm.midiOutShortMsg.argtypes = [wintypes.HANDLE, wintypes.DWORD]
winmm.midiOutShortMsg.restype = wintypes.UINT
winmm.midiOutClose.argtypes = [wintypes.HANDLE]
winmm.midiOutClose.restype = wintypes.UINT
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


def list_output_ports():
    ports = []
    count = winmm.midiOutGetNumDevs()
    for idx in range(count):
        caps = MIDIOUTCAPSW()
        rc = winmm.midiOutGetDevCapsW(idx, ctypes.byref(caps), ctypes.sizeof(caps))
        if rc == 0:
            ports.append((idx, caps.szPname))
    return ports


def list_input_ports():
    ports = []
    count = winmm.midiInGetNumDevs()
    for idx in range(count):
        caps = MIDIINCAPSW()
        rc = winmm.midiInGetDevCapsW(idx, ctypes.byref(caps), ctypes.sizeof(caps))
        if rc == 0:
            ports.append((idx, caps.szPname))
    return ports


def find_port(name_substring, ports):
    needle = name_substring.lower()
    for idx, name in ports:
        if needle in name.lower():
            return idx, name
    return None, None


def midi_word(data):
    return data[0] | (data[1] << 8) | (data[2] << 16)


def decode_midi(word):
    return word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF


def parse_debug_filter(raw):
    if not raw:
        return None
    raw_filters = {part.strip().lower() for part in raw.split(",") if part.strip()}
    expanded = set()
    for item in raw_filters:
        if item == "keys":
            expanded.update({"notes", "pressure"})
        else:
            expanded.add(item)
    return expanded


def classify_midi(status, data1, _data2):
    msg = status & 0xF0
    if msg == 0x80:
        return {"notes", "off"}
    if msg == 0x90:
        return {"notes", "on"} if _data2 != 0 else {"notes", "off"}
    if msg == 0xA0:
        return {"pressure", "poly-pressure"}
    if msg == 0xB0:
        tags = {"cc"}
        if data1 == 2:
            tags.add("breath")
        if data1 == 21:
            tags.add("ribbon")
        return tags
    return {"other"}


def should_print_midi(debug_filter, status, data1, data2):
    if debug_filter is None:
        return True
    return not classify_midi(status, data1, data2).isdisjoint(debug_filter)


class MidiInSink:
    """Opens a Windows MIDI input port (Bitwig's outbound/controller port).

    If forward_host is given, incoming messages are relayed via UDP to the WSL
    bridge's LED control listener instead of being discarded -- this is the
    Bitwig -> Pico LED feedback path. Without forward_host, messages are just
    discarded (original behaviour), optionally printed with debug=True.
    """

    def __init__(self, port_name, debug=False, forward_host=None, forward_port=5006):
        ports = list_input_ports()
        port_id, resolved_name = find_port(port_name, ports)
        if port_id is None:
            lines = ["Available MIDI input ports:"]
            lines.extend(f"  [{idx}] {name}" for idx, name in ports)
            lines.append(f"ERROR: no input port matched {port_name!r}")
            raise RuntimeError("\n".join(lines))

        self._debug = debug
        self._resolved_name = resolved_name
        self._handle = wintypes.HANDLE()
        self._forward_sock = None
        self._forward_addr = None
        if forward_host:
            self._forward_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._forward_addr = (forward_host, forward_port)

        @ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR)
        def midi_in_proc(_handle, msg, _instance, param1, _param2):
            if msg != MIM_DATA:
                return
            status, data1, data2 = decode_midi(param1)
            if self._debug:
                print(f"sink midi {status:02X} {data1:02X} {data2:02X}")
            if self._forward_sock is not None:
                self._forward_sock.sendto(bytes((status, data1, data2)), self._forward_addr)

        self._callback = midi_in_proc
        rc = winmm.midiInOpen(
            ctypes.byref(self._handle),
            port_id,
            ctypes.cast(self._callback, ctypes.c_void_p).value,
            0,
            CALLBACK_FUNCTION,
        )
        if rc != 0:
            raise RuntimeError(f"midiInOpen failed with code {rc}")

        rc = winmm.midiInStart(self._handle)
        if rc != 0:
            winmm.midiInClose(self._handle)
            raise RuntimeError(f"midiInStart failed with code {rc}")

    def close(self):
        if self._forward_sock is not None:
            self._forward_sock.close()
            self._forward_sock = None
        if not self._handle:
            return
        winmm.midiInStop(self._handle)
        winmm.midiInReset(self._handle)
        winmm.midiInClose(self._handle)
        self._handle = None

    @property
    def resolved_name(self):
        return self._resolved_name


def main():
    parser = argparse.ArgumentParser(description="Receive UDP MIDI and forward to a Windows MIDI output port.")
    parser.add_argument("--listen-host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5005)
    parser.add_argument("--out", required=True, help="substring of the MIDI output port name")
    parser.add_argument("--sink-in", default=None, help="optional substring of the MIDI input port name to open and discard")
    parser.add_argument(
        "--forward-host",
        default=None,
        help="WSL IP to forward sink-in MIDI to (Bitwig -> Pico LED control channel). Omit to discard as before.",
    )
    parser.add_argument("--forward-port", type=int, default=5006, help="UDP port the WSL bridge's LED listener is bound to")
    parser.add_argument("--debug", action="store_true", help="print incoming UDP MIDI packets")
    parser.add_argument(
        "--debug-filter",
        default="",
        help="optional comma-separated packet filter: notes,on,off,pressure,cc,breath,ribbon,other",
    )
    parser.add_argument("--sink-debug", action="store_true", help="print MIDI packets arriving on the sink input port")
    args = parser.parse_args()
    if args.debug_filter:
        args.debug = True
    debug_filter = parse_debug_filter(args.debug_filter)

    output_ports = list_output_ports()
    port_id, port_name = find_port(args.out, output_ports)
    if port_id is None:
        print("Available MIDI output ports:", file=sys.stderr)
        for idx, name in output_ports:
            print(f"  [{idx}] {name}", file=sys.stderr)
        print(f"ERROR: no output port matched {args.out!r}", file=sys.stderr)
        return 1

    handle = wintypes.HANDLE()
    rc = winmm.midiOutOpen(ctypes.byref(handle), port_id, 0, 0, 0)
    if rc != 0:
        print(f"ERROR: midiOutOpen failed with code {rc}", file=sys.stderr)
        return 1

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.listen_host, args.port))
    sock.settimeout(0.5)
    print(f"Forwarding UDP MIDI on {args.listen_host}:{args.port} to [{port_id}] {port_name}")
    if args.debug:
        if debug_filter is None:
            print("Debug filter: all")
        else:
            print(f"Debug filter: {','.join(sorted(debug_filter))}")

    sink = None
    try:
        if args.sink_in:
            sink = MidiInSink(
                args.sink_in,
                debug=args.sink_debug,
                forward_host=args.forward_host,
                forward_port=args.forward_port,
            )
            if args.forward_host:
                print(f"Forwarding MIDI input from {sink.resolved_name} to {args.forward_host}:{args.forward_port}")
            else:
                print(f"Discarding MIDI input from {sink.resolved_name} (no --forward-host given)")

        while True:
            try:
                data, _addr = sock.recvfrom(64)
            except socket.timeout:
                time.sleep(0.01)
                continue
            if len(data) != 3:
                continue
            if args.debug and should_print_midi(debug_filter, data[0], data[1], data[2]):
                print(f"midi {data[0]:02X} {data[1]:02X} {data[2]:02X}")
            rc = winmm.midiOutShortMsg(handle, midi_word(data))
            if rc != 0:
                print(f"warning: midiOutShortMsg failed with code {rc}", file=sys.stderr)
    except KeyboardInterrupt:
        return 0
    finally:
        if sink is not None:
            sink.close()
        sock.close()
        winmm.midiOutClose(handle)


if __name__ == "__main__":
    raise SystemExit(main())
