# Audio Bridge v3.0

Complete Android Audio Bridge System with Telephony & SMS Control.

## Features

- **Real-time audio streaming** — Capture phone speaker output and inject virtual microphone audio
- **Call control** — Dial, answer, hangup, hold, and handle call waiting remotely
- **SMS management** — Send SMS with delivery tracking, receive incoming SMS notifications
- **Opus codec** — High-quality 48kHz audio with FEC and packet loss concealment
- **Persistent connection** — Auto-reconnect TCP with keepalive
- **Zygisk integration** — Hooks AudioTrack/AudioRecord at system level via shared memory

## Architecture

```
┌──────────────────────────────────────────────────┐
│  Android Device (Rooted + Magisk)                │
│                                                  │
│  audio-bridge daemon ←→ Zygisk Module            │
│       ↕ JNI              (AudioTrack/Record hooks)│
│  TelephonyHelper.java                            │
│       ↕ Android APIs                             │
│  Telephony / SMS Manager                         │
└──────────────┬───────────────────────────────────┘
               │ TCP (Opus + JSON)
┌──────────────┴───────────────────────────────────┐
│  Remote Server (Python)                          │
│  - Interactive console                           │
│  - Speaker playback (PyAudio)                    │
│  - Mic capture → virtual mic                     │
└──────────────────────────────────────────────────┘
```

## Requirements

- **Android device** with root (Magisk) and Zygisk enabled
- **Android NDK r25+** for building
- **Python 3.8+** with `opuslib` and `pyaudio` for the server

## Quick Start

### 1. Build
```bash
export ANDROID_NDK_HOME=/path/to/ndk
chmod +x build.sh
./build.sh
```

### 2. Deploy to Device
```bash
adb push build/audio-bridge-arm64-v8a /data/local/tmp/audio-bridge
adb push zygisk/module /data/adb/modules/audio_bridge
adb shell chmod 755 /data/local/tmp/audio-bridge
adb reboot
```

### 3. Configure
```bash
adb shell "echo 'YOUR_SERVER_IP' > /data/local/tmp/audio_bridge.conf"
adb shell "/data/local/tmp/audio-bridge --host YOUR_SERVER_IP --daemon &"
```

### 4. Run Server
```bash
pip install opuslib pyaudio
python server/server_example.py <phone_ip>
```

## Server Commands

| Command | Description |
|---------|-------------|
| `dial <number>` | Place a call |
| `answer` | Answer incoming call |
| `hangup` | End current call |
| `sms <number> <message>` | Send SMS |
| `status` | Show call state |
| `info` | Show device info |
| `quit` | Disconnect |

## Project Structure

```
audio-bridge/
├── jni/                  # Native C++ daemon
├── java/com/audiobridge/ # Java telephony helper
├── zygisk/               # Zygisk module
├── server/               # Python server + protocol docs
├── config/               # Configuration files
├── scripts/              # Install/start/stop scripts
├── build.sh              # Build script
└── CMakeLists.txt        # CMake alternative
```

## License

MIT License - see [LICENSE](LICENSE)
