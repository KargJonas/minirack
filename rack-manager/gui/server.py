#!/usr/bin/env python3
"""
Rack manager GUI backend.

Three jobs in one process:

  1. terminate the rack-monitor's raw ADC stream (TCP, see firmware/src/wire.h)
  2. decimate 9615 SPS down to ~10 Hz by block-averaging
  3. serve the GUI, push blocks to it, and hold the analog front-end config

Stdlib + numpy only. The browser link is Server-Sent Events rather than a
WebSocket: the flow is one-directional, SSE needs no handshake, no framing and
no dependency, and the browser reconnects on its own.

  usage: server.py [--stream-port 9000] [--http-port 8080] [--bind 0.0.0.0]

No sample data is stored. This is a liveness and plausibility check, not the
collector. The front-end config *is* persisted, to config.json.
"""

import argparse
import asyncio
import copy
import json
import mimetypes
import struct
import time
from pathlib import Path

import numpy as np

import channels as chan

HERE = Path(__file__).parent
STATIC = HERE / "static"
CONFIG_PATH = HERE / "config.json"

MAGIC_HELLO = b"HELO"
MAGIC_ADC = b"ADC0"
HEADER_LEN = 16
FRAME_STRIDE = 15
NCH = chan.NCH

FLAG_GAP = 1 << 0
FLAG_CRC_ERR = 1 << 1

TARGET_HZ = 10.0        # blocks per second pushed to the GUI
WINDOW_SECONDS = 60     # the GUI's rolling window
DEFAULT_CLKIN_HZ = 4_000_000


# --------------------------------------------------------------------------
# front-end configuration
# --------------------------------------------------------------------------

def load_config():
    """Saved config merged over the defaults, so a field added to channels.py
    appears on an existing install instead of being missing."""
    cfg = copy.deepcopy(chan.DEFAULTS)
    if CONFIG_PATH.is_file():
        try:
            saved = json.loads(CONFIG_PATH.read_text())
            for entry in saved:
                i = entry.get("idx")
                if isinstance(i, int) and 0 <= i < NCH:
                    cfg[i].update(entry)
        except Exception as e:
            print(f"[config] ignoring {CONFIG_PATH.name}: {e!r}")
    return cfg


def save_config(cfg):
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n")


def validate_channel(incoming, current):
    """Coerce one channel's submitted config against the field schema.

    Returns (config, errors). Anything that does not parse is reported rather
    than silently dropped - a mistyped shunt value should not quietly become a
    plausible-looking graph.
    """
    errors = {}
    out = copy.deepcopy(current)

    ftype = incoming.get("type", current["type"])
    if ftype not in chan.FRONTEND_TYPES:
        return current, {"type": f"unknown front-end type {ftype!r}"}
    out["type"] = ftype

    def coerce(spec, raw, into, prefix=""):
        key = spec["key"]
        kind = spec.get("type", "number")
        if raw is None:
            return
        if kind == "text":
            into[key] = str(raw).strip()
        elif kind == "bool":
            into[key] = bool(raw)
        else:
            try:
                v = float(raw)
            except (TypeError, ValueError):
                errors[prefix + key] = "not a number"
                return
            if v != v or v in (float("inf"), float("-inf")):
                errors[prefix + key] = "not finite"
                return
            lo = spec.get("min")
            if lo is not None and v < lo:
                errors[prefix + key] = f"must be at least {lo:g}"
                return
            into[key] = v

    for spec in chan.COMMON_FIELDS:
        coerce(spec, incoming.get(spec["key"]), out)

    params = dict(current.get("params", {})) if ftype == current["type"] else {}
    submitted = incoming.get("params", {}) or {}
    for spec in chan.FRONTEND_TYPES[ftype]["fields"]:
        raw = submitted.get(spec["key"], params.get(spec["key"], spec.get("default")))
        coerce(spec, raw, params, prefix="params.")
    out["params"] = params

    if not errors:
        try:
            chan.channel_scale(out, STATE.clkin_hz)
        except Exception as e:
            errors["type"] = f"scale cannot be computed: {e}"

    return (current if errors else out), errors


# --------------------------------------------------------------------------
# shared state
# --------------------------------------------------------------------------

class State:
    def __init__(self):
        self.hello = None
        self.sps = 0.0
        self.block_frames = 0
        self.connected = False
        self.peer = ""
        self.packets = 0
        self.frames = 0
        self.gaps = 0
        self.lost = 0
        self.crc = 0
        self.seq = 0
        self.since = 0.0
        self.token = 0            # identifies the current board connection
        self.writer = None
        self.config = load_config()
        self.subscribers: set[asyncio.Queue] = set()

    @property
    def clkin_hz(self):
        if self.hello:
            return self.hello["format"]["clkin_hz"]
        return DEFAULT_CLKIN_HZ

    def scales(self):
        return [chan.channel_scale(c, self.clkin_hz) for c in self.config]

    def status(self):
        el = time.monotonic() - self.since if self.since else 0.0
        return {
            "type": "status",
            "connected": self.connected,
            "peer": self.peer,
            "session": (self.hello or {}).get("session"),
            "sps_nominal": round(self.sps, 1),
            "sps_actual": round(self.frames / el, 1) if el > 0.5 else None,
            "block_frames": self.block_frames,
            "packets": self.packets,
            "frames": self.frames,
            "gaps": self.gaps,
            "lost": self.lost,
            "crc": self.crc,
            "uptime_s": round(el, 1),
        }

    def channel_meta(self):
        out = []
        for c in self.config:
            try:
                scale = chan.channel_scale(c, self.clkin_hz)
            except Exception:
                scale = float("nan")
            out.append({
                "idx": c["idx"], "adc": c["adc"], "ch": c["ch"],
                "name": c["name"],
                "type": c["type"],
                "type_label": chan.FRONTEND_TYPES[c["type"]]["label"],
                "unit": chan.channel_unit(c),
                "scale": scale,
                "params": c.get("params", {}),
                "zero_offset_v": c.get("zero_offset_v", 0.0),
                "gain_correction": c.get("gain_correction", 1.0),
                "r_thevenin": c.get("r_thevenin", 0.0),
                "invert": bool(c.get("invert")),
                "note": c.get("note", ""),
            })
        return out

    def publish(self, msg):
        """Fan out to GUI clients. A slow browser is dropped from, never
        blocked on - the same rule the board applies to us."""
        dead = []
        for q in self.subscribers:
            try:
                q.put_nowait(msg)
            except asyncio.QueueFull:
                try:
                    q.get_nowait()          # drop oldest
                    q.put_nowait(msg)
                except Exception:
                    dead.append(q)
        for q in dead:
            self.subscribers.discard(q)


STATE = State()


# --------------------------------------------------------------------------
# wire format (firmware/src/wire.h)
# --------------------------------------------------------------------------

def decode_frames(payload: bytes, n_frames: int) -> np.ndarray:
    """int24 LE two's complement -> int32, (n_frames, 5).

    Same expression as i24le_get() in wire.h:  (u ^ 0x800000) - 0x800000
    """
    raw = np.frombuffer(payload, dtype=np.uint8).reshape(n_frames, NCH, 3)
    u = (
        raw[..., 0].astype(np.int32)
        | raw[..., 1].astype(np.int32) << 8
        | raw[..., 2].astype(np.int32) << 16
    )
    return (u ^ 0x800000) - 0x800000


def sample_rate(hello: dict) -> float:
    """Derived, never sent: the board reports how it is clocked and we work the
    rate out from tGC = tGC_DLY + 3 x OSR x tMOD, tMOD = 2/CLKIN. GC_DLY is a
    register code for 2^code conversion periods; code 3 at OSR 64 gives the
    8 us that design.md quotes."""
    fmt = hello["format"]
    adc = hello["adcs"][0]
    t_mod = 2.0 / fmt["clkin_hz"]
    if not adc["chop"]:
        return 1.0 / (adc["osr"] * t_mod)
    t_dly = (1 << adc["gc_delay"]) * 2 * t_mod
    return 1.0 / (t_dly + 3 * adc["osr"] * t_mod)


def pin_volts_per_count(hello: dict) -> np.ndarray:
    """Volts at the ADC pin per count, per frame slot. FSR = +/-vref/gain over
    a +/-2^23 span."""
    vref = hello["format"]["vref_v"]
    per = []
    for adc in hello["adcs"]:
        for ch in adc["channels"]:
            per.append(vref / ch["gain"] / 8388608.0)
    return np.array(per[:NCH])


def pin_offsets(hello: dict) -> np.ndarray:
    """Zero calibration in counts, reported by the board and applied here - the
    stream itself stays a faithful record of what the chip said."""
    off = []
    for adc in hello["adcs"]:
        for ch in adc["channels"]:
            off.append(ch["offset"])
    return np.array(off[:NCH])


# --------------------------------------------------------------------------
# the board connection
# --------------------------------------------------------------------------

async def handle_board(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    peer = writer.get_extra_info("peername")
    peer_s = f"{peer[0]}:{peer[1]}" if peer else "?"

    # A reconnecting board is the authority on whether the old socket is dead:
    # it only redials after its own send failed. Rejecting the new connection
    # because the old one has not finished tearing down just locks us out for a
    # backoff cycle, so the newcomer takes over instead.
    if STATE.connected and STATE.writer is not None:
        print(f"[board] {peer_s} takes over from {STATE.peer}", flush=True)
        try:
            STATE.writer.close()
        except Exception:
            pass

    STATE.token += 1
    token = STATE.token
    STATE.writer = writer
    print(f"[board] connected from {peer_s}", flush=True)

    try:
        head = await reader.readexactly(8)
        if head[:4] != MAGIC_HELLO:
            print(f"[board] expected HELO, got {head[:4]!r}")
            return
        (json_len,) = struct.unpack_from("<I", head, 4)
        hello = json.loads(await reader.readexactly(json_len))

        sps = sample_rate(hello)
        block = max(1, round(sps / TARGET_HZ))
        v_per_count = pin_volts_per_count(hello)
        offsets = pin_offsets(hello)

        STATE.hello = hello
        STATE.sps = sps
        STATE.block_frames = block
        STATE.connected = True
        STATE.peer = peer_s
        STATE.packets = STATE.frames = STATE.gaps = STATE.lost = STATE.crc = 0
        STATE.seq = 0
        STATE.since = time.monotonic()
        STATE.publish(STATE.status())

        print(f"[board] session {hello['session']}, {sps:.1f} SPS, "
              f"block {block} frames -> {sps/block:.2f} Hz", flush=True)

        pending = np.empty((0, NCH), dtype=np.int32)
        expect_idx = None

        while True:
            hdr = await reader.readexactly(HEADER_LEN)
            if hdr[:4] != MAGIC_ADC:
                print(f"[board] bad magic {hdr[:4]!r} - layout version mismatch")
                break

            first_idx, n_frames, ch_mask, flags = struct.unpack_from("<QHBB", hdr, 4)
            payload = await reader.readexactly(n_frames * FRAME_STRIDE)

            if expect_idx is not None and first_idx != expect_idx:
                STATE.lost += first_idx - expect_idx
            expect_idx = first_idx + n_frames

            STATE.packets += 1
            STATE.frames += n_frames
            STATE.gaps += bool(flags & FLAG_GAP)
            STATE.crc += bool(flags & FLAG_CRC_ERR)

            pending = np.concatenate([pending, decode_frames(payload, n_frames)])

            # The DSP, such as it is: average each block of `block` frames down
            # to one point. min/max come along because a mean alone cannot show
            # that an AC channel is swinging - see the note in emit().
            while len(pending) >= block:
                emit(pending[:block], v_per_count, offsets, ch_mask)
                pending = pending[block:]

    except (asyncio.IncompleteReadError, ConnectionResetError):
        print(f"[board] {peer_s} disconnected", flush=True)
    except Exception as e:
        print(f"[board] {peer_s} error: {e!r}", flush=True)
    finally:
        # Only clear state if this connection is still the current one; a
        # newer one may already have taken over.
        if STATE.token == token:
            STATE.connected = False
            STATE.peer = ""
            STATE.writer = None
            STATE.publish(STATE.status())
        try:
            writer.close()
        except Exception:
            pass


def emit(block: np.ndarray, v_per_count, offsets, ch_mask):
    """One GUI sample from one block of frames.

    The requested DSP is the windowed average, and `mean` is exactly that. rms
    ships alongside it because on the two wall channels the average of a
    symmetric AC waveform is ~0 by construction - the mean line would sit flat
    at zero on a perfectly healthy mains input, which is the opposite of a
    plausibility check. rms is what shows those channels are alive.

    Scales are read per block rather than cached at connect, so editing the
    front-end config takes effect on the next frame instead of the next reboot.
    """
    codes = block.astype(np.float64) - offsets
    volts = codes * v_per_count                       # at the ADC pin

    cfg = STATE.config
    zero = np.array([c.get("zero_offset_v", 0.0) or 0.0 for c in cfg])
    volts = volts - zero

    try:
        scale = np.array(STATE.scales())
    except Exception:
        scale = np.ones(NCH)

    mean = volts.mean(axis=0) * scale
    rms = np.sqrt((volts ** 2).mean(axis=0)) * np.abs(scale)
    a = volts.min(axis=0) * scale
    b = volts.max(axis=0) * scale
    lo = np.minimum(a, b)      # an inverted channel swaps them
    hi = np.maximum(a, b)

    out = [
        {
            "mean": float(mean[i]), "rms": float(rms[i]),
            "min": float(lo[i]), "max": float(hi[i]),
            "ok": bool(ch_mask & (1 << i)),
        }
        for i in range(NCH)
    ]

    STATE.seq += 1
    STATE.publish({"type": "block", "seq": STATE.seq, "t": time.time(), "ch": out})


# --------------------------------------------------------------------------
# HTTP + SSE
# --------------------------------------------------------------------------

async def handle_http(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    try:
        line = await asyncio.wait_for(reader.readline(), timeout=15)
        if not line:
            return
        parts = line.decode("latin1").split()
        if len(parts) < 2:
            return
        method, path = parts[0], parts[1].split("?")[0]

        length = 0
        while True:
            h = await reader.readline()
            if h in (b"\r\n", b"\n", b""):
                break
            name, _, value = h.decode("latin1").partition(":")
            if name.strip().lower() == "content-length":
                try:
                    length = int(value.strip())
                except ValueError:
                    length = 0

        body = await reader.readexactly(length) if length > 0 else b""

        if method == "GET" and path == "/events":
            await serve_events(writer)
        elif method == "GET" and path == "/api/meta":
            await send_json(writer, 200, {
                "channels": STATE.channel_meta(),
                "frontend_types": chan.FRONTEND_TYPES,
                "common_fields": chan.COMMON_FIELDS,
                "hello": STATE.hello,
                "window_seconds": WINDOW_SECONDS,
                "target_hz": TARGET_HZ,
            })
        elif method == "POST" and path == "/api/config":
            await post_config(writer, body)
        elif method == "GET":
            await serve_static(writer, path)
        else:
            await send_json(writer, 405, {"error": "method not allowed"})
    except (asyncio.TimeoutError, asyncio.IncompleteReadError,
            ConnectionResetError, BrokenPipeError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def post_config(writer, body: bytes):
    try:
        req = json.loads(body or b"{}")
    except json.JSONDecodeError as e:
        await send_json(writer, 400, {"error": f"bad JSON: {e}"})
        return

    idx = req.get("idx")
    if not isinstance(idx, int) or not (0 <= idx < NCH):
        await send_json(writer, 400, {"error": "idx out of range"})
        return

    updated, errors = validate_channel(req.get("config", {}), STATE.config[idx])
    if errors:
        await send_json(writer, 422, {"error": "invalid config", "fields": errors})
        return

    STATE.config[idx] = updated
    try:
        save_config(STATE.config)
    except OSError as e:
        await send_json(writer, 500, {"error": f"could not save: {e}"})
        return

    print(f"[config] channel {idx} ({updated['name']}) updated -> "
          f"{chan.channel_scale(updated, STATE.clkin_hz):.6g} "
          f"{chan.channel_unit(updated)}/V", flush=True)
    await send_json(writer, 200, {"channels": STATE.channel_meta()})


async def send_json(writer, code, obj):
    await send_simple(writer, code, "application/json",
                      json.dumps(obj, allow_nan=True).encode())


async def send_simple(writer, code, ctype, body: bytes):
    writer.write(
        f"HTTP/1.1 {code} OK\r\nContent-Type: {ctype}\r\n"
        f"Content-Length: {len(body)}\r\nCache-Control: no-store\r\n"
        f"Connection: close\r\n\r\n".encode()
        + body
    )
    await writer.drain()


async def serve_static(writer, path):
    name = "index.html" if path == "/" else path.lstrip("/")
    target = (STATIC / name).resolve()
    if not str(target).startswith(str(STATIC.resolve())) or not target.is_file():
        await send_simple(writer, 404, "text/plain", b"not found\n")
        return
    ctype = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
    if target.suffix in (".js", ".mjs"):
        ctype = "text/javascript"
    await send_simple(writer, 200, ctype, target.read_bytes())


async def serve_events(writer):
    q: asyncio.Queue = asyncio.Queue(maxsize=40)
    STATE.subscribers.add(q)
    writer.write(
        b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        b"Cache-Control: no-store\r\nConnection: keep-alive\r\n"
        b"X-Accel-Buffering: no\r\n\r\n"
    )
    await writer.drain()

    try:
        writer.write(f"data: {json.dumps(STATE.status())}\n\n".encode())
        await writer.drain()
        while True:
            try:
                msg = await asyncio.wait_for(q.get(), timeout=5.0)
            except asyncio.TimeoutError:
                writer.write(b": keepalive\n\n")   # keeps proxies from idling us out
                await writer.drain()
                continue
            writer.write(f"data: {json.dumps(msg)}\n\n".encode())
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError, asyncio.CancelledError):
        pass
    finally:
        STATE.subscribers.discard(q)


async def status_ticker():
    while True:
        await asyncio.sleep(1.0)
        STATE.publish(STATE.status())


# --------------------------------------------------------------------------

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stream-port", type=int, default=9000)
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    board = await asyncio.start_server(handle_board, args.bind, args.stream_port)
    http = await asyncio.start_server(handle_http, args.bind, args.http_port)

    print(f"stream  : {args.bind}:{args.stream_port}  (point the board here)")
    print(f"gui     : http://localhost:{args.http_port}/")
    print(f"config  : {CONFIG_PATH}", flush=True)

    async with board, http:
        await asyncio.gather(board.serve_forever(), http.serve_forever(),
                             status_ticker())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
