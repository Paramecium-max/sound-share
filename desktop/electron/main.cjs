const { app, BrowserWindow, ipcMain, dialog } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

const MAX_AUTO_RESTARTS = 3;
const STARTUP_TIMEOUT_MS = 8000;

let mainWindow = null;
let sidecarProc = null;
let sidecarConfig = null;
let isQuitting = false;
let restartCount = 0;
let restartTimer = null;
let sidecarStatus = {
  state: 'starting', // 'starting' | 'ready' | 'restarting' | 'failed'
  error: null,
  attempts: 0,
};

function notifySidecarStatus() {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('sidecar:status', sidecarStatus);
  }
}

function setStatus(next) {
  sidecarStatus = { ...sidecarStatus, ...next };
  notifySidecarStatus();
}

function resolveSidecarBinary() {
  const exeName = process.platform === 'win32' ? 'miplay_core_sidecar.exe' : 'miplay_core_sidecar';
  const candidates = [
    path.join(process.resourcesPath || '', 'bin', exeName),
    path.join(__dirname, '../../core/build', exeName),
    path.join(__dirname, '../../core/build/Release', exeName),
  ];
  for (const p of candidates) {
    if (p && fs.existsSync(p)) return p;
  }
  return null;
}

function stopSidecarProcess(proc) {
  if (!proc || proc.exitCode !== null || proc.signalCode !== null) return Promise.resolve();
  return new Promise((resolve) => {
    const force = setTimeout(() => {
      try { proc.kill('SIGKILL'); } catch {}
    }, 4000);
    force.unref();
    proc.once('close', () => {
      clearTimeout(force);
      resolve();
    });
    if (process.platform === 'win32') {
      proc.stdin.end();
    } else {
      try { proc.kill('SIGTERM'); } catch {}
    }
  });
}

function scheduleRestart(reason) {
  if (isQuitting) return;
  if (restartCount >= MAX_AUTO_RESTARTS) {
    setStatus({
      state: 'failed',
      error: `${reason}（已自动重启 ${MAX_AUTO_RESTARTS} 次仍失败）`,
      attempts: restartCount,
    });
    return;
  }
  restartCount += 1;
  const delayMs = 600 * restartCount;
  setStatus({
    state: 'restarting',
    error: `${reason}，${(delayMs / 1000).toFixed(1)}s 后尝试第 ${restartCount} 次重启…`,
    attempts: restartCount,
  });
  if (restartTimer) clearTimeout(restartTimer);
  restartTimer = setTimeout(() => {
    restartTimer = null;
    startSidecar().catch((err) => {
      console.error('[main] Sidecar restart failed:', err.message);
    });
  }, delayMs);
}

async function startSidecar() {
  if (sidecarProc) {
    const old = sidecarProc;
    sidecarProc = null;
    await stopSidecarProcess(old);
  }
  return new Promise((resolve, reject) => {
    if (isQuitting) {
      reject(new Error('应用正在退出'));
      return;
    }
    const bin = resolveSidecarBinary();
    if (!bin) {
      const msg = '未找到 miplay_core_sidecar 可执行文件，请检查安装包完整性或先编译 core/';
      sidecarConfig = null;
      setStatus({ state: 'failed', error: msg });
      reject(new Error(msg));
      return;
    }

    sidecarConfig = null;
    setStatus({ state: restartCount > 0 ? 'restarting' : 'starting', error: null });

    let proc;
    try {
      const win = process.platform === 'win32';
      proc = spawn(bin, win ? ['--exit-on-stdin-close'] : [], {
        stdio: [win ? 'pipe' : 'ignore', 'pipe', 'pipe'],
        windowsHide: true,
        env: win ? {
          ...process.env,
          SOUNDSHARE_PLAYBACK_RECOVERY_FILE: path.join(app.getPath('userData'), 'playback-route.txt'),
        } : process.env,
      });
      proc.stdin?.on('error', () => {});
    } catch (spawnErr) {
      const msg = `启动核心引擎失败: ${spawnErr.message}`;
      setStatus({ state: 'failed', error: msg });
      reject(new Error(msg));
      return;
    }

    sidecarProc = proc;
    let stdoutBuf = '';
    let settled = false;
    let ready = false;

    const failStartup = (reason, shouldKill) => {
      if (settled) return;
      settled = true;
      clearTimeout(startupTimer);
      if (sidecarProc === proc) sidecarProc = null;
      sidecarConfig = null;
      if (shouldKill) void stopSidecarProcess(proc);
      setStatus({ state: 'failed', error: reason });
      reject(new Error(reason));
    };

    const startupTimer = setTimeout(() => {
      failStartup('等待 miplay_core_sidecar 启动超时（8 秒未输出 READY）', true);
    }, STARTUP_TIMEOUT_MS);

    proc.stdout.on('data', (chunk) => {
      stdoutBuf += chunk.toString('utf8');
      const lines = stdoutBuf.split('\n');
      stdoutBuf = lines.pop() || '';
      for (const rawLine of lines) {
        const line = rawLine.trim();
        if (line.startsWith('READY ')) {
          try {
            const parsed = JSON.parse(line.slice(6).trim());
            if (!parsed || typeof parsed.port !== 'number' || parsed.port <= 0 || !parsed.token) {
              throw new Error('READY 配置缺少有效 port/token');
            }
            if (!settled) {
              settled = true;
              ready = true;
              clearTimeout(startupTimer);
              sidecarConfig = parsed;
              restartCount = 0;
              setStatus({ state: 'ready', error: null, attempts: 0 });
              resolve(parsed);
            }
          } catch (e) {
            failStartup(`Sidecar READY 配置格式无效: ${e.message}`, true);
          }
        }
      }
    });

    proc.stderr.on('data', (chunk) => {
      console.error('[sidecar]', chunk.toString('utf8'));
    });

    // SS-13: 显式捕获 spawn/exec error（例如权限不足或二进制不可执行）
    proc.on('error', (err) => {
      const msg = `核心引擎进程启动错误: ${err.message}`;
      console.error('[main]', msg);
      if (!settled) {
        failStartup(msg, false);
      } else if (sidecarProc === proc) {
        sidecarProc = null;
        sidecarConfig = null;
        scheduleRestart(msg);
      }
    });

    proc.on('exit', (code, signal) => {
      const desc = signal ? `signal=${signal}` : `code=${code}`;
      if (sidecarProc !== proc && settled) return;
      if (sidecarProc === proc) sidecarProc = null;
      sidecarConfig = null;
      if (!settled) {
        failStartup(`Sidecar 启动期间异常退出 (${desc})`, false);
      } else if (ready && !isQuitting) {
        // 运行中意外退出：立即清空失效配置并执行有限次重启
        scheduleRestart(`核心引擎运行中意外退出 (${desc})`);
      }
    });
  });
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1120,
    height: 760,
    minWidth: 920,
    minHeight: 640,
    backgroundColor: '#0b0f17',
    titleBarStyle: process.platform === 'darwin' ? 'hiddenInset' : 'default',
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  mainWindow.webContents.on('did-finish-load', () => {
    notifySidecarStatus();
  });

  const devUrl = process.env.VITE_DEV_SERVER_URL;
  if (devUrl) {
    mainWindow.loadURL(devUrl);
  } else {
    mainWindow.loadFile(path.join(__dirname, '../dist/index.html'));
  }
}

ipcMain.handle('sidecar:getConfig', async () => sidecarConfig);
ipcMain.handle('sidecar:getStatus', async () => sidecarStatus);
ipcMain.handle('sidecar:restart', async () => {
  restartCount = 0;
  try {
    await startSidecar();
    return { ok: true, status: sidecarStatus };
  } catch (e) {
    return { ok: false, error: e.message, status: sidecarStatus };
  }
});
ipcMain.handle('dialog:openWav', async () => {
  const res = await dialog.showOpenDialog(mainWindow, {
    title: '选择本地无损 WAV 音频文件',
    filters: [{ name: 'WAV Audio', extensions: ['wav'] }],
    properties: ['openFile'],
  });
  return res.canceled ? null : res.filePaths[0];
});

app.whenReady().then(async () => {
  try {
    await startSidecar();
  } catch (err) {
    console.error('[main] Sidecar start failed:', err.message);
  }
  createWindow();
});

app.on('before-quit', () => {
  isQuitting = true;
  if (restartTimer) {
    clearTimeout(restartTimer);
    restartTimer = null;
  }
  if (sidecarProc) {
    const p = sidecarProc;
    sidecarProc = null;
    void stopSidecarProcess(p);
  }
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
