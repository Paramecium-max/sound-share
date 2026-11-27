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
    muteLocalApp: false,
    muteLocalSupported: false,
    muteLocalHint: "",
    timerServicePort: 0,
    sampleRate: 48000,
    basePlayDelayMs: 350,
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
  const statusDot = document.getElementById("sender-status-dot");
  const statusLabel = document.getElementById("sender-status-label");
  const mainToggleBtn = document.getElementById("sender-toggle-btn");
  const errBanner = document.getElementById("sender-error-banner");
  const errText = document.getElementById("sender-error-text");

  const sourceSegBtns = Array.from(document.querySelectorAll("#sender-source-seg button"));
  const appFilterRow = document.getElementById("sender-app-row");
  const appSelect = document.getElementById("sender-app-select");
  const appRefreshBtn = document.getElementById("sender-app-refresh-btn");
  const muteLocalRow = document.getElementById("sender-mute-local-row");
  const muteLocalDesc = document.getElementById("sender-mute-local-desc");
  const muteLocalSwitch = document.getElementById("sender-mute-local-switch");
  const shizukuBtn = document.getElementById("sender-shizuku-btn");
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

  function rpcAsync(method, params = {}) {
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
        }, 8000);
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
          <div class="sender-dev-sub tnum"></div>
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
      const residualMs = Math.abs((Number(d.syncResidualUs) || 0) / 1000);
      badgeEl.textContent = d.syncPackets > 0 && Number.isFinite(residualMs) && residualMs <= 5000
        ? `同步 ±${residualMs.toFixed(1)}ms`
        : (d.syncPackets > 0 ? "时钟对齐中…" : "RTP 推流中");
      badgeEl.className = "sender-dev-badge tnum streaming";
    } else if (d.inGroup && state.multiroomRunning) {
      badgeEl.textContent = d.cmdConnected ? "握手就绪" : "连接中…";
      badgeEl.className = "sender-dev-badge tnum connecting";
    } else {
      badgeEl.textContent = d.online ? (d.isManual ? "手动目标" : "在线") : "离线";
      badgeEl.className = "sender-dev-badge tnum";
    }

    const subParts = [`${d.ip}:${d.cmdPort}`];
    if (d.statusText && d.statusText !== "待机") subParts.push(d.statusText);
    if (d.reportedLatencyMs > 0) subParts.push(`设备缓冲 ${d.reportedLatencyMs}ms`);
    card.querySelector(".sender-dev-sub").textContent = subParts.join(" · ");

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
    if (!appSelect) return;
    const desiredVal = state.targetAppUid > 0 && state.targetAppPkg
      ? `${state.targetAppUid}|${state.targetAppPkg}`
      : "0";
    const exists = Array.from(appSelect.options).some((opt) => opt.value === desiredVal);
    if (exists) {
      appSelect.value = desiredVal;
    } else if (state.targetAppUid > 0 && state.targetAppPkg) {
      const opt = document.createElement("option");
      opt.value = desiredVal;
      opt.textContent = `仅采集：${state.targetAppName || state.targetAppPkg}`;
      appSelect.appendChild(opt);
      appSelect.value = desiredVal;
    } else {
      appSelect.value = "0";
    }
  }

  function renderState() {
    const running = !!state.multiroomRunning;
    const { inGroup, streaming } = countGroupDevices();
    const codecText = state.codec === "LPCM"
      ? `无损 LPCM ${Math.round((state.sampleRate || 48000) / 1000)}kHz`
      : "AAC-LC 48kHz";
    const singleAppActive = state.sourceMode === MODE_SYSTEM_LOOPBACK && state.targetAppUid > 0;

    if (statusDot) {
      statusDot.className = "status-dot " + (running ? (streaming > 0 ? "playing" : "ready") : state.lastError ? "warn" : "");
    }
    if (statusLabel) {
      statusLabel.textContent = running
        ? `${singleAppActive ? `单应用「${state.targetAppName}」` : "全屋微秒级时钟同步"} · ${codecText}`
        : `本机发包 ${state.bindIp || "未连接 Wi-Fi"} · 自动屏蔽自身回环`;
    }
    if (statusHeadline) {
      statusHeadline.textContent = running
        ? (streaming > 0 ? `正在向 ${streaming} 台音箱同步播放` : "全屋推流已开启，等待音箱连接")
        : (inGroup > 0 ? `已选 ${inGroup} 台音箱，随时开始全屋播放` : "勾选音箱，一键开启全屋同步立体声");
    }
    if (statusSub) {
      statusSub.textContent = running
        ? `基准缓冲 ${state.basePlayDelayMs} ms · 校时 UDP :${state.timerServicePort || "--"}`
        : (singleAppActive
          ? `已锁定仅发送「${state.targetAppName}」的声音，其他应用通知音不会混入音箱`
          : "支持指定单个 App 声音独立发送或全局内录，毫秒级多音箱声场对齐");
    }
    if (mainToggleBtn) {
      mainToggleBtn.textContent = running ? "停止全屋发送" : "开始全屋播放";
      mainToggleBtn.className = "pill-cta " + (running ? "stop" : "primary");
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

    const isDirectPrivileged = state.captureBackend === "privileged_policy" || state.captureBackend === "shizuku";
    if (muteLocalRow) {
      muteLocalRow.style.display = state.sourceMode === MODE_SYSTEM_LOOPBACK ? "flex" : "none";
    }
    if (muteLocalSwitch) {
      muteLocalSwitch.setAttribute("aria-checked", String(!!state.muteLocalApp));
      muteLocalSwitch.disabled = !state.muteLocalSupported && !state.muteLocalApp;
    }
    if (shizukuBtn) {
      const showShizuku = !isDirectPrivileged && !!state.shizukuAvailable && !state.shizukuGranted && state.captureBackend === "media_projection";
      shizukuBtn.style.display = showShizuku ? "inline-flex" : "none";
    }
    if (muteLocalDesc) {
      if (isDirectPrivileged) {
        const badge = state.captureBackend === "privileged_policy" ? "系统特权直抓 · 免投屏" : "Shizuku 直抓 · 免投屏";
        muteLocalDesc.textContent = singleAppActive
          ? `${badge} · 仅在全屋音箱发声，本机其他应用照常外放`
          : `${badge} · 开启后拦截媒体外放仅走全屋音箱`;
      } else if (state.muteLocalSupported) {
        muteLocalDesc.textContent = singleAppActive
          ? "仅在全屋音箱发声，电脑本地其他应用照常外放"
          : "需选择单个目标应用后可仅对该应用开启本机消音";
      } else {
        muteLocalDesc.textContent = state.muteLocalHint || "当前环境暂不支持单应用本机独立消音";
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
        : (tr.hasLyrics ? "已就绪完整同步歌词，可展开全屏舞台" : "支持读取系统状态栏歌词或手动绑定本地 .lrc");
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
      setSliderFill(baseDelaySlider, ((state.basePlayDelayMs - 100) / 900) * 100);
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

  async function refreshInstalledApps(notify = false) {
    const res = await rpcAsync("app.listAudioApps");
    if (res?.error && notify) showToast(res.error);
    if (!Array.isArray(res?.result) || !appSelect) return;
    installedApps = res.result;
    appSelect.innerHTML = "";
    const allOpt = document.createElement("option");
    allOpt.value = "0";
    allOpt.textContent = "全部媒体应用（全局内录 · 自动排除本应用回声）";
    appSelect.appendChild(allOpt);

    for (const app of installedApps) {
      const opt = document.createElement("option");
      opt.value = `${app.uid}|${app.packageName}`;
      opt.dataset.label = app.label;
      opt.textContent = `仅发送：${app.label} (${app.packageName})`;
      appSelect.appendChild(opt);
    }
    syncAppSelectSelection();
    if (notify) showToast(`已加载本机 ${installedApps.length} 个应用`);
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

  mainToggleBtn?.addEventListener("click", toggleMultiroom);

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

  appSelect?.addEventListener("change", async () => {
    const val = appSelect.value || "0";
    if (val === "0" || !val.includes("|")) {
      const res = await rpcAsync("source.setTargetApp", { uid: -1, packageName: "", label: "" });
      if (res?.error) { syncAppSelectSelection(); showToast(res.error); return; }
      showToast("已切换为全局媒体内录（发送全部媒体声音）");
      return;
    }
    const sep = val.indexOf("|");
    const uid = Number(val.slice(0, sep)) || -1;
    const pkg = val.slice(sep + 1);
    const matched = installedApps.find((a) => a.uid === uid && a.packageName === pkg);
    const label = matched?.label || pkg;
    const res = await rpcAsync("source.setTargetApp", { uid, packageName: pkg, label });
    if (res?.error) { syncAppSelectSelection(); showToast(res.error); return; }
    showToast(`已锁定仅发送「${label}」的声音`);
  });

  appRefreshBtn?.addEventListener("click", () => refreshInstalledApps(true));

  muteLocalSwitch?.addEventListener("click", async () => {
    const next = !state.muteLocalApp;
    muteLocalSwitch.setAttribute("aria-checked", String(next));
    const res = await rpcAsync("source.setMuteLocalApp", { muteLocalApp: next });
    if (res?.error) {
      muteLocalSwitch.setAttribute("aria-checked", String(!!state.muteLocalApp));
      showToast(res.error);
      return;
    }
    showToast(next ? "已开启本机静音投送源" : "已恢复本机同响");
  });

  shizukuBtn?.addEventListener("click", async () => {
    const res = await rpcAsync("shizuku.requestPermission");
    if (res?.error) showToast(res.error);
    else showToast("已发起 Shizuku 授权请求");
  });

  projectionBtn?.addEventListener("click", () => {
    rpcAsync("source.setMode", { mode: MODE_SYSTEM_LOOPBACK });
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
      setSliderFill(baseDelaySlider, ((v - 100) / 900) * 100);
    });
    baseDelaySlider.addEventListener("change", () => {
      seekingBaseDelay = false;
      rpcAsync("sync.setBaseDelay", { delayMs: Number(baseDelaySlider.value) });
    });
    baseDelaySlider.addEventListener("pointercancel", () => { seekingBaseDelay = false; });
  }
  baseDelayReset?.addEventListener("click", () => {
    if (baseDelaySlider) baseDelaySlider.value = "350";
    if (baseDelayVal) baseDelayVal.textContent = "350 ms";
    if (baseDelaySlider) setSliderFill(baseDelaySlider, ((350 - 100) / 900) * 100);
    rpcAsync("sync.setBaseDelay", { delayMs: 350 });
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
        if (!installedApps.length) refreshInstalledApps(false);
      }
    },
  };
}
