import sys

with open('server/main.py', 'r', encoding='utf-8') as f:
    content = f.read()

old = 'logger.info(f"New TLS connection from {addr}")'
new = 'logger.info(f"[CONN] New TCP+TLS connection from {addr}")'

old2 = '        # Handshake\n        line = await reader.readuntil(b\'\\n\')\n        reg_info = json.loads(line.decode())'
new2 = '        # Handshake\n        logger.info(f"[CONN] Waiting for handshake from {addr}...")\n        line = await asyncio.wait_for(reader.readuntil(b\'\\n\'), timeout=10.0)\n        logger.info(f"[CONN] Got handshake: {line.decode().strip()[:200]}")\n        reg_info = json.loads(line.decode())'

old3 = '    except asyncio.IncompleteReadError:\n        logger.info(f"Connection closed by {addr}")\n    except Exception as e:\n        logger.error(f"Error handling device {addr}: {e}")'
new3 = '    except asyncio.TimeoutError:\n        logger.warning(f"[CONN] Timeout from {addr} - no handshake data in 10s")\n    except asyncio.IncompleteReadError:\n        logger.info(f"[CONN] Connection closed by {addr}")\n    except Exception as e:\n        logger.error(f"[CONN] Error from {addr}: {type(e).__name__}: {e}")'

for o, n in [(old, new), (old2, new2), (old3, new3)]:
    if o in content:
        content = content.replace(o, n)
        print(f"OK: replaced")
    else:
        print(f"NOT FOUND: {repr(o[:60])}")

with open('server/main.py', 'w', encoding='utf-8') as f:
    f.write(content)
