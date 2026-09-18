#!/usr/bin/env python3
"""
Rack manager GUI backend.

Four jobs in one process:

  1. find the board on the link and dial its raw ADC stream (firmware/src/wire.h)
  2. terminate that stream
  3. decimate 9615 SPS down to ~10 Hz by block-averaging, and run a Welch
     spectrum over the undecimated samples (spectrum.py)
  4. serve the GUI, push blocks to it, and hold the analog front-end config

Discovery is the same `avahi-browse _easyota._tcp` the firmware's flash.sh
uses, and for the same reason: nothing here holds the board's address, so a
DHCP lease or a rename cannot break the link. The board listens and we dial in
- TCP does not care which end called connect(), and this way the side that has
to be found is the one already advertising itself.

Stdlib + numpy only. The browser link is Server-Sent Events rather than a
WebSocket: the flow is one-directional, SSE needs no handshake, no framing and
no dependency, and the browser reconnects on its own.

  usage: server.py [--board <addr>] [--board-port 9000]
                   [--http-port 8080] [--bind 0.0.0.0]

No sample data is stored. This is a liveness and plausibility check, not the
collector. The front-end config *is* persisted, to config.json.
"""

import argparse
import asyncio
import collections
import copy
import json
import mimetypes
import struct
import time
from pathlib import Path

import numpy as np

import channels as chan
import spectrum as spec

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

SERVICE = "_easyota._tcp"   # what the board advertises
BROWSE_TIMEOUT_S = 10.0
CONNECT_TIMEOUT_S = 5.0
BACKOFF_MIN_S = 1.0
BACKOFF_MAX_S = 30.0


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
        self.writer = None        # the live board connection, if any
        self.spectra: spec.Spectra | None = None   # once the rate is known
        self.spectrum_axis: dict | None = None     # replayed to new clients
        self.history: spec.History | None = None   # the spectrogram's ring
        self.history_seconds = float(WINDOW_SECONDS)   # --history-seconds
        # The series' own ring, replayed alongside the spectrogram's. Without
        # it a reloaded page shows a full spectrogram above an almost empty
        # line, and the two plots share an x axis precisely so that they can be
        # read against each other - which needs both of them to have a past.
        self.blocks: collections.deque = collections.deque(
            maxlen=int(WINDOW_SECONDS * TARGET_HZ) + 4)
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
    """Zero calibration in counts, reported by the board and applied here.
    The stream itself stays a faithful record of what the chip said."""
    off = []
    for adc in hello["adcs"]:
        for ch in adc["channels"]:
            off.append(ch["offset"])
    return np.array(off[:NCH])


# --------------------------------------------------------------------------
# the board connection
# --------------------------------------------------------------------------

async def handle_board(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    """Drain one board connection. Returns True if the handshake landed, which
    is what the caller resets its backoff on - a board that accepts and then
    drops the hello must not be redialled at LAN speed."""
    peer = writer.get_extra_info("peername")
    peer_s = f"{peer[0]}:{peer[1]}" if peer else "?"

    STATE.writer = writer
    got_hello = False
    print(f"[board] connected to {peer_s}", flush=True)

    try:
        head = await reader.readexactly(8)
        if head[:4] != MAGIC_HELLO:
            print(f"[board] expected HELO, got {head[:4]!r}")
            return False
        (json_len,) = struct.unpack_from("<I", head, 4)
        hello = json.loads(await reader.readexactly(json_len))
        got_hello = True

        sps = sample_rate(hello)
        block = max(1, round(sps / TARGET_HZ))
        v_per_count = pin_volts_per_count(hello)
        offsets = pin_offsets(hello)

        # The analyzer is per session: its bin spacing is derived from the rate,
        # so a board that comes back clocked differently gets a new one rather
        # than a stale axis.
        spectra = spec.Spectra(sps, NCH)
        STATE.spectra = spectra
        STATE.spectrum_axis = spectra.axis()

        # The history ring outlives a reconnect on purpose - a dropout is
        # exactly the event you want the record of afterwards. Only a change of
        # axis invalidates it, because columns either side of that are not the
        # same measurement.
        nbins = len(spectra.centers)
        if STATE.history is None or STATE.history.nbins != nbins:
            STATE.history = spec.History(STATE.history_seconds, nbins, NCH)

        STATE.hello = hello
        STATE.sps = sps
        STATE.block_frames = block
        STATE.connected = True
        STATE.peer = peer_s
        STATE.packets = STATE.frames = STATE.gaps = STATE.lost = STATE.crc = 0
        STATE.seq = 0
        STATE.since = time.monotonic()
        STATE.publish(STATE.status())

        STATE.publish(STATE.spectrum_axis)

        print(f"[board] session {hello['session']}, {sps:.1f} SPS, "
              f"block {block} frames -> {sps/block:.2f} Hz", flush=True)
        print(f"[fft]   {spec.FFT_N}-point, {sps/spec.FFT_N:.3f} Hz bins, "
              f"{spec.FFT_SEGMENTS}x half-overlapped -> "
              f"{spectra.span/sps:.2f} s, {len(spectra.centers)} "
              f"display bins at {spec.SPECTRUM_HZ:g} Hz", flush=True)

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
        STATE.connected = False
        STATE.peer = ""
        STATE.writer = None
        STATE.spectra = None      # its ring belongs to the session that filled it
        STATE.publish(STATE.status())
        try:
            writer.close()
        except Exception:
            pass

    return got_hello


# --------------------------------------------------------------------------
# discovery
# --------------------------------------------------------------------------

async def discover_board():
    """One address for the board, or None.

    Parsed exactly as flash.sh parses it, from the same command: avahi-browse
    -p emits `=;if;proto;name;type;domain;host;addr;port;txt`, one line per
    interface, so a board answering on two shows up twice.

    flash.sh always asks which device to use, on the grounds that silently
    picking "the one that answered" is wrong when a second expected board has
    just dropped off the link. Nothing here can ask, so it takes the first and
    names the rest in the log instead - and there is meant to be one board.
    """
    try:
        proc = await asyncio.create_subprocess_exec(
            "avahi-browse", "-rtp", SERVICE,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
        )
    except FileNotFoundError:
        print("[mdns] avahi-browse not found (install avahi-utils), "
              "or pass --board <addr>", flush=True)
        return None

    try:
        out, _ = await asyncio.wait_for(proc.communicate(), BROWSE_TIMEOUT_S)
    except asyncio.TimeoutError:
        proc.kill()
        print("[mdns] browse timed out", flush=True)
        return None

    # Keyed by instance name, because one board answering on two interfaces is
    # two lines and not two boards - a distinction the log has to get right or
    # it reads as a rack that has grown a second monitor.
    boards: dict[str, list[str]] = {}
    for line in out.decode(errors="replace").splitlines():
        f = line.split(";")
        if len(f) >= 8 and f[0] == "=" and f[2] == "IPv4":
            addrs = boards.setdefault(f[3], [])
            if f[7] not in addrs:
                addrs.append(f[7])

    if not boards:
        print(f"[mdns] no {SERVICE} device found "
              "(is the board up, and avahi-daemon running?)", flush=True)
        return None

    name, addrs = next(iter(boards.items()))
    if len(boards) > 1:
        print(f"[mdns] {len(boards)} boards answered "
              f"({', '.join(boards)}) - using {name}", flush=True)
    if len(addrs) > 1:
        print(f"[mdns] {name} has several addresses "
              f"({', '.join(addrs)}) - using {addrs[0]}", flush=True)
    return addrs[0]


async def board_client(pinned, port: int):
    """Find the board, dial it, drain it, repeat.

    The retry loop lives here rather than in the firmware because this is the
    end that has to find the other one. Rediscovery happens on every cycle, not
    once at startup: that is what makes a board that has just taken a new lease
    reappear on its own.
    """
    backoff = BACKOFF_MIN_S

    while True:
        addr = pinned or await discover_board()
        if addr is not None:
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(addr, port), CONNECT_TIMEOUT_S)
            except (OSError, asyncio.TimeoutError) as e:
                print(f"[board] {addr}:{port} unreachable: {e!r}", flush=True)
            else:
                # Reset only once the connection was good for something. A
                # board that accepts and immediately drops would otherwise be
                # retried at full speed forever.
                if await handle_board(reader, writer):
                    backoff = BACKOFF_MIN_S

        await asyncio.sleep(backoff)
        backoff = min(backoff * 2, BACKOFF_MAX_S)


def sig(v, digits=7):
    """One number, at the precision anyone can actually use.

    Unrounded, `repr(float)` spends 17 digits on a value the page prints to
    five and a 24-bit ADC resolves to about seven. That is invisible in one
    block and expensive in six hundred: the history replayed on connect was
    400 KB of mostly trailing noise, and is ~150 KB rounded.
    """
    return float(f"{v:.{digits}g}")


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
            "mean": sig(mean[i]), "rms": sig(rms[i]),
            "min": sig(lo[i]), "max": sig(hi[i]),
            "ok": bool(ch_mask & (1 << i)),
        }
        for i in range(NCH)
    ]

    STATE.seq += 1
    block_msg = {"type": "block", "seq": STATE.seq, "t": time.time(), "ch": out}
    STATE.blocks.append(block_msg)
    STATE.publish(block_msg)

    # The frequency-domain view is fed the same volts, undecimated - averaging
    # to 10 Hz first would leave a 5 Hz Nyquist and nothing to look at. It runs
    # on its own schedule (every `hop` frames, not every block) and costs ~2.5 ms
    # when it does fire, which the socket buffer absorbs without a thread.
    if STATE.spectra is not None and STATE.spectra.push(volts):
        try:
            msg, db = STATE.spectra.compute(scale)
        except Exception as e:
            print(f"[fft] skipped: {e!r}", flush=True)
        else:
            msg["seq"] = STATE.seq
            msg["t"] = time.time()
            if STATE.history is not None:
                STATE.history.push(msg["t"], db)
            # The browser appends this same column to its own ring, so the
            # spectrogram needs nothing further on the wire while a page is up.
            STATE.publish(msg)


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
                "spectrum": STATE.spectrum_axis,
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
    if STATE.spectra is not None:
        STATE.spectra.reset_peak()
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
        # The axis is sent once per session, so a browser that loaded after the
        # board connected would otherwise have spectra and nowhere to plot them.
        if STATE.spectrum_axis:
            writer.write(f"data: {json.dumps(STATE.spectrum_axis)}\n\n".encode())
        # The past, which the browser cannot reconstruct: it only ever sees what
        # arrived after it connected. Both rings go, so the two plots that share
        # an x axis also share a history.
        bh = blocks_message()
        if bh is not None:
            writer.write(f"data: {json.dumps(bh)}\n\n".encode())
        if STATE.history is not None:
            writer.write(f"data: {json.dumps(STATE.history.message())}\n\n".encode())
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


def blocks_message():
    """The series ring, columnar.

    Replayed as the block messages themselves this is 288 KB for a minute,
    because `{"mean": ..., "rms": ..., "min": ..., "max": ..., "ok": ...}`
    repeated 3000 times is mostly key names. One array per field per channel
    carries the same numbers in about half the bytes, and the saving grows with
    --history-seconds, which is the knob most likely to be turned up.

    The page expands this back into block shape before ingesting it, so there
    is still exactly one definition of what a block does to its store.
    """
    bl = list(STATE.blocks)
    if not bl:
        return None
    return {
        "type": "block_history",
        "t": [b["t"] for b in bl],
        "ch": [
            {
                "mean": [b["ch"][i]["mean"] for b in bl],
                "rms": [b["ch"][i]["rms"] for b in bl],
                "min": [b["ch"][i]["min"] for b in bl],
                "max": [b["ch"][i]["max"] for b in bl],
                "ok": [int(b["ch"][i]["ok"]) for b in bl],
            }
            for i in range(NCH)
        ],
    }


async def status_ticker():
    while True:
        await asyncio.sleep(1.0)
        STATE.publish(STATE.status())


# --------------------------------------------------------------------------

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default=None,
                    help="board address; omit to discover it over mDNS")
    ap.add_argument("--board-port", type=int, default=9000,
                    help="stream port on the board (ADC_STREAM_PORT)")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--history-seconds", type=float, default=WINDOW_SECONDS,
                    help="spectrogram history kept in RAM; defaults to the "
                         "time series' own window so the two plots line up")
    args = ap.parse_args()

    STATE.history_seconds = max(1.0, args.history_seconds)

    http = await asyncio.start_server(handle_http, args.bind, args.http_port)

    print(f"board   : {args.board or f'discover {SERVICE}'}:{args.board_port}")
    print(f"gui     : http://localhost:{args.http_port}/")
    print(f"config  : {CONFIG_PATH}")
    print(f"history : {STATE.history_seconds:g} s of spectra in RAM "
          f"({STATE.history_seconds * spec.SPECTRUM_HZ:.0f} columns)", flush=True)

    async with http:
        await asyncio.gather(http.serve_forever(), status_ticker(),
                             board_client(args.board, args.board_port))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
