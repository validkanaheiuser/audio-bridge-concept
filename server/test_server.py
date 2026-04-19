"""Raw byte dump: intercept TLS handshake to see exactly what's happening"""
import socket
import ssl
import binascii

HOST = '0.0.0.0'
PORT = 59100

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, PORT))
    server.listen(5)
    print(f"Raw TCP server listening on {HOST}:{PORT}")
    print("Waiting for mbedtls ClientHello...\n")

    client, addr = server.accept()
    print(f"[TCP] Connection from {addr}")
    
    # Read the ClientHello raw bytes
    data = client.recv(4096)
    print(f"[RAW] Received {len(data)} bytes from client")
    print(f"[HEX] {binascii.hexlify(data[:64]).decode()}...")
    
    # Parse TLS record header
    if len(data) >= 5:
        content_type = data[0]
        tls_major = data[1]
        tls_minor = data[2]
        record_len = (data[3] << 8) | data[4]
        print(f"\n[TLS Record]")
        print(f"  Content Type: {content_type} (22=Handshake)")
        print(f"  Version: {tls_major}.{tls_minor} (3.1=TLS1.0, 3.3=TLS1.2)")
        print(f"  Length: {record_len}")
        
        if content_type == 22 and len(data) > 5:
            hs_type = data[5]
            print(f"  Handshake Type: {hs_type} (1=ClientHello)")
            
            if hs_type == 1 and len(data) > 9:
                hs_len = (data[6] << 16) | (data[7] << 8) | data[8]
                client_version_major = data[9]
                client_version_minor = data[10]
                print(f"  ClientHello Version: {client_version_major}.{client_version_minor}")
                
                # Skip random (32 bytes) and session_id
                offset = 11 + 32
                if offset < len(data):
                    session_id_len = data[offset]
                    offset += 1 + session_id_len
                    
                    # Cipher suites
                    if offset + 2 <= len(data):
                        cs_len = (data[offset] << 8) | data[offset+1]
                        offset += 2
                        num_ciphers = cs_len // 2
                        print(f"\n  Cipher Suites ({num_ciphers}):")
                        for i in range(min(num_ciphers, 20)):
                            cs = (data[offset + i*2] << 8) | data[offset + i*2 + 1]
                            # Known cipher suite names
                            names = {
                                0xC02C: "ECDHE-ECDSA-AES256-GCM-SHA384",
                                0xC030: "ECDHE-RSA-AES256-GCM-SHA384",
                                0xC02B: "ECDHE-ECDSA-AES128-GCM-SHA256",
                                0xC02F: "ECDHE-RSA-AES128-GCM-SHA256",
                                0xCCA9: "ECDHE-ECDSA-CHACHA20-POLY1305",
                                0xCCA8: "ECDHE-RSA-CHACHA20-POLY1305",
                                0x009D: "AES256-GCM-SHA384 (RSA)",
                                0x009C: "AES128-GCM-SHA256 (RSA)",
                                0x003D: "AES256-SHA256 (RSA)",
                                0x003C: "AES128-SHA256 (RSA)",
                                0x00FF: "RENEGOTIATION_INFO",
                            }
                            name = names.get(cs, f"Unknown")
                            print(f"    0x{cs:04X} = {name}")
    
    print(f"\n[FULL HEX DUMP]")
    for i in range(0, min(len(data), 256), 16):
        hex_part = binascii.hexlify(data[i:i+16]).decode()
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[i:i+16])
        print(f"  {i:04x}: {hex_part:<32}  {ascii_part}")
    
    client.close()
    print("\nDone. Now we know exactly what mbedtls sends.")
