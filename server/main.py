"""
Audio Bridge Server v4.0

Wire protocol with the phone daemon (port 59100, unchanged):
    newline-terminated JSON handshake + HMAC-SHA256
    binary frames: [1B type][4B len BE][data]
    types: 1=SPEAKER opus (phone→server), 2=VIRTUAL_MIC opus (server→phone),
           3=CONTROL json, 4=CALL_STATUS json, 5=SMS json, 6=PING, 7=PONG

HTTP/WS endpoints (port 8000):
    GET  /                               dashboard
    WS   /ws/ui                          control + state events
    WS   /ws/audio/{device_id}           PCM audio, binary frames
        ?rate=<int>   default 48000      client's native sample rate
        ?dir=<listen|speak|both>         default both

Audio is negotiated in s16 mono PCM at the client's requested rate. Opus
encode/decode happens in-process at 48kHz (the daemon's fixed native rate);
streaming soxr handles the conversion between 48kHz and the browser's rate.
"""

import asyncio
import hashlib
import hmac
import json
import logging
import os
import struct
import time
from contextlib import asynccontextmanager
from typing import Dict, Optional, Set, Tuple

import uvicorn
from fastapi import FastAPI, Query, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse

# Optional deps: server degrades gracefully without them.
try:
    import opuslib
    HAS_OPUS = True
except Exception:
    opuslib = None
    HAS_OPUS = False

try:
    import soxr
    HAS_SOXR = True
except Exception:
    soxr = None
    HAS_SOXR = False

try:
    import numpy as np
    HAS_NUMPY = True
except Exception:
    np = None
    HAS_NUMPY = False


log = logging.getLogger("audio-bridge")
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)

# ─── Protocol constants ──────────────────────────────────────────────────────
T_SPEAKER, T_VIRTUAL_MIC, T_CONTROL, T_CALL_STATUS, T_SMS, T_PING, T_PONG = range(1, 8)

AUTH_TOKEN = os.environ.get("AUDIO_BRIDGE_TOKEN", "default_secure_token_123")
TCP_PORT = int(os.environ.get("AUDIO_BRIDGE_TCP_PORT", "59100"))
HTTP_PORT = int(os.environ.get("AUDIO_BRIDGE_HTTP_PORT", "8000"))

NATIVE_RATE = 48000
FRAME_MS = 20
FRAME_SAMPLES = NATIVE_RATE * FRAME_MS // 1000     # 960
FRAME_BYTES = FRAME_SAMPLES * 2                    # 1920 bytes = 20ms @ 48k mono s16


# ─── Opus codec ──────────────────────────────────────────────────────────────
class OpusCodec:
    """Wraps opuslib encoder/decoder. No-ops if opuslib is missing."""

    def __init__(self, bitrate: int = 32000):
        self.enc = None
        self.dec = None
        if not HAS_OPUS:
            return
        self.enc = opuslib.Encoder(NATIVE_RATE, 1, opuslib.APPLICATION_VOIP)
        # opuslib uses properties, not ctl macros
        self.enc.bitrate = bitrate
        try:
            self.enc.inband_fec = True
            self.enc.packet_loss_perc = 10
            self.enc.complexity = 10
        except Exception:
            pass
        self.dec = opuslib.Decoder(NATIVE_RATE, 1)

    def decode(self, pkt: bytes) -> bytes:
        """Opus packet → 48kHz s16 mono PCM (one 20ms frame)."""
        if not self.dec or not pkt:
            return b""
        try:
            return self.dec.decode(pkt, FRAME_SAMPLES, decode_fec=False)
        except Exception as e:
            log.debug("opus decode: %s", e)
            # Loss concealment
            try:
                return self.dec.decode(b"", FRAME_SAMPLES, decode_fec=False)
            except Exception:
                return b"\x00" * FRAME_BYTES

    def encode(self, pcm: bytes) -> bytes:
        """48kHz s16 mono PCM (exactly one 20ms frame) → Opus packet."""
        if not self.enc or len(pcm) < FRAME_BYTES:
            return b""
        try:
            return self.enc.encode(pcm[:FRAME_BYTES], FRAME_SAMPLES)
        except Exception as e:
            log.debug("opus encode: %s", e)
            return b""


# ─── Streaming sample rate converter ─────────────────────────────────────────
class Resampler:
    """Stateful SRC via soxr. Pass-through when rates match or soxr is missing."""

    def __init__(self, in_rate: int, out_rate: int):
        self.in_rate = in_rate
        self.out_rate = out_rate
        self._stream = None
        if in_rate == out_rate:
            return
        if not (HAS_SOXR and HAS_NUMPY):
            log.warning("soxr/numpy missing; SRC %d→%d disabled", in_rate, out_rate)
            return
        # 'HQ' preset is the right balance for VoIP: ~90dB stopband, modest CPU.
        self._stream = soxr.ResampleStream(
            in_rate, out_rate, 1, dtype="int16", quality="HQ"
        )

    def process(self, pcm: bytes, last: bool = False) -> bytes:
        if self._stream is None:
            return pcm
        arr = np.frombuffer(pcm, dtype=np.int16)
        out = self._stream.resample_chunk(arr, last=last)
        return out.tobytes()


# ─── Audio hub per device ────────────────────────────────────────────────────
class UIListener:
    """One browser connection listening to the phone's speaker stream."""

    def __init__(self, target_rate: int):
        self.rate = target_rate
        self.resampler = Resampler(NATIVE_RATE, target_rate)
        # ~2s at 48kHz before we start dropping
        self.queue: asyncio.Queue = asyncio.Queue(maxsize=100)

    async def deliver(self, pcm48: bytes) -> None:
        pcm_out = self.resampler.process(pcm48)
        if not pcm_out:
            return
        try:
            self.queue.put_nowait(pcm_out)
        except asyncio.QueueFull:
            # Drop oldest to keep latency bounded
            try:
                self.queue.get_nowait()
                self.queue.put_nowait(pcm_out)
            except Exception:
                pass

    async def next(self) -> bytes:
        return await self.queue.get()


class MicUploader:
    """Per-connection accumulator turning UI PCM at src_rate into 20ms 48kHz frames."""

    def __init__(self, hub: "AudioHub", src_rate: int):
        self.hub = hub
        self.src_rate = src_rate
        self.resampler = Resampler(src_rate, NATIVE_RATE)
        self._buf = bytearray()

    async def feed(self, pcm_bytes: bytes) -> None:
        if not pcm_bytes:
            return
        pcm48 = self.resampler.process(pcm_bytes)
        self._buf.extend(pcm48)
        # Hand off complete 20ms frames
        while len(self._buf) >= FRAME_BYTES:
            frame = bytes(self._buf[:FRAME_BYTES])
            del self._buf[:FRAME_BYTES]
            await self.hub.queue_mic_frame(frame)


class AudioHub:
    """Per-device mux: fanout speaker to listeners, collect mic from uploaders."""

    def __init__(self, device: "Device"):
        self.device = device
        self.down = OpusCodec(bitrate=64000)   # decode phone speaker
        self.up = OpusCodec(bitrate=32000)     # encode UI mic
        self.listeners: Set[UIListener] = set()
        self.up_queue: asyncio.Queue = asyncio.Queue(maxsize=50)

    def add_listener(self, l: UIListener) -> None:
        self.listeners.add(l)

    def remove_listener(self, l: UIListener) -> None:
        self.listeners.discard(l)

    async def on_speaker_opus(self, pkt: bytes) -> None:
        pcm48 = self.down.decode(pkt)
        if not pcm48:
            return
        for l in list(self.listeners):
            await l.deliver(pcm48)

    async def queue_mic_frame(self, pcm48_frame: bytes) -> None:
        """Encode one 20ms 48kHz PCM frame and enqueue for the phone."""
        pkt = self.up.encode(pcm48_frame)
        if not pkt:
            return
        try:
            self.up_queue.put_nowait(pkt)
        except asyncio.QueueFull:
            # Keep latest: drop oldest
            try:
                self.up_queue.get_nowait()
                self.up_queue.put_nowait(pkt)
            except Exception:
                pass

    async def next_mic_packet(self) -> Optional[bytes]:
        return await self.up_queue.get()


# ─── Device session ──────────────────────────────────────────────────────────
# Liveness thresholds (seconds). The daemon's own watchdog also sends T_PING
# every 10s, so a device can't go quiet for long without us noticing — but
# we *additionally* probe from the server for RTT and to detect a one-way
# stuck socket (Wi-Fi NAT mid-stream, etc.) where the daemon might still
# think it's online.
HEARTBEAT_INTERVAL_S = 10
STALE_AFTER_S        = 30          # UI dims the row, raises a warning
DROP_AFTER_S         = 60          # server force-closes the TCP socket


def _now_ms() -> int:
    return int(time.monotonic() * 1000)


class Device:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, info: dict):
        self.reader = reader
        self.writer = writer
        self.id: str = info.get("id", "unknown")
        self.name: str = info.get("name", "Unknown")
        self.brand: str = info.get("brand", "")
        self.android: str = info.get("android", "")
        # Rich call state
        self.call_state: str = "IDLE"         # IDLE | DIALING | RINGING | ACTIVE
        self.call_direction: str = "unknown"  # incoming | outgoing | unknown
        self.active_number: str = ""
        self.call_started_at: int = 0         # ms epoch
        self.call_muted: bool = False
        # Per-SIM context surfaced from TelephonyHelper.emitCallState
        self.sim_slot: int = -1
        self.voice_network_name: str = ""     # 2G | 3G | 4G | 5G | unknown | ""
        self.data_concurrent_with_voice: bool = True
        self.connected: bool = True
        self.audio = AudioHub(self)
        self._write_lock = asyncio.Lock()
        # ── Heartbeat / metrics ────────────────────────────────────────────
        self.connected_at: int = _now_ms()
        self.last_seen_at: int = _now_ms()    # any frame counts
        self.last_rtt_ms: Optional[int] = None
        # Map<seq, sent_ms>; we keep at most a handful of in-flight pings.
        self._ping_inflight: Dict[int, int] = {}
        self._ping_seq: int = 0
        # Rolling counters (reset every metrics tick)
        self._spk_frames_acc: int = 0
        self._mic_frames_acc: int = 0
        self.spk_fps: float = 0.0             # speaker frames/sec, last interval
        self.mic_fps: float = 0.0             # mic frames/sec, last interval
        self._last_metrics_tick: int = _now_ms()

    def touch(self) -> None:
        """Mark the device as alive — call on every received frame."""
        self.last_seen_at = _now_ms()

    def is_stale(self) -> bool:
        return (_now_ms() - self.last_seen_at) > STALE_AFTER_S * 1000

    def should_drop(self) -> bool:
        return (_now_ms() - self.last_seen_at) > DROP_AFTER_S * 1000

    def note_speaker_frame(self) -> None:
        self._spk_frames_acc += 1

    def note_mic_frame(self) -> None:
        self._mic_frames_acc += 1

    def tick_metrics(self) -> None:
        """Convert raw counters → per-second rates. Called on heartbeat tick."""
        now = _now_ms()
        dt_ms = max(1, now - self._last_metrics_tick)
        self.spk_fps = self._spk_frames_acc * 1000.0 / dt_ms
        self.mic_fps = self._mic_frames_acc * 1000.0 / dt_ms
        self._spk_frames_acc = 0
        self._mic_frames_acc = 0
        self._last_metrics_tick = now

    def apply_call_event(self, payload: dict) -> None:
        self.call_state     = payload.get("state", "IDLE")
        self.call_direction = payload.get("direction", "unknown")
        self.active_number  = payload.get("number", "")
        self.call_started_at = int(payload.get("started_at", 0) or 0)
        self.call_muted     = bool(payload.get("muted", False))
        # New per-SIM/network fields from TelephonyHelper. Optional in payload
        # — defaults preserve last known value so we don't blank the UI on
        # a partial event.
        if "sim_slot" in payload:
            self.sim_slot = int(payload.get("sim_slot", -1))
        if "voice_network_name" in payload:
            self.voice_network_name = str(payload.get("voice_network_name", ""))
        if "data_concurrent_with_voice" in payload:
            self.data_concurrent_with_voice = bool(payload["data_concurrent_with_voice"])
        if self.call_state == "IDLE":
            self.call_direction = "unknown"
            self.active_number = ""
            self.call_started_at = 0
            self.call_muted = False

    async def send_frame(self, frame_type: int, data: bytes) -> None:
        hdr = struct.pack(">BI", frame_type, len(data))
        async with self._write_lock:
            try:
                self.writer.write(hdr + data)
                await self.writer.drain()
            except Exception as e:
                log.warning("send_frame %s: %s", self.id, e)
                self.connected = False

    async def send_control(self, command: str, **kw) -> None:
        payload = {"command": command, **kw}
        await self.send_frame(T_CONTROL, json.dumps(payload).encode())

    async def send_ping(self) -> None:
        """Server → daemon RTT probe. Daemon (post-update) echoes payload back."""
        self._ping_seq = (self._ping_seq + 1) & 0xFFFFFFFF
        seq = self._ping_seq
        self._ping_inflight[seq] = _now_ms()
        # Cap in-flight to avoid unbounded growth on a stuck peer.
        if len(self._ping_inflight) > 8:
            # Drop the oldest
            old_seq = min(self._ping_inflight)
            self._ping_inflight.pop(old_seq, None)
        await self.send_frame(T_PING, struct.pack(">I", seq))

    def note_pong(self, payload: bytes) -> None:
        """Match against an outstanding ping seq, compute RTT."""
        if len(payload) != 4:
            # Empty pong (legacy daemon, or a daemon-originated keepalive
            # response): just refresh the last_seen pulse.
            self.touch()
            return
        seq = struct.unpack(">I", payload)[0]
        sent = self._ping_inflight.pop(seq, None)
        if sent is not None:
            self.last_rtt_ms = max(0, _now_ms() - sent)
        self.touch()


class DeviceManager:
    def __init__(self):
        self.devices: Dict[str, Device] = {}
        self.ui_clients: Set[WebSocket] = set()

    def add(self, d: Device) -> None:
        self.devices[d.id] = d
        log.info("Device connected: %s (%s, %s %s)", d.name, d.id, d.brand, d.android)
        asyncio.create_task(self.broadcast_state())

    def remove(self, d: Device) -> None:
        self.devices.pop(d.id, None)
        log.info("Device disconnected: %s", d.id)
        asyncio.create_task(self.broadcast_state())

    async def connect_ui(self, ws: WebSocket) -> None:
        await ws.accept()
        self.ui_clients.add(ws)
        await ws.send_json(self._state())

    def disconnect_ui(self, ws: WebSocket) -> None:
        self.ui_clients.discard(ws)

    async def broadcast_state(self) -> None:
        state = self._state()
        for ws in list(self.ui_clients):
            try:
                await ws.send_json(state)
            except Exception:
                self.ui_clients.discard(ws)

    async def broadcast_event(self, event: dict) -> None:
        for ws in list(self.ui_clients):
            try:
                await ws.send_json(event)
            except Exception:
                self.ui_clients.discard(ws)

    def _state(self) -> dict:
        now = _now_ms()
        return {
            "type": "state_update",
            "devices": [
                {
                    "id": d.id,
                    "name": d.name,
                    "brand": d.brand,
                    "android": d.android,
                    "call": {
                        "state": d.call_state,
                        "direction": d.call_direction,
                        "number": d.active_number,
                        "started_at": d.call_started_at,
                        "muted": d.call_muted,
                        "sim_slot": d.sim_slot,
                        "voice_network_name": d.voice_network_name,
                        "data_concurrent_with_voice": d.data_concurrent_with_voice,
                    },
                    "health": {
                        "connected_at": d.connected_at,
                        "last_seen_ms_ago": max(0, now - d.last_seen_at),
                        "rtt_ms": d.last_rtt_ms,
                        "stale": d.is_stale(),
                        "spk_fps": round(d.spk_fps, 1),
                        "mic_fps": round(d.mic_fps, 1),
                    },
                }
                for d in self.devices.values()
            ],
        }


mgr = DeviceManager()


# ─── TCP: phone daemon ───────────────────────────────────────────────────────
async def do_handshake(reader: asyncio.StreamReader) -> Optional[dict]:
    line = await asyncio.wait_for(reader.readuntil(b"\n"), timeout=10.0)
    info = json.loads(line.decode())
    dev_id = info.get("id", "")
    date = info.get("date", "")
    recv_hmac = info.get("hmac", "")
    expected = hmac.new(
        AUTH_TOKEN.encode(), f"{dev_id}-{date}".encode(), hashlib.sha256
    ).hexdigest()
    if not hmac.compare_digest(recv_hmac, expected):
        return None
    return info


async def read_frame(reader: asyncio.StreamReader) -> Tuple[int, bytes]:
    hdr = await reader.readexactly(5)
    t = hdr[0]
    n = struct.unpack(">I", hdr[1:5])[0]
    data = await reader.readexactly(n) if n else b""
    return t, data


async def handle_device(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    addr = writer.get_extra_info("peername")
    log.info("TCP connect from %s", addr)
    device: Optional[Device] = None
    uplink_task: Optional[asyncio.Task] = None
    hb_task: Optional[asyncio.Task] = None

    try:
        info = await do_handshake(reader)
        if info is None:
            writer.write(b'{"status":"error","msg":"invalid hmac signature"}\n')
            await writer.drain()
            return
        writer.write(b'{"status":"ok"}\n')
        await writer.drain()

        device = Device(reader, writer, info)
        mgr.add(device)

        async def pump_mic_to_phone():
            while device.connected:
                pkt = await device.audio.next_mic_packet()
                if pkt is None:
                    break
                await device.send_frame(T_VIRTUAL_MIC, pkt)
                device.note_mic_frame()
        uplink_task = asyncio.create_task(pump_mic_to_phone())

        # Per-device heartbeat. Sends T_PING (4-byte seq) every
        # HEARTBEAT_INTERVAL_S, computes RTT on the matching T_PONG, ticks
        # frame-rate metrics, drops the connection if no traffic for
        # DROP_AFTER_S. Also broadcasts a state_update each tick so the
        # dashboard's last-seen pulse and RTT chip stay live without
        # waiting on a call event.
        async def heartbeat():
            while device.connected:
                try:
                    await asyncio.sleep(HEARTBEAT_INTERVAL_S)
                except asyncio.CancelledError:
                    break
                if not device.connected:
                    break
                if device.should_drop():
                    log.warning("device %s silent for >%ds — dropping",
                                device.id, DROP_AFTER_S)
                    device.connected = False
                    try:
                        writer.close()
                    except Exception:
                        pass
                    break
                device.tick_metrics()
                try:
                    await device.send_ping()
                except Exception as e:
                    log.debug("ping %s: %s", device.id, e)
                # Push refreshed health to all UI clients.
                await mgr.broadcast_state()
        hb_task = asyncio.create_task(heartbeat())

        while device.connected:
            t, data = await read_frame(reader)
            device.touch()
            if t == T_SPEAKER:
                device.note_speaker_frame()
                await device.audio.on_speaker_opus(data)
            elif t == T_CALL_STATUS:
                try:
                    payload = json.loads(data.decode())
                except Exception as e:
                    log.debug("call_status parse: %s", e)
                    continue

                ptype = payload.get("type", "call")
                if ptype == "call":
                    device.apply_call_event(payload)
                    await mgr.broadcast_state()
                    await mgr.broadcast_event({
                        "type": "event", "kind": "call",
                        "device_id": device.id, "data": payload,
                    })
                elif ptype == "error":
                    # Phone-side operation failed — surface to dashboard.
                    log.warning("device error %s: %s", device.id, payload)
                    await mgr.broadcast_event({
                        "type": "event", "kind": "error",
                        "device_id": device.id, "data": payload,
                    })
                else:
                    # Legacy / unknown: still forward for dashboard logging.
                    await mgr.broadcast_event({
                        "type": "event", "kind": ptype,
                        "device_id": device.id, "data": payload,
                    })
            elif t == T_SMS:
                try:
                    sms = json.loads(data.decode())
                    sms["device_id"] = device.id
                    await mgr.broadcast_event(sms)
                except Exception as e:
                    log.debug("sms parse: %s", e)
            elif t == T_PING:
                # Daemon-originated keepalive: echo the payload (typically
                # empty) back so the daemon's watchdog stays satisfied.
                await device.send_frame(T_PONG, data)
            elif t == T_PONG:
                # Reply to our own T_PING — measures RTT and refreshes
                # last_seen. Also called for daemon-originated pongs that
                # carry no seq, in which case note_pong only touches.
                device.note_pong(data)
    except asyncio.TimeoutError:
        log.info("handshake timeout from %s", addr)
    except asyncio.IncompleteReadError:
        pass
    except Exception as e:
        log.warning("device %s: %s: %s", addr, type(e).__name__, e)
    finally:
        if device:
            device.connected = False
            # Unblock the uplink pump
            try:
                device.audio.up_queue.put_nowait(None)
            except Exception:
                pass
            mgr.remove(device)
        if uplink_task:
            uplink_task.cancel()
        if hb_task:
            try:
                hb_task.cancel()
            except Exception:
                pass
        try:
            writer.close()
        except Exception:
            pass


async def start_tcp() -> None:
    srv = await asyncio.start_server(handle_device, "0.0.0.0", TCP_PORT)
    log.info("TCP on :%d (HMAC auth, Opus@48k)", TCP_PORT)
    async with srv:
        await srv.serve_forever()


# ─── FastAPI ────────────────────────────────────────────────────────────────
@asynccontextmanager
async def lifespan(_app: FastAPI):
    task = asyncio.create_task(start_tcp())
    if not HAS_OPUS:
        log.warning("opuslib missing — audio bridging disabled (pip install opuslib)")
    if not HAS_SOXR:
        log.warning("python-soxr missing — sample-rate conversion disabled (pip install soxr numpy)")
    yield
    task.cancel()


app = FastAPI(title="Audio Bridge Server", lifespan=lifespan)


@app.websocket("/ws/ui")
async def ws_ui(ws: WebSocket) -> None:
    await mgr.connect_ui(ws)
    try:
        while True:
            data = await ws.receive_json()
            cmd = data.get("command")
            did = data.get("device_id")
            if not did or did not in mgr.devices:
                continue
            d = mgr.devices[did]
            if cmd == "dial":
                await d.send_control("dial", number=data.get("number"))
            elif cmd == "hangup":
                await d.send_control("hangup")
            elif cmd == "answer":
                await d.send_control("answer")
            elif cmd == "mute":
                await d.send_control("mute", on=bool(data.get("on", True)))
            elif cmd == "send_sms":
                await d.send_control(
                    "send_sms",
                    number=data.get("number"),
                    message=data.get("message"),
                )
    except WebSocketDisconnect:
        pass
    finally:
        mgr.disconnect_ui(ws)


@app.websocket("/ws/audio/{device_id}")
async def ws_audio(
    ws: WebSocket,
    device_id: str,
    rate: int = Query(NATIVE_RATE, ge=8000, le=96000),
    dir: str = Query("both", regex="^(listen|speak|both)$"),
) -> None:
    """
    Binary frames: raw s16 mono PCM at `rate`.
    dir=listen  server → client (phone speaker)
    dir=speak   client → server (phone virtual mic)
    dir=both    full duplex
    """
    if device_id not in mgr.devices:
        await ws.close(code=4404)
        return
    await ws.accept()
    device = mgr.devices[device_id]
    hub = device.audio

    want_listen = dir in ("listen", "both")
    want_speak = dir in ("speak", "both")

    listener = UIListener(rate) if want_listen else None
    uploader = MicUploader(hub, rate) if want_speak else None
    if listener:
        hub.add_listener(listener)

    async def pump_to_client():
        try:
            while True:
                pcm = await listener.next()
                await ws.send_bytes(pcm)
        except Exception:
            pass

    pump_task = asyncio.create_task(pump_to_client()) if listener else None

    try:
        while True:
            msg = await ws.receive()
            mtype = msg.get("type")
            if mtype == "websocket.disconnect":
                break
            payload_bytes = msg.get("bytes")
            payload_text = msg.get("text")
            if payload_bytes is not None and uploader:
                await uploader.feed(payload_bytes)
            elif payload_text is not None:
                # Optional JSON control on audio socket (e.g. change rate)
                try:
                    j = json.loads(payload_text)
                    new_rate = int(j.get("rate", rate))
                    if new_rate != rate:
                        rate = new_rate
                        if listener:
                            listener.rate = new_rate
                            listener.resampler = Resampler(NATIVE_RATE, new_rate)
                        if uploader:
                            uploader.src_rate = new_rate
                            uploader.resampler = Resampler(new_rate, NATIVE_RATE)
                except Exception:
                    pass
    except WebSocketDisconnect:
        pass
    finally:
        if pump_task:
            pump_task.cancel()
        if listener:
            hub.remove_listener(listener)


@app.get("/", response_class=HTMLResponse)
async def dashboard() -> str:
    path = os.path.join(os.path.dirname(__file__), "dashboard.html")
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        return (
            "<h1>Audio Bridge</h1>"
            "<p>dashboard.html is missing next to main.py</p>"
        )


if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=HTTP_PORT, reload=False)
