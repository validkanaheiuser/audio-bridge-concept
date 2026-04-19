document.addEventListener('DOMContentLoaded', async () => {
    // UI Elements
    const hostInput = document.getElementById('host');
    const portInput = document.getElementById('port');
    const tokenInput = document.getElementById('token');
    
    const daemonBadge = document.getElementById('daemon-badge');
    const daemonPid = document.getElementById('daemon-pid');
    const logOutput = document.getElementById('log-output');
    
    const btnTest = document.getElementById('btn-test');
    const btnSave = document.getElementById('btn-save');
    const btnRefreshLogs = document.getElementById('btn-refresh-logs');
    const testResult = document.getElementById('test-result');

    // Default configuration fallback
    let config = {
        HOST: "192.168.1.100",
        PORT: "59100",
        TOKEN: "default_secure_token_123"
    };

    // Helper: Run shell command via KernelSU
    async function runCmd(cmd) {
        if (typeof ksu === 'undefined') {
            return { errno: 1, stdout: "Error: Not running inside KernelSU WebUI", stderr: "" };
        }
        return await ksu.exec(cmd);
    }

    // Load Configuration
    async function loadConfig() {
        const res = await runCmd('cat /data/local/tmp/audio_bridge.conf');
        if (res.errno === 0 && res.stdout) {
            const lines = res.stdout.split('\n');
            lines.forEach(line => {
                const parts = line.split('=');
                if (parts.length >= 2) {
                    config[parts[0].trim()] = parts.slice(1).join('=').trim();
                }
            });
        }
        
        hostInput.value = config.HOST || "";
        portInput.value = config.PORT || "";
        tokenInput.value = config.TOKEN || "";
    }

    // Check Daemon Status
    async function checkStatus() {
        const res = await runCmd('cat /data/local/tmp/audio_bridge.pid');
        if (res.errno === 0 && res.stdout.trim() !== '') {
            const pid = res.stdout.trim();
            // Verify process is actually running
            const procCheck = await runCmd(`kill -0 ${pid}`);
            if (procCheck.errno === 0) {
                daemonBadge.textContent = 'Running';
                daemonBadge.className = 'badge running';
                daemonPid.textContent = pid;
                return;
            }
        }
        
        daemonBadge.textContent = 'Stopped';
        daemonBadge.className = 'badge stopped';
        daemonPid.textContent = '--';
    }

    // Fetch Logs
    async function fetchLogs() {
        const res = await runCmd('tail -n 25 /data/local/tmp/audio_bridge.log');
        if (res.errno === 0) {
            logOutput.textContent = res.stdout || "No logs available yet.";
        } else {
            logOutput.textContent = "Log file not found or unreadable.";
        }
        // Auto scroll to bottom
        const terminal = document.querySelector('.terminal');
        terminal.scrollTop = terminal.scrollHeight;
    }

    // Test Server Connection
    btnTest.addEventListener('click', async () => {
        const host = hostInput.value.trim();
        const port = portInput.value.trim();
        const token = tokenInput.value.trim();
        
        if (!host || !port) {
            showResult('error', 'Please enter a valid Host and Port.');
            return;
        }

        btnTest.disabled = true;
        btnTest.textContent = 'Testing...';
        showResult('hidden', '');

        // Run the daemon with --check-server flag
        const cmd = `/system/bin/audio-bridge --host "${host}" --port ${port} --token "${token}" --check-server`;
        const res = await runCmd(cmd);

        if (res.errno === 0) {
            showResult('success', 'Connection successful! TLS Handshake passed.');
        } else {
            showResult('error', `Connection failed.\n${res.stderr || res.stdout || 'Check your IP/Port/Token.'}`);
        }

        btnTest.disabled = false;
        btnTest.textContent = 'Test Connection';
    });

    // Save & Restart Daemon
    btnSave.addEventListener('click', async () => {
        const host = hostInput.value.trim();
        const port = portInput.value.trim();
        const token = tokenInput.value.trim();
        
        if (!host || !port) {
            showResult('error', 'Please enter a valid Host and Port.');
            return;
        }

        btnSave.disabled = true;
        btnSave.textContent = 'Saving...';

        // Write config
        const confContent = `HOST=${host}\nPORT=${port}\nTOKEN=${token}\n`;
        // Use printf to handle newlines correctly in shell
        await runCmd(`printf '${confContent}' > /data/local/tmp/audio_bridge.conf`);
        
        // Restart Daemon
        await runCmd('kill -9 $(cat /data/local/tmp/audio_bridge.pid)');
        await runCmd('rm /data/local/tmp/audio_bridge.pid');
        await runCmd('/system/bin/audio-bridge --daemon &');

        showResult('success', 'Configuration saved. Daemon restarted!');
        
        setTimeout(() => {
            btnSave.disabled = false;
            btnSave.textContent = 'Save & Restart';
            checkStatus();
            fetchLogs();
            showResult('hidden', '');
        }, 2000);
    });

    btnRefreshLogs.addEventListener('click', () => {
        fetchLogs();
        checkStatus();
    });

    function showResult(type, message) {
        testResult.className = `test-result ${type}`;
        testResult.textContent = message;
    }

    // Initialization
    await loadConfig();
    await checkStatus();
    await fetchLogs();

    // Auto-refresh logs every 5 seconds
    setInterval(() => {
        checkStatus();
        fetchLogs();
    }, 5000);
});
