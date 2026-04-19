"""Quick TLS 1.2 connection test to port 59100"""
import ssl
import socket

host = "127.0.0.1"
port = 59100

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
ctx.maximum_version = ssl.TLSVersion.TLSv1_2

try:
    with socket.create_connection((host, port), timeout=5) as sock:
        with ctx.wrap_socket(sock) as ssock:
            print(f"Connected! TLS version: {ssock.version()}")
            print(f"Cipher: {ssock.cipher()}")
            ssock.send(b'{"type":"register","name":"test","id":"test123","token":"default_secure_token_123"}\n')
            resp = ssock.recv(1024)
            print(f"Response: {resp}")
except Exception as e:
    print(f"FAILED: {type(e).__name__}: {e}")
