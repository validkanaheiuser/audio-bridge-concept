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
  "type": "call_status",
  "state": 2,
  "state_name": "OFFHOOK",
  "number": "+1234567890",
  "timestamp": 1713524400
}
```

**States:** 0=IDLE, 1=RINGING, 2=OFFHOOK, 3=DIALING, 4=HOLDING

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
