const { spawn } = require('child_process');

/**
 * Windows 原生 SMTC (GlobalSystemMediaTransportControlsSessionManager) 桥接：
 * - 自动读取网易云 / QQ 音乐 / Spotify / 浏览器正在播放的歌名、歌手、专辑、时长与进度
 * - 精准执行接收端反向发来的 pause / resume / next / previous / seek
 * - 非 Windows 平台或系统 SMTC 不可用时自动优雅降级，由 C++ 侧 Win32 SendInput / playerctl 兜底
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

class SmtcBridge {
  constructor(onTrackUpdate) {
    this.onTrackUpdate = onTrackUpdate;
    this.proc = null;
    this.available = false;
    this.lastTrack = { available: false };
    this.stdoutBuf = '';
    this.targetImage = '';
  }

  start() {
    if (process.platform !== 'win32' || this.proc) return;
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
            this.available = Boolean(parsed && parsed.available);
            this.lastTrack = parsed || { available: false };
            this.onTrackUpdate?.(this.lastTrack);
          } catch {}
        }
      });
      this.proc.on('error', () => {
        this.available = false;
        this.proc = null;
      });
      this.proc.on('exit', () => {
        this.available = false;
        this.proc = null;
      });
    } catch {
      this.available = false;
      this.proc = null;
    }
  }

  setTargetImage(image) {
    this.targetImage = image;
    this.available = false;
    this.lastTrack = { available: false };
    this.proc?.stdin.write(`TARGET ${Buffer.from(image, 'utf8').toString('base64')}\n`);
  }

  refreshNow() {
    this.proc?.stdin.write('REFRESH\n');
  }

  dispatchAction(action, positionMs = 0) {
    if (!this.proc || !this.available || this.proc.killed) return false;
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

  stop() {
    if (!this.proc) return;
    const p = this.proc;
    this.proc = null;
    this.available = false;
    try { p.stdin.end(); } catch {}
    try { p.kill(); } catch {}
  }
}

module.exports = { SmtcBridge };
