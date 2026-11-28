/**
 * SoundFlow Multi-Room Sender UI Controller (Zero-Network, CSP 'connect-src none' compliant).
 * Supports per-app audio capture isolation (addMatchingUid) and system-wide loopback.
 */

const MODE_TEST_TONE = 0;
const MODE_WAV_FILE = 1;
const MODE_SYSTEM_LOOPBACK = 2;

export function initSenderController({
  getActiveTab,
  showToast,
  setSliderFill,
  onSenderStateChanged,
}) {
  let rpcSeq = 1;
  const pendingRpcs = new Map();

  let state = {
    bindIp: "127.0.0.1",
    autoBind: true,
    discoveryRunning: false,
    multiroomRunning: false,
    hasProjection: false,
    captureBackend: "media_projection",
    shizukuAvailable: false,
    shizukuGranted: false,
    adbDaemonActive: false,
    adbPort: 5555,
    adbLastError: "",
    muteLocalApp: false,
    muteLocalSupported: false,
    muteLocalHint: "",
    timerServicePort: 0,
    sampleRate: 48000,
    basePlayDelayMs: 300,
    codec: "AAC_LC",
    masterVolume: 70,
    sourceMode: MODE_SYSTEM_LOOPBACK,
    targetAppUid: -1,
    targetAppPkg: "",
    targetAppName: "",
    lastError: "",
    notificationAccessGranted: false,
    track: {
      title: "",
      artist: "",
      album: "",
      durationMs: 0,
      positionMs: 0,
      lyricLine: "",
      hasLyrics: false,
      paused: false,
    },
    devices: [],
  };

  let interfaces = [];
  let installedApps = [];
  let manualDrawerOpen = false;
  let seekingMasterVol = false;
  let seekingWavPos = false;
  let seekingBaseDelay = false;
  const activeDeviceSliders = new Set();
  const deviceDomMap = new Map();

  const spectrumBars = Array.from(document.querySelectorAll("#sender-spectrum .spec-bar"));
  const statusHeadline = document.getElementById("sender-hero-status");
  const statusSub = document.getElementById("sender-hero-sub");
  const errBanner = document.getElementById("sender-error-banner");
  const errText = document.getElementById("sender-error-text");

  const sourceSegBtns = Array.from(document.querySelectorAll("#sender-source-seg button"));
  const appFilterRow = document.getElementById("sender-app-row");
  const appTriggerBtn = document.getElementById("sender-app-trigger");
  const appTriggerText = document.getElementById("sender-app-trigger-text");
  const appPickerBackdrop = document.getElementById("sender-app-picker-backdrop");
  const appPickerClose = document.getElementById("sender-app-picker-close");
  const appPickerList = document.getElementById("sender-app-picker-list");
  const appSearchInput = document.getElementById("sender-app-search");
  const appRefreshBtn = document.getElementById("sender-app-refresh-btn");
  const muteLocalRow = document.getElementById("sender-mute-local-row");
  const muteLocalDesc = document.getElementById("sender-mute-local-desc");
  const muteLocalSwitch = document.getElementById("sender-mute-local-switch");
  const adbBtn = document.getElementById("sender-adb-btn");
  const shizukuBtn = document.getElementById("sender-shizuku-btn");
  const guardAdbDesc = document.getElementById("guard-adb-desc");
  const guardAdbPort = document.getElementById("guard-adb-port");
  const btnAdbPair = document.getElementById("btn-adb-pair");
  const btnAdbConnect = document.getElementById("btn-adb-connect");
  const btnAdbDisconnect = document.getElementById("btn-adb-disconnect");
  const rowGuardShizuku = document.getElementById("row-guard-shizuku");
  const guardShizukuDesc = document.getElementById("guard-shizuku-desc");
  const guardShizukuIcon = document.getElementById("guard-shizuku-icon");
  const rowDevSettings = document.getElementById("row-dev-settings");
  const projectionRow = document.getElementById("sender-projection-banner");
  const projectionBtn = document.getElementById("sender-projection-btn");
  const notifBanner = document.getElementById("sender-notif-banner");
  const notifBtn = document.getElementById("sender-notif-btn");

  const trackTitleEl = document.getElementById("sender-track-title");
  const trackSubEl = document.getElementById("sender-track-sub");
  const trackLyricEl = document.getElementById("sender-track-lyric");
  const wavSeekWrap = document.getElementById("sender-wav-seek-wrap");
  const wavSeekSlider = document.getElementById("sender-wav-seek");
  const wavTimeCur = document.getElementById("sender-wav-cur");
  const wavTimeDur = document.getElementById("sender-wav-dur");

  const btnPickWav = document.getElementById("sender-pick-wav-btn");
  const btnPickLrc = document.getElementById("sender-pick-lrc-btn");
  const btnOpenStage = document.getElementById("sender-open-stage-btn");
  const btnPrev = document.getElementById("sender-prev-btn");
  const btnPlayPause = document.getElementById("sender-playpause-btn");
  const btnNext = document.getElementById("sender-next-btn");

  const masterVolSlider = document.getElementById("sender-master-vol");
  const masterVolVal = document.getElementById("sender-master-vol-val");

  const devCountBadge = document.getElementById("sender-dev-count");
  const btnRefreshScan = document.getElementById("sender-scan-btn");
  const btnToggleManual = document.getElementById("sender-manual-toggle-btn");
  const manualDrawer = document.getElementById("sender-manual-drawer");
  const manualNameInput = document.getElementById("sender-manual-name");
  const manualIpInput = document.getElementById("sender-manual-ip");
  const manualPortInput = document.getElementById("sender-manual-port");
  const btnAddManualSubmit = document.getElementById("sender-manual-submit-btn");
  const deviceListEl = document.getElementById("sender-device-list");
  const emptyDevicesEl = document.getElementById("sender-empty-devices");

  // Preferences & Guard elements
  const codecSegBtns = Array.from(document.querySelectorAll("#sender-codec-seg button"));
  const baseDelaySlider = document.getElementById("sender-base-delay-slider");
  const baseDelayVal = document.getElementById("sender-base-delay-val");
  const baseDelayReset = document.getElementById("sender-base-delay-reset");
  const bindIpSelect = document.getElementById("sender-bind-ip-select");
  const guardNotifRow = document.getElementById("row-notif-access");
  const guardNotifDesc = document.getElementById("notif-access-desc");
  const guardNotifIcon = document.getElementById("notif-icon-box");

  function rpcAsync(method, params = {}, timeoutMs = 8000) {
    return new Promise((resolve) => {
      const id = "srpc_" + (rpcSeq++);
      const payload = JSON.stringify({ id, method, ...params });
      if (window.SoundFlowBridge?.senderRpcAsync) {
        pendingRpcs.set(id, resolve);
        window.SoundFlowBridge.senderRpcAsync(payload);
        setTimeout(() => {
          if (pendingRpcs.has(id)) {
            pendingRpcs.delete(id);
            resolve({ error: "timeout" });
          }
        }, timeoutMs);
      } else if (window.SoundFlowBridge?.senderRpc) {
        try {
          const raw = window.SoundFlowBridge.senderRpc(payload);
          resolve(JSON.parse(raw));
        } catch (e) {
          resolve({ error: String(e) });
        }
      } else {
        resolve({ error: "bridge_unavailable" });
      }
    });
  }

  function formatTimeMs(ms) {
    const totalSec = Math.max(0, Math.floor((Number(ms) || 0) / 1000));
    const min = Math.floor(totalSec / 60);
    const sec = totalSec % 60;
    return `${String(min).padStart(2, "0")}:${String(sec).padStart(2, "0")}`;
  }

  function countGroupDevices() {
    let inGroup = 0;
    let streaming = 0;
    for (const d of state.devices || []) {
      if (d.inGroup) inGroup++;
      if (d.rtspStreaming) streaming++;
    }
    return { inGroup, streaming };
  }

  async function toggleMultiroom() {
    if (state.multiroomRunning) {
      const res = await rpcAsync("multiroom.stop");
      if (res?.error) showToast(res.error);
      else showToast("已停止全屋同步发送");
      return;
    }
    const { inGroup } = countGroupDevices();
    if (inGroup === 0 && (state.devices || []).length > 0) {
      const firstOnline = state.devices.find((d) => d.online);
      if (firstOnline) {
        await rpcAsync("multiroom.addDevice", { deviceId: firstOnline.id });
      }
    }
    const res = await rpcAsync("multiroom.start", {
      codec: state.codec,
      sampleRate: state.sampleRate,
      basePlayDelayMs: state.basePlayDelayMs,
    });
    if (res?.error) {
      showToast(res.error);
    } else {
      showToast("全屋同步推流已启动");
    }
  }

  function renderSpectrum(levels) {
    if (!spectrumBars.length) return;
    if (!Array.isArray(levels) || !state.multiroomRunning) {
      spectrumBars.forEach((bar) => {
        bar.style.transform = "scaleY(0.14)";
        bar.style.opacity = "0.35";
      });
      return;
    }
    const n = spectrumBars.length;
    for (let i = 0; i < n; i++) {
      const srcIdx = Math.min(levels.length - 1, Math.floor((i * levels.length) / n));
      const v = Math.max(0.12, Math.min(1, Number(levels[srcIdx]) || 0.12));
      spectrumBars[i].style.transform = `scaleY(${v.toFixed(3)})`;
      spectrumBars[i].style.opacity = (0.42 + v * 0.58).toFixed(2);
    }
  }

  function buildDeviceCardDom(d) {
    const card = document.createElement("div");
    card.className = "sender-device-card";
    card.dataset.devId = d.id;

    card.innerHTML = `
      <div class="sender-dev-top">
        <div class="sender-dev-meta">
          <div class="sender-dev-title-row">
            <span class="status-dot dev-dot"></span>
            <span class="sender-dev-name"></span>
            <span class="sender-dev-badge tnum"></span>
          </div>
          <div class="sender-dev-facts">
            <span class="sender-dev-sub tnum"></span>
            <span class="sender-dev-timing tnum" title="接收端上报的播放队列与音频输出延迟之和"></span>
          </div>
        </div>
        <div class="sender-dev-actions">
          <button type="button" class="row-btn destructive dev-remove-btn" style="display:none">移除</button>
          <button type="button" class="ios-switch dev-group-switch" role="switch" aria-checked="false" aria-label="加入全屋播放组"></button>
        </div>
      </div>
      <div class="sender-dev-controls">
        <div class="sender-dev-slider-row">
          <span class="sender-slider-lbl">音量</span>
          <input type="range" min="0" max="100" value="65" class="lg-slider dev-vol-slider" aria-label="单设备音量" />
          <span class="volume-val tnum dev-vol-val">65%</span>
        </div>
        <div class="sender-dev-slider-row">
          <span class="sender-slider-lbl">微调</span>
          <input type="range" min="-300" max="300" step="5" value="0" class="lg-slider dev-bias-slider" aria-label="单设备同步偏移毫秒" />
          <span class="volume-val tnum dev-bias-val">0ms</span>
          <button type="button" class="row-btn dev-bias-zero">归零</button>
        </div>
      </div>
    `;

    const groupSwitch = card.querySelector(".dev-group-switch");
    const removeBtn = card.querySelector(".dev-remove-btn");
    const volSlider = card.querySelector(".dev-vol-slider");
    const volVal = card.querySelector(".dev-vol-val");
    const biasSlider = card.querySelector(".dev-bias-slider");
    const biasVal = card.querySelector(".dev-bias-val");
    const biasZero = card.querySelector(".dev-bias-zero");

    groupSwitch.addEventListener("click", async () => {
      const devId = card.dataset.devId;
      const cur = (state.devices || []).find((x) => x.id === devId);
      const next = !(cur && cur.inGroup);
      groupSwitch.setAttribute("aria-checked", String(next));
      const res = await rpcAsync(next ? "multiroom.addDevice" : "multiroom.removeDevice", { deviceId: devId });
      if (res?.error) showToast(res.error);
    });

    removeBtn.addEventListener("click", async () => {
      const devId = card.dataset.devId;
      await rpcAsync("device.remove", { deviceId: devId });
      showToast("已移除手动设备");
    });

    const volKey = `vol:${d.id}`;
    volSlider.addEventListener("pointerdown", () => activeDeviceSliders.add(volKey));
    volSlider.addEventListener("input", () => {
      const v = Number(volSlider.value);
      volVal.textContent = v + "%";
      setSliderFill(volSlider, v);
    });
    const commitVol = () => {
      activeDeviceSliders.delete(volKey);
      rpcAsync("volume.setDevice", { deviceId: card.dataset.devId, volume: Number(volSlider.value) });
    };
    volSlider.addEventListener("change", commitVol);
    volSlider.addEventListener("pointercancel", () => activeDeviceSliders.delete(volKey));

    const biasKey = `bias:${d.id}`;
    biasSlider.addEventListener("pointerdown", () => activeDeviceSliders.add(biasKey));
    const paintBias = (v) => {
      biasVal.textContent = (v > 0 ? "+" : "") + v + "ms";
      setSliderFill(biasSlider, ((v + 300) / 600) * 100);
    };
    biasSlider.addEventListener("input", () => paintBias(Number(biasSlider.value)));
    biasSlider.addEventListener("change", () => {
      activeDeviceSliders.delete(biasKey);
      const v = Number(biasSlider.value);
      paintBias(v);
      rpcAsync("sync.setDeviceBias", { deviceId: card.dataset.devId, delayBiasMs: v });
    });
    biasSlider.addEventListener("pointercancel", () => activeDeviceSliders.delete(biasKey));
    biasZero.addEventListener("click", () => {
      biasSlider.value = "0";
      paintBias(0);
      rpcAsync("sync.setDeviceBias", { deviceId: card.dataset.devId, delayBiasMs: 0 });
    });

    return card;
  }

  function updateDeviceCardDom(card, d) {
    card.classList.toggle("in-group", !!d.inGroup);
    card.classList.toggle("is-offline", !d.online);

    const dot = card.querySelector(".dev-dot");
    dot.className = "status-dot dev-dot " + (d.rtspStreaming ? "playing" : d.online ? "ready" : "");

    const nameEl = card.querySelector(".sender-dev-name");
    const roomPrefix = d.roomName ? `${d.roomName} · ` : "";
    nameEl.textContent = roomPrefix + (d.name || d.ip);

    const badgeEl = card.querySelector(".sender-dev-badge");
    if (d.rtspStreaming) {
      badgeEl.textContent = d.syncPackets > 0 ? "推流中 · 有校时数据" : "推流中";
      badgeEl.className = "sender-dev-badge tnum streaming";
    } else if (d.inGroup && state.multiroomRunning) {
      badgeEl.textContent = d.cmdConnected ? "握手就绪" : "连接中…";
      badgeEl.className = "sender-dev-badge tnum connecting";
    } else {
      badgeEl.textContent = d.online ? (d.isManual ? "手动目标" : "在线") : "离线";
      badgeEl.className = "sender-dev-badge tnum";
    }

    const timing = card.querySelector(".sender-dev-timing");
    const latency = d.rtspStreaming && Number(d.reportedLatencyMs) > 0;
    timing.hidden = !latency;
    timing.textContent = latency ? `接收播放延迟 ${d.reportedLatencyMs} ms` : "";
    const sub = card.querySelector(".sender-dev-sub");
    const endpoint = `${d.ip}:${d.cmdPort}`;
    sub.textContent = endpoint;
    sub.title = d.statusText && d.statusText !== "待机" ? d.statusText : endpoint;

    const removeBtn = card.querySelector(".dev-remove-btn");
    removeBtn.style.display = d.isManual ? "inline-flex" : "none";

    const groupSwitch = card.querySelector(".dev-group-switch");
    groupSwitch.setAttribute("aria-checked", String(!!d.inGroup));

    const volKey = `vol:${d.id}`;
    if (!activeDeviceSliders.has(volKey)) {
      const vol = typeof d.volume === "number" ? d.volume : 65;
      const volSlider = card.querySelector(".dev-vol-slider");
      volSlider.value = String(vol);
      card.querySelector(".dev-vol-val").textContent = vol + "%";
      setSliderFill(volSlider, vol);
    }

    const biasKey = `bias:${d.id}`;
    if (!activeDeviceSliders.has(biasKey)) {
      const bias = typeof d.delayBiasMs === "number" ? d.delayBiasMs : 0;
      const biasSlider = card.querySelector(".dev-bias-slider");
      biasSlider.value = String(bias);
      card.querySelector(".dev-bias-val").textContent = (bias > 0 ? "+" : "") + bias + "ms";
      setSliderFill(biasSlider, ((bias + 300) / 600) * 100);
    }

    card.querySelector(".sender-dev-controls").style.display = d.inGroup ? "grid" : "none";
  }

  function renderDevices() {
    const list = Array.isArray(state.devices) ? state.devices : [];
    const { inGroup, streaming } = countGroupDevices();
    if (devCountBadge) {
      devCountBadge.textContent = list.length > 0
        ? `已发现 ${list.length} 台 · 入组 ${inGroup} 台${streaming > 0 ? ` · 播放 ${streaming} 台` : ""}`
        : (state.discoveryRunning ? "正在局域网扫描小米音箱 / 接收器…" : "暂无设备");
    }
    if (emptyDevicesEl) {
      emptyDevicesEl.style.display = list.length === 0 ? "block" : "none";
    }
    if (!deviceListEl) return;

    const seenIds = new Set();
    for (const d of list) {
      seenIds.add(d.id);
      let card = deviceDomMap.get(d.id);
      if (!card) {
        card = buildDeviceCardDom(d);
        deviceDomMap.set(d.id, card);
        deviceListEl.appendChild(card);
      }
      updateDeviceCardDom(card, d);
    }
    for (const [id, card] of deviceDomMap.entries()) {
      if (!seenIds.has(id)) {
        card.remove();
        deviceDomMap.delete(id);
      }
    }
  }

  function syncAppSelectSelection() {
    if (!appTriggerText) return;
    if (state.targetAppUid > 0 && state.targetAppPkg) {
      const matched = installedApps.find((a) => a.uid === state.targetAppUid && a.packageName === state.targetAppPkg);
      const label = state.targetAppName || matched?.label || state.targetAppPkg;
      const prefix = matched?.playing ? "🔊 " : "仅发送：";
      appTriggerText.textContent = `${prefix}${label}`;
    } else {
      const playingCount = installedApps.filter((a) => !!a.playing).length;
      appTriggerText.textContent = playingCount > 0
        ? `全部媒体应用（${playingCount} 个发声中 · 点此选择）`
        : "全部媒体应用（点此选择或搜索）";
    }
  }

  function renderState() {
    const running = !!state.multiroomRunning;
    const { inGroup, streaming } = countGroupDevices();
    const codecText = state.codec === "LPCM"
      ? `无损 LPCM ${Math.round((state.sampleRate || 48000) / 1000)}kHz`
      : "AAC-LC 48kHz";
    const singleAppActive = state.sourceMode === MODE_SYSTEM_LOOPBACK && state.targetAppUid > 0;

    if (statusHeadline) {
      statusHeadline.textContent = running
        ? (streaming ? `${streaming} 台设备播放中` : "等待设备连接")
        : (inGroup ? `已选择 ${inGroup} 台设备` : "选择音源和播放设备");
    }
    if (statusSub) {
      statusSub.textContent = running ? `${codecText} / 发送基准 ${state.basePlayDelayMs} ms` : "";
    }
    if (errBanner && errText) {
      errBanner.classList.toggle("show", !!state.lastError);
      errText.textContent = state.lastError || "";
    }
    if (!running) renderSpectrum(null);

    sourceSegBtns.forEach((btn) => {
      btn.setAttribute("aria-selected", String(Number(btn.dataset.mode) === state.sourceMode));
    });

    if (appFilterRow) {
      appFilterRow.style.display = state.sourceMode === MODE_SYSTEM_LOOPBACK ? "flex" : "none";
    }
    syncAppSelectSelection();

    const isDirectPrivileged = state.captureBackend === "privileged_policy"
      || state.captureBackend === "wireless_adb"
      || state.captureBackend === "shizuku";
    const isAndroidHost = window.SoundFlowBridge?.platform !== "win32";
    if (muteLocalRow) {
      muteLocalRow.style.display = state.sourceMode === MODE_SYSTEM_LOOPBACK ? "flex" : "none";
    }
    if (muteLocalSwitch) {
      muteLocalSwitch.setAttribute("aria-checked", String(!!state.muteLocalApp));
      muteLocalSwitch.disabled = !isAndroidHost && !state.muteLocalSupported && !state.muteLocalApp;
    }
    if (adbBtn) {
      const showAdb = isAndroidHost && state.captureBackend !== "privileged_policy";
      adbBtn.style.display = showAdb ? "inline-flex" : "none";
      adbBtn.textContent = state.adbDaemonActive ? "ADB 直抓已就绪" : "无线 ADB 免投屏";
    }
    if (shizukuBtn) {
      const showShizuku = isAndroidHost && !isDirectPrivileged && !!state.shizukuAvailable && !state.shizukuGranted;
      shizukuBtn.style.display = showShizuku ? "inline-flex" : "none";
    }
    if (muteLocalDesc) {
      if (isDirectPrivileged) {
        const badge = state.captureBackend === "privileged_policy"
          ? "系统特权直抓 · 免投屏"
          : state.captureBackend === "wireless_adb"
            ? "无线 ADB 直抓 · 免投屏"
            : "Shizuku 直抓 · 免投屏";
        muteLocalDesc.textContent = singleAppActive
          ? `${badge} · 仅在全屋音箱发声，本机其他应用照常外放`
          : `${badge} · 开启后拦截媒体外放仅走全屋音箱`;
      } else if (state.muteLocalSupported) {
        muteLocalDesc.textContent = singleAppActive
          ? "仅在全屋音箱发声，电脑本地其他应用照常外放"
          : "需选择单个目标应用后可仅对该应用开启本机消音";
      } else {
        muteLocalDesc.textContent = state.muteLocalHint || "点击「无线 ADB 免投屏」连接本机调试端口即可开启单应用本机消音";
      }
    }
    if (guardAdbDesc) {
      if (state.adbDaemonActive) {
        guardAdbDesc.textContent = `已激活 · 正在通过本机无线 ADB (127.0.0.1:${state.adbPort || "自动发现"}) 提供 UID 2000 免投屏单应用静音抓音`;
      } else if (state.adbLastError) {
        guardAdbDesc.textContent = state.adbLastError;
      } else {
        guardAdbDesc.textContent = "未连接 · 首次使用先配对，已配对可直接连接本机";
      }
    }
    if (btnAdbPair) btnAdbPair.hidden = !state.adbPairingSupported;
    if (btnAdbConnect) {
      btnAdbConnect.textContent = state.adbDaemonActive ? "检查连接" : "连接并启动";
    }
    if (btnAdbDisconnect) {
      btnAdbDisconnect.style.display = state.adbDaemonActive ? "inline-flex" : "none";
    }
    if (guardShizukuDesc && guardShizukuIcon) {
      if (state.shizukuGranted) {
        guardShizukuDesc.textContent = "已授权 · Shizuku 免投屏直抓通道可用";
        guardShizukuIcon.className = "row-icon green";
      } else if (state.shizukuAvailable) {
        guardShizukuDesc.textContent = "已检测到 Shizuku 服务 · 点击发起授权";
        guardShizukuIcon.className = "row-icon orange";
      } else {
        guardShizukuDesc.textContent = "未检测到运行中的 Shizuku；已优先推荐使用上方内置无线 ADB 直连";
        guardShizukuIcon.className = "row-icon";
      }
    }

    if (projectionRow) {
      projectionRow.style.display = (state.sourceMode === MODE_SYSTEM_LOOPBACK && !state.hasProjection && !isDirectPrivileged) ? "flex" : "none";
    }
    if (notifBanner) {
      notifBanner.style.display = (state.sourceMode === MODE_SYSTEM_LOOPBACK && !state.notificationAccessGranted) ? "flex" : "none";
    }

    const tr = state.track || {};
    if (trackTitleEl) {
      trackTitleEl.textContent = tr.title || (
        state.sourceMode === MODE_WAV_FILE
          ? "未选择本地 WAV 音频"
          : state.sourceMode === MODE_TEST_TONE
            ? "440Hz 同步校准测试音"
            : (singleAppActive ? `等待「${state.targetAppName}」播放音频` : "等待应用或系统音乐播放")
      );
    }
    if (trackSubEl) {
      const parts = [];
      if (singleAppActive) parts.push(`仅捕获 ${state.targetAppName}`);
      if (tr.artist) parts.push(tr.artist);
      if (tr.album) parts.push(tr.album);
      trackSubEl.textContent = parts.join(" · ") || codecText;
    }
    if (trackLyricEl) {
      trackLyricEl.textContent = tr.lyricLine
        ? `♪ ${tr.lyricLine}`
        : "";
      trackLyricEl.hidden = !tr.lyricLine;
    }
    if (btnPlayPause) {
      const span = btnPlayPause.querySelector("span");
      if (span) span.textContent = tr.paused ? "继续" : "暂停";
    }

    const showWavSeek = state.sourceMode === MODE_WAV_FILE && (tr.durationMs || 0) > 0;
    if (wavSeekWrap) wavSeekWrap.style.display = showWavSeek ? "flex" : "none";
    if (showWavSeek && wavSeekSlider && !seekingWavPos) {
      const dur = Math.max(1, Number(tr.durationMs) || 1);
      const pos = Math.max(0, Math.min(dur, Number(tr.positionMs) || 0));
      wavSeekSlider.max = String(dur);
      wavSeekSlider.value = String(pos);
      setSliderFill(wavSeekSlider, (pos / dur) * 100);
      if (wavTimeCur) wavTimeCur.textContent = formatTimeMs(pos);
      if (wavTimeDur) wavTimeDur.textContent = formatTimeMs(dur);
    }

    if (masterVolSlider && !seekingMasterVol && typeof state.masterVolume === "number") {
      masterVolSlider.value = String(state.masterVolume);
      if (masterVolVal) masterVolVal.textContent = state.masterVolume + "%";
      setSliderFill(masterVolSlider, state.masterVolume);
    }

    codecSegBtns.forEach((btn) => {
      btn.setAttribute("aria-selected", String(btn.dataset.codec === state.codec));
    });
    if (baseDelaySlider && !seekingBaseDelay && typeof state.basePlayDelayMs === "number") {
      baseDelaySlider.value = String(state.basePlayDelayMs);
      if (baseDelayVal) baseDelayVal.textContent = state.basePlayDelayMs + " ms";
      setSliderFill(baseDelaySlider, ((state.basePlayDelayMs - 40) / 760) * 100);
    }
    if (guardNotifDesc && guardNotifIcon && window.SoundFlowBridge?.platform !== "win32") {
      guardNotifDesc.textContent = state.notificationAccessGranted
        ? "已授权 · 自动同步音乐 App 曲目与状态栏实时歌词至全屋音箱"
        : "未开启 · 点击授权通知读取以同步全屋歌名与实时歌词";
      guardNotifIcon.className = "row-icon " + (state.notificationAccessGranted ? "green" : "orange");
    }

    renderDevices();
    onSenderStateChanged?.(state);
  }

  function closeAppPicker() {
    appPickerBackdrop?.classList.remove("open");
  }

  async function selectTargetApp(uid, pkg, label) {
    closeAppPicker();
    if (uid <= 0 || !pkg) {
      const res = await rpcAsync("source.setTargetApp", { uid: -1, packageName: "", label: "" });
      if (res?.error) { syncAppSelectSelection(); showToast(res.error); return; }
      showToast("已切换为全局媒体内录（发送全部媒体声音）");
      return;
    }
    const res = await rpcAsync("source.setTargetApp", { uid, packageName: pkg, label });
    if (res?.error) { syncAppSelectSelection(); showToast(res.error); return; }
    showToast(`已锁定仅发送「${label}」的声音`);
  }

  function renderInstalledAppOptions() {
    syncAppSelectSelection();
    if (!appPickerList) return;
    const kw = (appSearchInput?.value || "").trim().toLowerCase();
    const filtered = kw
      ? installedApps.filter((a) =>
          (a.label || "").toLowerCase().includes(kw)
          || (a.packageName || "").toLowerCase().includes(kw))
      : installedApps;

    appPickerList.innerHTML = "";

    const makeItem = ({ title, sub, selected, playing, onClick }) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "app-picker-item" + (selected ? " selected" : "");
      btn.innerHTML = `
        <div class="app-picker-item-copy">
          <div class="app-picker-item-title"></div>
          <div class="app-picker-item-sub"></div>
        </div>
        ${playing ? '<span class="app-picker-badge">正在发声</span>' : ""}
      `;
      btn.querySelector(".app-picker-item-title").textContent = title;
      btn.querySelector(".app-picker-item-sub").textContent = sub;
      btn.addEventListener("click", onClick);
      return btn;
    };

    if (!kw) {
      appPickerList.appendChild(makeItem({
        title: "全部媒体应用（全局内录）",
        sub: "自动排除本应用回声，转发手机所有媒体声音",
        selected: !(state.targetAppUid > 0),
        playing: false,
        onClick: () => selectTargetApp(-1, "", ""),
      }));
    }

    const playingApps = filtered.filter((a) => !!a.playing);
    const otherApps = filtered.filter((a) => !a.playing);

    if (playingApps.length > 0) {
      const hdr = document.createElement("div");
      hdr.className = "app-picker-section";
      hdr.textContent = `正在发声的应用 (${playingApps.length})`;
      appPickerList.appendChild(hdr);
      for (const app of playingApps) {
        const isSel = state.targetAppUid === app.uid && state.targetAppPkg === app.packageName;
        appPickerList.appendChild(makeItem({
          title: app.label || app.packageName,
          sub: app.packageName,
          selected: isSel,
          playing: true,
          onClick: () => selectTargetApp(app.uid, app.packageName, app.label || app.packageName),
        }));
      }
    }

    if (otherApps.length > 0) {
      const hdr = document.createElement("div");
      hdr.className = "app-picker-section";
      hdr.textContent = kw ? `搜索结果 (${otherApps.length})` : `全部已安装应用 (${otherApps.length})`;
      appPickerList.appendChild(hdr);
      for (const app of otherApps) {
        const isSel = state.targetAppUid === app.uid && state.targetAppPkg === app.packageName;
        appPickerList.appendChild(makeItem({
          title: app.label || app.packageName,
          sub: app.packageName,
          selected: isSel,
          playing: false,
          onClick: () => selectTargetApp(app.uid, app.packageName, app.label || app.packageName),
        }));
      }
    }

    if (kw && filtered.length === 0) {
      const empty = document.createElement("div");
      empty.className = "app-picker-section";
      empty.textContent = `未找到匹配「${kw}」的应用`;
      appPickerList.appendChild(empty);
    }
  }

  async function refreshInstalledApps(notify = false) {
    const res = await rpcAsync("app.listAudioApps");
    if (res?.error && notify) showToast(res.error);
    if (!Array.isArray(res?.result)) return;
    installedApps = res.result;
    renderInstalledAppOptions();
    if (notify) {
      const playingCount = installedApps.filter((a) => !!a.playing).length;
      showToast(playingCount > 0
        ? `已刷新 ${installedApps.length} 个应用，置顶 ${playingCount} 个正在发声的应用`
        : `已加载本机 ${installedApps.length} 个应用`);
    }
  }

  async function refreshInterfaces() {
    const res = await rpcAsync("net.listInterfaces");
    if (Array.isArray(res?.result)) {
      interfaces = res.result;
      if (!bindIpSelect) return;
      const currentVal = state.autoBind ? "" : (state.bindIp || "");
      bindIpSelect.innerHTML = "";
      const autoOpt = document.createElement("option");
      autoOpt.value = "";
      autoOpt.textContent = `自动选择路由网卡 (${state.bindIp || "默认"})`;
      bindIpSelect.appendChild(autoOpt);
      for (const iface of interfaces) {
        const opt = document.createElement("option");
        opt.value = iface.ipv4;
        opt.textContent = `${iface.name} · ${iface.ipv4}`;
        bindIpSelect.appendChild(opt);
      }
      bindIpSelect.value = currentVal;
    }
  }


  sourceSegBtns.forEach((btn) => {
    btn.addEventListener("click", async () => {
      const mode = Number(btn.dataset.mode);
      if (mode === MODE_WAV_FILE && (state.sourceMode !== MODE_WAV_FILE || !(state.track?.durationMs > 0))) {
        window.SoundFlowBridge?.pickSenderWav?.();
      } else {
        const res = await rpcAsync("source.setMode", { mode });
        if (res?.error) showToast(res.error);
      }
    });
  });

  appTriggerBtn?.addEventListener("click", async () => {
    if (appSearchInput) appSearchInput.value = "";
    appPickerBackdrop?.classList.add("open");
    renderInstalledAppOptions();
    await refreshInstalledApps(false);
    setTimeout(() => appSearchInput?.focus(), 60);
  });

  appPickerClose?.addEventListener("click", closeAppPicker);
  appPickerBackdrop?.addEventListener("click", (e) => {
    if (e.target === appPickerBackdrop) closeAppPicker();
  });

  appSearchInput?.addEventListener("input", () => {
    renderInstalledAppOptions();
  });

  appRefreshBtn?.addEventListener("click", () => refreshInstalledApps(true));

  let adbConnecting = false;
  async function connectWirelessAdb(enableMute = false) {
    if (adbConnecting) return false;
    const port = Number(guardAdbPort?.value || 0);
    if (!Number.isInteger(port) || port < 0 || port > 65535) {
      showToast("连接端口须在 1–65535 之间，留空可自动发现");
      return false;
    }
    adbConnecting = true;
    showToast("正在连接本机无线调试…");
    let res;
    try {
      res = await rpcAsync("adb.connect", { port, enableMute }, 45000);
    } finally {
      adbConnecting = false;
    }
    if (res?.result) {
      state = { ...state, ...res.result };
      renderState();
    }
    if (!res || res.error) {
      if (res?.result?.adbPairingSuggested && window.SoundFlowBridge?.openAdbPairing) {
        window.SoundFlowBridge.openAdbPairing(enableMute);
      } else {
        showToast(res?.error || "连接本机无线调试超时");
      }
      return false;
    }
    refreshInstalledApps(false);
    const boundHint = state.targetAppUid > 0 && state.targetAppName
      ? ` · 已自动锁定「${state.targetAppName}」`
      : "";
    showToast((enableMute ? "无线 ADB 已激活并开启本机静音投送源" : "无线 ADB 特权免投屏通道已激活") + boundHint);
    return true;
  }

  muteLocalSwitch?.addEventListener("click", async () => {
    const next = !state.muteLocalApp;
    if (next && !state.muteLocalSupported && window.SoundFlowBridge?.platform !== "win32") {
      await connectWirelessAdb(true);
      return;
    }
    muteLocalSwitch.setAttribute("aria-checked", String(next));
    const res = await rpcAsync("source.setMuteLocalApp", { muteLocalApp: next });
    if (res?.error) {
      muteLocalSwitch.setAttribute("aria-checked", String(!!state.muteLocalApp));
      showToast(res.error);
      return;
    }
    showToast(next ? "已开启本机静音投送源" : "已恢复本机同响");
  });

  adbBtn?.addEventListener("click", () => {
    connectWirelessAdb(false);
  });

  btnAdbConnect?.addEventListener("click", () => {
    connectWirelessAdb(false);
  });

  btnAdbPair?.addEventListener("click", () => window.SoundFlowBridge?.openAdbPairing?.(false));

  btnAdbDisconnect?.addEventListener("click", async () => {
    const res = await rpcAsync("adb.disconnect");
    if (res?.result) {
      state = { ...state, ...res.result };
      renderState();
    }
    showToast("已断开无线 ADB 特权音频守护进程");
  });

  rowGuardShizuku?.addEventListener("click", async () => {
    const res = await rpcAsync("shizuku.requestPermission");
    if (res?.error) showToast(res.error);
    else showToast("已发起 Shizuku 授权请求");
  });

  rowDevSettings?.addEventListener("click", () => {
    window.SoundFlowBridge?.openDeveloperSettings?.();
  });

  shizukuBtn?.addEventListener("click", async () => {
    const res = await rpcAsync("shizuku.requestPermission");
    if (res?.error) showToast(res.error);
    else showToast("已发起 Shizuku 授权请求");
  });

  projectionBtn?.addEventListener("click", () => {
    rpcAsync("source.setMode", { mode: MODE_SYSTEM_LOOPBACK, requestProjection: true });
  });

  notifBtn?.addEventListener("click", () => {
    window.SoundFlowBridge?.openNotificationAccessSettings?.();
  });
  guardNotifRow?.addEventListener("click", () => {
    window.SoundFlowBridge?.openNotificationAccessSettings?.();
  });

  btnPickWav?.addEventListener("click", () => window.SoundFlowBridge?.pickSenderWav?.());
  btnPickLrc?.addEventListener("click", () => window.SoundFlowBridge?.pickSenderLrc?.());
  btnOpenStage?.addEventListener("click", () => window.SoundFlowBridge?.openSenderLyricsStage?.());

  btnPrev?.addEventListener("click", () => rpcAsync("media.control", { action: "prev" }));
  btnNext?.addEventListener("click", () => rpcAsync("media.control", { action: "next" }));
  btnPlayPause?.addEventListener("click", () => {
    const act = state.track?.paused ? "resume" : "pause";
    rpcAsync("media.control", { action: act });
  });

  if (wavSeekSlider) {
    wavSeekSlider.addEventListener("pointerdown", () => { seekingWavPos = true; });
    wavSeekSlider.addEventListener("input", () => {
      const dur = Math.max(1, Number(wavSeekSlider.max) || 1);
      const pos = Number(wavSeekSlider.value);
      setSliderFill(wavSeekSlider, (pos / dur) * 100);
      if (wavTimeCur) wavTimeCur.textContent = formatTimeMs(pos);
    });
    wavSeekSlider.addEventListener("change", () => {
      seekingWavPos = false;
      rpcAsync("wav.seek", { positionMs: Number(wavSeekSlider.value) });
    });
    wavSeekSlider.addEventListener("pointercancel", () => { seekingWavPos = false; });
  }

  if (masterVolSlider) {
    masterVolSlider.addEventListener("pointerdown", () => { seekingMasterVol = true; });
    masterVolSlider.addEventListener("input", () => {
      const v = Number(masterVolSlider.value);
      if (masterVolVal) masterVolVal.textContent = v + "%";
      setSliderFill(masterVolSlider, v);
    });
    masterVolSlider.addEventListener("change", () => {
      seekingMasterVol = false;
      rpcAsync("volume.setMaster", { volume: Number(masterVolSlider.value) });
    });
    masterVolSlider.addEventListener("pointercancel", () => { seekingMasterVol = false; });
  }

  btnRefreshScan?.addEventListener("click", async () => {
    await rpcAsync("discovery.refresh", { automatic: false });
    await refreshInterfaces();
    showToast("已触发局域网 mDNS 音箱扫描");
  });

  btnToggleManual?.addEventListener("click", () => {
    manualDrawerOpen = !manualDrawerOpen;
    if (manualDrawer) manualDrawer.style.display = manualDrawerOpen ? "grid" : "none";
    if (manualDrawerOpen) manualIpInput?.focus();
  });

  btnAddManualSubmit?.addEventListener("click", async () => {
    const name = (manualNameInput?.value || "").trim();
    const ip = (manualIpInput?.value || "").trim();
    const cmdPort = Number(manualPortInput?.value || 8899) || 8899;
    if (!ip) {
      showToast("请输入目标音箱的局域网 IPv4 地址");
      return;
    }
    const res = await rpcAsync("device.addManual", { name, ip, cmdPort });
    if (res?.error) {
      showToast(res.error);
      return;
    }
    if (res?.result) {
      await rpcAsync("multiroom.addDevice", { deviceId: res.result });
    }
    if (manualIpInput) manualIpInput.value = "";
    if (manualNameInput) manualNameInput.value = "";
    manualDrawerOpen = false;
    if (manualDrawer) manualDrawer.style.display = "none";
    showToast("已添加并勾选目标音箱");
  });

  codecSegBtns.forEach((btn) => {
    btn.addEventListener("click", async () => {
      const codec = btn.dataset.codec || "AAC_LC";
      await rpcAsync("codec.set", { codec, sampleRate: 48000 });
      showToast(codec === "LPCM" ? "已切换全屋编码：无损 LPCM 48kHz（下次推流生效）" : "已切换全屋编码：AAC-LC 48kHz 低延迟");
    });
  });

  if (baseDelaySlider) {
    baseDelaySlider.addEventListener("pointerdown", () => { seekingBaseDelay = true; });
    baseDelaySlider.addEventListener("input", () => {
      const v = Number(baseDelaySlider.value);
      if (baseDelayVal) baseDelayVal.textContent = v + " ms";
      setSliderFill(baseDelaySlider, ((v - 40) / 760) * 100);
    });
    baseDelaySlider.addEventListener("change", () => {
      seekingBaseDelay = false;
      rpcAsync("sync.setBaseDelay", { delayMs: Number(baseDelaySlider.value) });
    });
    baseDelaySlider.addEventListener("pointercancel", () => { seekingBaseDelay = false; });
  }
  baseDelayReset?.addEventListener("click", () => {
    if (baseDelaySlider) baseDelaySlider.value = "300";
    if (baseDelayVal) baseDelayVal.textContent = "300 ms";
    if (baseDelaySlider) setSliderFill(baseDelaySlider, ((300 - 40) / 760) * 100);
    rpcAsync("sync.setBaseDelay", { delayMs: 300 });
  });

  bindIpSelect?.addEventListener("change", async () => {
    const ip = bindIpSelect.value || "";
    const res = await rpcAsync("net.setBindIp", { ip });
    if (res?.error) showToast(res.error);
    else showToast(ip ? `已锁定发包网卡 ${ip}` : "已恢复自动路由选择");
  });

  window.SoundFlowSender = {
    onRpcReply(jsonStr) {
      try {
        const msg = typeof jsonStr === "string" ? JSON.parse(jsonStr) : jsonStr;
        if (msg && msg.id && pendingRpcs.has(msg.id)) {
          const fn = pendingRpcs.get(msg.id);
          pendingRpcs.delete(msg.id);
          fn(msg);
        }
      } catch (_) {}
    },
    onEvent(jsonStr) {
      try {
        const evt = typeof jsonStr === "string" ? JSON.parse(jsonStr) : jsonStr;
        if (!evt) return;
        if (evt.type === "state" && evt.data) {
          state = { ...state, ...evt.data };
          renderState();
        } else if (evt.type === "spectrum") {
          if (getActiveTab() === "sender") {
            renderSpectrum(evt.levels);
          }
        } else if (evt.type === "remoteControl") {
          const actionMap = {
            pause: "暂停播放",
            resume: "继续播放",
            next: "切换下一首",
            previous: "切换上一首",
            seek: "跳转进度",
          };
          const desc = actionMap[evt.action] || evt.action;
          showToast(`音箱「${evt.deviceName || "远端设备"}」反向控制：${desc}`);
        }
      } catch (_) {}
    },
  };

  rpcAsync("state.get").then((res) => {
    if (res?.result) {
      state = { ...state, ...res.result };
      renderState();
    }
    refreshInterfaces();
    refreshInstalledApps(false);
  });

  return {
    getState: () => state,
    toggleMultiroom,
    onTabChanged(tab) {
      if (tab === "sender") {
        rpcAsync("state.get").then((res) => {
          if (res?.result) {
            state = { ...state, ...res.result };
            renderState();
          }
        });
        refreshInterfaces();
        refreshInstalledApps(false);
      }
    },
  };
}
