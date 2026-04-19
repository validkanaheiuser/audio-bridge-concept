import asyncio
import ssl
import json
import struct
import logging
from typing import Dict, Optional, Set
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
import uvicorn

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("AudioBridgeServer")

# Protocol Constants
T_SPEAKER = 0x01
T_VIRTUAL_MIC = 0x02
T_CONTROL = 0x03
T_CALL_STATUS = 0x04
T_SMS = 0x05
T_PING = 0x06
T_PONG = 0x07

AUTH_TOKEN = "default_secure_token_123"

app = FastAPI(title="Audio Bridge Server")

class Device:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, info: dict):
        self.reader = reader
        self.writer = writer
        self.id = info.get("id", "unknown")
        self.name = info.get("name", "Unknown Device")
        self.call_state = "IDLE"
        self.active_number = ""
        self.is_connected = True

    async def send_control(self, command: str, **kwargs):
        payload = {"command": command}
        payload.update(kwargs)
        data = json.dumps(payload).encode()
        await self._send_frame(T_CONTROL, data)

    async def _send_frame(self, frame_type: int, data: bytes):
        try:
            hdr = struct.pack('>BI', frame_type, len(data))
            self.writer.write(hdr + data)
            await self.writer.drain()
        except Exception as e:
            logger.error(f"Error sending frame to {self.id}: {e}")
            self.is_connected = False

class DeviceManager:
    def __init__(self):
        self.devices: Dict[str, Device] = {}
        self.ui_clients: Set[WebSocket] = set()

    def add_device(self, device: Device):
        self.devices[device.id] = device
        logger.info(f"Device connected: {device.name} ({device.id})")
        asyncio.create_task(self.broadcast_state())

    def remove_device(self, device_id: str):
        if device_id in self.devices:
            del self.devices[device_id]
            logger.info(f"Device disconnected: {device_id}")
            asyncio.create_task(self.broadcast_state())

    async def connect_ui(self, websocket: WebSocket):
        await websocket.accept()
        self.ui_clients.add(websocket)
        await self.send_state_to_client(websocket)

    def disconnect_ui(self, websocket: WebSocket):
        self.ui_clients.remove(websocket)

    async def broadcast_state(self):
        if not self.ui_clients:
            return
        state = self._get_state()
        for client in list(self.ui_clients):
            try:
                await client.send_json(state)
            except:
                self.ui_clients.remove(client)

    async def send_state_to_client(self, websocket: WebSocket):
        try:
            await websocket.send_json(self._get_state())
        except:
            pass

    def _get_state(self):
        return {
            "type": "state_update",
            "devices": [
                {
                    "id": d.id,
                    "name": d.name,
                    "state": d.call_state,
                    "number": d.active_number
                }
                for d in self.devices.values()
            ]
        }

manager = DeviceManager()

@app.websocket("/ws/ui")
async def websocket_endpoint(websocket: WebSocket):
    await manager.connect_ui(websocket)
    try:
        while True:
            data = await websocket.receive_json()
            command = data.get("command")
            device_id = data.get("device_id")
            
            if not device_id or device_id not in manager.devices:
                continue
                
            device = manager.devices[device_id]
            
            if command == "dial":
                await device.send_control("dial", number=data.get("number"))
            elif command == "hangup":
                await device.send_control("hangup")
            elif command == "answer":
                await device.send_control("answer")
            elif command == "send_sms":
                await device.send_control("send_sms", number=data.get("number"), message=data.get("message"))
                
    except WebSocketDisconnect:
        manager.disconnect_ui(websocket)

async def handle_device_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    addr = writer.get_extra_info('peername')
    logger.info(f"New TLS connection from {addr}")

    try:
        # Handshake
        line = await reader.readuntil(b'\n')
        reg_info = json.loads(line.decode())
        
        if reg_info.get("token") != AUTH_TOKEN:
            logger.warning(f"Invalid auth token from {addr}")
            writer.write(b'{"status":"error","msg":"invalid token"}\n')
            await writer.drain()
            writer.close()
            return
            
        writer.write(b'{"status":"ok"}\n')
        await writer.drain()
        
        device = Device(reader, writer, reg_info)
        manager.add_device(device)
        
        # Read frames loop
        while device.is_connected:
            hdr = await reader.readexactly(5)
            frame_type = hdr[0]
            length = struct.unpack('>I', hdr[1:5])[0]
            
            if length > 0:
                data = await reader.readexactly(length)
            else:
                data = b''
                
            if frame_type == T_CALL_STATUS:
                status = json.loads(data.decode())
                device.call_state = status.get("state_name", "UNKNOWN")
                device.active_number = status.get("number", "")
                await manager.broadcast_state()
            elif frame_type == T_SMS:
                # Forward SMS to UI
                sms_data = json.loads(data.decode())
                for client in list(manager.ui_clients):
                    try:
                        await client.send_json(sms_data)
                    except:
                        pass
            elif frame_type == T_PING:
                await device._send_frame(T_PONG, b'')
                
    except asyncio.IncompleteReadError:
        logger.info(f"Connection closed by {addr}")
    except Exception as e:
        logger.error(f"Error handling device {addr}: {e}")
    finally:
        if 'device' in locals():
            manager.remove_device(device.id)
        writer.close()

async def start_tcp_server():
    # Setup TLS context for TCP server
    ssl_context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ssl_context.check_hostname = False
    try:
        ssl_context.load_cert_chain(certfile="server.crt", keyfile="server.key")
    except Exception as e:
        logger.warning(f"Could not load server.crt/key ({e}). Generating temporary self-signed cert in memory... (FOR TESTING ONLY)")
        import subprocess
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", "server.key", "-out", "server.crt", "-days", "365", "-nodes", "-subj", "/CN=localhost"], check=False)
        ssl_context.load_cert_chain(certfile="server.crt", keyfile="server.key")

    server = await asyncio.start_server(
        handle_device_client, 
        '0.0.0.0', 
        59100, 
        ssl=ssl_context
    )
    logger.info("TLS Audio Bridge Server listening on 0.0.0.0:59100")
    async with server:
        await server.serve_forever()

@app.on_event("startup")
async def startup_event():
    asyncio.create_task(start_tcp_server())

if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=False)
