const { app, BrowserWindow, Menu, Tray, dialog, ipcMain, nativeImage, session } = require('electron');
const { execFile } = require('node:child_process');
const { randomUUID } = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { promisify } = require('node:util');
const { SmtcBridge } = require('./smtc_bridge.cjs');
const { DesktopLyrics, isTimed } = require('./soundflow-lyrics.cjs');
const { SoundFlowNative } = require('./soundflow-native.cjs');

const APP_URL = 'https://app.soundflow.local/index.html';
const localFiles = new Set(['index.html', 'app.css', 'redesign.css', 'tokens.css', 'refraction.js', 'app.bundle.js', 'amll-style.css']);
const savedDefaults = {
  device_name: os.hostname().slice(0, 32) || '音流',
  device_type: 4, enable_dlna: true, auto_online_lyrics: false, enabled: false,
  volume: 45, audio_sync_offset_ms: 0, tray: true,
};

let window, tray, core, smtc, settings, settingsFile, pollTimer, lyrics;
let quitting = false;
let visualDemand = false;
let senderTab = false;
let senderStage = false;
let lastSenderPoll = 0;
let lastSenderRender = 0;
let publishedSenderSnapshot = '';
const SENDER_RENDER_INTERVAL_MS = 200;
let target = null;
let sender = {};
let receiver = {};
let track = {};
let localLrc = '';
let lyricLookup;
let senderLrc = '';
let wavPath = '';
let codec = 'AAC_LC';
let trackRevision = 0;
let operation = Promise.resolve();
const logs = [];

function log(value) {
  logs.push(`${new Date().toISOString()}  ${String(value).trim().slice(0, 2000)}`);
  if (logs.length > 240) logs.shift();
}

function send(type, data) {
  if (window && !window.isDestroyed()) window.webContents.send('soundflow:event', { type, data });
}

function save() {
  const tmp = settingsFile + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(settings, null, 2));
  fs.renameSync(tmp, settingsFile);
}

function snapshot() {
  send('snapshot', { ...settings, ...receiver });
}

function publishSystem() {
  send('system', { tray: settings.tray, autostart: app.getLoginItemSettings().openAtLogin });
}

function openWindow() {
  window.show();
  window.focus();
}

function isUi(event) {
  return event.sender === window?.webContents && event.senderFrame?.url === APP_URL;
}

function native(method, args) { return core.request(method, args); }

function resolveUiDir() {
  for (const candidate of [
    path.join(__dirname, '../soundflow-ui'),
    path.join(__dirname, '../../soundflow-android/assets/ui'),
  ]) {
    if (fs.existsSync(path.join(candidate, 'index.html'))) return candidate;
  }
  return path.join(__dirname, '../soundflow-ui');
}

function resolveCoreBinary() {
  const exeName = process.platform === 'win32' ? 'soundflow-core.exe' : 'soundflow-core';
  const candidates = [
    process.env.SOUNDFLOW_CORE_BIN,
    process.resourcesPath ? path.join(process.resourcesPath, 'bin', exeName) : '',
    path.join(__dirname, '../../soundflow-android/build/desktop-target/x86_64-pc-windows-msvc/release', exeName),
    path.join(__dirname, '../../core/build-soundflow-win/rust/x86_64-pc-windows-gnu/release', exeName),
    path.join(__dirname, '../../soundflow-android/target/release', exeName),
  ].filter(Boolean);
  return candidates.find((file) => fs.existsSync(file)) || candidates[0];
}

async function senderRequest(method, params = {}) {
  const reply = await native('sender', { method, ...params });
  if (reply.error) throw new Error(reply.error);
  return reply.result;
}

async function startReceiver() {
  const interfaces = await senderRequest('net.listInterfaces');
  const usable = interfaces.filter((entry) => !entry.isLoopback && !entry.ipv4.startsWith('169.254.'));
  const ip = sender.bindIp && usable.find((entry) => entry.ipv4 === sender.bindIp) || usable[0];
  if (!ip) throw new Error('未找到可用的局域网 IPv4 地址');
  const result = await native('receiver.start', {
    name: settings.device_name, device_type: settings.device_type,
    enable_dlna: settings.enable_dlna, volume: settings.volume, interface: ip.name, ip: ip.ipv4,
    uuid: settings.uuid, lyra: settings.uuid.replaceAll('-', '').slice(0, 8).toUpperCase(), media: settings.uuid,
  });
  receiver = {
    miplay_running: result.miplay_ready, dlna_running: result.dlna_ready,
    last_error: result.last_error, endpoint: `${result.endpoint}/8899`,
    session_active: false, playing: false,
  };
  settings.enabled = true;
}

async function stopReceiver() {
  await native('receiver.stop');
  receiver = {};
  track = {};
  lyricLookup?.abort();
  localLrc = '';
  settings.enabled = false;
  if (!senderStage) send('playing', false);
  send('track', { revision: ++trackRevision });
}

async function configure(name, type, enableDlna, autoLyrics, toggle) {
  const restart = settings.enabled && !toggle &&
    (name !== settings.device_name || type !== settings.device_type || Boolean(enableDlna) !== settings.enable_dlna);
  if (restart) await stopReceiver();
  Object.assign(settings, {
    device_name: typeof name === 'string' && name.trim() ? Array.from(name.trim()).slice(0, 32).join('') : savedDefaults.device_name,
    device_type: [2, 4, 5, 16, 18].includes(type) ? type : 4,
    enable_dlna: Boolean(enableDlna), auto_online_lyrics: Boolean(autoLyrics),
  });
  try {
    if (restart || toggle) await startReceiver();
    else if (settings.auto_online_lyrics) refreshLyrics();
    else lyricLookup?.abort();
  } catch (error) {
    receiver.last_error = error.message;
    log(error.message);
  } finally {
    save();
    snapshot();
  }
}

function setPlaying(playing, session = receiver.session_active) {
  receiver.playing = playing;
  receiver.session_active = session;
  if (!senderStage) send('playing', playing);
}

function publishTrack() {
  if (senderStage) return;
  send('track', { ...track, lyrics: localLrc || track.lyrics, revision: trackRevision, mode: track.protocol === 'dlna' ? 'DLNA' : '小米妙播' });
}

async function publishLyricCount() {
  send('lyricCount', (await lyrics.files()).length);
}

function refreshLyrics() {
  lyricLookup?.abort();
  if (!track.title) return;
  const current = track;
  const revision = trackRevision;
  const lookup = new AbortController();
  lyricLookup = lookup;
  (async () => {
    const cached = await lyrics.read(current);
    let text = cached;
    if (!text && settings.auto_online_lyrics && !current.lyrics && !lookup.signal.aborted) {
      const timeout = AbortSignal.timeout(6000);
      text = await lyrics.lookup(current, AbortSignal.any([lookup.signal, timeout]));
    }
    if (!text || revision !== trackRevision || lookup.signal.aborted || localLrc) return;
    if (!cached) {
      await lyrics.write(current, text);
      await publishLyricCount();
    }
    if (revision !== trackRevision || lookup.signal.aborted || localLrc) return;
    localLrc = text;
    publishTrack();
  })().catch((error) => { if (!lookup.signal.aborted) log(`歌词读取失败 ${error.name}`); });
}

function receiverEvent(event) {
  const protocol = event.protocol === 'dlna' ? 'dlna' : 'miplay';
  if (!['progress', 'audio_presentation_clock'].includes(event.event)) log(`${protocol} ${event.event} ${event.stage || event.message || ''}`);
  switch (event.event) {
    case 'stream_started':
    case 'audio_output_signal_started':
      receiver.protocol = protocol;
      setPlaying(true, true);
      break;
    case 'playback_state':
      if (event.playing || (event.session_active && !receiver.playing)) receiver.protocol = protocol;
      if (receiver.protocol === protocol) setPlaying(event.playing === true, event.session_active ?? receiver.session_active);
      break;
    case 'media_channel_closed':
    case 'stream_stopped':
      if (receiver.protocol === protocol) {
        setPlaying(false, false);
        track = {};
        lyricLookup?.abort();
        localLrc = '';
        trackRevision++;
        publishTrack();
      }
      break;
    case 'media_info': {
      const changed = event.title !== track.title || event.artist !== track.artist || track.protocol !== protocol;
      if (changed) { trackRevision++; localLrc = ''; }
      track = {
        title: event.title, artist: event.artist, artwork: event.artwork, protocol,
        lyrics: isTimed(event.lyrics) ? event.lyrics : '', lyricLine: event.lyric_line,
        album: event.album, durationMs: event.duration_ms, positionMs: event.position_ms ?? track.positionMs ?? 0,
      };
      publishTrack();
      if (changed) refreshLyrics();
      break;
    }
    case 'progress':
      if (receiver.protocol === protocol) {
        track.positionMs = event.position_ms;
        if (!senderStage) send('position', track.positionMs);
      }
      return;
    case 'source_info': receiver.source_name = event.source_name; break;
    case 'audio_format': receiver.format = `${event.codec} · ${event.sample_rate} Hz`; break;
    case 'volume': receiver.volume = event.percent; break;
    case 'error': receiver.last_error = event.message || event.error; break;
    default: return;
  }
  snapshot();
}

function publishSenderState() {
  send('senderEvent', {
    type: 'state',
    data: {
      ...sender, codec, hasProjection: true, notificationAccessGranted: true,
      captureBackend: 'wasapi_process',
      shizukuAvailable: false,
      shizukuGranted: false,
      muteLocalApp: Boolean(sender.muteLocalApp),
      muteLocalSupported: Boolean(sender.muteLocalSupported),
      muteLocalHint: sender.muteLocalHint || '',
      targetAppUid: target?.uid ?? -1, targetAppPkg: target?.packageName ?? '',
      targetAppName: target?.label ?? '',
      track: sender.track ? { ...sender.track, hasLyrics: Boolean(senderLrc || sender.track.hasLyrics) } : undefined,
    },
  });
  if (senderStage) {
    send('track', { ...sender.track, lyrics: senderLrc || sender.track?.lyrics, revision: 'sender', mode: '全屋发送' });
    send('playing', sender.multiroomRunning && !sender.track?.paused);
  }
}

let pollInFlight = false;
async function poll() {
  if (pollInFlight || quitting) return;
  pollInFlight = true;
  try {
    const shouldPollSender = senderTab || sender.multiroomRunning || Date.now() - lastSenderPoll > 700;
    const data = await native('poll', { sender: shouldPollSender });
    for (const event of data.receiver) receiverEvent(event);
    if (data.sender) {
      lastSenderPoll = Date.now();
      sender = data.sender.data;
      // Real-time audio and control events keep their existing polling cadence.
      // Device counters can change each poll, so bound full renderer updates separately.
      if (lastSenderPoll - lastSenderRender >= SENDER_RENDER_INTERVAL_MS) {
        lastSenderRender = lastSenderPoll;
        const snapshot = JSON.stringify(sender);
        if (snapshot !== publishedSenderSnapshot) {
          publishedSenderSnapshot = snapshot;
          publishSenderState();
        }
      }
      for (const event of data.sender.events) {
        if (event.type === 'spectrum') {
          if (visualDemand && senderStage && sender.multiroomRunning && Array.isArray(event.levels) && event.levels.length >= 4) {
            const lv = event.levels;
            const n = lv.length;
            const bEnd = Math.max(1, Math.floor(n / 4));
            const mEnd = Math.max(bEnd + 1, Math.floor((n * 3) / 4));
            let bass = 0, mid = 0, high = 0, level = 0;
            for (let i = 0; i < n; i++) {
              const v = Math.max(0, Math.min(1, Number(lv[i]) || 0));
              if (v > level) level = v;
              if (i < bEnd) bass = Math.max(bass, v);
              else if (i < mEnd) mid = Math.max(mid, v);
              else high = Math.max(high, v);
            }
            send('audio', { level, bands: [bass, mid, high], preview: true });
          }
          if (!senderTab) continue;
        }
        if (event.type === 'remoteControl' && sender.sourceMode === 2) {
          smtc.dispatchAction(event.action, event.positionMs);
        }
        send('senderEvent', event);
      }
    }
    if (visualDemand && !senderStage && receiver.playing) {
      send('audio', { level: data.level, bands: data.bands.map((v) => v / 255), onsets: data.onsets });
    }
  } catch (error) {
    if (receiver.last_error !== error.message) {
      receiver.last_error = error.message;
      snapshot();
      log(error.message);
    }
  } finally {
    pollInFlight = false;
  }
}

async function pickFile(extension) {
  const { canceled, filePaths } = await dialog.showOpenDialog(window, {
    properties: ['openFile'], filters: [{ name: extension.toUpperCase(), extensions: [extension] }],
  });
  return canceled ? '' : filePaths[0];
}

async function readLrc(file) {
  if ((await fs.promises.stat(file)).size > 1024 * 1024) throw new Error('LRC 文件超过 1 MiB');
  return fs.promises.readFile(file, 'utf8');
}

async function updateTarget(params) {
  const apps = await senderRequest('app.listAudioApps');
  const selection = params.uid > 0 ? apps.find((item) => item.uid === params.uid && item.packageName === params.packageName) : null;
  if (params.uid > 0 && !selection) throw new Error('该音频进程已退出，请刷新音源应用列表');
  await senderRequest('source.setMode', { mode: 2, param: `process:${selection?.uid ?? 0}` });
  target = selection;
  await senderRequest('media.setMetadata', { title: '', artist: '', album: '', sourceName: target?.label ?? 'Windows 音频' });
  smtc.setTargetImage(target?.packageName ?? '');
  smtc.refreshNow();
  return true;
}

async function uiSenderRpc(raw) {
  const request = JSON.parse(raw);
  const { id, method } = request;
  try {
    let result;
    switch (method) {
      case 'source.setMode':
        if (request.mode === 1 && !wavPath) throw new Error('请先选择 WAV 文件');
        result = await senderRequest(method, { mode: request.mode, param: request.mode === 2 ? `process:${target?.uid ?? 0}` : request.mode === 1 ? wavPath : '' });
        break;
      case 'source.setTargetApp': result = await updateTarget(request); break;
      case 'source.setMuteLocalApp':
        result = await senderRequest(method, { muteLocalApp: Boolean(request.muteLocalApp) });
        break;
      case 'codec.set':
        if (!['AAC_LC', 'LPCM'].includes(request.codec)) throw new Error('无效的音频编码');
        if (sender.multiroomRunning) throw new Error('请先停止全屋发送再切换编码');
        codec = request.codec;
        result = true;
        break;
      case 'media.control':
        result = sender.sourceMode === 1
          ? await senderRequest('wav.pause', { paused: request.action === 'pause' })
          : smtc.dispatchAction(request.action);
        break;
      case 'multiroom.start':
        result = await senderRequest(method, { codec, sampleRate: request.sampleRate, basePlayDelayMs: request.basePlayDelayMs });
        break;
      case 'state.get':
        sender = await senderRequest(method);
        publishSenderState();
        return send('senderReply', { id, result: true });
      case 'app.listAudioApps':
        result = await senderRequest(method);
        break;
      case 'net.listInterfaces': case 'net.setBindIp': case 'discovery.refresh':
      case 'device.addManual': case 'device.remove': case 'multiroom.addDevice': case 'multiroom.removeDevice':
      case 'multiroom.stop': case 'sync.setBaseDelay': case 'sync.setDeviceBias': case 'volume.setDevice':
      case 'volume.setMaster': case 'wav.seek':
        result = await senderRequest(method, request);
        break;
      default: throw new Error('不支持的发送台命令');
    }
    send('senderReply', { id, result });
    if (method !== 'app.listAudioApps' && method !== 'net.listInterfaces') {
      sender = await senderRequest('state.get');
      publishSenderState();
    }
  } catch (error) { send('senderReply', { id, error: error.message }); }
}

async function firewall() {
  const command = `
    $ErrorActionPreference = 'Stop'
    $ports = Get-NetFirewallRule -PolicyStore ActiveStore -Enabled True -Direction Inbound |
      Get-NetFirewallPortFilter | Select-Object Protocol, LocalPort
    $profiles = Get-NetFirewallProfile -PolicyStore ActiveStore | Select-Object Name, Enabled, DefaultInboundAction
    @{ ports = @($ports); profiles = @($profiles) } | ConvertTo-Json -Depth 5 -Compress
  `;
  const { stdout } = await promisify(execFile)('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', command], { windowsHide: true, timeout: 8000 });
  const status = JSON.parse(stdout);
  const ports = [['TCP', 8899], ['UDP', 5355], ['TCP', 8901], ['UDP', 1900]];
  const lines = ports.map(([protocol, port]) => {
    const matches = status.ports.filter((rule) =>
      [protocol, protocol === 'TCP' ? '6' : '17'].includes(String(rule.Protocol)) &&
      [rule.LocalPort].flat().some((value) => String(value) === String(port)));
    return `${protocol} ${port} · ${matches.length ? `存在 ${matches.length} 条启用的入站端口规则` : '无显式入站端口规则'}`;
  });
  send('logSheet', [...lines, '', ...status.profiles.map((p) => `${p.Name} · ${p.Enabled ? '已启用' : '已关闭'}`)].join('\n'));
}

async function handle(method, args) {
  switch (method) {
    case 'onJsReady':
      snapshot();
      publishSystem();
      publishSenderState();
      await publishLyricCount();
      return;
    case 'toggleReceiver':
      if (settings.enabled) { await stopReceiver(); save(); snapshot(); }
      else await configure(...args, true);
      return;
    case 'applyConfig': await configure(...args, false); return;
    case 'controlAction': await native('receiver.control', { action: args[0] }); return;
    case 'setVolume':
      settings.volume = Math.max(0, Math.min(100, Number(args[0]) || 0));
      if (settings.enabled) await native('receiver.control', { action: 'volume', value: settings.volume });
      save();
      return;
    case 'setAudioSyncOffset':
      settings.audio_sync_offset_ms = Math.max(-400, Math.min(400, Number(args[0]) || 0));
      await native('receiver.syncOffset', { value: settings.audio_sync_offset_ms });
      save();
      return;
    case 'setRippleSensitivity': await native('receiver.sensitivity', { value: args[0] }); return;
    case 'setVisualDemand':
      visualDemand = Boolean(args[0] || args[1]);
      await native('receiver.visual', { onsets: Boolean(args[0] && args[1]) });
      return;
    case 'senderRpcAsync': await uiSenderRpc(args[0]); return;
    case 'setSenderTabActive': senderTab = args[0] === true; return;
    case 'pickSenderWav': {
      const file = await pickFile('wav');
      if (!file) return;
      await senderRequest('source.setMode', { mode: 1, param: file });
      wavPath = file;
      senderLrc = '';
      try { senderLrc = await readLrc(file.replace(/\.wav$/i, '.lrc')); } catch {}
      return;
    }
    case 'pickSenderLrc': {
      const file = await pickFile('lrc');
      if (!file) return;
      const text = await readLrc(file);
      await senderRequest('media.loadLrcFile', { path: file });
      senderLrc = text;
      publishSenderState();
      return;
    }
    case 'importLocalLrc': {
      const file = await pickFile('lrc');
      if (!file) return;
      if (!track.title) throw new Error('请先播放需要绑定歌词的曲目');
      const imported = await readLrc(file);
      lyricLookup?.abort();
      await lyrics.write(track, imported);
      localLrc = imported;
      publishTrack();
      await publishLyricCount();
      return;
    }
    case 'clearLyricCache':
      lyricLookup?.abort();
      await lyrics.clear();
      localLrc = '';
      trackRevision++;
      publishTrack();
      await publishLyricCount();
      return;
    case 'openSenderLyricsStage':
      senderStage = !receiver.session_active;
      if (senderStage) publishSenderState();
      send('fullscreen');
      return;
    case 'closeSenderLyricsStage':
      if (senderStage) {
        senderStage = false;
        publishTrack();
        send('playing', Boolean(receiver.playing));
        snapshot();
      }
      return;
    case 'openAutoStartSettings':
      app.setLoginItemSettings({ openAtLogin: !app.getLoginItemSettings().openAtLogin });
      publishSystem();
      return;
    case 'setTrayEnabled': settings.tray = !settings.tray; save(); publishSystem(); return;
    case 'checkFirewall': await firewall(); return;
    case 'releasePortConflict':
      send('logSheet', `请关闭占用 8899 或 55982 端口的应用后重试。\n\n${logs.join('\n')}`);
      return;
    case 'fetchLogs': send('logSheet', logs.join('\n')); return;
    case 'exportLogs': {
      const { canceled, filePath } = await dialog.showSaveDialog(window, { defaultPath: 'SoundFlow-diagnostics.txt' });
      if (!canceled) await fs.promises.writeFile(filePath, logs.join('\n'), 'utf8');
      return;
    }
    case 'fullscreenChanged': window.setFullScreen(args[0] === true); return;
    case 'onUiState': case 'onRenderStats': case 'modalChanged': case 'themeApplied': return;
    default: return;
  }
}

app.setName('SoundFlow');
if (!app.requestSingleInstanceLock()) app.quit();
else {
  app.on('second-instance', () => { if (window) openWindow(); });
  app.whenReady().then(async () => {
    const appData = app.getPath('userData');
    fs.mkdirSync(appData, { recursive: true });
    settingsFile = path.join(appData, 'soundflow.json');
    lyrics = new DesktopLyrics(path.join(appData, 'lyrics'));
    try { settings = { ...savedDefaults, ...JSON.parse(fs.readFileSync(settingsFile, 'utf8')) }; }
    catch { settings = { ...savedDefaults }; }
    settings.uuid ||= randomUUID();
    const ui = resolveUiDir();
    const binary = resolveCoreBinary();
    core = new SoundFlowNative(binary, log);
    smtc = new SmtcBridge((current) => {
      if (sender.sourceMode !== 2) return;
      senderRequest('media.setMetadata', current.available ? {
        title: current.title, artist: current.artist, album: current.album,
        positionMs: current.positionMs, durationMs: current.durationMs,
      } : { title: '', artist: '', album: '' }).catch((error) => log(error.message));
    });
    smtc.start();
    core.ready.then(async () => {
      await native('receiver.syncOffset', { value: settings.audio_sync_offset_ms });
      await senderRequest('discovery.start');
      await senderRequest('source.setMode', { mode: 2, param: 'process:0' });
      await senderRequest('media.setRemoteControlHandledByHost', { handled: true });
      if (settings.enabled) await startReceiver();
    }).catch((error) => { receiver.last_error = error.message; log(error.message); })
      .finally(snapshot);

    const ses = session.fromPartition('persist:soundflow-ui');
    ses.setPermissionRequestHandler((_contents, _permission, callback) => callback(false));
    ses.setPermissionCheckHandler(() => false);
    ses.protocol.handle('https', async (request) => {
      const url = new URL(request.url);
      const filename = url.pathname.slice(1);
      if (url.origin !== 'https://app.soundflow.local' || !localFiles.has(filename) || request.method !== 'GET') return new Response(null, { status: 404 });
      const mime = { '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript' };
      return new Response(await fs.promises.readFile(path.join(ui, filename)), {
        headers: { 'Content-Type': mime[path.extname(filename)] + '; charset=utf-8' },
      });
    });
    window = new BrowserWindow({
      width: 1080, height: 760, minWidth: 320, minHeight: 560, title: '音流 · 全屋收发一体',
      webPreferences: { session: ses, preload: path.join(__dirname, 'soundflow-preload.cjs'),
        sandbox: true, contextIsolation: true, nodeIntegration: false },
    });
    window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
    window.webContents.on('will-navigate', (event) => event.preventDefault());
    window.on('minimize', (event) => { if (settings.tray) { event.preventDefault(); window.hide(); } });
    window.on('close', (event) => { if (!quitting && settings.tray) { event.preventDefault(); window.hide(); } });
    const icon = nativeImage.createFromPath(path.join(__dirname, 'soundflow.ico'));
    tray = new Tray(icon);
    tray.setToolTip('音流 · 全屋收发一体');
    tray.setContextMenu(Menu.buildFromTemplate([{ label: '打开音流', click: openWindow }, { label: '退出音流', click: () => app.quit() }]));
    tray.on('double-click', openWindow);
    ipcMain.on('soundflow:request', (event, method, args) => {
      if (!isUi(event)) return;
      operation = operation.then(() => handle(method, args)).catch((error) => { log(error.message); send('toast', error.message); });
    });
    pollTimer = setInterval(poll, 16);
    await window.loadURL(APP_URL);
  }).catch((error) => { log(error.message); dialog.showErrorBox('音流启动失败', error.message); app.quit(); });
}

app.on('window-all-closed', () => {
  if (quitting || !settings?.tray) app.quit();
});
app.on('before-quit', () => {
  quitting = true;
  clearInterval(pollTimer);
  smtc?.stop();
  core?.close();
});
