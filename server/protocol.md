# Audio Bridge Server Protocol v3.0

## Overview

The Audio Bridge uses a custom multiplexed TCP protocol with the following frame structure:

```
[1 byte Type][4 bytes Length (Big Endian)][Payload Data]
```

### Frame Types

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| T_SPEAKER | 0x01 | Phone → Server | Opus-encoded speaker audio |
| T_VIRTUAL_MIC | 0x02 | Server → Phone | Opus-encoded virtual mic audio |
| T_CONTROL | 0x03 | Server → Phone | Control commands (JSON) |
| T_CALL_STATUS | 0x04 | Phone → Server | Call status updates (JSON) |
| T_SMS | 0x05 | Both | SMS control and status (JSON) |
| T_PING | 0x06 | Server → Phone | Keepalive ping |
| T_PONG | 0x07 | Phone → Server | Keepalive response |

## Connection Flow

1. **TCP Connect** - Connect to phone on port 59100
2. **Registration** - Phone sends JSON registration (newline-terminated)
3. **Ready** - Server responds with `{"status":"ok"}\n`
4. **Multiplexed frames** - Binary framed protocol begins

### Registration (Phone → Server)
```json
{
  "type": "register",
  "name": "Pixel 8 Pro",
  "brand": "Google",
  "android": "14",
  "id": "abc123def456",
  "mode": "full_control",
  "version": "3.0.0",
  "features": ["audio", "call_control", "sms"]
}
```

## Server Commands (T_CONTROL)

### Dial
```json
{"command": "dial", "number": "+1234567890"}
```

### Hangup
```json
{"command": "hangup"}
```

### Answer
```json
{"command": "answer"}
```

### Send SMS
```json
{"command": "send_sms", "number": "+1234567890", "message": "Hello!"}
```

### Ping
```json
{"command": "ping"}
```

## Phone Events (T_CALL_STATUS)

### Call State Change
```json
{
  "type": "call",
  "state": "ACTIVE",
  "direction": "outgoing",
  "number": "+1234567890",
  "started_at": 1713524400000,
  "duration_ms": 0,
  "muted": false,
  "sub_id": 1,
  "sim_slot": 0,
  "voice_network_type": 13,
  "voice_network_name": "4G",
  "data_concurrent_with_voice": true
}
```

**States:** `IDLE`, `RINGING`, `DIALING`, `ACTIVE` (legacy numeric: 0=IDLE, 1=RINGING, 2=OFFHOOK, 3=DIALING, 4=HOLDING).

**Per-SIM fields** (added for dual-SIM and Samsung KR + Vietnamese-SIM operations):

| Field | Meaning |
|-------|---------|
| `sub_id` | Android subscription id of the SIM the call is on |
| `sim_slot` | Physical slot index (0 or 1 on DSDS) |
| `voice_network_type` | Numeric `TelephonyManager.NETWORK_TYPE_*` |
| `voice_network_name` | Human label: `2G`, `3G`, `4G`, `5G`, `unknown` |
| `data_concurrent_with_voice` | `false` on 3G WCDMA — server should expect the daemon's TCP to stall during the call unless on Wi-Fi |

### Call Waiting
```json
{
  "type": "call_waiting",
  "incoming_number": "+1987654321",
  "active_number": "+1234567890",
  "available_actions": ["hold_and_answer", "hangup_and_answer", "ignore"]
}
```

### SMS Status
```json
{"type": "sms_status", "message_id": "uuid", "result": "sent", "result_code": -1}
```

### SMS Received
```json
{"type": "sms_received", "sender": "+1987654321", "message": "Reply", "timestamp": 1713524500}
```

## Audio Format

- **Codec**: Opus
- **Sample Rate**: 48kHz
- **Channels**: 1 (Mono)
- **Frame Size**: 20ms (960 samples)
- **Bitrate**: 64kbps (speaker), 32kbps (mic)
- **FEC**: Enabled

### Frame origin tagging

The phone-side SHM ring carries a `flags` field on every audio frame so
multiple capture sources can share a single Opus stream over `T_SPEAKER`
without needing a second TCP channel.

| Flag bit | Constant | Meaning |
|---------:|----------|---------|
| `0x01` | `FRAME_FLAG_ORIGIN_APP` | App audio (Discord, browser, etc.) caught by the Zygisk hook |
| `0x02` | `FRAME_FLAG_ORIGIN_CELL` | Cellular call audio (`MediaRecorder.AudioSource.VOICE_CALL` via the helper APK, or HAL hook) |

The server does not need to interpret the flags to play audio back; it
only needs them if the UI wants to label or route the two streams
separately. Bits 8–31 are reserved.

## Cellular call audio path

Cellular voice never traverses AudioFlinger, so the Zygisk
`AudioRecord`/`AudioTrack` hooks miss it entirely. The helper APK opens
an `AudioRecord` on `MediaRecorder.AudioSource.VOICE_CALL` (falling back
to `VOICE_DOWNLINK`/`VOICE_UPLINK`/`VOICE_COMMUNICATION`) and streams
20 ms 48 kHz mono int16 frames over a dedicated abstract Unix socket
named `audio_bridge`, prefixed with the literal command
`HELO_AUDIO_CELL`. The daemon pushes those frames into the speaker ring
tagged with `FRAME_FLAG_ORIGIN_CELL`.

This path requires `CAPTURE_AUDIO_OUTPUT`, a signature-only permission.
The Magisk/KernelSU module ships
`/system/etc/permissions/privapp-permissions-com.audiobridge.xml` to
whitelist it — but the grant only takes effect when AudioBridge.apk is
installed under `/system/priv-app/`. On a normal `/data` install the
helper degrades cleanly: cellular calls go through, just without the
server-side audio leg.
