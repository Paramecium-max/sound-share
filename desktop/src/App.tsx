import React, { memo, useEffect, useRef, useState } from 'react';
import { attachButtonHighlights, attachLiquidDock } from './liquid-glass/liquid-dock.mjs';

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
  loopbackSupported?: boolean;
  timerServicePort: number;
  sessionTimeOffsetUs: number;
  sampleRate: number;
  basePlayDelayMs: number;
  codec: 'LPCM' | 'AAC_LC';
  masterVolume: number;
  sourceMode: number;
  sourceParam?: string;
  lastError?: string;
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

interface LoopbackSource {
  id: string;
  label: string;
}

interface SidecarStatus {
  state: 'starting' | 'ready' | 'restarting' | 'failed';
  error: string | null;
  attempts: number;
}

type TabKey = 'devices' | 'source' | 'settings';

declare global {
  interface Window {
    miplayBridge?: {
      getSidecarConfig: () => Promise<{ port: number; token: string } | null>;
      getSidecarStatus?: () => Promise<SidecarStatus>;
      restartSidecar?: () => Promise<{ ok: boolean; error?: string; status: SidecarStatus }>;
      onSidecarStatus?: (cb: (status: SidecarStatus) => void) => () => void;
      openWavFile: () => Promise<string | null>;
      platform: string;
    };
    _androidMiPlayBridge?: {
      getSidecarConfigJson: () => string;
      openWavPicker: () => void;
    };
    LiquidGlassRefraction?: {
      refresh: () => void;
      setExtraDisplacement: (el: Element, pixels: number) => void;
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

function friendlyDeviceStatus(
  dev: DeviceState,
  multiroomRunning: boolean
): { text: string; tone: 'idle' | 'live' | 'error' } {
  if (dev.rtspStreaming) return { text: '正在同步播放', tone: 'live' };
  if (!dev.inGroup) return { text: '未连接 · 点击即可投送', tone: 'idle' };
  if (!multiroomRunning) return { text: '已选中 · 点击顶栏开始播放', tone: 'idle' };
  const raw = dev.statusText || '';
  if (raw.includes('无应用层响应') || raw.includes('休眠')) {
    return { text: '设备未响应，请确认接收端在前台', tone: 'error' };
  }
  if (raw.includes('重置') || raw.includes('失败') || raw.includes('超时') || raw.includes('断开')) {
    return { text: raw.replace(/^8899\s*/, ''), tone: 'error' };
  }
  if (raw.includes('等待设备反连')) return { text: '已发起连接，等待设备接收音频…', tone: 'idle' };
  if (raw.includes('RTSP')) return { text: '正在建立音频通道…', tone: 'idle' };
  return { text: '正在连接设备…', tone: 'idle' };
}

const Icons = {
  Airplay: () => (
    <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.1" strokeLinecap="round" strokeLinejoin="round">
      <path d="M5 17H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-1" />
      <polygon points="12 15 17 21 7 21 12 15" fill="currentColor" stroke="none" />
    </svg>
  ),
  Speaker: () => (
    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.9" strokeLinecap="round" strokeLinejoin="round">
      <rect x="4" y="2" width="16" height="20" rx="3" />
      <circle cx="12" cy="14" r="4" />
      <line x1="12" y1="6" x2="12.01" y2="6" strokeWidth="2.6" />
    </svg>
  ),
  Wave: () => (
    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
      <path d="M4 12v0M8 8v8M12 4v16M16 8v8M20 12v0" />
    </svg>
  ),
  Settings: () => (
    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.9" strokeLinecap="round" strokeLinejoin="round">
      <circle cx="12" cy="12" r="3" />
      <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9c.26.6.85 1 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z" />
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
  Volume: () => (
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5" />
      <path d="M15.54 8.46a5 5 0 0 1 0 7.07" />
      <path d="M19.07 4.93a10 10 0 0 1 0 14.14" />
    </svg>
  ),
  Play: () => (
    <svg width="15" height="15" viewBox="0 0 24 24" fill="currentColor">
      <polygon points="6 4 20 12 6 20 6 4" />
    </svg>
  ),
  Stop: () => (
    <svg width="13" height="13" viewBox="0 0 24 24" fill="currentColor">
      <rect x="5" y="5" width="14" height="14" rx="2.5" />
    </svg>
  ),
  Close: () => (
    <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round">
      <line x1="18" y1="6" x2="6" y2="18" />
      <line x1="6" y1="6" x2="18" y2="18" />
    </svg>
  ),
};

export default function App() {
  const [activeTab, setActiveTab] = useState<TabKey>('devices');
  const [wsConnected, setWsConnected] = useState(false);
  const [state, setState] = useState<EngineState>(DEFAULT_STATE);
  const [interfaces, setInterfaces] = useState<NetIface[]>([]);
  const [loopbackSources, setLoopbackSources] = useState<LoopbackSource[]>([]);
  const [selectedLoopback, setSelectedLoopback] = useState<string>('');
  const [sidecarStatus, setSidecarStatus] = useState<SidecarStatus | null>(null);
  const [spectrum, setSpectrum] = useState<number[]>(Array(16).fill(0.08));
  const [selectedCodec, setSelectedCodec] = useState<'LPCM' | 'AAC_LC'>('AAC_LC');
  const [toast, setToast] = useState<{ text: string; warn?: boolean } | null>(null);
  const [refreshingDiscovery, setRefreshingDiscovery] = useState(false);
  const [showAddSheet, setShowAddSheet] = useState(false);
  const [showDiagnostics, setShowDiagnostics] = useState(false);

  const [manualName, setManualName] = useState('');
  const [manualIp, setManualIp] = useState('192.168.5.84');
  const [manualPort, setManualPort] = useState('8899');

  const [theme, setTheme] = useState<'auto' | 'light' | 'dark'>(() => {
    const saved = localStorage.getItem('miplay.theme');
    return saved === 'light' || saved === 'dark' ? saved : 'auto';
  });

  const [glassTint, setGlassTint] = useState<number>(() => {
    const saved = parseFloat(localStorage.getItem('miplay.glassTint') ?? '0.35');
    return Number.isFinite(saved) ? Math.max(0, Math.min(1, saved)) : 0.35;
  });

  const wsRef = useRef<WebSocket | null>(null);
  const dockRef = useRef<HTMLElement | null>(null);
  const rpcIdRef = useRef(1);
  const lastSpecTsRef = useRef(0);
  const lastStateErrRef = useRef('');
  const pendingRpcRef = useRef<
    Map<
      string,
      {
        resolve: (val: unknown) => void;
        reject: (err: Error) => void;
        timer: ReturnType<typeof setTimeout>;
      }
    >
  >(new Map());

  // Subscribe to Electron sidecar health/crash status (SS-13)
  useEffect(() => {
    window.miplayBridge?.getSidecarStatus?.().then((st) => {
      if (st) setSidecarStatus(st);
    });
    const unsub = window.miplayBridge?.onSidecarStatus?.((st) => {
      setSidecarStatus(st);
      if (st.error) setToast({ text: st.error, warn: true });
    });
    return () => unsub?.();
  }, []);

  // Sync theme attribute
  useEffect(() => {
    const root = document.documentElement;
    if (theme === 'auto') root.removeAttribute('data-theme');
    else root.setAttribute('data-theme', theme);
    localStorage.setItem('miplay.theme', theme);
  }, [theme]);

  // Sync single --glass-tint variable (0.00 = true zero blur & zero tint)
  useEffect(() => {
    document.documentElement.style.setProperty('--glass-tint', glassTint.toFixed(2));
    localStorage.setItem('miplay.glassTint', glassTint.toFixed(2));
  }, [glassTint]);

  // Attach Liquid Glass SOP spring water-drop physics (dock + all buttons)
  useEffect(() => {
    attachButtonHighlights();
    const dockEl = dockRef.current;
    const detachDock = dockEl
      ? attachLiquidDock(dockEl, (tab: string) => {
          if (tab === 'devices' || tab === 'source' || tab === 'settings') {
            setActiveTab(tab);
          }
        })
      : undefined;
    const t = setTimeout(() => window.LiquidGlassRefraction?.refresh(), 40);
    return () => {
      clearTimeout(t);
      detachDock?.();
    };
  }, []);

  const rejectAllPending = (reason: string) => {
    for (const [, entry] of pendingRpcRef.current) {
      clearTimeout(entry.timer);
      entry.reject(new Error(reason));
    }
    pendingRpcRef.current.clear();
  };

  // SS-14: 按请求 ID 关联响应，完整处理 error、false、超时和断线取消
  const callRpc = <T = unknown>(
    method: string,
    params: Record<string, unknown> = {},
    opts: { expectTrue?: boolean; silentError?: boolean; timeoutMs?: number } = {}
  ): Promise<T> => {
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      const msg = sidecarStatus?.error || '正在连接本机音频引擎，请稍候…';
      if (!opts.silentError) setToast({ text: msg, warn: true });
      return Promise.reject(new Error(msg));
    }
    const id = String(rpcIdRef.current++);
    const timeoutMs = opts.timeoutMs ?? 8000;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        pendingRpcRef.current.delete(id);
        const err = new Error(`操作超时 (${method})`);
        if (!opts.silentError) setToast({ text: err.message, warn: true });
        reject(err);
      }, timeoutMs);
      pendingRpcRef.current.set(id, {
        resolve: (val) => {
          if (opts.expectTrue && val === false) {
            const err = new Error(`操作未成功 (${method})`);
            if (!opts.silentError) setToast({ text: err.message, warn: true });
            reject(err);
            return;
          }
          resolve(val as T);
        },
        reject: (err) => {
          if (!opts.silentError) setToast({ text: err.message, warn: true });
          reject(err);
        },
        timer,
      });
      try {
        ws.send(JSON.stringify({ id, method, ...params }));
      } catch (e) {
        clearTimeout(timer);
        pendingRpcRef.current.delete(id);
        const err = e instanceof Error ? e : new Error('发送请求失败');
        if (!opts.silentError) setToast({ text: err.message, warn: true });
        reject(err);
      }
    });
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
        callRpc<EngineState>('state.get', {}, { silentError: true })
          .then((st) => {
            if (st && st.devices) setState(st);
          })
          .catch(() => {});
        callRpc<NetIface[]>('net.listInterfaces', {}, { silentError: true })
          .then((list) => {
            if (Array.isArray(list)) setInterfaces(list);
          })
          .catch(() => {});
        callRpc<LoopbackSource[]>('source.listLoopback', {}, { silentError: true })
          .then((list) => {
            if (Array.isArray(list)) setLoopbackSources(list);
          })
          .catch(() => {});
        // 连接引擎就绪后立即触发一次无感极速探测，确保开屏秒现设备
        callRpc('discovery.refresh', {}, { silentError: true }).catch(() => {});
      };

      ws.onmessage = (ev) => {
        try {
          const msg = JSON.parse(ev.data);
          if (msg.type === 'state' && msg.data) {
            setState(msg.data);
            if (msg.data.lastError && msg.data.lastError !== lastStateErrRef.current) {
              lastStateErrRef.current = msg.data.lastError;
              setToast({ text: msg.data.lastError, warn: true });
            } else if (!msg.data.lastError) {
              lastStateErrRef.current = '';
            }
          } else if (msg.type === 'spectrum' && Array.isArray(msg.levels)) {
            const now = performance.now();
            if (now - lastSpecTsRef.current >= 120) {
              lastSpecTsRef.current = now;
              setSpectrum(msg.levels);
            }
          } else if (msg.type === 'rpc' && msg.id != null) {
            const key = String(msg.id);
            const pending = pendingRpcRef.current.get(key);
            if (pending) {
              clearTimeout(pending.timer);
              pendingRpcRef.current.delete(key);
              if (msg.error) {
                pending.reject(new Error(String(msg.error)));
              } else {
                pending.resolve(msg.result);
              }
            }
          }
        } catch {
          // ignore
        }
      };

      ws.onclose = () => {
        setWsConnected(false);
        rejectAllPending('本机音频引擎连接已断开');
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
      rejectAllPending('页面已卸载');
      wsRef.current?.close();
    };
  }, []);

  const selectedDevices = state.devices.filter((d) => d.inGroup);
  const streamingCount = state.devices.filter((d) => d.rtspStreaming).length;

  const triggerDiscoveryRefresh = (interactive = false) => {
    if (interactive) {
      setRefreshingDiscovery(true);
      setToast({ text: '正在极速扫描局域网内的妙播设备…' });
      setTimeout(() => setRefreshingDiscovery(false), 900);
    }
    callRpc('discovery.refresh', {}, { silentError: !interactive }).catch(() => {});
  };

  // 窗口重新获得焦点、切回前台或切回“设备”标签时自动无感刷新，随时发现新上线设备
  useEffect(() => {
    if (!wsConnected) return;
    if (activeTab === 'devices') {
      callRpc('discovery.refresh', {}, { silentError: true }).catch(() => {});
    }
  }, [activeTab, wsConnected]);

  useEffect(() => {
    const onWake = () => {
      if (document.visibilityState === 'hidden') return;
      if (wsRef.current?.readyState === WebSocket.OPEN) {
        callRpc('discovery.refresh', {}, { silentError: true }).catch(() => {});
      }
    };
    window.addEventListener('focus', onWake);
    document.addEventListener('visibilitychange', onWake);
    return () => {
      window.removeEventListener('focus', onWake);
      document.removeEventListener('visibilitychange', onWake);
    };
  }, []);

  const handleTogglePlay = async () => {
    if (state.multiroomRunning) {
      try {
        await callRpc('multiroom.stop');
        setToast({ text: '已停止播放并断开全部设备连接' });
      } catch {
        // toast already shown
      }
      return;
    }
    if (selectedDevices.length === 0) {
      if (state.devices.length === 0) {
        setShowAddSheet(true);
        setToast({ text: '暂未发现设备，可直接输入音箱或手机 IP 添加', warn: true });
        return;
      }
      for (const dev of state.devices) {
        await callRpc('multiroom.addDevice', { deviceId: dev.id }, { silentError: true }).catch(() => {});
      }
    }
    try {
      await callRpc(
        'multiroom.start',
        {
          codec: selectedCodec,
          sampleRate: 48000,
          basePlayDelayMs: state.basePlayDelayMs,
        },
        { expectTrue: true }
      );
      setToast(
        selectedDevices.length === 0 && state.devices.length > 0
          ? { text: `已自动选中 ${state.devices.length} 台设备并开始同步播放` }
          : null
      );
    } catch {
      // error toast shown by callRpc
    }
  };

  const handleDisconnectDevice = async (dev: DeviceState) => {
    try {
      await callRpc('multiroom.removeDevice', { deviceId: dev.id }, { expectTrue: true });
      setToast({ text: `已断开「${dev.name}」` });
    } catch {
      // toast already shown
    }
  };

  const handleSelectSourceMode = async (mode: number, param = '') => {
    try {
      await callRpc('source.setMode', { mode, param }, { expectTrue: true });
      setToast(null);
    } catch {
      // error toast shown by callRpc; state stays at previous valid source
    }
  };

  const handlePickWav = async () => {
    if (window._androidMiPlayBridge) {
      window._androidMiPlayBridge.openWavPicker();
      return;
    }
    const file = await window.miplayBridge?.openWavFile();
    if (file) {
      await handleSelectSourceMode(1, file);
    }
  };

  const handleAddManual = async (e: React.FormEvent) => {
    e.preventDefault();
    const ip = manualIp.trim();
    if (!ip) return;
    const port = Number(manualPort) || 8899;
    const name = manualName.trim() || `妙播设备 (${ip})`;
    try {
      const devId = await callRpc<string>('device.addManual', { name, ip, cmdPort: port });
      await callRpc('multiroom.addDevice', { deviceId: devId || `manual_${ip}_${port}` });
      setManualName('');
      setShowAddSheet(false);
      setToast({ text: `已添加并选中「${name}」，点击顶栏开始播放即可连接` });
    } catch {
      // error toast shown by callRpc
    }
  };

  const sourceHint =
    state.sourceMode === 2
      ? '实时抓取本机所有播放中的声音（音乐 / 视频 / 游戏）'
      : state.sourceMode === 1
        ? `正在使用本地音频：${state.track.title}`
        : '内置柔和和弦测试音，最适合首次验证音箱出声与多房间对齐';

  return (
    <>
      {/* Floating Top Toolbar: Backdrop-Root Isolated Sibling Tray */}
      <div className="top-toolbar-wrap">
        <header className="top-toolbar">
          <div className="top-toolbar-tray" aria-hidden="true" />
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
                    ? `同步中 ${streamingCount}/${selectedDevices.length} 台`
                    : `发现 ${state.devices.length} 台设备`
                  : '引擎连接中…'}
              </span>
            </div>
          </div>

          <div className="lg-toolbar-actions">
            <button
              type="button"
              className="lg-btn icon-only"
              onClick={() => triggerDiscoveryRefresh(true)}
              title="立即刷新搜索局域网设备"
              aria-label="重新搜索"
              style={
                refreshingDiscovery
                  ? { transform: 'rotate(360deg)', transition: 'transform 0.75s ease' }
                  : { transition: 'transform 0.2s ease' }
              }
            >
              <Icons.Refresh />
            </button>
            <button
              type="button"
              className="lg-btn icon-only"
              onClick={() => setShowAddSheet(true)}
              title="手动添加设备"
              aria-label="添加设备"
            >
              <Icons.Plus />
            </button>
            <button
              type="button"
              className={`lg-btn top-cta-btn ${state.multiroomRunning ? 'destructive' : 'primary'}`}
              onClick={handleTogglePlay}
            >
              {state.multiroomRunning ? (
                <>
                  <Icons.Stop />
                  <span>停止并断开</span>
                </>
              ) : (
                <>
                  <Icons.Play />
                  <span>开始播放</span>
                </>
              )}
            </button>
          </div>
        </header>
      </div>

      {/* Main Content */}
      <main className="lg-content">
        {toast && (
          <div className={`lg-toast ${toast.warn ? 'warn' : ''}`} role="status">
            <span>{toast.text}</span>
            <button
              type="button"
              className="lg-btn sm icon-only"
              onClick={() => setToast(null)}
              aria-label="关闭提示"
            >
              <Icons.Close />
            </button>
          </div>
        )}

        {activeTab === 'devices' && (
          <>
            {/* Master Playback & Volume Strip */}
            <section className="content-card">
              <div className="now-strip">
                <div className="now-strip-left">
                  <MiniWave levels={spectrum} live={state.multiroomRunning} />
                  <div className="now-info">
                    <div className="now-title">{state.track.title}</div>
                    <div className="now-sub">
                      {state.multiroomRunning
                        ? streamingCount > 0
                          ? `正在向 ${streamingCount} 台设备同步推流`
                          : '正在等待设备响应…'
                        : selectedDevices.length > 0
                          ? `已选中 ${selectedDevices.length} 台设备，点击顶栏开始播放`
                          : '点选下方设备即可加入同步组'}
                    </div>
                  </div>
                </div>
                <div className="now-vol">
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
              </div>
            </section>

            {/* Speaker Device Wall */}
            <section className="section-block">
              <div className="section-header-row">
                <span className="section-heading">播放到这些设备</span>
                <span className="section-hint">
                  {selectedDevices.length > 0
                    ? `已选 ${selectedDevices.length} 台 · 可随时单独断开`
                    : '点击设备卡片或按钮即可加入'}
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
                      type="button"
                      className="lg-btn primary"
                      onClick={() => setShowAddSheet(true)}
                    >
                      <Icons.Plus />
                      <span>手动输入 IP 添加</span>
                    </button>
                    <button
                      type="button"
                      className="lg-btn"
                      onClick={() => triggerDiscoveryRefresh(true)}
                    >
                      <Icons.Refresh />
                      <span>立即刷新搜索</span>
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
                          role="button"
                          tabIndex={0}
                          onClick={() =>
                            dev.inGroup
                              ? handleDisconnectDevice(dev)
                              : callRpc('multiroom.addDevice', { deviceId: dev.id })
                          }
                          onKeyDown={(e) => {
                            if (e.key === 'Enter' || e.key === ' ') {
                              e.preventDefault();
                              if (dev.inGroup) handleDisconnectDevice(dev);
                              else callRpc('multiroom.addDevice', { deviceId: dev.id });
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
                        </div>

                        <div className="speaker-actions-row">
                          <span className="speaker-ip-tag">
                            {dev.ip}:{dev.cmdPort}
                          </span>
                          <div style={{ display: 'flex', gap: 6 }}>
                            {dev.inGroup ? (
                              <button
                                type="button"
                                className="lg-btn destructive sm"
                                onClick={() => handleDisconnectDevice(dev)}
                              >
                                断开连接
                              </button>
                            ) : (
                              <button
                                type="button"
                                className="lg-btn sm"
                                onClick={() => callRpc('multiroom.addDevice', { deviceId: dev.id })}
                              >
                                加入投送
                              </button>
                            )}
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
          </>
        )}

        {activeTab === 'source' && (
          <section className="section-block">
            <div className="section-header-row">
              <span className="section-heading">声音来源</span>
              <span className="section-hint">选择要投送到全屋音箱的声音</span>
            </div>
            <div className="content-card">
              <div className="segmented" role="tablist" aria-label="声音来源">
                <button
                  type="button"
                  role="tab"
                  aria-selected={state.sourceMode === 2}
                  disabled={state.loopbackSupported === false}
                  onClick={() => handleSelectSourceMode(2, selectedLoopback)}
                >
                  {state.loopbackSupported === false ? '本机声音(不支持)' : '本机声音'}
                </button>
                <button
                  type="button"
                  role="tab"
                  aria-selected={state.sourceMode === 1}
                  onClick={handlePickWav}
                >
                  本地 WAV
                </button>
                <button
                  type="button"
                  role="tab"
                  aria-selected={state.sourceMode === 0}
                  onClick={() => handleSelectSourceMode(0, '')}
                >
                  测试音
                </button>
              </div>
              {state.sourceMode === 2 && loopbackSources.length >= 1 && (
                <div className="setting-row" style={{ padding: '4px 0 0' }}>
                  <span className="setting-title">音频输出接管与采集通道</span>
                  <select
                    className="sheet-select"
                    value={selectedLoopback || state.sourceParam || loopbackSources[0]?.id || ''}
                    onChange={(e) => {
                      setSelectedLoopback(e.target.value);
                      handleSelectSourceMode(2, e.target.value);
                    }}
                  >
                    {loopbackSources.map((s) => (
                      <option key={s.id} value={s.id}>
                        {s.label}
                      </option>
                    ))}
                  </select>
                </div>
              )}
              <div className="source-caption-row">
                <span>
                  {state.loopbackSupported === false && state.sourceMode === 2
                    ? '当前平台暂不支持系统全局环回抓音，请使用本地 WAV 或测试音'
                    : sourceHint}
                </span>
                {state.sourceMode === 1 && (
                  <button type="button" className="lg-btn sm" onClick={handlePickWav}>
                    更换 WAV 文件
                  </button>
                )}
              </div>
              <div className="spectrum-stage" aria-hidden="true">
                {spectrum.map((v, idx) => (
                  <div
                    key={idx}
                    className={`spectrum-bar ${state.multiroomRunning ? 'live' : ''}`}
                    style={{
                      height: `${state.multiroomRunning ? Math.max(12, Math.min(100, Math.round(v * 100))) : 16}%`,
                    }}
                  />
                ))}
              </div>
            </div>
          </section>
        )}

        {activeTab === 'settings' && (
          <>
            <section className="section-block">
              <div className="section-header-row">
                <span className="section-heading">液态玻璃与外观</span>
                <span className="section-hint">调节玻璃清透度与主题</span>
              </div>
              <div className="list-group">
                <div className="setting-row">
                  <div className="setting-row-top">
                    <span className="setting-title">玻璃通透度</span>
                    <strong className="tnum" style={{ fontSize: 13 }}>
                      {Math.round(glassTint * 100)}%
                    </strong>
                  </div>
                  <input
                    id="glass-tint-slider"
                    className="vol-slider"
                    type="range"
                    min={0}
                    max={100}
                    step={1}
                    value={Math.round(glassTint * 100)}
                    aria-label="玻璃通透度"
                    onChange={(e) => setGlassTint(Number(e.target.value) / 100)}
                  />
                  <span className="setting-desc">
                    0% 为完全清透无模糊，默认 35% 呈现清透液态玻璃与边缘折光。
                  </span>
                </div>

                <div className="setting-row">
                  <div className="setting-row-top">
                    <span className="setting-title">外观模式</span>
                  </div>
                  <div className="segmented" role="tablist" aria-label="外观模式">
                    <button
                      type="button"
                      role="tab"
                      aria-selected={theme === 'auto'}
                      onClick={() => setTheme('auto')}
                    >
                      跟随系统
                    </button>
                    <button
                      type="button"
                      role="tab"
                      aria-selected={theme === 'light'}
                      onClick={() => setTheme('light')}
                    >
                      浅色
                    </button>
                    <button
                      type="button"
                      role="tab"
                      aria-selected={theme === 'dark'}
                      onClick={() => setTheme('dark')}
                    >
                      深色
                    </button>
                  </div>
                </div>
              </div>
            </section>

            <section className="section-block">
              <div className="section-header-row">
                <span className="section-heading">音频同步与网络</span>
                <span className="section-hint">{state.codec} · {state.basePlayDelayMs} ms</span>
              </div>
              <div className="list-group">
                <div className="setting-row">
                  <div className="setting-row-top">
                    <span className="setting-title">音频传输格式</span>
                    <span className="section-hint">
                      {state.multiroomRunning ? '停止播放后可切换' : '推荐 AAC'}
                    </span>
                  </div>
                  <select
                    className="sheet-select"
                    value={selectedCodec}
                    disabled={state.multiroomRunning}
                    onChange={(e) => setSelectedCodec(e.target.value as 'LPCM' | 'AAC_LC')}
                  >
                    <option value="AAC_LC">高兼容流畅模式 · AAC-LC（推荐）</option>
                    <option value="LPCM">无损低延迟模式 · LPCM</option>
                  </select>
                </div>

                <div className="setting-row">
                  <div className="setting-row-top">
                    <span className="setting-title">全屋同步缓冲</span>
                    <strong className="tnum" style={{ fontSize: 13 }}>{state.basePlayDelayMs} ms</strong>
                  </div>
                  <input
                    className="vol-slider"
                    type="range"
                    min={150}
                    max={800}
                    step={10}
                    value={state.basePlayDelayMs}
                    aria-label="全屋同步缓冲"
                    onChange={(e) => callRpc('sync.setBaseDelay', { delayMs: Number(e.target.value) })}
                  />
                  <span className="setting-desc">
                    数值越大抗 Wi-Fi 波动越稳，默认 350 ms 适合大多数家庭网络。
                  </span>
                </div>

                {interfaces.filter((i) => !i.isLoopback).length > 0 && (
                  <div className="setting-row">
                    <div className="setting-row-top">
                      <span className="setting-title">本机通信网卡</span>
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
                  </div>
                )}

                {selectedDevices.map((dev) => (
                  <div key={dev.id} className="setting-row">
                    <div className="setting-row-top">
                      <span className="setting-title">{dev.name} · 硬件延迟微调</span>
                      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                        <strong className="tnum" style={{ fontSize: 13 }}>
                          {dev.delayBiasMs >= 0 ? `+${dev.delayBiasMs}` : dev.delayBiasMs} ms
                        </strong>
                        {dev.delayBiasMs !== 0 && (
                          <button
                            type="button"
                            className="lg-btn sm"
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

                <div className="setting-row">
                  <div className="setting-row-top">
                    <span className="setting-title">专业协议诊断信息</span>
                    <button
                      type="button"
                      className="lg-btn sm"
                      onClick={() => setShowDiagnostics((v) => !v)}
                    >
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
                          <div
                            key={d.id}
                            style={{ marginTop: 8, paddingTop: 6, borderTop: '0.5px solid var(--separator)' }}
                          >
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
            </section>
          </>
        )}
      </main>

      {/* Floating Bottom Capsule Dock (SOP v1.1 Water-Drop Lens) */}
      <div className="dock-wrap">
        <nav className="bottom-dock" ref={dockRef} role="tablist" aria-label="主导航">
          <div className="dock-tray" aria-hidden="true" />
          <div
            className="dock-lens"
            data-refract="0"
            data-refract-bezel="11"
            data-refract-blur="0"
            data-refract-dispersion="0.14"
            data-refract-dynamic=""
            aria-hidden="true"
          />
          <button
            type="button"
            role="tab"
            className="dock-tab"
            data-tab="devices"
            aria-selected={activeTab === 'devices'}
            tabIndex={activeTab === 'devices' ? 0 : -1}
          >
            <Icons.Speaker />
            <span>设备</span>
          </button>
          <button
            type="button"
            role="tab"
            className="dock-tab"
            data-tab="source"
            aria-selected={activeTab === 'source'}
            tabIndex={activeTab === 'source' ? 0 : -1}
          >
            <Icons.Wave />
            <span>音源</span>
          </button>
          <button
            type="button"
            role="tab"
            className="dock-tab"
            data-tab="settings"
            aria-selected={activeTab === 'settings'}
            tabIndex={activeTab === 'settings' ? 0 : -1}
          >
            <Icons.Settings />
            <span>设置</span>
          </button>
        </nav>
      </div>

      {/* Add Device Modal Sheet */}
      {showAddSheet && (
        <div className="sheet-backdrop" onClick={() => setShowAddSheet(false)}>
          <div
            className="sheet-panel"
            onClick={(e) => e.stopPropagation()}
            role="dialog"
            aria-label="手动添加设备"
          >
            <div className="sheet-tray" aria-hidden="true" />
            <div className="sheet-header">
              <h2>添加妙播设备</h2>
              <button
                type="button"
                className="lg-btn sm icon-only"
                onClick={() => setShowAddSheet(false)}
                aria-label="关闭"
              >
                <Icons.Close />
              </button>
            </div>
            <form className="sheet-body" onSubmit={handleAddManual}>
              <div className="list-group">
                <div className="setting-row">
                  <label className="setting-title" htmlFor="ip-input">
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
                  <span className="setting-desc">可在音箱/平板的 Wi-Fi 详情或接收端界面查看 IP</span>
                </div>
                <div className="setting-row">
                  <label className="setting-title" htmlFor="name-input">
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
                <div className="setting-row">
                  <label className="setting-title" htmlFor="port-input">
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
              <div className="sheet-actions">
                <button type="button" className="lg-btn" onClick={() => setShowAddSheet(false)}>
                  取消
                </button>
                <button type="submit" className="lg-btn primary">
                  添加并选中
                </button>
              </div>
            </form>
          </div>
        </div>
      )}
    </>
  );
}
