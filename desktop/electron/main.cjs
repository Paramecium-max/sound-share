const { app, BrowserWindow, ipcMain, dialog } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

let mainWindow = null;
let sidecarProc = null;
let sidecarConfig = null;

function resolveSidecarBinary() {
  const exeName = process.platform === 'win32' ? 'miplay_core_sidecar.exe' : 'miplay_core_sidecar';
  const candidates = [
    path.join(__dirname, '../../core/build', exeName),
    path.join(__dirname, '../../core/build/Release', exeName),
    path.join(process.resourcesPath || '', 'bin', exeName),
  ];
  for (const p of candidates) {
    if (fs.existsSync(p)) return p;
  }
  return null;
}

function startSidecar() {
  return new Promise((resolve, reject) => {
    const bin = resolveSidecarBinary();
    if (!bin) {
      reject(new Error('未找到 miplay_core_sidecar 二进制文件，请先编译 core/'));
      return;
    }

    sidecarProc = spawn(bin, [], { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
    let stdoutBuf = '';
    let resolved = false;

    sidecarProc.stdout.on('data', (chunk) => {
      stdoutBuf += chunk.toString('utf8');
      const lines = stdoutBuf.split('\n');
      stdoutBuf = lines.pop() || '';
      for (const line of lines) {
        if (line.startsWith('READY ')) {
          try {
            sidecarConfig = JSON.parse(line.slice(6).trim());
            resolved = true;
            resolve(sidecarConfig);
          } catch (e) {
            reject(e);
          }
        }
      }
    });

    sidecarProc.stderr.on('data', (chunk) => {
      console.error('[sidecar]', chunk.toString('utf8'));
    });

    sidecarProc.on('exit', (code) => {
      if (!resolved) reject(new Error(`Sidecar 异常退出 code=${code}`));
    });

    setTimeout(() => {
      if (!resolved) reject(new Error('等待 miplay_core_sidecar 启动超时'));
    }, 8000);
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

  const devUrl = process.env.VITE_DEV_SERVER_URL;
  if (devUrl) {
    mainWindow.loadURL(devUrl);
  } else {
    mainWindow.loadFile(path.join(__dirname, '../dist/index.html'));
  }
}

ipcMain.handle('sidecar:getConfig', async () => sidecarConfig);
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
  if (sidecarProc) {
    sidecarProc.kill('SIGTERM');
    sidecarProc = null;
  }
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
