import React, { memo, useEffect, useRef, useState } from 'react';

interface DeviceState {
  id: string;
  name: string;
  ip: string;
  cmdPort: number;
  mac: string;
  deviceType: number;
  roomName: string;
  isManual: boolean;
  inGroup: boolean;
  cmdConnected: boolean;
  rtspStreaming: boolean;
  volume: number;
  delayBiasMs: number;
  reportedLatencyMs: number;
  syncResidualUs: number;
  syncPackets: number;
  audioPacketsSent: number;
  remoteVersion: string;
  statusText?: string;
}

interface EngineState {
  bindIp: string;
  discoveryRunning: boolean;
  multiroomRunning: boolean;
  timerServicePort: number;
  sessionTimeOffsetUs: number;
  sampleRate: number;
  basePlayDelayMs: number;
  codec: 'LPCM' | 'AAC_LC';
  masterVolume: number;
  sourceMode: number;
  track: {
    title: string;
    artist: string;
    album: string;
    durationMs: number;
    positionMs: number;
  };
  devices: DeviceState[];
}

interface NetIface {
  name: string;
  ipv4: string;
  isLoopback: boolean;
}

declare global {
  interface Window {
    miplayBridge?: {
      getSidecarConfig: () => Promise<{ port: number; token: string } | null>;
      openWavFile: () => Promise<string | null>;
      platform: string;
    };
    _androidMiPlayBridge?: {
      getSidecarConfigJson: () => string;
      openWavPicker: () => void;
    };
    LiquidGlassRefraction?: {
      refresh: () => void;
    };
  }
}

const DEFAULT_STATE: EngineState = {
  bindIp: '127.0.0.1',
  discoveryRunning: false,
  multiroomRunning: false,
  timerServicePort: 0,
  sessionTimeOffsetUs: 0,
  sampleRate: 48000,
  basePlayDelayMs: 350,
  codec: 'AAC_LC',
  masterVolume: 70,
  sourceMode: 0,
  track: { title: '全屋同步校准测试音', artist: 'MiPlay', album: '', durationMs: 0, positionMs: 0 },
  devices: [],
};

// 独立轻量化 6 柱声波组件：避免频谱跳动触发整页重渲染
const MiniWave = memo(function MiniWave({ levels, live }: { levels: number[]; live: boolean }) {
  const bars = [
    levels[1] ?? 0.12,
    levels[3] ?? 0.18,
    levels[6] ?? 0.25,
    levels[9] ?? 0.2,
    levels[12] ?? 0.15,
    levels[14] ?? 0.1,
  ];
  return (
    <div className={`mini-wave ${live ? 'live' : ''}`} aria-hidden="true">
      {bars.map((v, i) => (
        <div
          key={i}
          className="mini-wave-bar"
          style={{ height: `${live ? Math.max(16, Math.min(100, Math.round(v * 100))) : 20}%` }}
        />
      ))}
    </div>
  );
});

async function resolveSidecarConfig(): Promise<{ port: number; token: string } | null> {
  if (window._androidMiPlayBridge) {
    try {
      const parsed = JSON.parse(window._androidMiPlayBridge.getSidecarConfigJson());
      if (parsed && parsed.port > 0) return parsed;
    } catch {
      // retry
    }
  }
  if (window.miplayBridge) {
    try {
      const cfg = await window.miplayBridge.getSidecarConfig();
      if (cfg && cfg.port > 0) return cfg;
    } catch {
      // retry
    }
  }
  return null;
}

// 将底层协议状态转为普通用户一眼能懂的白话
function friendlyDeviceStatus(dev: DeviceState, multiroomRunning: boolean): { text: string; tone: 'idle' | 'live' | 'error' } {
  if (dev.rtspStreaming) {
    return { text: '正在同步播放', tone: 'live' };
  }
  if (!dev.inGroup) {
    return { text: '点击即可选中投送', tone: 'idle' };
  }
  if (!multiroomRunning) {
    return { text: '已选中 · 点击下方开始播放', tone: 'idle' };
  }
  const raw = dev.statusText || '';
  if (raw.includes('无应用层响应') || raw.includes('休眠')) {
    return { text: '设备未响应，请确认接收端在前台', tone: 'error' };
  }
  if (raw.includes('重置') || raw.includes('失败') || raw.includes('超时') || raw.includes('断开')) {
    return { text: raw.replace(/^8899\s*/, ''), tone: 'error' };
  }
  if (raw.includes('等待设备反连')) {
    return { text: '已发起连接，等待设备接收音频…', tone: 'idle' };
  }
  if (raw.includes('RTSP')) {
    return { text: '正在建立音频通道…', tone: 'idle' };
  }
  return { text: '正在连接设备…', tone: 'idle' };
}

// 精简线性图标集合（遵循 Apple 一致性规范）
const Icons = {
  Airplay: () => (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.1" strokeLinecap="round" strokeLinejoin="round">
      <path d="M5 17H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-1" />
      <polygon points="12 15 17 21 7 21 12 15" fill="currentColor" stroke="none" />
    </svg>
  ),
  Speaker: () => (
    <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.9" strokeLinecap="round" strokeLinejoin="round">
      <rect x="4" y="2" width="16" height="20" rx="3" />
      <circle cx="12" cy="14" r="4" />
      <line x1="12" y1="6" x2="12.01" y2="6" strokeWidth="2.6" />
    </svg>
  ),
  Check: () => (
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round">
      <polyline points="20 6 9 17 4 12" />
    </svg>
  ),
  Refresh: () => (
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <path d="M21 12a9 9 0 1 1-2.64-6.36" />
      <polyline points="21 3 21 9 15 9" />
    </svg>
  ),
  Plus: () => (
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round">
      <line x1="12" y1="5" x2="12" y2="19" />
      <line x1="5" y1="12" x2="19" y2="12" />
    </svg>
  ),
  Settings: () => (
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.9" strokeLinecap="round" strokeLinejoin="round">
      <circle cx="12" cy="12" r="3" />
      <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9c.26.6.85 1 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z" />
    </svg>
  ),
  Volume: () => (
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5" />
      <path d="M15.54 8.46a5 5 0 0 1 0 7.07" />
      <path d="M19.07 4.93a10 10 0 0 1 0 14.14" />
    </svg>
  ),
  Play: () => (
    <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
      <polygon points="6 4 20 12 6 20 6 4" />
    </svg>
  ),
  Stop: () => (
    <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
      <rect x="5" y="5" width="14" height="14" rx="2.5" />
    </svg>
  ),
  Theme: () => (
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <circle cx="12" cy="12" r="4" />
      <path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41" />
    </svg>
  ),
  Close: () => (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round">
      <line x1="18" y1="6" x2="6" y2="18" />
      <line x1="6" y1="6" x2="18" y2="18" />
    </svg>
  ),
};

export default function App() {
  const [wsConnected, setWsConnected] = useState(false);
  const [state, setState] = useState<EngineState>(DEFAULT_STATE);
  const [interfaces, setInterfaces] = useState<NetIface[]>([]);
  const [spectrum, setSpectrum] = useState<number[]>(Array(16).fill(0.08));
  const [selectedCodec, setSelectedCodec] = useState<'LPCM' | 'AAC_LC'>('AAC_LC');
  const [toast, setToast] = useState<{ text: string; warn?: boolean } | null>(null);

  // 弹出式液态玻璃 Sheet（同一时刻只开一个，遵循 Apple HIG）
  const [activeSheet, setActiveSheet] = useState<'none' | 'add' | 'settings'>('none');
  const [showDiagnostics, setShowDiagnostics] = useState(false);

  // 手动添加设备表单
  const [manualName, setManualName] = useState('');
  const [manualIp, setManualIp] = useState('192.168.5.84');
  const [manualPort, setManualPort] = useState('8899');

  // 主题模式（默认跟随系统，支持手动一键切换）
  const [theme, setTheme] = useState<'auto' | 'light' | 'dark'>('auto');

  const wsRef = useRef<WebSocket | null>(null);
  const rpcIdRef = useRef(1);
  const lastSpecTsRef = useRef(0);

  useEffect(() => {
    const root = document.documentElement;
    if (theme === 'auto') {
      root.removeAttribute('data-theme');
    } else {
      root.setAttribute('data-theme', theme);
    }
  }, [theme]);

  // 首次加载及 Sheet 状态变化后刷新 Liquid Glass 光学折射边界
  useEffect(() => {
    const t = setTimeout(() => {
      window.LiquidGlassRefraction?.refresh();
    }, 60);
    return () => clearTimeout(t);
  }, [activeSheet, wsConnected]);

  const callRpc = (method: string, params: Record<string, unknown> = {}) => {
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      setToast({ text: '正在连接本机音频引擎，请稍候…', warn: true });
      return;
    }
    const id = String(rpcIdRef.current++);
    ws.send(JSON.stringify({ id, method, ...params }));
  };

  useEffect(() => {
    let closed = false;
    let retryTimer: ReturnType<typeof setTimeout> | null = null;

    const connectLoop = async () => {
      if (closed) return;
      const cfg = await resolveSidecarConfig();
      if (closed) return;
      if (!cfg) {
        retryTimer = setTimeout(connectLoop, 500);
        return;
      }

      const ws = new WebSocket(`ws://127.0.0.1:${cfg.port}/?token=${cfg.token}`);
      wsRef.current = ws;

      ws.onopen = () => {
        if (closed) {
          ws.close();
          return;
        }
        setWsConnected(true);
        setToast(null);
        ws.send(JSON.stringify({ id: String(rpcIdRef.current++), method: 'state.get' }));
        ws.send(JSON.stringify({ id: String(rpcIdRef.current++), method: 'net.listInterfaces' }));
      };

      ws.onmessage = (ev) => {
        try {
          const msg = JSON.parse(ev.data);
          if (msg.type === 'state' && msg.data) {
            setState(msg.data);
          } else if (msg.type === 'spectrum' && Array.isArray(msg.levels)) {
            const now = performance.now();
            if (now - lastSpecTsRef.current >= 120) {
              lastSpecTsRef.current = now;
              setSpectrum(msg.levels);
            }
          } else if (msg.type === 'rpc' && Array.isArray(msg.result)) {
            setInterfaces(msg.result);
          } else if (msg.type === 'rpc' && msg.result && msg.result.devices) {
            setState(msg.result);
          }
        } catch {
          // ignore
        }
      };

      ws.onclose = () => {
        setWsConnected(false);
        if (!closed) retryTimer = setTimeout(connectLoop, 800);
      };

      ws.onerror = () => {
        try {
          ws.close();
        } catch {
          // ignore
        }
      };
    };

    connectLoop();
    return () => {
      closed = true;
      if (retryTimer) clearTimeout(retryTimer);
      wsRef.current?.close();
    };
  }, []);

  const selectedDevices = state.devices.filter((d) => d.inGroup);
  const streamingCount = state.devices.filter((d) => d.rtspStreaming).length;

  const handleTogglePlay = () => {
    if (state.multiroomRunning) {
      callRpc('multiroom.stop');
      setToast(null);
      return;
    }
    if (selectedDevices.length === 0) {
      if (state.devices.length === 0) {
        setActiveSheet('add');
        setToast({ text: '暂未发现设备，可直接输入音箱或手机 IP 添加', warn: true });
        return;
      }
      // 零门槛：未手动勾设备时自动勾选所有已发现设备
      for (const dev of state.devices) {
        callRpc('multiroom.addDevice', { deviceId: dev.id });
      }
      setToast({ text: `已自动选中 ${state.devices.length} 台设备并开始同步播放` });
    } else {
      setToast(null);
    }
    callRpc('multiroom.start', {
      codec: selectedCodec,
      sampleRate: 48000,
      basePlayDelayMs: state.basePlayDelayMs,
    });
  };

  const handlePickWav = async () => {
    if (window._androidMiPlayBridge) {
      window._androidMiPlayBridge.openWavPicker();
      return;
    }
    const file = await window.miplayBridge?.openWavFile();
    if (file) {
      callRpc('source.setMode', { mode: 1, param: file });
    }
  };

  const handleAddManual = (e: React.FormEvent) => {
    e.preventDefault();
    const ip = manualIp.trim();
    if (!ip) return;
    const port = Number(manualPort) || 8899;
    const name = manualName.trim() || `妙播设备 (${ip})`;
    callRpc('device.addManual', { name, ip, cmdPort: port });
    // 添加后自动勾选为投送目标，减少用户操作步骤
    const devId = `manual_${ip}_${port}`;
    setTimeout(() => {
      callRpc('multiroom.addDevice', { deviceId: devId });
    }, 80);
    setManualName('');
    setActiveSheet('none');
    setToast({ text: `已添加并选中「${name}」，点击下方按钮即可播放` });
  };

  const cycleTheme = () => {
    const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    if (theme === 'auto') setTheme(prefersDark ? 'light' : 'dark');
    else setTheme(theme === 'dark' ? 'light' : 'dark');
  };

  const sourceHint =
    state.sourceMode === 2
      ? '实时抓取本机所有播放中的声音（音乐 / 视频 / 游戏）'
      : state.sourceMode === 1
        ? `正在使用本地音频：${state.track.title}`
        : '内置柔和和弦测试音，最适合首次验证音箱出声与多房间对齐';

  return (
    <>
      {/* 功能层 1：顶部单行液态玻璃导航栏 */}
      <header className="lg-topbar glass glass-text">
        <div className="lg-brand">
          <div className="lg-brand-icon">
            <Icons.Airplay />
          </div>
          <div className="lg-brand-text">
            <span className="lg-brand-title">全屋妙播</span>
            <span className="lg-brand-sub">
              <span className={`status-dot ${wsConnected ? '' : 'offline'}`} />
              {wsConnected
                ? state.multiroomRunning
                  ? `已连接 ${streamingCount}/${selectedDevices.length} 台设备`
                  : `发现 ${state.devices.length} 台设备`
                : '引擎连接中…'}
            </span>
          </div>
        </div>

        <div className="lg-toolbar-actions">
          <button
            className="lg-chip-btn"
            onClick={() => {
              callRpc('discovery.refresh');
              setToast({ text: '正在重新搜索局域网内的妙播设备…' });
            }}
            title="重新搜索局域网设备"
          >
            <Icons.Refresh />
            <span className="btn-label">扫描</span>
          </button>
          <button
            className="lg-chip-btn"
            onClick={() => setActiveSheet('add')}
            title="手动输入 IP 添加设备"
          >
            <Icons.Plus />
            <span className="btn-label">添加设备</span>
          </button>
          <button className="lg-chip-btn icon-only" onClick={cycleTheme} title="切换浅色 / 深色外观">
            <Icons.Theme />
          </button>
          <button
            className="lg-chip-btn"
            onClick={() => setActiveSheet('settings')}
            title="音频品质、多设备延迟对齐与网络设置"
          >
            <Icons.Settings />
            <span className="btn-label">设置</span>
          </button>
        </div>
      </header>

      {/* 滚动边缘光学溶解过渡 */}
      <div className="scroll-edge" aria-hidden="true" />

      {/* 内容层：实体分组卡片，极简两步操作（选音源 -> 勾设备） */}
      <main className="lg-content">
        {toast && (
          <div className={`lg-toast ${toast.warn ? 'warn' : ''}`} role="status">
            <span>{toast.text}</span>
            <button className="lg-toast-close" onClick={() => setToast(null)} aria-label="关闭提示">
              ×
            </button>
          </div>
        )}

        {/* 第一步：声音来源 */}
        <section className="section-block">
          <div className="section-header-row">
            <span className="section-heading">声音来源</span>
            <span className="section-hint">选择要投送到音箱的声音</span>
          </div>
          <div className="content-card">
            <div className="lg-segmented" role="tablist" aria-label="声音来源">
              <button
                role="tab"
                className="lg-seg-btn"
                aria-selected={state.sourceMode === 2}
                onClick={() => callRpc('source.setMode', { mode: 2, param: '' })}
              >
                本机全部声音
              </button>
              <button
                role="tab"
                className="lg-seg-btn"
                aria-selected={state.sourceMode === 1}
                onClick={handlePickWav}
              >
                本地音频 WAV
              </button>
              <button
                role="tab"
                className="lg-seg-btn"
                aria-selected={state.sourceMode === 0}
                onClick={() => callRpc('source.setMode', { mode: 0, param: '' })}
              >
                测试音
              </button>
            </div>
            <div className="source-caption-row">
              <span>{sourceHint}</span>
              {state.sourceMode === 1 && (
                <button className="link-btn" onClick={handlePickWav}>
                  更换文件
                </button>
              )}
            </div>
          </div>
        </section>

        {/* 第二步：选择投送设备（点击卡片即勾选/热加入） */}
        <section className="section-block">
          <div className="section-header-row">
            <span className="section-heading">播放到这些设备</span>
            <span className="section-hint">
              {selectedDevices.length > 0
                ? `已选 ${selectedDevices.length} 台 · 支持播放中途随时增减`
                : '点选下方设备，支持多台同步播放'}
            </span>
          </div>

          {state.devices.length === 0 ? (
            <div className="empty-guide">
              <div className="empty-guide-icon">
                <Icons.Speaker />
              </div>
              <h3>正在搜索附近的小米妙播设备</h3>
              <p>
                请确保音箱、电视或接收端与当前设备在同一 Wi-Fi 下。如果路由器拦截了自动发现，也可以直接输入设备 IP 添加。
              </p>
              <div className="empty-guide-actions">
                <button
                  className="pill-action primary"
                  onClick={() => setActiveSheet('add')}
                >
                  <Icons.Plus />
                  手动输入 IP 添加
                </button>
                <button
                  className="pill-action"
                  onClick={() => callRpc('discovery.refresh')}
                >
                  <Icons.Refresh />
                  重新搜索
                </button>
              </div>
            </div>
          ) : (
            <div className="devices-wall">
              {state.devices.map((dev) => {
                const st = friendlyDeviceStatus(dev, state.multiroomRunning);
                return (
                  <div
                    key={dev.id}
                    className={`speaker-card ${dev.rtspStreaming ? 'streaming' : dev.inGroup ? 'selected' : ''}`}
                  >
                    <div
                      className="speaker-top"
                      role="checkbox"
                      aria-checked={dev.inGroup}
                      tabIndex={0}
                      onClick={() =>
                        callRpc(dev.inGroup ? 'multiroom.removeDevice' : 'multiroom.addDevice', {
                          deviceId: dev.id,
                        })
                      }
                      onKeyDown={(e) => {
                        if (e.key === 'Enter' || e.key === ' ') {
                          e.preventDefault();
                          callRpc(dev.inGroup ? 'multiroom.removeDevice' : 'multiroom.addDevice', {
                            deviceId: dev.id,
                          });
                        }
                      }}
                    >
                      <div className="speaker-avatar">
                        <Icons.Speaker />
                      </div>
                      <div className="speaker-meta">
                        <div className="speaker-name">{dev.name}</div>
                        <div className={`speaker-state-line ${st.tone}`}>
                          <span>{st.text}</span>
                        </div>
                      </div>
                      <div className="speaker-check" aria-hidden="true">
                        {dev.inGroup && <Icons.Check />}
                      </div>
                    </div>

                    <div className="vol-row">
                      <span className="vol-icon">
                        <Icons.Volume />
                      </span>
                      <input
                        className="vol-slider"
                        type="range"
                        min={0}
                        max={100}
                        value={dev.volume}
                        aria-label={`${dev.name} 音量`}
                        onChange={(e) =>
                          callRpc('volume.setDevice', {
                            deviceId: dev.id,
                            volume: Number(e.target.value),
                          })
                        }
                      />
                      <span className="vol-val">{dev.volume}%</span>
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </section>
      </main>

      {/* 功能层 2：底部悬浮液态玻璃播控胶囊（启用真实光学边缘折射） */}
      <div className="dock-wrap">
        <div className="dock-capsule glass glass-text" data-refract="0.85" data-refract-bezel="14">
          <div className="dock-left">
            <MiniWave levels={spectrum} live={state.multiroomRunning} />
            <div className="dock-info">
              <div className="dock-title">{state.track.title}</div>
              <div className="dock-sub">
                {state.multiroomRunning
                  ? streamingCount > 0
                    ? `正在向 ${streamingCount} 台设备同步播放`
                    : '正在等待设备响应…'
                  : selectedDevices.length > 0
                    ? `已准备投送到 ${selectedDevices.length} 台设备`
                    : '选择设备后点击右侧按钮开始'}
              </div>
            </div>
          </div>

          <div className="dock-vol">
            <span className="vol-icon" title="全屋统一主音量">
              <Icons.Volume />
            </span>
            <input
              className="vol-slider"
              type="range"
              min={0}
              max={100}
              value={state.masterVolume}
              aria-label="全屋主音量"
              onChange={(e) => callRpc('volume.setMaster', { volume: Number(e.target.value) })}
            />
            <span className="vol-val">{state.masterVolume}%</span>
          </div>

          <button
            className={`dock-play-btn ${state.multiroomRunning ? 'playing' : ''}`}
            onClick={handleTogglePlay}
          >
            {state.multiroomRunning ? (
              <>
                <Icons.Stop />
                <span>停止</span>
              </>
            ) : (
              <>
                <Icons.Play />
                <span>开始播放</span>
              </>
            )}
          </button>
        </div>
      </div>

      {/* 功能层 3：弹出式液态玻璃 Sheet — 手动添加设备 */}
      {activeSheet === 'add' && (
        <div className="sheet-backdrop" onClick={() => setActiveSheet('none')}>
          <div
            className="sheet-panel glass glass-text"
            onClick={(e) => e.stopPropagation()}
            role="dialog"
            aria-label="手动添加设备"
          >
            <div className="sheet-header">
              <h2>添加妙播设备</h2>
              <button className="lg-chip-btn icon-only" onClick={() => setActiveSheet('none')} aria-label="关闭">
                <Icons.Close />
              </button>
            </div>
            <form className="sheet-body" onSubmit={handleAddManual}>
              <div className="sheet-group">
                <div className="sheet-row">
                  <label className="sheet-row-title" htmlFor="ip-input">
                    设备局域网 IP 地址
                  </label>
                  <input
                    id="ip-input"
                    className="sheet-input"
                    placeholder="例如 192.168.5.84"
                    value={manualIp}
                    onChange={(e) => setManualIp(e.target.value)}
                    required
                    autoFocus
                  />
                  <span className="sheet-row-desc">可在音箱/平板的 Wi-Fi 详情或接收端界面查看 IP</span>
                </div>
                <div className="sheet-row">
                  <label className="sheet-row-title" htmlFor="name-input">
                    备注名称（选填）
                  </label>
                  <input
                    id="name-input"
                    className="sheet-input"
                    placeholder="例如 客厅小爱、卧室平板"
                    value={manualName}
                    onChange={(e) => setManualName(e.target.value)}
                  />
                </div>
                <div className="sheet-row">
                  <label className="sheet-row-title" htmlFor="port-input">
                    控制端口（默认 8899）
                  </label>
                  <input
                    id="port-input"
                    className="sheet-input"
                    value={manualPort}
                    onChange={(e) => setManualPort(e.target.value)}
                  />
                </div>
              </div>
              <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 10 }}>
                <button type="button" className="pill-action" onClick={() => setActiveSheet('none')}>
                  取消
                </button>
                <button type="submit" className="pill-action primary">
                  添加并选中
                </button>
              </div>
            </form>
          </div>
        </div>
      )}

      {/* 功能层 4：弹出式液态玻璃 Sheet — 偏好设置、多设备毫秒对齐与诊断 */}
      {activeSheet === 'settings' && (
        <div className="sheet-backdrop" onClick={() => setActiveSheet('none')}>
          <div
            className="sheet-panel glass glass-text"
            onClick={(e) => e.stopPropagation()}
            role="dialog"
            aria-label="偏好设置与专业同步"
          >
            <div className="sheet-header">
              <h2>设置与多房间同步</h2>
              <button className="lg-chip-btn icon-only" onClick={() => setActiveSheet('none')} aria-label="关闭">
                <Icons.Close />
              </button>
            </div>
            <div className="sheet-body">
              {/* 音质与网络 */}
              <div className="sheet-group">
                <div className="sheet-row">
                  <div className="sheet-row-top">
                    <span className="sheet-row-title">音频传输格式</span>
                    <span className="section-hint">{state.multiroomRunning ? '停止播放后可切换' : '推荐 AAC'}</span>
                  </div>
                  <select
                    className="sheet-select"
                    value={selectedCodec}
                    disabled={state.multiroomRunning}
                    onChange={(e) => setSelectedCodec(e.target.value as 'LPCM' | 'AAC_LC')}
                  >
                    <option value="AAC_LC">高兼容流畅模式 · AAC-LC（推荐，兼容全部音箱）</option>
                    <option value="LPCM">无损低延迟模式 · LPCM（局域网带宽要求高）</option>
                  </select>
                </div>

                <div className="sheet-row">
                  <div className="sheet-row-top">
                    <span className="sheet-row-title">全屋同步缓冲</span>
                    <strong className="tnum" style={{ fontSize: 13 }}>{state.basePlayDelayMs} ms</strong>
                  </div>
                  <input
                    className="vol-slider"
                    type="range"
                    min={150}
                    max={800}
                    step={10}
                    value={state.basePlayDelayMs}
                    onChange={(e) => callRpc('sync.setBaseDelay', { delayMs: Number(e.target.value) })}
                  />
                  <span className="sheet-row-desc">
                    数值越大抗 Wi-Fi 波动越稳，默认 350 ms 适合绝大多数家庭网络。
                  </span>
                </div>

                {interfaces.filter((i) => !i.isLoopback).length > 0 && (
                  <div className="sheet-row">
                    <div className="sheet-row-top">
                      <span className="sheet-row-title">本机通信网卡</span>
                      <span className="section-hint tnum">{state.bindIp}</span>
                    </div>
                    <select
                      className="sheet-select"
                      value={state.bindIp}
                      onChange={(e) => callRpc('net.setBindIp', { ip: e.target.value })}
                    >
                      {interfaces
                        .filter((i) => !i.isLoopback)
                        .map((i) => (
                          <option key={`${i.name}-${i.ipv4}`} value={i.ipv4}>
                            {i.name} ({i.ipv4})
                          </option>
                        ))}
                    </select>
                    <span className="sheet-row-desc">
                      若电脑装有虚拟机/VPN 网卡，请确保选择与音箱同一网段的局域网 IP。
                    </span>
                  </div>
                )}
              </div>

              {/* 单设备毫秒级延迟微调（多型号音箱回声消除） */}
              {selectedDevices.length > 0 && (
                <div className="sheet-group">
                  {selectedDevices.map((dev) => (
                    <div key={dev.id} className="sheet-row">
                      <div className="sheet-row-top">
                        <span className="sheet-row-title">{dev.name} · 硬件延迟微调</span>
                        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                          <strong className="tnum" style={{ fontSize: 13 }}>
                            {dev.delayBiasMs >= 0 ? `+${dev.delayBiasMs}` : dev.delayBiasMs} ms
                          </strong>
                          {dev.delayBiasMs !== 0 && (
                            <button
                              className="link-btn"
                              onClick={() => callRpc('sync.setDeviceBias', { deviceId: dev.id, delayBiasMs: 0 })}
                            >
                              归零
                            </button>
                          )}
                        </div>
                      </div>
                      <input
                        className="vol-slider"
                        type="range"
                        min={-300}
                        max={300}
                        step={1}
                        value={dev.delayBiasMs}
                        onChange={(e) =>
                          callRpc('sync.setDeviceBias', {
                            deviceId: dev.id,
                            delayBiasMs: Number(e.target.value),
                          })
                        }
                      />
                    </div>
                  ))}
                </div>
              )}

              {/* 折叠式专业协议诊断 */}
              <div className="sheet-group">
                <div className="sheet-row">
                  <div className="sheet-row-top">
                    <span className="sheet-row-title">专业协议诊断信息</span>
                    <button className="link-btn" onClick={() => setShowDiagnostics((v) => !v)}>
                      {showDiagnostics ? '收起' : '展开查看'}
                    </button>
                  </div>
                  {showDiagnostics && (
                    <div className="diag-box">
                      <div>
                        本机 IP: {state.bindIp} · 编码: {state.codec} · TimerPort: {state.timerServicePort || '-'}
                      </div>
                      {state.devices.length === 0 ? (
                        <div style={{ marginTop: 6 }}>暂无设备记录</div>
                      ) : (
                        state.devices.map((d) => (
                          <div key={d.id} style={{ marginTop: 8, paddingTop: 6, borderTop: '0.5px solid var(--separator)' }}>
                            <strong>{d.name}</strong> ({d.ip}:{d.cmdPort}) {d.remoteVersion ? `v${d.remoteVersion}` : ''}
                            <br />
                            8899: {d.cmdConnected ? '已连' : '未连'} · RTSP: {d.rtspStreaming ? '推流中' : '待机'} · 校时包: {d.syncPackets} · 发包: {d.audioPacketsSent}
                            {d.statusText ? (
                              <>
                                <br />
                                详情: {d.statusText}
                              </>
                            ) : null}
                          </div>
                        ))
                      )}
                    </div>
                  )}
                </div>
              </div>
            </div>
          </div>
        </div>
      )}
    </>
  );
}
