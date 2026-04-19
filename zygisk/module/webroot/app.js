document.addEventListener('DOMContentLoaded', async () => {
    console.log('[DEBUG] DOM Content Loaded - Initializing Audio Bridge WebUI');

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

    // Verify all UI elements exist
    const elements = {
        hostInput, portInput, tokenInput, daemonBadge, daemonPid,
        logOutput, btnTest, btnSave, btnRefreshLogs, testResult
    };
    for (const [name, element] of Object.entries(elements)) {
        if (!element) {
            console.error(`[ERROR] UI element not found: ${name}`);
        } else {
            console.log(`[DEBUG] UI element found: ${name}`);
        }
    }

    // Default configuration fallback
    let config = {
        HOST: "192.168.1.100",
        PORT: "59100",
        TOKEN: "default_secure_token_123"
    };
    console.log('[DEBUG] Default config loaded:', { ...config, TOKEN: '***HIDDEN***' });

    // Helper: Run shell command via KernelSU
    async function runCmd(cmd) {
        console.log(`[DEBUG] Executing command: ${cmd}`);

        if (typeof ksu === 'undefined') {
            console.error('[ERROR] KernelSU API not available - Not running inside KernelSU WebUI');
            return { errno: 1, stdout: "Error: Not running inside KernelSU WebUI", stderr: "" };
        }

        try {
            const result = await ksu.exec(cmd);
            console.log(`[DEBUG] Command result - errno: ${result.errno}, stdout length: ${result.stdout?.length || 0}, stderr length: ${result.stderr?.length || 0}`);
            if (result.errno !== 0) {
                console.warn(`[WARN] Command failed with errno ${result.errno}: ${cmd}`);
                if (result.stderr) console.warn(`[WARN] stderr: ${result.stderr}`);
            }
            return result;
        } catch (error) {
            console.error(`[ERROR] Exception executing command: ${cmd}`, error);
            return { errno: 1, stdout: "", stderr: error.message };
        }
    }

    // Load Configuration
    async function loadConfig() {
        console.log('[DEBUG] Loading configuration from /data/local/tmp/audio_bridge.conf');
        const res = await runCmd('cat /data/local/tmp/audio_bridge.conf');

        if (res.errno === 0 && res.stdout) {
            console.log('[DEBUG] Config file found, parsing content');
            const lines = res.stdout.split('\n');
            console.log(`[DEBUG] Config file has ${lines.length} lines`);

            lines.forEach(line => {
                const parts = line.split('=');
                if (parts.length >= 2) {
                    const key = parts[0].trim();
                    const value = parts.slice(1).join('=').trim();
                    config[key] = value;
                    console.log(`[DEBUG] Config parsed: ${key}=${key === 'TOKEN' ? '***HIDDEN***' : value}`);
                }
            });
        } else {
            console.warn('[WARN] Config file not found or empty, using defaults');
        }

        hostInput.value = config.HOST || "";
        portInput.value = config.PORT || "";
        tokenInput.value = config.TOKEN || "";

        console.log('[DEBUG] Configuration loaded - Host:', hostInput.value, 'Port:', portInput.value, 'Token:', tokenInput.value ? '***SET***' : '***EMPTY***');
    }

    // Check Daemon Status
    async function checkStatus() {
        console.log('[DEBUG] Checking daemon status');
        
        // Verify process is actually running via pidof
        const procCheck = await runCmd('pidof audio-bridge');
        if (procCheck.errno === 0 && procCheck.stdout.trim() !== '') {
            const runningPid = procCheck.stdout.trim().split(' ')[0];
            console.log(`[DEBUG] Process ${runningPid} is running`);
            daemonBadge.textContent = 'Running';
            daemonBadge.className = 'badge running';
            daemonPid.textContent = runningPid;
            return;
        }

        console.log('[DEBUG] No running daemon detected via pidof');
        daemonBadge.textContent = 'Stopped';
        daemonBadge.className = 'badge stopped';
        daemonPid.textContent = '--';
    }

    // Fetch Logs
    async function fetchLogs() {
        console.log('[DEBUG] Fetching logs from /data/local/tmp/audio_bridge.log');
        const res = await runCmd('tail -n 25 /data/local/tmp/audio_bridge.log');

        if (res.errno === 0) {
            const logContent = res.stdout || "No logs available yet.";
            logOutput.textContent = logContent;
            console.log(`[DEBUG] Logs fetched successfully, ${logContent.length} characters`);
        } else {
            console.error('[ERROR] Failed to fetch logs:', res.stderr);
            logOutput.textContent = "Log file not found or unreadable.";
        }

        // Auto scroll to bottom
        const terminal = document.querySelector('.terminal');
        if (terminal) {
            terminal.scrollTop = terminal.scrollHeight;
            console.log('[DEBUG] Terminal scrolled to bottom');
        } else {
            console.warn('[WARN] Terminal element not found for auto-scroll');
        }
    }

    // Test Server Connection
    btnTest.addEventListener('click', async () => {
        console.log('[DEBUG] Test connection button clicked');
        const host = hostInput.value.trim();
        const port = portInput.value.trim();
        const token = tokenInput.value.trim();

        console.log(`[DEBUG] Test parameters - Host: ${host}, Port: ${port}, Token: ${token ? '***PROVIDED***' : '***MISSING***'}`);

        if (!host || !port) {
            console.warn('[WARN] Test failed - missing host or port');
            showResult('error', 'Please enter a valid Host and Port.');
            return;
        }

        btnTest.disabled = true;
        btnTest.textContent = 'Testing...';
        showResult('hidden', '');

        // Run the daemon with --check-server flag
        const cmd = `/system/bin/audio-bridge --host "${host}" --port ${port} --token "${token}" --check-server`;
        console.log(`[DEBUG] Executing test command: ${cmd.replace(token, '***HIDDEN***')}`);

        const res = await runCmd(cmd);
        console.log(`[DEBUG] Test command result - errno: ${res.errno}`);

        if (res.errno === 0) {
            console.log('[DEBUG] Connection test successful');
            showResult('success', 'Connection successful! TLS Handshake passed.');
        } else {
            const errorMsg = `Connection failed.\n${res.stderr || res.stdout || 'Check your IP/Port/Token.'}`;
            console.error('[ERROR] Connection test failed:', errorMsg);
            showResult('error', errorMsg);
        }

        btnTest.disabled = false;
        btnTest.textContent = 'Test Connection';
    });

    // Save & Restart Daemon
    btnSave.addEventListener('click', async () => {
        console.log('[DEBUG] Save button clicked');
        const host = hostInput.value.trim();
        const port = portInput.value.trim();
        const token = tokenInput.value.trim();

        console.log(`[DEBUG] Save parameters - Host: ${host}, Port: ${port}, Token: ${token ? '***PROVIDED***' : '***MISSING***'}`);

        if (!host || !port) {
            console.warn('[WARN] Save failed - missing host or port');
            showResult('error', 'Please enter a valid Host and Port.');
            return;
        }

        btnSave.disabled = true;
        btnSave.textContent = 'Saving...';

        // Write config
        const confContent = `HOST=${host}\nPORT=${port}\nTOKEN=${token}\n`;
        console.log('[DEBUG] Writing config file with content:\n', confContent.replace(token, '***HIDDEN***'));

        // Use printf to handle newlines correctly in shell
        const writeResult = await runCmd(`printf '${confContent}' > /data/local/tmp/audio_bridge.conf`);
        if (writeResult.errno !== 0) {
            console.error('[ERROR] Failed to write config file:', writeResult.stderr);
        } else {
            console.log('[DEBUG] Config file written successfully');
        }

        // Restart Daemon
        console.log('[DEBUG] Attempting to kill existing daemon process');
        const killResult = await runCmd('killall audio-bridge || kill -9 $(cat /data/local/tmp/audio_bridge.pid 2>/dev/null)');
        console.log(`[DEBUG] Kill command result - errno: ${killResult.errno}`);

        console.log('[DEBUG] Removing PID file');
        await runCmd('rm -f /data/local/tmp/audio_bridge.pid');

        console.log('[DEBUG] Starting daemon via service script');
        // Run service.sh in background to handle SELinux context safely
        const startResult = await runCmd('nohup sh /data/adb/modules/audio_bridge/service.sh >/dev/null 2>&1 &');
        console.log(`[DEBUG] Start daemon result - errno: ${startResult.errno}`);

        showResult('success', 'Configuration saved. Daemon restarted!');
        console.log('[DEBUG] Save operation completed');

        setTimeout(() => {
            btnSave.disabled = false;
            btnSave.textContent = 'Save & Restart';
            console.log('[DEBUG] Checking status and fetching logs after save');
            checkStatus();
            fetchLogs();
            showResult('hidden', '');
        }, 2000);
    });

    btnRefreshLogs.addEventListener('click', () => {
        console.log('[DEBUG] Manual refresh requested');
        fetchLogs();
        checkStatus();
    });

    function showResult(type, message) {
        console.log(`[DEBUG] Showing result - Type: ${type}, Message: ${message}`);
        testResult.className = `test-result ${type}`;
        testResult.textContent = message;
    }

    // Initialization
    console.log('[DEBUG] Starting initialization sequence');
    await loadConfig();
    await checkStatus();
    await fetchLogs();
    console.log('[DEBUG] Initialization complete');

    // Auto-refresh logs every 5 seconds
    const intervalId = setInterval(() => {
        console.log('[DEBUG] Auto-refresh triggered (5 second interval)');
        checkStatus();
        fetchLogs();
    }, 5000);

    console.log('[DEBUG] Auto-refresh interval set with ID:', intervalId);

    // Optional: Cleanup interval on page unload
    window.addEventListener('beforeunload', () => {
        console.log('[DEBUG] Page unloading, clearing auto-refresh interval');
        clearInterval(intervalId);
    });
});