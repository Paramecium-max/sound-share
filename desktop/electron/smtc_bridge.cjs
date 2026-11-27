const { spawn } = require('child_process');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');

/**
 * Windows 原生 SMTC (GlobalSystemMediaTransportControlsSessionManager) + 酷我音乐 PC 版 (KwMusic v9) 双模桥接：
 * - 优先通过 Windows Runtime SMTC 读取现代播放器（网易云 / QQ 音乐 / Spotify / 浏览器）元数据与进度
 * - 当目标或活跃播放器为未注册 SMTC 的酷我音乐 PC 版 (kwmusic.exe) 时，自动通过酷我实时行为日志 act.log
 *   与 Kw_Song_Cache 缓存毫秒级提取当前歌名、歌手、专辑、MUSICRID 与播放进度，并通过 Win32 媒体键支持反向控制
 */
const PS_SMTC_SCRIPT = `
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::InputEncoding = [System.Text.Encoding]::UTF8

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation\`1'
})[0]
function Await-WinRT($WinRtTask, $ResultType) {
  $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
  $netTask = $asTask.Invoke($null, @($WinRtTask))
  $netTask.Wait(1500) | Out-Null
  return $netTask.Result
}

[Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager,Windows.Media.Control,ContentType=WindowsRuntime] | Out-Null
$mgr = Await-WinRT ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])
if (-not $mgr) { exit 0 }

$reader = [Console]::In
$pendingRead = $reader.ReadLineAsync()
$lastSig = ''
$nextPoll = [DateTime]::UtcNow
$targetImage = ''
function Selected-Session {
  if (-not $targetImage) { return $mgr.GetCurrentSession() }
  $targetName = [IO.Path]::GetFileNameWithoutExtension($targetImage)
  return $mgr.GetSessions() | Where-Object {
    $sourceId = [string]$_.SourceAppUserModelId
    $sourceName = [IO.Path]::GetFileNameWithoutExtension($sourceId)
    $sourceName -eq $targetName -or
      $sourceId -match ('(?:^|[.])' + [regex]::Escape($targetName) + '(?:_|!|$)')
  } | Select-Object -First 1
}

while ($true) {
  if ($pendingRead.IsCompleted) {
    $cmdLine = $pendingRead.Result
    if ($null -eq $cmdLine) { break }
    $pendingRead = $reader.ReadLineAsync()
    $parts = $cmdLine.Trim().Split(' ')
    $act = $parts[0]
    if ($act -eq 'TARGET') {
      $targetImage = if ($parts.Length -ge 2) {
        [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($parts[1]))
      } else { '' }
      $lastSig = ''
    } elseif ($act -eq 'REFRESH') {
      $lastSig = ''
    }
    $session = Selected-Session
    if ($session) {
      try {
        if ($act -eq 'PAUSE') { [void](Await-WinRT ($session.TryPauseAsync()) ([bool])) }
        elseif ($act -eq 'RESUME') { [void](Await-WinRT ($session.TryPlayAsync()) ([bool])) }
        elseif ($act -eq 'NEXT') { [void](Await-WinRT ($session.TrySkipNextAsync()) ([bool])) }
        elseif ($act -eq 'PREVIOUS') { [void](Await-WinRT ($session.TrySkipPreviousAsync()) ([bool])) }
        elseif ($act -eq 'SEEK' -and $parts.Length -ge 2) {
          $ms = [int64]$parts[1]
          [void](Await-WinRT ($session.TryChangePlaybackPositionAsync($ms * 10000)) ([bool]))
        }
      } catch {}
    }
    $nextPoll = [DateTime]::UtcNow
  }

  $now = [DateTime]::UtcNow
  if ($now -ge $nextPoll) {
    $nextPoll = $now.AddMilliseconds(1200)
    $session = Selected-Session
    if ($session) {
      try {
        $props = Await-WinRT ($session.TryGetMediaPropertiesAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties])
        $tl = $session.GetTimelineProperties()
        $pb = $session.GetPlaybackInfo()
        $title = if ($props -and $props.Title) { [string]$props.Title } else { '' }
        $artist = if ($props -and $props.Artist) { [string]$props.Artist } else { '' }
        $album = if ($props -and $props.AlbumTitle) { [string]$props.AlbumTitle } else { '' }
        $durMs = if ($tl -and $tl.EndTime) { [int64][Math]::Max(0, $tl.EndTime.TotalMilliseconds) } else { 0 }
        $posMs = if ($tl -and $tl.Position) { [int64][Math]::Max(0, $tl.Position.TotalMilliseconds) } else { 0 }
        $status = if ($pb) { [string]$pb.PlaybackStatus } else { 'Unknown' }
        $payload = @{
          available = $true
          targetImage = $targetImage
          title = $title
          artist = $artist
          album = $album
          durationMs = $durMs
          positionMs = $posMs
          paused = ($status -ne 'Playing')
          status = $status
        } | ConvertTo-Json -Compress
        if ($payload -ne $lastSig) {
          $lastSig = $payload
          [Console]::Out.WriteLine("SMTC " + $payload)
          [Console]::Out.Flush()
        }
      } catch {}
    } else {
      $empty = @{ available = $false; targetImage = $targetImage } | ConvertTo-Json -Compress
      if ($empty -ne $lastSig) {
        $lastSig = $empty
        [Console]::Out.WriteLine("SMTC " + $empty)
        [Console]::Out.Flush()
      }
    }
  }
  Start-Sleep -Milliseconds 80
}
`;

function locateKuwoDataRoot() {
  const candidates = [
    process.env.ALLUSERSPROFILE ? path.join(process.env.ALLUSERSPROFILE, 'Kuwodata', 'kwmusic2013') : '',
    'B:\\ProgramData\\Kuwodata\\kwmusic2013',
    'C:\\ProgramData\\Kuwodata\\kwmusic2013',
    'D:\\ProgramData\\Kuwodata\\kwmusic2013',
  ].filter(Boolean);
  return candidates.find((dir) => fs.existsSync(dir)) || '';
}

function locateKuwoActLog(kuwoDataRoot) {
  const candidates = [];
  if (kuwoDataRoot) {
    try {
      const cfgPath = path.join(kuwoDataRoot, 'Conf', 'User', 'config.ini');
      if (fs.existsSync(cfgPath)) {
        const raw = fs.readFileSync(cfgPath);
        const text = new TextDecoder('gb18030').decode(raw);
        const m = text.match(/^htmlpath=(.+)$/im);
        if (m && m[1]) {
          // htmlpath=<KwMusic>\bin\html\webdata\netsong -> ascend 3 levels to <KwMusic>\bin
          const binDir = path.resolve(m[1].trim(), '..', '..', '..');
          candidates.push(path.join(binDir, 'Log', 'act.log'));
        }
      }
    } catch {}
  }
  candidates.push(
    'C:\\Documents\\酷我音乐 v9.1.0.0\\KwMusic\\Bin\\Log\\act.log',
    'C:\\Documents\\酷我音乐 v9.1.0.0\\KwMusic\\bin\\Log\\act.log',
    'C:\\Program Files (x86)\\kuwo\\kwmusic\\Bin\\Log\\act.log',
    'C:\\Program Files\\kuwo\\kwmusic\\Bin\\Log\\act.log'
  );
  return candidates.find((p) => fs.existsSync(p)) || '';
}

function parseActLogTimestampMs(line) {
  // Format: [][act][11-27 16:24:08.338][KNF]: ...
  const m = line.match(/\[(\d{2})-(\d{2})\s+(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?\]/);
  if (!m) return 0;
  const now = new Date();
  const month = Number(m[1]) - 1;
  const day = Number(m[2]);
  const hour = Number(m[3]);
  const min = Number(m[4]);
  const sec = Number(m[5]);
  const ms = m[6] ? Number(m[6].padEnd(3, '0')) : 0;
  const d = new Date(now.getFullYear(), month, day, hour, min, sec, ms);
  return d.getTime();
}

class SmtcBridge {
  constructor(onTrackUpdate) {
    this.onTrackUpdate = onTrackUpdate;
    this.proc = null;
    this.available = false;
    this.smtcAvailable = false;
    this.kuwoActive = false;
    this.lastTrack = { available: false };
    this.stdoutBuf = '';
    this.targetImage = '';
    this.kuwoTimer = null;
    this.kuwoDataRoot = '';
    this.kuwoActLogPath = '';
    this.kuwoState = {
      key: '',
      title: '',
      artist: '',
      album: '',
      kuwoMusicId: '',
      durationMs: 0,
      anchorEpochMs: 0,
    };
  }

  start() {
    if (process.platform !== 'win32' || this.proc) return;
    this.kuwoDataRoot = locateKuwoDataRoot();
    this.kuwoActLogPath = locateKuwoActLog(this.kuwoDataRoot);
    this.kuwoTimer = setInterval(() => {
      this._pollKuwoFallback().catch(() => {});
    }, 750);

    try {
      const encoded = Buffer.from(PS_SMTC_SCRIPT, 'utf16le').toString('base64');
      this.proc = spawn(
        'powershell.exe',
        ['-NoProfile', '-NonInteractive', '-WindowStyle', 'Hidden', '-EncodedCommand', encoded],
        { stdio: ['pipe', 'pipe', 'ignore'], windowsHide: true }
      );
      this.proc.stdin.on('error', () => {});
      this.proc.stdout.on('data', (chunk) => {
        this.stdoutBuf += chunk.toString('utf8');
        const lines = this.stdoutBuf.split('\n');
        this.stdoutBuf = lines.pop() || '';
        for (const raw of lines) {
          const line = raw.trim();
          if (!line.startsWith('SMTC ')) continue;
          try {
            const parsed = JSON.parse(line.slice(5));
            if ((parsed?.targetImage || '') !== this.targetImage) continue;
            this.smtcAvailable = Boolean(parsed && parsed.available && parsed.title);
            if (this.smtcAvailable) {
              this.kuwoActive = false;
              this.available = true;
              this.lastTrack = parsed;
              this.onTrackUpdate?.(this.lastTrack);
            } else if (!this.kuwoActive) {
              this._pollKuwoFallback().catch(() => {});
            }
          } catch {}
        }
      });
      this.proc.on('error', () => {
        this.smtcAvailable = false;
        this.proc = null;
      });
      this.proc.on('exit', () => {
        this.smtcAvailable = false;
        this.proc = null;
      });
    } catch {
      this.smtcAvailable = false;
      this.proc = null;
    }
  }

  async _readLatestKuwoSongId(referenceTimeMs) {
    if (!this.kuwoDataRoot) return '';
    const cacheDir = path.join(this.kuwoDataRoot, 'Res', 'cache', 'Kw_Song_Cache');
    try {
      const names = await fsp.readdir(cacheDir);
      let newest = null;
      for (const name of names) {
        if (!name.toLowerCase().endsWith('.dat')) continue;
        const full = path.join(cacheDir, name);
        try {
          const st = await fsp.stat(full);
          if (st.isFile() && (!newest || st.mtimeMs > newest.mtimeMs)) {
            newest = { full, mtimeMs: st.mtimeMs };
          }
        } catch {}
      }
      if (!newest) return '';
      // Strictly bind Kw_Song_Cache MUSICRID only when modified within +-15s of the ACT:LYRIC event
      // to prevent cross-attaching a stale/pre-buffered song's MUSICRID to the current track.
      if (referenceTimeMs > 0) {
        if (Math.abs(newest.mtimeMs - referenceTimeMs) > 15 * 1000) return '';
      } else if (Date.now() - newest.mtimeMs > 60 * 1000) {
        return '';
      }
      const text = await fsp.readFile(newest.full, 'ascii');
      const m = text.match(/MUSICRID=MUSIC_(\d+)/i);
      return m ? m[1] : '';
    } catch {
      return '';
    }
  }

  async _pollKuwoFallback() {
    if (this.smtcAvailable) return;
    const targetLower = (this.targetImage || '').toLowerCase();
    const allowKuwo = !targetLower || targetLower.includes('kwmusic');
    if (!allowKuwo) {
      if (this.kuwoActive) {
        this.kuwoActive = false;
        this.available = false;
        this.lastTrack = { available: false, targetImage: this.targetImage };
        this.onTrackUpdate?.(this.lastTrack);
      }
      return;
    }

    if (!this.kuwoDataRoot) this.kuwoDataRoot = locateKuwoDataRoot();
    if (!this.kuwoActLogPath || !fs.existsSync(this.kuwoActLogPath)) {
      this.kuwoActLogPath = locateKuwoActLog(this.kuwoDataRoot);
    }
    if (!this.kuwoActLogPath) return;

    let st;
    try {
      st = await fsp.stat(this.kuwoActLogPath);
    } catch {
      return;
    }
    // Only treat Kuwo as active if act.log was touched within the last 6 hours
    if (Date.now() - st.mtimeMs > 6 * 3600 * 1000) return;

    const readLen = Math.min(st.size, 24576);
    if (readLen <= 0) return;
    const fh = await fsp.open(this.kuwoActLogPath, 'r');
    let tailBuf;
    try {
      tailBuf = Buffer.allocUnsafe(readLen);
      await fh.read(tailBuf, 0, readLen, st.size - readLen);
    } finally {
      await fh.close();
    }

    const tailText = new TextDecoder('gb18030').decode(tailBuf);
    const lines = tailText.split(/\r?\n/);

    let foundTitle = '';
    let foundArtist = '';
    let foundLogMs = 0;
    for (let i = lines.length - 1; i >= 0; i--) {
      const line = lines[i];
      if (!line.includes('ACT:LYRIC|')) continue;
      const mTitle = line.match(/\|N:([^|]*)/);
      const mArtist = line.match(/\|A:([^|]*)/);
      if (mTitle && mTitle[1].trim()) {
        const candMs = parseActLogTimestampMs(line);
        // Check if this track immediately failed playback (RESFAIL/NoRes + ENDTYPE:1/PT:0)
        let failedImmediately = false;
        let sawNoRes = false;
        for (let j = i + 1; j < Math.min(lines.length, i + 12); j++) {
          const nextLine = lines[j];
          if (nextLine.includes('ACT:LYRIC|')) break;
          const nextMs = parseActLogTimestampMs(nextLine);
          if (candMs > 0 && nextMs > 0 && (nextMs - candMs) > 3000) break;
          if (nextLine.includes('ACT:RESFAIL|') || nextLine.includes('FAILED:NORES')) {
            sawNoRes = true;
          }
          if (sawNoRes && nextLine.includes('ACT:PLAY_MUSIC|') && (nextLine.includes('|PT:0|') || nextLine.includes('|ENDTYPE:1|'))) {
            failedImmediately = true;
            break;
          }
        }
        if (failedImmediately) continue;
        foundTitle = mTitle[1].trim();
        foundArtist = mArtist ? mArtist[1].trim() : '';
        foundLogMs = candMs;
        break;
      }
    }

    if (!foundTitle) return;
    const kuwoMusicId = await this._readLatestKuwoSongId(foundLogMs);
    const trackKey = `${foundTitle}|${foundArtist}|${foundLogMs}`;

    if (this.kuwoState.key !== trackKey) {
      const now = Date.now();
      const validLogAnchor = foundLogMs > 0 && now >= foundLogMs && (now - foundLogMs) < 30 * 60 * 1000;
      this.kuwoState = {
        key: trackKey,
        title: foundTitle,
        artist: foundArtist,
        album: '',
        kuwoMusicId,
        durationMs: 0,
        anchorEpochMs: validLogAnchor ? foundLogMs : now,
      };
    } else if (kuwoMusicId && !this.kuwoState.kuwoMusicId) {
      this.kuwoState.kuwoMusicId = kuwoMusicId;
    }

    let posMs = Math.max(0, Date.now() - this.kuwoState.anchorEpochMs);
    if (this.kuwoState.durationMs > 5000 && posMs >= this.kuwoState.durationMs) {
      // Handle single-track repeat gracefully
      posMs = posMs % this.kuwoState.durationMs;
    }

    this.kuwoActive = true;
    this.available = true;
    this.lastTrack = {
      available: true,
      source: 'kwmusic',
      targetImage: this.targetImage,
      title: this.kuwoState.title,
      artist: this.kuwoState.artist,
      album: this.kuwoState.album,
      kuwoMusicId: this.kuwoState.kuwoMusicId,
      durationMs: this.kuwoState.durationMs,
      positionMs: posMs,
      paused: false,
      status: 'Playing',
    };
    this.onTrackUpdate?.(this.lastTrack);
  }

  updateEnrichedMeta({ album, durationMs }) {
    if (!this.kuwoActive) return;
    if (album && !this.kuwoState.album) this.kuwoState.album = album;
    if (durationMs > 0 && !this.kuwoState.durationMs) this.kuwoState.durationMs = durationMs;
  }

  setTargetImage(image) {
    this.targetImage = image;
    this.smtcAvailable = false;
    this.kuwoActive = false;
    this.available = false;
    this.lastTrack = { available: false };
    this.proc?.stdin.write(`TARGET ${Buffer.from(image, 'utf8').toString('base64')}\n`);
    this._pollKuwoFallback().catch(() => {});
  }

  refreshNow() {
    this.proc?.stdin.write('REFRESH\n');
    this._pollKuwoFallback().catch(() => {});
  }

  dispatchAction(action, positionMs = 0) {
    if (this.smtcAvailable && this.proc && !this.proc.killed) {
      let cmd = null;
      switch (action) {
        case 'pause': cmd = 'PAUSE'; break;
        case 'resume': cmd = 'RESUME'; break;
        case 'next': cmd = 'NEXT'; break;
        case 'previous': cmd = 'PREVIOUS'; break;
        case 'seek': cmd = `SEEK ${Math.max(0, Math.floor(Number(positionMs) || 0))}`; break;
        default: return false;
      }
      try {
        this.proc.stdin.write(`${cmd}\n`);
        return true;
      } catch {
        return false;
      }
    }
    return false;
  }

  stop() {
    if (this.kuwoTimer) {
      clearInterval(this.kuwoTimer);
      this.kuwoTimer = null;
    }
    if (!this.proc) return;
    const p = this.proc;
    this.proc = null;
    this.available = false;
    this.smtcAvailable = false;
    try { p.stdin.end(); } catch {}
    try { p.kill(); } catch {}
  }
}

module.exports = { SmtcBridge };
