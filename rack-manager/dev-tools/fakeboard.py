#!/usr/bin/env python3
"""
Pretend to be the rack-monitor's ADC stream.

Dials a collector, sends the same handshake the firmware sends, then pushes
real-time packets carrying plausible signals: 50 Hz on the two wall channels,
DC with ripple on the rails. For developing the GUI without hardware, and for
checking the collector's decode against a waveform whose answer is known.

  usage: fakeboard.py [host] [port] [--seconds N]

Wire format is firmware/src/wire.h.
"""

import argparse
import json
import math
import random
import socket
import struct
import time

MAGIC_HELLO = b"HELO"
MAGIC_ADC = b"ADC0"
FRAMES_PER_PACKET = 96
NCH = 5

# The handshake the real board sends, as captured from /stream/hello.
HELLO = {
    "stream": "adc",
    "session": random.getrandbits(32),
    "format": {
        "encoding": "int24_le", "frame_stride": 15, "channels_per_frame": 5,
        "ring_frames": 4800, "clkin_hz": 4000000, "vref_v": 1.2,
    },
    "adcs": [
        {"adc": 1, "online": True, "osr": 64, "chop": True, "gc_delay": 3,
         "channels": [{"channel": 0, "gain": 1, "offset": 0.0},
                      {"channel": 1, "gain": 1, "offset": 0.0}]},
        {"adc": 2, "online": True, "osr": 64, "chop": True, "gc_delay": 3,
         "channels": [{"channel": 0, "gain": 1, "offset": 0.0},
                      {"channel": 1, "gain": 32, "offset": 0.0}]},
        {"adc": 3, "online": True, "osr": 64, "chop": True, "gc_delay": 3,
         "channels": [{"channel": 0, "gain": 1, "offset": 0.0},
                      {"channel": 1, "gain": 1, "offset": 0.0}]},
    ],
}

SPS = 1.0 / (8e-6 + 3 * 64 * 0.5e-6)     # 9615.4, same derivation as the server


def counts(volts_at_pin, gain=1):
    return int(volts_at_pin / (1.2 / gain / 8388608.0))


def frame(n):
    """One conversion instant. Amplitudes chosen so each channel lands where
    design.md says its front end puts it."""
    t = n / SPS
    w = 2 * math.pi * 50.0 * t

    wall_i = counts(0.55 * math.sin(w) + 0.004 * random.gauss(0, 1))
    wall_v = counts(0.98 * math.sin(w + 0.12) + 0.004 * random.gauss(0, 1))
    # 12 V through the 11.5k/1k divider, with 100 Hz buck ripple
    rail_v = counts(12.05 / 12.5 + 0.0016 * math.sin(2 * w) + 3e-5 * random.gauss(0, 1))
    # ~4.2 A through the 1 mOhm shunt, on the gain-32 PGA
    rail_i = counts(4.2 * 0.001 + 4e-5 * math.sin(2 * w), gain=32)
    bus_v = counts(25.4 / 27.0 + 2e-5 * random.gauss(0, 1))

    return (wall_i, wall_v, rail_v, rail_i, bus_v)


def pack_i24le(v):
    v = max(-8388608, min(8388607, v))
    u = v & 0xFFFFFF
    return bytes((u & 0xFF, (u >> 8) & 0xFF, (u >> 16) & 0xFF))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("port", nargs="?", type=int, default=9000)
    ap.add_argument("--seconds", type=float, default=0)
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    blob = json.dumps(HELLO).encode()
    s.sendall(MAGIC_HELLO + struct.pack("<I", len(blob)) + blob)
    print(f"connected to {args.host}:{args.port}, session {HELLO['session']:08X}",
          flush=True)

    idx = 0
    t0 = time.monotonic()
    packets = 0
    try:
        while True:
            body = bytearray()
            for _ in range(FRAMES_PER_PACKET):
                for v in frame(idx):
                    body += pack_i24le(v)
                idx += 1

            hdr = MAGIC_ADC + struct.pack("<QHBB", idx - FRAMES_PER_PACKET,
                                          FRAMES_PER_PACKET, 0x1F, 0)
            s.sendall(hdr + bytes(body))
            packets += 1

            # Pace against absolute time so the stream does not drift.
            due = t0 + idx / SPS
            slack = due - time.monotonic()
            if slack > 0:
                time.sleep(slack)

            if args.seconds and time.monotonic() - t0 >= args.seconds:
                break
    except (BrokenPipeError, ConnectionResetError):
        print("collector went away", flush=True)
    finally:
        el = time.monotonic() - t0
        print(f"sent {packets} packets, {idx} frames in {el:.1f}s "
              f"({idx/el:.1f} SPS)", flush=True)
        s.close()


if __name__ == "__main__":
    main()
