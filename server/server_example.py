#!/usr/bin/env python3
"""
Audio Bridge Server Example
Connects to Android phone and provides remote control interface
"""

import socket
import struct
import json
import threading
import time
import queue
import cmd
import sys

try:
    import opuslib
    HAS_OPUS = True
except ImportError:
    HAS_OPUS = False
    print("[!] opuslib not found - audio features disabled")

try:
    import pyaudio
    HAS_PYAUDIO = True
except ImportError:
    HAS_PYAUDIO = False
    print("[!] pyaudio not found - audio playback disabled")


class AudioBridgeClient:
    """Client connection to a single Android device"""

    # Frame types
    T_SPEAKER = 0x01
    T_VIRTUAL_MIC = 0x02
    T_CONTROL = 0x03
    T_CALL_STATUS = 0x04
    T_SMS = 0x05
    T_PING = 0x06
    T_PONG = 0x07

    def __init__(self, host, port=59100):
        self.host = host
        self.port = port
        self.sock = None
        self.running = False
        self.device_info = {}
        self.call_state = 0
        self.current_number = ""

        # Audio
        self.SAMPLE_RATE = 48000
        self.CHANNELS = 1
        self.FRAME_SAMPLES = 960

        # Opus codecs
        if HAS_OPUS:
            self.opus_decoder = opuslib.Decoder(self.SAMPLE_RATE, self.CHANNELS)
            self.opus_encoder = opuslib.Encoder(self.SAMPLE_RATE, self.CHANNELS, 'voip')
            self.opus_encoder.bitrate = 32000
        else:
            self.opus_decoder = None
            self.opus_encoder = None

        # Audio queues
        self.speaker_queue = queue.Queue(maxsize=100)
        self.mic_queue = queue.Queue(maxsize=100)

        # PyAudio
        self.pa = pyaudio.PyAudio() if HAS_PYAUDIO else None
        self.speaker_stream = None
        self.mic_stream = None

        # Event handlers
        self.on_call_status = None
        self.on_sms_status = None
        self.on_sms_received = None

    def connect(self):
        """Connect to the device"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(30)

        try:
            self.sock.connect((self.host, self.port))
            print(f"[+] Connected to {self.host}:{self.port}")

            # Wait for registration
            reg_data = self._recv_line()
            reg = json.loads(reg_data)
            self.device_info = reg
            print(f"[+] Device: {reg.get('name')} ({reg.get('id')})")

            # Send OK
            self.sock.send(b'{"status":"ok"}\n')

            self.running = True
            return True

        except Exception as e:
            print(f"[-] Connection failed: {e}")
            return False

    def _recv_line(self):
        """Receive a line (until newline)"""
        data = b''
        while True:
            c = self.sock.recv(1)
            if not c or c == b'\n':
                break
            data += c
        return data.decode('utf-8')

    def _recv_frame(self):
        """Receive a framed message"""
        hdr = self._recv_exact(5)
        if not hdr:
            return None, None

        frame_type = hdr[0]
        length = struct.unpack('>I', hdr[1:5])[0]

        data = self._recv_exact(length)
        return frame_type, data

    def _recv_exact(self, length):
        """Receive exact number of bytes"""
        data = b''
        while len(data) < length:
            chunk = self.sock.recv(length - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def send_frame(self, frame_type, data):
        """Send a framed message"""
        hdr = struct.pack('>BI', frame_type, len(data))
        self.sock.send(hdr + data)

    def send_control(self, command_data):
        """Send a control command"""
        if isinstance(command_data, dict):
            command_data = json.dumps(command_data)
        self.send_frame(self.T_CONTROL, command_data.encode())

    def send_audio_frame(self, pcm_data):
        """Send microphone audio to phone"""
        if self.opus_encoder:
            opus_data = self.opus_encoder.encode(pcm_data, self.FRAME_SAMPLES)
            self.send_frame(self.T_VIRTUAL_MIC, opus_data)

    # Commands

    def dial(self, number):
        """Dial a number"""
        self.send_control({"command": "dial", "number": number})
        print(f"[+] Dialing {number}")

    def hangup(self):
        """End current call"""
        self.send_control({"command": "hangup"})
        print("[+] Hanging up")

    def answer(self):
        """Answer incoming call"""
        self.send_control({"command": "answer"})
        print("[+] Answering call")

    def send_sms(self, number, message):
        """Send SMS"""
        self.send_control({
            "command": "send_sms",
            "number": number,
            "message": message
        })
        print(f"[+] Sending SMS to {number}")

    def ping(self):
        """Send keepalive ping"""
        self.send_control({"command": "ping"})

    # Audio callbacks

    def _mic_callback(self, in_data, frame_count, time_info, status):
        """Called when microphone data is available (to send to phone)"""
        try:
            self.mic_queue.put_nowait(in_data)
        except queue.Full:
            pass
        return (None, pyaudio.paContinue)

    def start_audio(self):
        """Start audio streams"""
        if not HAS_PYAUDIO or not self.pa:
            print("[!] Audio disabled - pyaudio not available")
            return

        # Speaker output (from phone to speakers)
        self.speaker_stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=self.CHANNELS,
            rate=self.SAMPLE_RATE,
            output=True,
            frames_per_buffer=self.FRAME_SAMPLES
        )

        # Microphone input (to send to phone)
        self.mic_stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=self.CHANNELS,
            rate=self.SAMPLE_RATE,
            input=True,
            frames_per_buffer=self.FRAME_SAMPLES,
            stream_callback=self._mic_callback
        )

        self.mic_stream.start_stream()
        print("[+] Audio streams started")

    def stop_audio(self):
        """Stop audio streams"""
        if self.speaker_stream:
            self.speaker_stream.stop_stream()
            self.speaker_stream.close()
        if self.mic_stream:
            self.mic_stream.stop_stream()
            self.mic_stream.close()
        print("[+] Audio streams stopped")

    def run(self):
        """Main receive loop"""
        self.start_audio()

        last_ping = time.time()

        while self.running:
            try:
                # Check for ping
                if time.time() - last_ping > 30:
                    self.ping()
                    last_ping = time.time()

                # Receive frame
                frame_type, data = self._recv_frame()

                if frame_type is None:
                    break

                if frame_type == self.T_SPEAKER:
                    # Decode and play speaker audio
                    if self.opus_decoder and self.speaker_stream:
                        pcm = self.opus_decoder.decode(data, self.FRAME_SAMPLES)
                        self.speaker_stream.write(pcm)

                elif frame_type == self.T_CALL_STATUS:
                    status = json.loads(data.decode())
                    self.call_state = status.get('state', 0)
                    self.current_number = status.get('number', '')

                    print(f"[Call] {status.get('state_name')} - {self.current_number}")

                    if self.on_call_status:
                        self.on_call_status(status)

                elif frame_type == self.T_SMS:
                    status = json.loads(data.decode())

                    if status.get('type') == 'sms_status':
                        print(f"[SMS] {status.get('message_id')}: {status.get('result', status.get('status'))}")
                        if self.on_sms_status:
                            self.on_sms_status(status)

                    elif status.get('type') == 'sms_received':
                        print(f"[SMS] From: {status.get('sender')}: {status.get('message')}")
                        if self.on_sms_received:
                            self.on_sms_received(status)

                elif frame_type == self.T_PONG:
                    pass  # Keepalive response

                # Send microphone audio if available
                try:
                    mic_data = self.mic_queue.get_nowait()
                    self.send_audio_frame(mic_data)
                except queue.Empty:
                    pass

            except socket.timeout:
                continue
            except Exception as e:
                print(f"[-] Error: {e}")
                break

        self.stop_audio()
        self.disconnect()

    def disconnect(self):
        """Disconnect from device"""
        self.running = False
        if self.sock:
            self.sock.close()
        print("[+] Disconnected")


class AudioBridgeConsole(cmd.Cmd):
    """Interactive console for Audio Bridge control"""

    intro = """
╔══════════════════════════════════════════════════════════════╗
║           Audio Bridge Server Console v3.0                   ║
║      Type 'help' for available commands                     ║
╚══════════════════════════════════════════════════════════════╝
"""
    prompt = 'audio-bridge> '

    def __init__(self, client):
        super().__init__()
        self.client = client

    def do_dial(self, arg):
        """dial <number> - Dial a phone number"""
        if arg:
            self.client.dial(arg)
        else:
            print("Usage: dial <number>")

    def do_hangup(self, arg):
        """hangup - End current call"""
        self.client.hangup()

    def do_answer(self, arg):
        """answer - Answer incoming call"""
        self.client.answer()

    def do_sms(self, arg):
        """sms <number> <message> - Send SMS"""
        parts = arg.split(' ', 1)
        if len(parts) == 2:
            self.client.send_sms(parts[0], parts[1])
        else:
            print("Usage: sms <number> <message>")

    def do_status(self, arg):
        """status - Show current call status"""
        state_names = ['IDLE', 'RINGING', 'OFFHOOK', 'DIALING', 'HOLDING']
        state = self.client.call_state
        print(f"Call State: {state_names[state] if state < 5 else 'UNKNOWN'}")
        if self.client.current_number:
            print(f"Number: {self.client.current_number}")

    def do_info(self, arg):
        """info - Show device information"""
        print(f"Device: {self.client.device_info.get('name', 'Unknown')}")
        print(f"Brand: {self.client.device_info.get('brand', 'Unknown')}")
        print(f"Android: {self.client.device_info.get('android', 'Unknown')}")
        print(f"ID: {self.client.device_info.get('id', 'Unknown')}")

    def do_quit(self, arg):
        """quit - Disconnect and exit"""
        self.client.running = False
        return True

    def do_exit(self, arg):
        """exit - Disconnect and exit"""
        return self.do_quit(arg)


def main():
    if len(sys.argv) < 2:
        print("Usage: python server_example.py <phone_ip> [port]")
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 59100

    client = AudioBridgeClient(host, port)

    if not client.connect():
        sys.exit(1)

    # Start receiver thread
    receiver_thread = threading.Thread(target=client.run)
    receiver_thread.daemon = True
    receiver_thread.start()

    # Start interactive console
    console = AudioBridgeConsole(client)

    try:
        console.cmdloop()
    except KeyboardInterrupt:
        print("\n[!] Interrupted")
    finally:
        client.running = False
        client.disconnect()


if __name__ == '__main__':
    main()
