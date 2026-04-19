import asyncio
import ssl
import json
import struct
import logging
import os
from contextlib import asynccontextmanager
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

@asynccontextmanager
async def lifespan(a):
    asyncio.create_task(start_tcp_server())
    yield

app = FastAPI(title="Audio Bridge Server", lifespan=lifespan)

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
    logger.info(f"[CONN] New TCP+TLS connection from {addr}")

    try:
        # Handshake
        logger.info(f"[CONN] Waiting for handshake from {addr}...")
        line = await asyncio.wait_for(reader.readuntil(b'\n'), timeout=10.0)
        logger.info(f"[CONN] Got handshake: {line.decode().strip()[:200]}")
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
                
    except asyncio.TimeoutError:
        logger.warning(f"[CONN] Timeout from {addr} - no handshake data in 10s")
    except asyncio.IncompleteReadError:
        logger.info(f"[CONN] Connection closed by {addr}")
    except Exception as e:
        logger.error(f"[CONN] Error from {addr}: {type(e).__name__}: {e}")
    finally:
        if 'device' in locals():
            manager.remove_device(device.id)
        writer.close()

def generate_self_signed_cert():
    """Generate a self-signed cert using the cryptography library (cross-platform)."""
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    import datetime

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Audio Bridge Server")])
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.utcnow())
        .not_valid_after(datetime.datetime.utcnow() + datetime.timedelta(days=365))
        .sign(key, hashes.SHA256())
    )

    with open("server.key", "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
    with open("server.crt", "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    logger.info("Generated self-signed certificate (server.crt / server.key)")

async def start_tcp_server():
    # Setup TLS context for TCP server
    ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE  # Don't require client certs
    ssl_context.minimum_version = ssl.TLSVersion.TLSv1_2
    
    if not os.path.exists("server.crt") or not os.path.exists("server.key"):
        logger.warning("No server.crt/server.key found. Generating self-signed cert... (FOR TESTING ONLY)")
        generate_self_signed_cert()
    
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

from fastapi.responses import HTMLResponse

@app.get("/", response_class=HTMLResponse)
async def dashboard():
    return DASHBOARD_HTML

DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Audio Bridge — Control Panel</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap');
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { font-family: 'Inter', sans-serif; background: #0a0e1a; color: #e0e4f0; min-height: 100vh; }
  
  .header {
    background: linear-gradient(135deg, #1a1f35 0%, #0d1225 100%);
    border-bottom: 1px solid rgba(99,102,241,0.2);
    padding: 1.5rem 2rem;
    display: flex; align-items: center; gap: 1rem;
  }
  .header h1 { font-size: 1.5rem; font-weight: 600; background: linear-gradient(135deg, #818cf8, #6366f1); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
  .header .status { margin-left: auto; display: flex; align-items: center; gap: 0.5rem; font-size: 0.85rem; }
  .dot { width: 10px; height: 10px; border-radius: 50%; }
  .dot.online { background: #22c55e; box-shadow: 0 0 8px #22c55e88; }
  .dot.offline { background: #ef4444; box-shadow: 0 0 8px #ef444488; }
  
  .container { max-width: 1200px; margin: 2rem auto; padding: 0 1.5rem; display: grid; grid-template-columns: 1fr 1fr; gap: 1.5rem; }
  
  .card {
    background: linear-gradient(135deg, #141829 0%, #0f1322 100%);
    border: 1px solid rgba(99,102,241,0.15);
    border-radius: 16px; padding: 1.5rem;
    transition: border-color 0.3s;
  }
  .card:hover { border-color: rgba(99,102,241,0.4); }
  .card h2 { font-size: 1rem; font-weight: 600; color: #818cf8; margin-bottom: 1rem; text-transform: uppercase; letter-spacing: 0.5px; }
  
  .devices-list { display: flex; flex-direction: column; gap: 0.75rem; }
  .device-item {
    background: rgba(99,102,241,0.05); border: 1px solid rgba(99,102,241,0.1);
    border-radius: 12px; padding: 1rem; cursor: pointer;
    transition: all 0.2s;
  }
  .device-item:hover { background: rgba(99,102,241,0.1); transform: translateY(-1px); }
  .device-item.selected { border-color: #6366f1; background: rgba(99,102,241,0.15); }
  .device-name { font-weight: 600; font-size: 0.95rem; }
  .device-meta { font-size: 0.75rem; color: #9ca3af; margin-top: 0.25rem; }
  .device-state { font-size: 0.8rem; color: #22c55e; margin-top: 0.25rem; }
  
  .no-devices { color: #6b7280; text-align: center; padding: 2rem; font-size: 0.9rem; }
  
  input, textarea {
    width: 100%; background: rgba(255,255,255,0.05); border: 1px solid rgba(99,102,241,0.2);
    border-radius: 10px; padding: 0.75rem 1rem; color: #e0e4f0; font-family: inherit; font-size: 0.9rem;
    outline: none; transition: border-color 0.2s;
  }
  input:focus, textarea:focus { border-color: #6366f1; }
  textarea { resize: vertical; min-height: 80px; }
  
  .input-group { margin-bottom: 1rem; }
  .input-group label { display: block; font-size: 0.8rem; color: #9ca3af; margin-bottom: 0.4rem; font-weight: 500; }
  
  .btn-row { display: flex; gap: 0.75rem; margin-top: 1rem; }
  button {
    flex: 1; padding: 0.7rem 1rem; border: none; border-radius: 10px;
    font-family: inherit; font-weight: 600; font-size: 0.85rem; cursor: pointer;
    transition: all 0.2s;
  }
  .btn-call { background: linear-gradient(135deg, #22c55e, #16a34a); color: #fff; }
  .btn-call:hover { transform: translateY(-1px); box-shadow: 0 4px 12px #22c55e44; }
  .btn-hangup { background: linear-gradient(135deg, #ef4444, #dc2626); color: #fff; }
  .btn-hangup:hover { transform: translateY(-1px); box-shadow: 0 4px 12px #ef444444; }
  .btn-answer { background: linear-gradient(135deg, #3b82f6, #2563eb); color: #fff; }
  .btn-answer:hover { transform: translateY(-1px); box-shadow: 0 4px 12px #3b82f644; }
  .btn-sms { background: linear-gradient(135deg, #818cf8, #6366f1); color: #fff; }
  .btn-sms:hover { transform: translateY(-1px); box-shadow: 0 4px 12px #6366f144; }
  button:disabled { opacity: 0.4; cursor: not-allowed; transform: none !important; }
  
  .log-area {
    background: #0a0d14; border: 1px solid rgba(99,102,241,0.1); border-radius: 12px;
    padding: 1rem; font-family: 'Courier New', monospace; font-size: 0.75rem;
    max-height: 200px; overflow-y: auto; color: #9ca3af; line-height: 1.6;
  }
  .log-entry.info { color: #60a5fa; }
  .log-entry.event { color: #22c55e; }
  .log-entry.error { color: #f87171; }
  
  .full-width { grid-column: 1 / -1; }
  
  @media (max-width: 768px) {
    .container { grid-template-columns: 1fr; }
  }
</style>
</head>
<body>
<div class="header">
  <h1>🎧 Audio Bridge</h1>
  <div class="status">
    <div class="dot" id="ws-dot"></div>
    <span id="ws-status">Connecting...</span>
  </div>
</div>

<div class="container">
  <div class="card">
    <h2>📱 Connected Devices</h2>
    <div id="devices" class="devices-list">
      <div class="no-devices">No devices connected. Configure the daemon on your phone.</div>
    </div>
  </div>
  
  <div class="card">
    <h2>📞 Call Control</h2>
    <div class="input-group">
      <label>Phone Number</label>
      <input type="tel" id="phone-number" placeholder="+1234567890">
    </div>
    <div class="btn-row">
      <button class="btn-call" id="btn-dial" disabled>Dial</button>
      <button class="btn-answer" id="btn-answer" disabled>Answer</button>
      <button class="btn-hangup" id="btn-hangup" disabled>Hang Up</button>
    </div>
  </div>
  
  <div class="card">
    <h2>💬 SMS</h2>
    <div class="input-group">
      <label>Recipient Number</label>
      <input type="tel" id="sms-number" placeholder="+1234567890">
    </div>
    <div class="input-group">
      <label>Message</label>
      <textarea id="sms-message" placeholder="Type your message..."></textarea>
    </div>
    <div class="btn-row">
      <button class="btn-sms" id="btn-sms" disabled>Send SMS</button>
    </div>
  </div>
  
  <div class="card">
    <h2>📋 Event Log</h2>
    <div class="log-area" id="log-area"></div>
  </div>
</div>

<script>
let ws = null;
let selectedDevice = null;

function addLog(msg, type = 'info') {
    const area = document.getElementById('log-area');
    const entry = document.createElement('div');
    entry.className = 'log-entry ' + type;
    entry.textContent = new Date().toLocaleTimeString() + ' — ' + msg;
    area.appendChild(entry);
    area.scrollTop = area.scrollHeight;
}

function updateButtons() {
    const has = selectedDevice !== null;
    document.getElementById('btn-dial').disabled = !has;
    document.getElementById('btn-answer').disabled = !has;
    document.getElementById('btn-hangup').disabled = !has;
    document.getElementById('btn-sms').disabled = !has;
}

function renderDevices(devices) {
    const container = document.getElementById('devices');
    if (!devices || Object.keys(devices).length === 0) {
        container.innerHTML = '<div class="no-devices">No devices connected. Configure the daemon on your phone.</div>';
        selectedDevice = null;
        updateButtons();
        return;
    }
    container.innerHTML = '';
    for (const [id, d] of Object.entries(devices)) {
        const item = document.createElement('div');
        item.className = 'device-item' + (selectedDevice === id ? ' selected' : '');
        item.innerHTML = '<div class="device-name">' + (d.name || id) + '</div>'
            + '<div class="device-meta">ID: ' + id + '</div>'
            + '<div class="device-state">Call: ' + (d.call_state || 'IDLE') + (d.active_number ? ' (' + d.active_number + ')' : '') + '</div>';
        item.onclick = () => { selectedDevice = id; renderDevices(devices); updateButtons(); };
        container.appendChild(item);
    }
    if (!selectedDevice && Object.keys(devices).length > 0) {
        selectedDevice = Object.keys(devices)[0];
        renderDevices(devices);
    }
    updateButtons();
}

function connect() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    ws = new WebSocket(proto + '://' + location.host + '/ws/ui');
    ws.onopen = () => {
        document.getElementById('ws-dot').className = 'dot online';
        document.getElementById('ws-status').textContent = 'Connected';
        addLog('WebSocket connected to server', 'event');
    };
    ws.onclose = () => {
        document.getElementById('ws-dot').className = 'dot offline';
        document.getElementById('ws-status').textContent = 'Disconnected';
        addLog('WebSocket disconnected, reconnecting...', 'error');
        setTimeout(connect, 3000);
    };
    ws.onmessage = (e) => {
        const data = JSON.parse(e.data);
        if (data.type === 'state') {
            renderDevices(data.devices);
        } else if (data.event) {
            addLog('Event: ' + JSON.stringify(data), 'event');
        } else {
            addLog('Received: ' + JSON.stringify(data), 'info');
        }
    };
}

document.getElementById('btn-dial').onclick = () => {
    const num = document.getElementById('phone-number').value.trim();
    if (!num || !selectedDevice) return;
    ws.send(JSON.stringify({ command: 'dial', device_id: selectedDevice, number: num }));
    addLog('Dialing ' + num + '...', 'info');
};
document.getElementById('btn-answer').onclick = () => {
    if (!selectedDevice) return;
    ws.send(JSON.stringify({ command: 'answer', device_id: selectedDevice }));
    addLog('Answering call...', 'info');
};
document.getElementById('btn-hangup').onclick = () => {
    if (!selectedDevice) return;
    ws.send(JSON.stringify({ command: 'hangup', device_id: selectedDevice }));
    addLog('Hanging up...', 'info');
};
document.getElementById('btn-sms').onclick = () => {
    const num = document.getElementById('sms-number').value.trim();
    const msg = document.getElementById('sms-message').value.trim();
    if (!num || !msg || !selectedDevice) return;
    ws.send(JSON.stringify({ command: 'send_sms', device_id: selectedDevice, number: num, message: msg }));
    addLog('Sending SMS to ' + num + '...', 'info');
    document.getElementById('sms-message').value = '';
};

connect();
</script>
</body>
</html>"""

if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=False)
