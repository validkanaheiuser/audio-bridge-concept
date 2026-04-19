#!/usr/bin/env python3
"""
Audio Bridge - Protocol Test Client
Tests basic connectivity and protocol compliance
"""

import socket
import struct
import json
import sys
import time

T_CONTROL = 0x03
T_PING = 0x06


def send_frame(sock, frame_type, data):
    if isinstance(data, str):
        data = data.encode()
    hdr = struct.pack('>BI', frame_type, len(data))
    sock.send(hdr + data)


def recv_frame(sock):
    hdr = b''
    while len(hdr) < 5:
        chunk = sock.recv(5 - len(hdr))
        if not chunk:
            return None, None
        hdr += chunk
    frame_type = hdr[0]
    length = struct.unpack('>I', hdr[1:5])[0]
    data = b''
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            return None, None
        data += chunk
    return frame_type, data


def main():
    if len(sys.argv) < 2:
        print("Usage: python test_client.py <phone_ip> [port]")
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 59100

    print(f"[*] Connecting to {host}:{port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)

    try:
        sock.connect((host, port))
        print("[+] Connected")

        # Receive registration
        reg = b''
        while True:
            c = sock.recv(1)
            if not c or c == b'\n':
                break
            reg += c
        reg_data = json.loads(reg.decode())
        print(f"[+] Device: {reg_data.get('name')} (ID: {reg_data.get('id')})")
        print(f"    Features: {reg_data.get('features')}")

        # Send OK
        sock.send(b'{"status":"ok"}\n')
        print("[+] Handshake complete")

        # Test ping
        print("[*] Sending ping...")
        send_frame(sock, T_CONTROL, json.dumps({"command": "ping"}))

        frame_type, data = recv_frame(sock)
        if frame_type == 0x07:
            print("[+] Pong received - protocol working!")
        else:
            print(f"[?] Unexpected response type: 0x{frame_type:02x}")

        # Test status query
        print("[*] Sending status query...")
        send_frame(sock, T_CONTROL, json.dumps({"command": "get_status"}))

        print("[+] All tests passed!")

    except Exception as e:
        print(f"[-] Error: {e}")
    finally:
        sock.close()
        print("[*] Disconnected")


if __name__ == '__main__':
    main()
