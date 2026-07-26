#!/usr/bin/env python3
"""
Minimal collector for the rack-manager raw ADC stream.

Terminates one TCP connection, reads the JSON handshake, then decodes packets
and reports what it sees. It stores nothing: this is for exercising the wire
format and the board's timing on the bench, not for production.

  usage: collector.py [--port 9000] [--bind 0.0.0.0] [--seconds N]

Wire format is firmware/src/wire.h.
"""

import argparse
import json
import socket
import struct
import sys
import time

import numpy as np

MAGIC_HELLO = b"HELO"
MAGIC_ADC = b"ADC0"

HEADER_LEN = 16
FRAME_STRIDE = 15
CHANNELS = 5

FLAG_GAP = 1 << 0
FLAG_CRC_ERR = 1 << 1
FLAG_RESYNC = 1 << 2


def recv_exact(sock, n):
    """TCP is a byte stream: a read can return half a packet or several. The
    only framing is the length we computed from the header."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def decode_frames(payload, n_frames):
    """int24 LE -> int32, with the same expression the firmware uses:
    codes = (u ^ 0x800000) - 0x800000"""
    raw = np.frombuffer(payload, dtype=np.uint8).reshape(n_frames, CHANNELS, 3)
    u = (
        raw[..., 0].astype(np.int32)
        | raw[..., 1].astype(np.int32) << 8
        | raw[..., 2].astype(np.int32) << 16
    )
    return (u ^ 0x800000) - 0x800000


def counts_to_volts(codes, hello):
    """Volts at the ADC pin. The board reports only how it is configured, so
    the scale is derived here: FSR = +/-vref / gain over a +/-2^23 span. What
    is wired to each input is not the board's business, so nothing further
    (dividers, shunt, CT/PT) is applied."""
    vref = hello["format"]["vref_v"]
    gains = []
    for adc in hello["adcs"]:
        for ch in adc["channels"]:
            gains.append((ch["gain"], ch["offset"]))
    gains = gains[:CHANNELS]  # ADC3 ch1 is not sent

    out = np.empty(codes.shape, dtype=np.float64)
    for i, (gain, offset) in enumerate(gains):
        out[:, i] = (codes[:, i] - offset) * (vref / gain) / 8388608.0
    return out


def sample_rate(hello):
    """Derived, not sent: tGC = tGC_DLY + 3 x OSR x tMOD, tMOD = 2/CLKIN.
    The GC_DLY code maps to 2^code conversion periods, with code 3 -> the
    8 us at OSR 64 that system-design/rack-manager.md quotes."""
    fmt = hello["format"]
    adc = hello["adcs"][0]
    t_mod = 2.0 / fmt["clkin_hz"]
    if not adc["chop"]:
        return 1.0 / (adc["osr"] * t_mod)
    t_dly = (1 << adc["gc_delay"]) * 2 * t_mod
    return 1.0 / (t_dly + 3 * adc["osr"] * t_mod)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--seconds", type=float, default=0, help="0 = run forever")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(1)
    print(f"listening on {args.bind}:{args.port}", flush=True)

    conn, peer = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"connected from {peer[0]}:{peer[1]}", flush=True)

    head = recv_exact(conn, 8)
    if head is None or head[:4] != MAGIC_HELLO:
        sys.exit(f"expected {MAGIC_HELLO!r} handshake, got {head!r}")

    (json_len,) = struct.unpack_from("<I", head, 4)
    hello = json.loads(recv_exact(conn, json_len))
    print(json.dumps(hello, indent=2), flush=True)

    sps = sample_rate(hello)
    print(f"\nderived sample rate: {sps:.1f} SPS", flush=True)
    if hello["format"]["frame_stride"] != FRAME_STRIDE:
        sys.exit("frame stride is not what this decoder expects")

    packets = frames = gaps = crc = 0
    lost = 0
    expect_idx = None
    peak = np.zeros(CHANNELS, dtype=np.int64)
    t0 = time.monotonic()
    last_report = t0

    while True:
        hdr = recv_exact(conn, HEADER_LEN)
        if hdr is None:
            print("\nboard closed the connection", flush=True)
            break
        if hdr[:4] != MAGIC_ADC:
            sys.exit(f"bad magic {hdr[:4]!r} - layout version mismatch")

        first_idx, n_frames, ch_mask, flags = struct.unpack_from("<QHBB", hdr, 4)
        payload = recv_exact(conn, n_frames * FRAME_STRIDE)
        if payload is None:
            print("\ntruncated packet", flush=True)
            break

        if expect_idx is not None and first_idx != expect_idx:
            lost += first_idx - expect_idx
            if not flags & FLAG_GAP:
                print(f"!! index jump {expect_idx}->{first_idx} without GAP flag",
                      flush=True)
        expect_idx = first_idx + n_frames

        codes = decode_frames(payload, n_frames)
        peak = np.maximum(peak, np.abs(codes).max(axis=0))

        packets += 1
        frames += n_frames
        gaps += bool(flags & FLAG_GAP)
        crc += bool(flags & FLAG_CRC_ERR)

        now = time.monotonic()
        if now - last_report >= 2.0:
            el = now - t0
            volts = counts_to_volts(codes, hello)
            print(
                f"[{el:6.1f}s] {packets:6d} pkt  {frames:9d} frames  "
                f"{frames/el:7.1f} SPS  {packets*(HEADER_LEN+n_frames*FRAME_STRIDE)/el/1024:6.1f} KiB/s  "
                f"gaps={gaps} lost={lost} crc={crc}  "
                f"ch_mask=0x{ch_mask:02X}  last={np.array2string(volts[-1], precision=4, floatmode='fixed')}",
                flush=True,
            )
            last_report = now

        if args.seconds and now - t0 >= args.seconds:
            break

    el = time.monotonic() - t0
    print(f"\n--- {packets} packets, {frames} frames in {el:.2f}s ---")
    print(f"mean rate      : {frames/el:.1f} SPS (derived {sps:.1f})")
    print(f"mean payload   : {frames*FRAME_STRIDE/el/1024:.1f} KiB/s")
    print(f"gap packets    : {gaps}   frames lost: {lost}")
    print(f"crc-flag pkts  : {crc}")
    print(f"peak |code| per channel: {peak.tolist()}  (full scale 8388608)")
    conn.close()


if __name__ == "__main__":
    main()
