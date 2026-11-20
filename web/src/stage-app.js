import { RippleField } from "./ripple-field.mjs";
import { RIPPLE_TRIGGER_MODES } from "./ripple-trigger.mjs";
import { RippleOnsetQueue } from "./ripple-onsets.mjs";
import { WaterRippleRenderer } from "./ripple-water.mjs";
import { attachButtonHighlights, attachLiquidDock } from "./liquid-dock.mjs";
import {
  BackgroundRender,
  DomLyricPlayer,
  LayoutReason,
  MeshGradientRenderer,
} from "@applemusic-like-lyrics/core";

const stageWrap = document.getElementById("lyric-stage-wrap");
const stageAnchor = document.createComment("lyric-stage-anchor");
stageWrap.before(stageAnchor);
const stageCard = document.getElementById("lyric-stage-card");
const stageRight = document.querySelector(".stage-right");
const bgHost = document.getElementById("bg-host");
const lyricHost = document.getElementById("lyric-host");
const smoothRail = document.getElementById("smooth-rail");
const platterDisc = document.getElementById("platter-disc");
const rippleCanvas = document.getElementById("center-ripple-canvas");
const songTitleEl = document.getElementById("song-title");
const songArtistEl = document.getElementById("song-artist");
const styleBadgeEl = document.getElementById("style-badge");
const modeBadgeEl = document.getElementById("mode-badge");
const codecBadgeEl = document.getElementById("codec-badge");
const coverCanvas = document.getElementById("cover-canvas");

const STYLE_LABELS = {
  amll: "AMLL 流体",
  "qq-vinyl": "QQ 黑胶水波",
  "qq-water": "QQ 极光水波",
  "qq-karaoke": "QQ 逐字动感",
};

const bgRender = BackgroundRender.new(MeshGradientRenderer);
// Low offscreen resolution upscaled softly by GPU cuts pixel fill ~17x while maintaining 60+ fps smoothness
bgRender.setRenderScale(0.14);
bgRender.setFPS(60);
bgRender.setFlowSpeed(1.4);
bgRender.setHasLyric(true);
bgHost.appendChild(bgRender.getElement());
bgRender.pause();

// Official AMLL DomLyricPlayer (spring physics + word sweep at native refresh; blur disabled to prevent per-frame CPU rasterization)
const lyricPlayer = new DomLyricPlayer();
lyricPlayer.setAlignAnchor("center");
lyricPlayer.setAlignPosition(0.45);
lyricPlayer.setEnableSpring(true);
lyricPlayer.setEnableBlur(false);
lyricPlayer.setEnableScale(true);
lyricPlayer.setWordFadeWidth(0.55);
lyricHost.appendChild(lyricPlayer.getElement());

let isPlaying = false;
let isStageShown = false;
let isFullscreen = false;
let userDismissedFullscreen = false;
let tunerOpen = false;
let rafId = 0;
let lastFrameTs = 0;
let frameSamples = [];
let statsWindowStart = 0;
let currentTimeMs = 0;
let audioEnergy = 0;
let smoothedEnergy = 0;
let trackKey = "";
let fullLrcActive = false;
let fullLrcLines = [];
let fullLyricSignature = "";
let streamLines = [];
let nextStreamStartMs = 0;
let hasCustomArtwork = false;
let lastSentFlowSpeed = -1;
const rippleField = new RippleField();
const rippleOnsets = new RippleOnsetQueue();
const rippleRenderer = new WaterRippleRenderer(rippleCanvas);
let rippleTriggerMode = localStorage.getItem("sf_ripple_trigger") || "classic";
if (!RIPPLE_TRIGGER_MODES.includes(rippleTriggerMode)) rippleTriggerMode = "classic";
let rippleWidth = 0;
let rippleHeight = 0;
let rippleOriginX = 0.5;
let rippleOriginY = 0.5;
let rippleCoverRadius = 0.08;
const platterWrapEl = document.getElementById("platter-wrap");
const platterPulse = { current: 1, velocity: 0 };
let avgBass = 0;
let avgMid = 0;
let avgHigh = 0;
let prevBass = 0;
let prevMid = 0;
let prevHigh = 0;
let envPeakRef = 0.14;
let lastBassSpawnTs = 0;
let lastMidSpawnTs = 0;
let lastHighSpawnTs = 0;
let lastAnySpawnTs = 0;
let canvasNeedsClear = false;

// FIFO delay queue for negative sync offset (fixes "歌词太快" cleanly without losing lines)
let pendingLineQueue = [];
let lastSmoothIdx = -1;
let lastMatchedLrcIdx = -1;

// Persisted user preferences
const reduceMotionQuery = matchMedia("(prefers-reduced-motion: reduce)");
let reduceMotion = reduceMotionQuery.matches;
let lyricStyle = localStorage.getItem("sf_lyric_style") || "amll";
let centerRippleEnabled = localStorage.getItem("sf_center_ripple") !== "0";
let syncOffsetMs = Number(localStorage.getItem("sf_sync_offset") ?? -600);
if (!Number.isFinite(syncOffsetMs)) syncOffsetMs = -600;
let animSpeed = Number(localStorage.getItem("sf_anim_speed") ?? 1.0);
if (!Number.isFinite(animSpeed) || animSpeed < 0.25 || animSpeed > 2.5) animSpeed = 1.0;

function usingNativeAmll() {
  return lyricStyle === "amll" && fullLrcActive && fullLrcLines.length > 0;
}

function updateRendererModeClass() {
  stageRight.classList.toggle("use-native-amll", usingNativeAmll());
}

function effectiveClockMs() {
  if (!fullLrcActive) {
    const last = streamLines[streamLines.length - 1];
    if (!last) return Math.max(0, currentTimeMs);
    return last.startTime + Math.max(0, (currentTimeMs - last.startTime) * animSpeed);
  }
  return Math.max(0, currentTimeMs + syncOffsetMs);
}

function hashStr(input) {
  let h = 2166136261;
  for (let i = 0; i < input.length; i++) {
    h ^= input.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}

function paintGenerativeCover(seedText) {
  if (hasCustomArtwork) return;
  const ctx = coverCanvas.getContext("2d");
  const w = coverCanvas.width;
  const h = hashStr(seedText || "SoundFlow");
  const hue1 = h % 360;
  const hue2 = (hue1 + 58 + ((h >> 8) % 60)) % 360;
  const hue3 = (hue2 + 72) % 360;

  const grad = ctx.createLinearGradient(0, 0, w, w);
  grad.addColorStop(0, `hsl(${hue1}, 48%, 36%)`);
  grad.addColorStop(0.55, `hsl(${hue2}, 44%, 28%)`);
  grad.addColorStop(1, `hsl(${hue3}, 50%, 22%)`);
  ctx.fillStyle = grad;
  ctx.fillRect(0, 0, w, w);

  ctx.globalAlpha = 0.42;
  ctx.fillStyle = `hsl(${hue3}, 58%, 60%)`;
  ctx.beginPath();
  ctx.arc(w * (0.28 + ((h % 35) / 100)), w * (0.28 + (((h >> 5) % 35) / 100)), w * 0.30, 0, Math.PI * 2);
  ctx.fill();

  ctx.globalAlpha = 0.28;
  ctx.fillStyle = "#ffffff";
  ctx.beginPath();
  ctx.arc(w * 0.66, w * 0.68, w * 0.22, 0, Math.PI * 2);
  ctx.fill();
  ctx.globalAlpha = 1;

  try {
    bgRender.setAlbum(coverCanvas.toDataURL("image/png"), false);
  } catch (_) {}
}

function buildWordsForLine(text, startMs, endMs) {
  const safeStart = Math.max(0, Math.round(startMs));
  const safeEnd = Math.max(safeStart + 200, Math.round(endMs));
  const trimmed = (text || "").trim();
  if (!trimmed) {
    return [{ word: "···", romanWord: "", startTime: safeStart, endTime: safeEnd, obscene: false, emptyBeat: 0 }];
  }
  const hasCjk = /[\u4e00-\u9fff\u3040-\u30ff]/.test(trimmed);
  const tokens = hasCjk ? Array.from(trimmed) : trimmed.split(/(\s+)/).filter(Boolean);
  const count = Math.max(1, tokens.length);
  const span = Math.max(400, safeEnd - safeStart);
  const step = span / count;
  return tokens.map((tok, i) => {
    const ws = Math.round(safeStart + i * step);
    const we = i + 1 === count ? safeEnd : Math.max(ws + 40, Math.round(safeStart + (i + 1) * step));
    return {
      word: tok,
      romanWord: "",
      startTime: ws,
      endTime: we,
      obscene: false,
      emptyBeat: 0,
    };
  });
}

function activeLinesSource() {
  return fullLrcActive && fullLrcLines.length > 0 ? fullLrcLines : streamLines;
}

function findActiveLineInfo(lines, clockMs) {
  if (!lines || lines.length === 0) return { idx: -1, progress: 0 };
  if (!fullLrcActive) {
    const idx = lines.length - 1;
    const line = lines[idx];
    const span = Math.max(1400, line.estDur || 3200);
    const p = Math.max(0.08, Math.min(1, ((currentTimeMs - line.startTime) * animSpeed) / span));
    return { idx, progress: p };
  }
  let idx = 0;
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].startTime <= clockMs) idx = i;
    else break;
  }
  const cur = lines[idx];
  const span = Math.max(300, cur.endTime - cur.startTime);
  const progress = Math.max(0, Math.min(1, (clockMs - cur.startTime) / span));
  return { idx, progress };
}

/* ============================================================================
   Smooth Non-Jumping Incremental Rail (Fixes AMLL line-jump + Drives QQ Views)
   ============================================================================ */

function rebuildSmoothRailDom() {
  const lines = activeLinesSource();
  smoothRail.innerHTML = "";
  lastSmoothIdx = -1;
  if (lines.length === 0) {
    const el = document.createElement("div");
    el.className = "s-line active";
    el.textContent = songTitleEl.textContent || "等待歌词同步";
    smoothRail.appendChild(el);
    smoothRail.style.transform = "translateY(-18px)";
    return;
  }
  lines.forEach((line) => appendSmoothLineNode(line.rawText));
  syncSmoothRailHighlight(true);
}

function appendSmoothLineNode(text) {
  if (smoothRail.children.length === 1 && !smoothRail.firstElementChild.dataset.real) {
    smoothRail.innerHTML = "";
  }
  const div = document.createElement("div");
  div.className = "s-line";
  div.dataset.real = "1";
  div.textContent = text;
  smoothRail.appendChild(div);
  while (!fullLrcActive && smoothRail.children.length > 56) {
    smoothRail.firstElementChild.remove();
  }
}

function syncSmoothRailHighlight(forceScroll = false) {
  if (usingNativeAmll()) return;
  const lines = activeLinesSource();
  const { idx, progress } = findActiveLineInfo(lines, effectiveClockMs());
  const children = smoothRail.children;
  if (!children.length || idx < 0) return;

  // Offset if smoothRail clipped oldest items above 56
  const domIdx = Math.max(0, Math.min(children.length - 1, idx - Math.max(0, lines.length - children.length)));
  const activeEl = children[domIdx];
  const sweepPct = Math.round(Math.max(10, Math.min(100, progress * 100)));

  if (forceScroll || domIdx !== lastSmoothIdx) {
    for (let i = 0; i < children.length; i++) {
      const c = children[i];
      const dist = Math.abs(i - domIdx);
      c.classList.toggle("active", i === domIdx);
      c.classList.toggle("near", dist === 1);
      if (i !== domIdx) c.style.removeProperty("--sweep");
    }
    if (activeEl) {
      activeEl.style.setProperty("--sweep", sweepPct + "%");
      const targetY = -(activeEl.offsetTop + activeEl.offsetHeight * 0.5);
      smoothRail.style.transform = `translateY(${targetY.toFixed(1)}px)`;
    }
    lastSmoothIdx = domIdx;
  } else if (activeEl) {
    activeEl.style.setProperty("--sweep", sweepPct + "%");
  }
}

/* ============================================================================
   Beat & Frequency Triggered Center Water Ripples (Translucent Caustic Glass)
   ============================================================================ */

function ripplePalette(kind) {
  if (kind === "bass" || kind === "drum") {
    return lyricStyle === "qq-water"
      ? "104, 216, 255"
      : lyricStyle === "qq-karaoke"
        ? "64, 232, 168"
        : "120, 224, 255";
  }
  if (kind === "mid") {
    return lyricStyle === "qq-karaoke" ? "167, 243, 208" : "180, 210, 255";
  }
  return lyricStyle === "qq-karaoke" ? "253, 236, 148" : "234, 246, 255";
}

function triggerCoverThump(strength = 0.7) {
  if (reduceMotion || !platterWrapEl) return;
  const w = Math.max(0.25, Math.min(1, Number(strength) || 0.7));
  // Outward physical thump impulse on the album cover that launches the water wave.
  platterPulse.velocity = Math.min(2.4, platterPulse.velocity + 0.95 + w * 0.95);
}

function stepCoverThump(dt) {
  if (!platterWrapEl) return;
  if (reduceMotion) {
    if (platterPulse.current !== 1 || platterPulse.velocity !== 0) {
      platterPulse.current = 1;
      platterPulse.velocity = 0;
      platterWrapEl.style.transform = "";
    }
    return;
  }
  if (platterPulse.current === 1 && platterPulse.velocity === 0) return;
  const steps = Math.max(1, Math.ceil(dt * 120));
  const h = dt / steps;
  const freq = 34;
  const damp = 0.66;
  for (let i = 0; i < steps; i++) {
    platterPulse.velocity += (freq * freq * (1 - platterPulse.current) - 2 * damp * freq * platterPulse.velocity) * h;
    platterPulse.current += platterPulse.velocity * h;
  }
  platterPulse.current = Math.max(0.96, Math.min(1.10, platterPulse.current));
  if (Math.abs(platterPulse.current - 1) < 0.0008 && Math.abs(platterPulse.velocity) < 0.005) {
    platterPulse.current = 1;
    platterPulse.velocity = 0;
    platterWrapEl.style.transform = "";
  } else {
    platterWrapEl.style.transform = `scale(${platterPulse.current.toFixed(4)})`;
  }
}

function spawnCenterRipple(kind, strength) {
  if (!centerRippleEnabled || !isPlaying || reduceMotion) return;
  if (rippleField.spawn(ripplePalette(kind), rippleTriggerMode === "classic" ? null : strength, 0, false, rippleCoverRadius)) {
    triggerCoverThump(strength ?? 0.65);
  }
}

function processSpectrumOnset(peak, bass, mid, high) {
  if (!centerRippleEnabled || !isPlaying || reduceMotion) return;
  const now = performance.now();
  if (rippleTriggerMode !== "classic") return;

  // Fallback when a sender only pushes scalar level (no PCM 3-band yet)
  const rawMax = Math.max(bass, mid, high);
  let b = bass;
  let m = mid;
  let h = high;
  if (rawMax < 0.004 && peak > 0.01) {
    b = peak * 0.85;
    m = peak * 0.62;
    h = peak * 0.36;
  }

  // Adaptive auto-gain reference so soft verses and hot drops modulate ripple density cleanly
  const instantMax = Math.max(peak, b, m, h);
  envPeakRef = Math.max(0.055, instantMax, envPeakRef * 0.992);

  avgBass = avgBass * 0.82 + b * 0.18;
  avgMid = avgMid * 0.82 + m * 0.18;
  avgHigh = avgHigh * 0.80 + h * 0.20;

  const fluxBass = Math.max(0, b - prevBass);
  const fluxMid = Math.max(0, m - prevMid);
  const fluxHigh = Math.max(0, h - prevHigh);
  prevBass = b;
  prevMid = m;
  prevHigh = h;

  const normB = Math.min(1.0, Math.pow(Math.max(0, b / envPeakRef), 0.78));
  const normM = Math.min(1.0, Math.pow(Math.max(0, m / envPeakRef), 0.82));
  const normH = Math.min(1.0, Math.pow(Math.max(0, h / envPeakRef), 0.85));

  // Song intensity modulates wave density (spacing between successive ripples) while keeping waves spacious & fast enough for 90fps
  const minBassGap = Math.round(350 - normB * 120);
  const minMidGap = Math.round(330 - normM * 100);
  const minHighGap = Math.round(310 - normH * 90);

  let spawned = false;
  if (b > 0.018 && (b > avgBass * 1.10 + 0.014 || fluxBass > envPeakRef * 0.13) && now - lastBassSpawnTs >= minBassGap && now - lastAnySpawnTs >= 210) {
    lastBassSpawnTs = now;
    lastAnySpawnTs = now;
    const hitStrength = Math.min(1.0, normB * 0.68 + (fluxBass / envPeakRef) * 0.72 + (b > avgBass * 1.45 ? 0.22 : 0));
    spawnCenterRipple("bass", Math.max(0.24, hitStrength));
    spawned = true;
  }
  if (m > 0.016 && (m > avgMid * 1.12 + 0.012 || fluxMid > envPeakRef * 0.14) && now - lastMidSpawnTs >= minMidGap && now - lastAnySpawnTs >= 210 && !spawned) {
    lastMidSpawnTs = now;
    lastAnySpawnTs = now;
    const hitStrength = Math.min(1.0, normM * 0.64 + (fluxMid / envPeakRef) * 0.65);
    spawnCenterRipple("mid", Math.max(0.20, hitStrength));
    spawned = true;
  }
  if (h > 0.014 && (h > avgHigh * 1.15 + 0.010 || fluxHigh > envPeakRef * 0.15) && now - lastHighSpawnTs >= minHighGap && now - lastAnySpawnTs >= 210 && !spawned) {
    lastHighSpawnTs = now;
    lastAnySpawnTs = now;
    const hitStrength = Math.min(0.84, normH * 0.56 + (fluxHigh / envPeakRef) * 0.52);
    spawnCenterRipple("high", Math.max(0.16, hitStrength));
    spawned = true;
  }
  // Calm sustained passages keep sparse gentle ripples drifting out to the edges
  const quietCadenceMs = Math.round(780 - Math.max(normB, normM) * 320);
  if (!spawned && instantMax > 0.02 && now - lastAnySpawnTs >= quietCadenceMs) {
    lastAnySpawnTs = now;
    const dominantKind = normB >= normM ? "bass" : "mid";
    const softStrength = Math.max(0.18, Math.min(0.62, Math.max(normB, normM) * 0.58));
    spawnCenterRipple(dominantKind, softStrength);
  }
}

function measureRippleOrigin() {
  if (!stageCard) return;
  const cardRect = stageCard.getBoundingClientRect();
  const target = platterDisc || platterWrapEl;
  if (target && cardRect.width > 0 && cardRect.height > 0) {
    const r = target.getBoundingClientRect();
    if (r.width > 4 && r.height > 4) {
      const cx = r.left + r.width * 0.5 - cardRect.left;
      const cy = r.top + r.height * 0.5 - cardRect.top;
      rippleOriginX = Math.max(0.05, Math.min(0.95, cx / cardRect.width));
      rippleOriginY = Math.max(0.05, Math.min(0.95, cy / cardRect.height));
      const maxDist = Math.max(
        Math.hypot(cx, cy),
        Math.hypot(cardRect.width - cx, cy),
        Math.hypot(cx, cardRect.height - cy),
        Math.hypot(cardRect.width - cx, cardRect.height - cy),
      );
      rippleCoverRadius = Math.max(0.04, Math.min(0.24, (Math.min(r.width, r.height) * 0.44) / Math.max(1, maxDist)));
      return;
    }
  }
  rippleOriginX = 0.5;
  rippleOriginY = 0.5;
  rippleCoverRadius = 0.06;
}

function resizeRippleCanvas() {
  if (!rippleCanvas) return;
  const rect = stageCard.getBoundingClientRect();
  const scale = rippleRenderer.gl ? 0.55 : 0.36;
  rippleWidth = Math.max(1, Math.round(rect.width * scale));
  rippleHeight = Math.max(1, Math.round(rect.height * scale));
  if (rippleCanvas.width !== rippleWidth || rippleCanvas.height !== rippleHeight) {
    rippleCanvas.width = rippleWidth;
    rippleCanvas.height = rippleHeight;
    canvasNeedsClear = true;
  }
  measureRippleOrigin();
}

function stepAndDrawCenterRipples(rawDeltaSec) {
  stepCoverThump(Math.max(0, Math.min(0.05, rawDeltaSec)));
  if (!rippleCanvas) return;
  if (!centerRippleEnabled) rippleField.clear();
  if (!rippleField.waves.length && !canvasNeedsClear && !rippleOnsets.pending.length) return;
  if (!rippleWidth || !rippleHeight) resizeRippleCanvas();
  const now = performance.now();
  const event = rippleOnsets.takeDue(now);
  if (event && isPlaying && centerRippleEnabled && !reduceMotion && rippleTriggerMode !== "classic") {
    measureRippleOrigin();
    if (rippleField.spawn(
      ripplePalette(event.band),
      event.strength,
      Math.max(0, now - event.at) / 1000,
      rippleTriggerMode === "percussion",
      rippleCoverRadius,
    )) {
      triggerCoverThump(event.strength);
    }
  }
  rippleField.step(rawDeltaSec);
  measureRippleOrigin();
  rippleRenderer.render(rippleField.waves, rippleWidth, rippleHeight, rippleOriginX, rippleOriginY);
  canvasNeedsClear = rippleField.waves.length > 0;
  rippleCanvas.classList.toggle("active", centerRippleEnabled && (isPlaying || rippleField.waves.length > 0));
}

function applySpeedPhysics() {
  lyricPlayer.setEnableSpring(!reduceMotion);
  document.documentElement.style.setProperty("--lyric-speed-scale", (1 / animSpeed).toFixed(3));
  try {
    const k = Math.round(140 * animSpeed);
    const d = Math.max(Math.round(2.1 * Math.sqrt(k)), Math.round(25 * Math.sqrt(animSpeed)));
    lyricPlayer.setLinePosYSpringParams?.({ stiffness: k, damping: d, mass: 1 });
    lyricPlayer.setLinePosXSpringParams?.({ stiffness: k, damping: d, mass: 1 });
    lyricPlayer.setLineScaleSpringParams?.({ stiffness: k, damping: d, mass: 1 });
  } catch (_) {}
  platterDisc.getAnimations?.().forEach((a) => { a.playbackRate = animSpeed; });
}

function scheduleStageRelayout() {
  if (!isStageShown) return;
  const run = () => {
    updateRendererModeClass();
    try {
      if (usingNativeAmll()) {
        const clock = Math.round(effectiveClockMs());
        lyricPlayer.onResize?.();
        lyricPlayer.calcLayout?.(LayoutReason.ContinuousScroll);
        lyricPlayer.setCurrentTime(clock, true);
        lyricPlayer.update(0);
      } else {
        syncSmoothRailHighlight(true);
      }
      window.LiquidGlassRefraction?.refresh();
    } catch (e) {
      console.warn("scheduleStageRelayout error", e);
    }
  };
  requestAnimationFrame(run);
  setTimeout(run, 140);
}

function flushPendingLineQueue(nowTs) {
  while (pendingLineQueue.length > 0) {
    const item = pendingLineQueue[0];
    if (syncOffsetMs >= -80 || nowTs >= item.deliverAt) {
      pendingLineQueue.shift();
      if (item.songKey === trackKey) commitStreamLineNow(item.text);
    } else {
      break;
    }
  }
}

function tick(ts) {
  if (document.hidden || !isStageShown || (!isPlaying && rippleField.waves.length === 0)) {
    rafId = 0;
    return;
  }
  if (!lastFrameTs) { lastFrameTs = ts; statsWindowStart = ts; frameSamples = []; }
  else frameSamples.push(ts - lastFrameTs);
  if (ts - statsWindowStart >= 5000 && frameSamples.length) {
    const sorted = [...frameSamples].sort((a, b) => a - b);
    window.SoundFlowBridge?.onRenderStats?.(JSON.stringify({
      rafFps: +(frameSamples.length * 1000 / (ts - statsWindowStart)).toFixed(1),
      p95Ms: +sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95))].toFixed(1),
      over24Ms: frameSamples.filter(value => value > 24).length,
      samples: frameSamples.length,
    }));
    frameSamples = [];
    statsWindowStart = ts;
  }
  const rawDelta = Math.min(50, Math.max(4, ts - lastFrameTs));
  lastFrameTs = ts;

  stepAndDrawCenterRipples(rawDelta * 0.001);

  if (isPlaying) {
    flushPendingLineQueue(performance.now());
    currentTimeMs += rawDelta;
    const lerpK = 1 - Math.exp(-rawDelta * 0.012);
    smoothedEnergy += (audioEnergy - smoothedEnergy) * lerpK;

    const flowSpd = (1.2 + smoothedEnergy * 3.4) * animSpeed;
    if (Math.abs(flowSpd - lastSentFlowSpeed) >= 0.14) {
      lastSentFlowSpeed = flowSpd;
      bgRender.setFlowSpeed(flowSpd);
    }

    if (usingNativeAmll()) {
      try {
        lyricPlayer.setCurrentTime(Math.round(effectiveClockMs()), false);
        lyricPlayer.update(rawDelta * animSpeed);
      } catch (_) {}
    } else {
      syncSmoothRailHighlight(false);
    }
  }

  rafId = requestAnimationFrame(tick);
}

if (typeof ResizeObserver !== "undefined") {
  new ResizeObserver(() => scheduleStageRelayout()).observe(stageRight);
  new ResizeObserver(resizeRippleCanvas).observe(stageCard);
}
window.addEventListener("resize", scheduleStageRelayout);
function syncVisualActivity() {
  const active = isPlaying && isStageShown && !document.hidden;
  if (active && !reduceMotion) bgRender.resume();
  else bgRender.pause();
  if (active && usingNativeAmll()) lyricPlayer.resume();
  else lyricPlayer.pause();
  if (active || rippleField.waves.length > 0) ensureRaf();
  else if (rafId) {
    cancelAnimationFrame(rafId);
    rafId = 0;
  }
}

reduceMotionQuery.addEventListener("change", (event) => {
  reduceMotion = event.matches;
  if (reduceMotion) { rippleField.clear(); canvasNeedsClear = true; stepAndDrawCenterRipples(0); }
  applySpeedPhysics();
  syncVisualActivity();
});

document.addEventListener("visibilitychange", () => {
  // Do not count time spent hidden as a huge animation step on return.
  lastFrameTs = 0;
  syncVisualActivity();
  if (!document.hidden) scheduleStageRelayout();
});

function ensureRaf() {
  if (!document.hidden && isStageShown && (isPlaying || rippleField.waves.length > 0) && !rafId) {
    lastFrameTs = 0;
    rafId = requestAnimationFrame(tick);
  }
}

let hideTimer = null;

function setStageVisible(show) {
  document.body.classList.toggle("has-lyric-stage", !!show);
  clearTimeout(hideTimer);
  if (show) {
    if (!isStageShown) {
      isStageShown = true;
      stageWrap.classList.add("is-open");
      void stageWrap.offsetHeight;
      stageWrap.classList.add("is-visible");
      // Automatically expand fullscreen when audio casting playback starts!
      if (!userDismissedFullscreen) {
        toggleFullscreen(true);
      }
      syncVisualActivity();
      resizeRippleCanvas();
      scheduleStageRelayout();
    }
  } else {
    userDismissedFullscreen = false;
    if (isFullscreen) toggleFullscreen(false);
    setTunerOpen(false);
    isStageShown = false;
    syncVisualActivity();
    stageWrap.classList.remove("is-visible");
    hideTimer = setTimeout(() => {
      if (!isStageShown) stageWrap.classList.remove("is-open");
    }, 360);
  }
}

function setTunerOpen(open) {
  tunerOpen = !!open;
  window.SoundFlowBridge?.onUiState?.("tuner", tunerOpen);
  stageCard.classList.toggle("tuner-open", tunerOpen);
  document.getElementById("stage-tuner-popover").classList.toggle("open", tunerOpen);
  document.getElementById("stage-tuner-btn").classList.toggle("active", tunerOpen);
  document.getElementById("stage-tuner-btn").setAttribute("aria-expanded", String(tunerOpen));
  scheduleStageRelayout();
  if (tunerOpen) requestAnimationFrame(() => window.LiquidGlassRefraction?.refresh());
}

function toggleFullscreen(forceState, fromUser = false) {
  const next = typeof forceState === "boolean" ? forceState : !isFullscreen;
  if (fromUser && !next) userDismissedFullscreen = true;
  if (isFullscreen === next) return;
  isFullscreen = next;
  window.SoundFlowBridge?.onUiState?.("fullscreen", next);
  // A fullscreen stage must live at the root, outside animated/hidden tab panes.
  if (next) document.body.appendChild(stageWrap);
  else stageAnchor.after(stageWrap);
  stageWrap.classList.toggle("is-fullscreen", next);
  document.body.classList.toggle("is-stage-fullscreen", next);
  const fsIcon = document.getElementById("stage-fs-icon");
  fsIcon.innerHTML = next
    ? '<polyline points="4 14 10 14 10 20"></polyline><polyline points="20 10 14 10 14 4"></polyline><line x1="14" y1="10" x2="21" y2="3"></line><line x1="3" y1="21" x2="10" y2="14"></line>'
    : '<polyline points="15 3 21 3 21 9"></polyline><polyline points="9 21 3 21 3 15"></polyline><line x1="21" y1="3" x2="14" y2="10"></line><line x1="3" y1="21" x2="10" y2="14"></line>';
  window.SoundFlowBridge?.fullscreenChanged?.(next);
  scheduleStageRelayout();
}

function commitStreamLineNow(clean) {
  const last = streamLines[streamLines.length - 1];
  if (last && last.rawText === clean) return;

  const nowMs = Math.round(Math.max(currentTimeMs, nextStreamStartMs));
  const estDur = Math.max(2400, Math.min(7200, clean.length * 240));
  if (last) {
    last.endTime = nowMs;
  }
  const entry = {
    rawText: clean,
    startTime: nowMs,
    endTime: nowMs + 60000,
    estDur,
  };
  streamLines.push(entry);
  if (streamLines.length > 56) streamLines = streamLines.slice(streamLines.length - 56);
  currentTimeMs = nowMs;
  nextStreamStartMs = nowMs + 350;

  // Incremental append: NEVER rebuild existing lines or jump all old lyrics from bottom!
  updateRendererModeClass();
  appendSmoothLineNode(clean);
  syncSmoothRailHighlight(true);
}

window.SoundFlowStage = {
  onAudioOnsets(events, bridgeElapsed = 0) {
    if (!isPlaying || !centerRippleEnabled || reduceMotion || rippleTriggerMode === "classic" || !Array.isArray(events)) return;
    rippleOnsets.accept(events, rippleTriggerMode, performance.now(), bridgeElapsed);
    ensureRaf();
  },
  setPlaying(playing) {
    const next = !!playing;
    if (isPlaying === next) return;
    isPlaying = next;
    const playIcon = document.getElementById("stage-play-icon");
    platterDisc.classList.toggle("spinning", isPlaying);
    rippleCanvas?.classList.toggle("active", centerRippleEnabled && (isPlaying || rippleField.waves.length > 0));
    platterDisc.getAnimations?.().forEach((a) => { a.playbackRate = animSpeed; });
    if (isPlaying) {
      playIcon.innerHTML = '<rect x="6" y="4" width="4" height="16"></rect><rect x="14" y="4" width="4" height="16"></rect>';
      setStageVisible(true);
      syncVisualActivity();
      ensureRaf();
    } else {
      playIcon.innerHTML = '<polygon points="5 3 19 12 5 21 5 3"></polygon>';
      audioEnergy = 0;
      smoothedEnergy = 0;
      avgBass = 0;
      avgMid = 0;
      avgHigh = 0;
      prevBass = 0;
      prevMid = 0;
      prevHigh = 0;
      envPeakRef = 0.14;
      rippleOnsets.reset();
      // Do NOT clear active ripples on pause/stop; let existing waves finish expanding off screen!
      ensureRaf();
      bgRender.pause();
      lyricPlayer.pause();
    }
  },

  setVisibility(show) {
    setStageVisible(!!show);
  },

  setAudioEnergy(level) {
    const v = Number(level);
    audioEnergy = Number.isFinite(v) ? Math.max(0, Math.min(1, v * 1.45)) : 0;
  },

  onTick(level, posMs, bass = 0, mid = 0, high = 0) {
    const v = Number(level);
    audioEnergy = Number.isFinite(v) ? Math.max(0, Math.min(1, v * 1.45)) : 0;
    const p = Number(posMs);
    if (Number.isFinite(p) && p >= 0) {
      currentTimeMs = p;
    }
    processSpectrumOnset(audioEnergy, Number(bass) || 0, Number(mid) || 0, Number(high) || 0);
  },

  onViewportChanged() {
    scheduleStageRelayout();
  },

  setTrackHeader(songName, artistName, codecText, modeText) {
    songTitleEl.textContent = songName || "正在播放音乐";
    songArtistEl.textContent = artistName || "局域网音频投送";
    if (codecText) codecBadgeEl.textContent = codecText;
    if (modeText) modeBadgeEl.textContent = modeText;
    const key = `${songName || ""}::${artistName || ""}`;
    if (key !== trackKey) {
      rippleOnsets.reset();
      trackKey = key;
      pendingLineQueue = [];
      hasCustomArtwork = false;
      paintGenerativeCover(key);
    }
  },

  setArtworkBase64(base64Str) {
    if (!base64Str || base64Str.length < 512) return;
    const img = new Image();
    const url = "data:image/webp;base64," + base64Str;
    img.onload = () => {
      hasCustomArtwork = true;
      const ctx = coverCanvas.getContext("2d");
      ctx.clearRect(0, 0, coverCanvas.width, coverCanvas.height);
      ctx.drawImage(img, 0, 0, coverCanvas.width, coverCanvas.height);
      try { bgRender.setAlbum(url, false); } catch (_) {}
    };
    img.src = url;
  },

  setFullLyrics(linesJson, currentPosMs, modeLabel) {
    try {
      const parsed = typeof linesJson === "string" ? JSON.parse(linesJson) : linesJson;
      if (!Array.isArray(parsed) || parsed.length === 0) return;
      const signature = trackKey + JSON.stringify(parsed);
      if (signature === fullLyricSignature && fullLrcActive) {
        this.syncPosition(currentPosMs, false);
        return;
      }
      fullLyricSignature = signature;
      fullLrcActive = true;
      lastMatchedLrcIdx = -1;
      pendingLineQueue = [];
      modeBadgeEl.textContent = modeLabel || "完整同步歌词";
      const mapped = parsed.map((item, idx) => {
        const start = Math.max(0, Math.round(Number(item.startTime) || 0));
        const nextStart = parsed[idx + 1] ? Math.max(start + 250, Math.round(Number(parsed[idx + 1].startTime) || start + 3500)) : start + 5000;
        const end = Math.max(start + 250, Math.round(Number(item.endTime) || nextStart));
        return {
          rawText: (item.text || "").trim(),
          words: buildWordsForLine(item.text || "", start, end),
          translatedLyric: item.translation || "",
          romanLyric: "",
          startTime: start,
          endTime: end,
          isBG: false,
          isDuet: false,
        };
      });
      fullLrcLines = mapped;
      const pos = Math.max(0, Number(currentPosMs) || 0);
      currentTimeMs = pos;
      updateRendererModeClass();
      const eff = Math.round(effectiveClockMs());
      lyricPlayer.setLyricLines(mapped, eff);
      lyricPlayer.setCurrentTime(eff, true);
      if (isPlaying) lyricPlayer.update(16 * animSpeed);
      rebuildSmoothRailDom();
      scheduleStageRelayout();
    } catch (e) {
      console.warn("setFullLyrics error", e);
    }
  },

  syncPosition(positionMs, seek) {
    const p = Math.max(0, Number(positionMs) || 0);
    currentTimeMs = p;
    if (usingNativeAmll()) {
      try {
        lyricPlayer.setCurrentTime(Math.round(effectiveClockMs()), !!seek);
        if (isPlaying) lyricPlayer.update(16 * animSpeed);
      } catch (_) {}
    } else {
      syncSmoothRailHighlight(!!seek);
    }
  },

  pushRealtimeLine(lineText, isResetSong) {
    const clean = (lineText || "").trim();
    if (isResetSong) {
      fullLrcActive = false;
      fullLyricSignature = "";
      fullLrcLines = [];
      streamLines = [];
      pendingLineQueue = [];
      lastMatchedLrcIdx = -1;
      currentTimeMs = 0;
      nextStreamStartMs = 0;
      modeBadgeEl.textContent = "纯离线 · 局域网实时";
      updateRendererModeClass();
      rebuildSmoothRailDom();
    }
    if (!clean) return;
    if (fullLrcActive && fullLrcLines.length > 0) {
      const needle = clean.replace(/[\s\p{P}]/gu, "").toLowerCase();
      if (needle.length >= 2) {
        const startIdx = Math.max(0, lastMatchedLrcIdx - 1);
        let matchedIdx = fullLrcLines.findIndex((item, i) => {
          if (i < startIdx) return false;
          const hay = (item.rawText || "").replace(/[\s\p{P}]/gu, "").toLowerCase();
          return hay && (hay === needle || hay.includes(needle) || needle.includes(hay));
        });
        if (matchedIdx < 0) {
          matchedIdx = fullLrcLines.findIndex((item) => {
            const hay = (item.rawText || "").replace(/[\s\p{P}]/gu, "").toLowerCase();
            return hay && (hay === needle || hay.includes(needle) || needle.includes(hay));
          });
        }
        if (matchedIdx >= 0) {
          lastMatchedLrcIdx = matchedIdx;
          const matched = fullLrcLines[matchedIdx];
          currentTimeMs = Math.max(0, matched.startTime + 60);
          if (usingNativeAmll()) {
            try {
              lyricPlayer.setCurrentTime(Math.round(effectiveClockMs()), false);
              if (isPlaying) lyricPlayer.update(16 * animSpeed);
            } catch (_) {}
          } else {
            syncSmoothRailHighlight(true);
          }
        }
      }
      return;
    }

    if (syncOffsetMs < -80) {
      const delay = Math.min(5000, -syncOffsetMs);
      pendingLineQueue.push({
        deliverAt: performance.now() + delay,
        songKey: trackKey,
        text: clean,
      });
    } else {
      commitStreamLineNow(clean);
    }
  },
};

/* ============================================================================
   UI Wiring: Styles, Sync Offset, Stepless Anim Speed & Liquid Glass Bridge
   ============================================================================ */

stageCard.appendChild(document.querySelector(".stage-glass-hud"));

let activeTab = "receiver";
let systemDark = true;
let themePref = localStorage.getItem("sf_theme") || "auto";
let selectedDevType = 4;
let dlnaEnabled = true;
let lrcEnabled = false;
let seekingVol = false;
let toastTimer = null;
let lastLogText = "";

const nameInput = document.getElementById("name-input");
const nameErr = document.getElementById("name-error");
const volSlider = document.getElementById("volume-slider");
const volVal = document.getElementById("volume-val");
const sheetBackdrop = document.getElementById("sheet-backdrop");
const sheetTitle = document.getElementById("sheet-title");
const sheetBody = document.getElementById("sheet-body");
const sheetActions = document.getElementById("sheet-actions");

function setSliderFill(el, pct) {
  el.style.setProperty("--pct", Math.max(0, Math.min(100, pct)) + "%");
}

function applyLyricStyle(style) {
  lyricStyle = STYLE_LABELS[style] ? style : "amll";
  localStorage.setItem("sf_lyric_style", lyricStyle);
  stageCard.setAttribute("data-style", lyricStyle);
  styleBadgeEl.textContent = STYLE_LABELS[lyricStyle];
  document.querySelectorAll("#stage-style-seg button, #pref-style-seg button").forEach((b) => {
    b.setAttribute("aria-selected", String(b.dataset.val === lyricStyle));
  });
  updateRendererModeClass();
  if (usingNativeAmll()) {
    try {
      const clock = Math.round(effectiveClockMs());
      if (isPlaying) lyricPlayer.resume();
      lyricPlayer.resetScroll?.();
      lyricPlayer.onResize?.();
      lyricPlayer.calcLayout?.(LayoutReason.ContinuousScroll);
      lyricPlayer.setCurrentTime(clock, true);
      lyricPlayer.update?.(0);
    } catch (_) {}
  } else {
    try { lyricPlayer.pause(); } catch (_) {}
  }
  rebuildSmoothRailDom();
  syncVisualActivity();
  scheduleStageRelayout();
}

function applySyncOffset(ms) {
  syncOffsetMs = Math.max(-5000, Math.min(5000, Math.round(ms)));
  localStorage.setItem("sf_sync_offset", String(syncOffsetMs));
  const secStr = (syncOffsetMs > 0 ? "+" : "") + (syncOffsetMs / 1000).toFixed(1) + "s";
  const pct = ((syncOffsetMs + 5000) / 10000) * 100;
  ["stage-sync-slider", "pref-sync-slider"].forEach((id) => {
    const s = document.getElementById(id);
    if (s) { s.value = syncOffsetMs; setSliderFill(s, pct); }
  });
  ["stage-sync-val", "pref-sync-val"].forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.textContent = secStr;
  });
  flushPendingLineQueue(performance.now());
  if (usingNativeAmll()) {
    try {
      lyricPlayer.setCurrentTime(Math.round(effectiveClockMs()), false);
      if (isPlaying) lyricPlayer.update(16 * animSpeed);
    } catch (_) {}
  } else {
    syncSmoothRailHighlight(false);
  }
}

function applyAnimSpeed(spd) {
  animSpeed = Math.max(0.25, Math.min(2.50, Number(spd) || 1.0));
  localStorage.setItem("sf_anim_speed", animSpeed.toFixed(2));
  const lbl = animSpeed.toFixed(2) + "x";
  const pct = ((animSpeed - 0.25) / 2.25) * 100;
  ["stage-speed-slider", "pref-speed-slider"].forEach((id) => {
    const s = document.getElementById(id);
    if (s) { s.value = animSpeed.toFixed(2); setSliderFill(s, pct); }
  });
  ["stage-speed-val", "pref-speed-val"].forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.textContent = lbl;
  });
  applySpeedPhysics();
}

function syncTheme() {
  const effective = themePref === "auto" ? (systemDark ? "dark" : "light") : themePref;
  document.documentElement.setAttribute("data-theme", effective);
  document.querySelectorAll("#theme-seg button").forEach((b) => {
    b.setAttribute("aria-selected", String(b.dataset.val === themePref));
  });
  window.SoundFlowBridge?.themeApplied?.(effective === "dark");
}

function showToast(msg) {
  const el = document.getElementById("hud-toast");
  el.textContent = msg;
  el.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove("show"), 2400);
}

function closeSheet() {
  sheetBackdrop.classList.remove("open");
  window.SoundFlowBridge?.modalChanged?.(false);
}

function openSheet({ title, body, mono = false, buttons = [] }) {
  sheetTitle.textContent = title;
  sheetBody.textContent = body;
  sheetBody.className = "sheet-body" + (mono ? " mono" : "");
  sheetActions.innerHTML = "";
  buttons.forEach((cfg) => {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = cfg.primary ? "pill-cta primary" : cfg.destructive ? "row-btn destructive" : "row-btn";
    btn.textContent = cfg.label;
    btn.onclick = () => {
      if (cfg.close !== false) closeSheet();
      cfg.onClick?.();
    };
    sheetActions.appendChild(btn);
  });
  sheetBackdrop.classList.add("open");
  window.SoundFlowBridge?.modalChanged?.(true);
  requestAnimationFrame(() => window.LiquidGlassRefraction?.refresh());
}

sheetBackdrop.addEventListener("click", (e) => { if (e.target === sheetBackdrop) closeSheet(); });

function switchTab(tab) {
  activeTab = tab;
  document.querySelectorAll(".tab-pane").forEach((p) => p.classList.toggle("active", p.id === "pane-" + tab));
  requestAnimationFrame(() => window.LiquidGlassRefraction?.refresh());
}

if (!document.body.classList.contains("ios-lyrics")) {
  attachLiquidDock(document.querySelector(".bottom-dock"), switchTab);
  attachButtonHighlights();
}

function readValidatedName() {
  const v = nameInput.value.trim();
  const len = Array.from(v).length;
  if (!v || len > 32) {
    nameErr.classList.add("show");
    return null;
  }
  nameErr.classList.remove("show");
  return v;
}

document.getElementById("top-toggle-btn").addEventListener("click", () => {
  const validName = readValidatedName() || "SoundFlow-Pad";
  window.SoundFlowBridge?.toggleReceiver?.(validName, selectedDevType, dlnaEnabled, lrcEnabled);
});

document.getElementById("apply-config-btn").addEventListener("click", () => {
  const validName = readValidatedName();
  if (!validName) return;
  window.SoundFlowBridge?.applyConfig?.(validName, selectedDevType, dlnaEnabled, lrcEnabled);
  showToast("配置已应用");
});

document.querySelectorAll("#stage-style-seg button, #pref-style-seg button").forEach((btn) => {
  btn.addEventListener("click", () => applyLyricStyle(btn.dataset.val));
});

["stage-sync-slider", "pref-sync-slider"].forEach((id) => {
  const s = document.getElementById(id);
  s.addEventListener("input", () => applySyncOffset(Number(s.value)));
});
document.getElementById("nudge-minus").onclick = () => applySyncOffset(syncOffsetMs - 500);
document.getElementById("nudge-zero").onclick = () => applySyncOffset(0);
document.getElementById("nudge-plus").onclick = () => applySyncOffset(syncOffsetMs + 500);
document.getElementById("pref-nudge-minus").onclick = () => applySyncOffset(syncOffsetMs - 500);
document.getElementById("pref-nudge-zero").onclick = () => applySyncOffset(0);
document.getElementById("pref-nudge-plus").onclick = () => applySyncOffset(syncOffsetMs + 500);

["stage-speed-slider", "pref-speed-slider"].forEach((id) => {
  const s = document.getElementById(id);
  s.addEventListener("input", () => applyAnimSpeed(Number(s.value)));
});

document.querySelectorAll("#devtype-seg button").forEach((btn) => {
  btn.addEventListener("click", () => {
    selectedDevType = Number(btn.dataset.val) || 4;
    document.querySelectorAll("#devtype-seg button").forEach((x) => x.setAttribute("aria-selected", String(x === btn)));
    const validName = readValidatedName() || "SoundFlow-Pad";
    window.SoundFlowBridge?.applyConfig?.(validName, selectedDevType, dlnaEnabled, lrcEnabled);
  });
});

document.getElementById("sw-dlna").addEventListener("click", (e) => {
  dlnaEnabled = !dlnaEnabled;
  e.currentTarget.setAttribute("aria-checked", String(dlnaEnabled));
  const validName = readValidatedName() || "SoundFlow-Pad";
  window.SoundFlowBridge?.applyConfig?.(validName, selectedDevType, dlnaEnabled, lrcEnabled);
});

function syncOnlineToggleUi(enabled) {
  lrcEnabled = !!enabled;
  const sw = document.getElementById("sw-lrc");
  if (sw) sw.setAttribute("aria-checked", String(lrcEnabled));
  const stageBtn = document.getElementById("stage-online-btn");
  if (stageBtn) {
    stageBtn.setAttribute("aria-pressed", String(lrcEnabled));
    stageBtn.classList.toggle("active-pill", lrcEnabled);
    stageBtn.textContent = lrcEnabled ? "联网完整歌词：开" : "联网完整歌词：关（纯离线）";
  }
}

function toggleOnlineLyricsSetting() {
  syncOnlineToggleUi(!lrcEnabled);
  const validName = readValidatedName() || "SoundFlow-Pad";
  window.SoundFlowBridge?.applyConfig?.(validName, selectedDevType, dlnaEnabled, lrcEnabled);
  showToast(lrcEnabled ? "已开启联网匹配（自动离线缓存）" : "已切换为 100% 纯离线歌词模式");
}

function applyCenterRipple(enabled) {
  rippleOnsets.reset();
  centerRippleEnabled = !!enabled;
  window.SoundFlowBridge?.onUiState?.("ripple", centerRippleEnabled);
  localStorage.setItem("sf_center_ripple", centerRippleEnabled ? "1" : "0");
  rippleCanvas?.classList.toggle("active", centerRippleEnabled && isPlaying);
  if (!centerRippleEnabled) {
    rippleField.clear();
    canvasNeedsClear = true;
    stepAndDrawCenterRipples(0);
  } else if (rippleTriggerMode === "classic" && isPlaying && rippleField.waves.length === 0) {
    spawnCenterRipple("bass", Math.max(0.45, audioEnergy));
  }
  const sw = document.getElementById("sw-ripple");
  if (sw) sw.setAttribute("aria-checked", String(centerRippleEnabled));
  const stageBtn = document.getElementById("stage-ripple-btn");
  if (stageBtn) {
    stageBtn.setAttribute("aria-pressed", String(centerRippleEnabled));
    stageBtn.classList.toggle("active-pill", centerRippleEnabled);
    stageBtn.textContent = centerRippleEnabled ? "中心水波：开" : "中心水波：关";
  }
  const hudBtn = document.getElementById("stage-hud-ripple-btn");
  if (hudBtn) {
    hudBtn.setAttribute("aria-pressed", String(centerRippleEnabled));
    hudBtn.classList.toggle("active", centerRippleEnabled);
  }
}

function toggleCenterRippleSetting() {
  applyCenterRipple(!centerRippleEnabled);
  showToast(centerRippleEnabled ? "已开启中心扩散水波涟漪" : "已关闭中心水波特效");
}

function bindReliableTap(el, handler) {
  el?.addEventListener("click", handler);
}

document.getElementById("dlna-visualizer-permission").addEventListener("click", () => window.SoundFlowBridge?.requestDlnaVisualization?.());
document.getElementById("sw-ripple").addEventListener("click", toggleCenterRippleSetting);
const rippleTriggerChoices = document.querySelectorAll("[data-ripple-trigger]");
function syncRippleTriggerChoices() {
  rippleTriggerChoices.forEach(button =>
    button.setAttribute("aria-pressed", String(button.dataset.rippleTrigger === rippleTriggerMode)));
}
rippleTriggerChoices.forEach(button => button.addEventListener("click", () => {
  rippleTriggerMode = button.dataset.rippleTrigger;
  localStorage.setItem("sf_ripple_trigger", rippleTriggerMode);
  rippleOnsets.reset();
  syncRippleTriggerChoices();
}));
syncRippleTriggerChoices();
bindReliableTap(document.getElementById("stage-ripple-btn"), toggleCenterRippleSetting);
bindReliableTap(document.getElementById("stage-hud-ripple-btn"), toggleCenterRippleSetting);
document.getElementById("sw-lrc").addEventListener("click", toggleOnlineLyricsSetting);
bindReliableTap(document.getElementById("stage-online-btn"), toggleOnlineLyricsSetting);
bindReliableTap(document.getElementById("stage-import-btn"), () => window.SoundFlowBridge?.importLocalLrc?.());
document.getElementById("row-import-lrc").addEventListener("click", () => window.SoundFlowBridge?.importLocalLrc?.());
document.getElementById("btn-clear-cache").addEventListener("click", () => window.SoundFlowBridge?.clearLyricCache?.());

document.querySelectorAll("#theme-seg button").forEach((btn) => {
  btn.addEventListener("click", () => {
    themePref = btn.dataset.val;
    localStorage.setItem("sf_theme", themePref);
    syncTheme();
    requestAnimationFrame(() => window.LiquidGlassRefraction?.refresh());
  });
});

bindReliableTap(document.getElementById("stage-tuner-btn"), () => setTunerOpen(!tunerOpen));
document.getElementById("btn-prev").onclick = () => window.SoundFlowBridge?.controlAction?.("prev");
document.getElementById("btn-pause").onclick = () => window.SoundFlowBridge?.controlAction?.("pause");
document.getElementById("btn-resume").onclick = () => window.SoundFlowBridge?.controlAction?.("resume");
document.getElementById("btn-next").onclick = () => window.SoundFlowBridge?.controlAction?.("next");
bindReliableTap(document.getElementById("stage-prev-btn"), () => window.SoundFlowBridge?.controlAction?.("prev"));
bindReliableTap(document.getElementById("stage-next-btn"), () => window.SoundFlowBridge?.controlAction?.("next"));
bindReliableTap(document.getElementById("stage-playpause-btn"), () => {
  window.SoundFlowBridge?.controlAction?.(isPlaying ? "pause" : "resume");
});
bindReliableTap(document.getElementById("preview-fullscreen-button"), () => toggleFullscreen(true, true));
bindReliableTap(document.getElementById("stage-expand-btn"), () => toggleFullscreen(true, true));
bindReliableTap(document.getElementById("stage-fs-btn"), () => toggleFullscreen(undefined, true));
document.getElementById("conflict-btn").onclick = () => window.SoundFlowBridge?.releasePortConflict?.();

volSlider.addEventListener("pointerdown", () => { seekingVol = true; });
volSlider.addEventListener("input", () => {
  volVal.textContent = volSlider.value + "%";
  setSliderFill(volSlider, Number(volSlider.value));
});
volSlider.addEventListener("change", () => {
  seekingVol = false;
  window.SoundFlowBridge?.setVolume?.(Number(volSlider.value));
});

const audioDelay = document.getElementById("audio-delay-slider");
function displayAudioDelay() {
  const value = Number(audioDelay.value);
  const label = value === 0 ? "0 ms" : `${value > 0 ? "延后" : "提前"} ${Math.abs(value)} ms`;
  document.getElementById("audio-delay-value").textContent = label;
  audioDelay.setAttribute("aria-valuetext", label);
  const min = Number(audioDelay.min);
  setSliderFill(audioDelay, 100 * (value - min) / (Number(audioDelay.max) - min));
}
function commitAudioDelay() {
  displayAudioDelay();
  window.SoundFlowBridge?.setAudioSyncOffset?.(Number(audioDelay.value));
}
audioDelay.addEventListener("input", displayAudioDelay);
audioDelay.addEventListener("change", commitAudioDelay);
document.getElementById("audio-delay-reset").onclick = () => {
  audioDelay.value = "0";
  commitAudioDelay();
};


document.getElementById("row-battery").onclick = () => window.SoundFlowBridge?.openBatterySettings?.();
document.getElementById("row-autostart").onclick = () => window.SoundFlowBridge?.openAutoStartSettings?.();
document.getElementById("row-hot-guard").onclick = () => window.SoundFlowBridge?.runRootTask?.("hot");
document.getElementById("row-install-mod").onclick = () => {
  openSheet({
    title: "固化为 Magisk 系统特权服务",
    body: "将通过 Root 在 /data/adb/modules/soundflow_receiver 部署独立特权系统模块、HyperOS 白名单与开机守护脚本，完全不触碰小米原有系统组件。是否继续？",
    buttons: [
      { label: "取消" },
      { label: "立即固化", primary: true, onClick: () => window.SoundFlowBridge?.runRootTask?.("install") },
    ],
  });
};
document.getElementById("row-remove-mod").onclick = () => window.SoundFlowBridge?.runRootTask?.("uninstall");
document.getElementById("row-logs").onclick = () => window.SoundFlowBridge?.fetchLogs?.();
document.getElementById("row-license").onclick = () => {
  openSheet({
    title: "开源许可与技术说明",
    body: "音流独立音频接收器 (SoundFlow Receiver)\n许可协议：AGPL-3.0-only\n\n1. 协议引擎：纯 Rust 独立实现 Xiaomi MiPlay v3 (基于开源 FusionPlay.MiPlaySdk)，不依赖任何小米系统服务 APK 或账号。\n2. 跨品牌通用：内置标准开放 UPnP / DLNA MediaRenderer (1900 SSDP + 8901 HTTP/SOAP)。\n3. 多样式歌词舞台：集成 AMLL (@applemusic-like-lyrics/core) 与 QQ 音乐黑胶水波 / 极光水波 / 动感逐字样式，采用双栏唱片视觉布局。\n4. 界面材料语言：iOS 26/27 通透液态玻璃与 Chromium SVG 边缘光学折射。",
    buttons: [{ label: "关闭", primary: true }],
  });
};

window.SoundFlowApp = {
  setReducedMotion(reduce) {
    document.documentElement.toggleAttribute("data-reduce-motion", reduce);
  },

  setSafeInsets(top, right, bottom, left, isSysDark) {
    const r = document.documentElement.style;
    r.setProperty("--safe-top", Math.max(12, top) + "px");
    r.setProperty("--safe-right", Math.max(0, right) + "px");
    r.setProperty("--safe-bottom", Math.max(12, bottom) + "px");
    r.setProperty("--safe-left", Math.max(0, left) + "px");
    systemDark = !!isSysDark;
    syncTheme();
    requestAnimationFrame(() => window.LiquidGlassRefraction?.refresh());
  },

  applySnapshot(jsonStr) {
    let s;
    try { s = typeof jsonStr === "string" ? JSON.parse(jsonStr) : jsonStr; } catch (_) { return; }
    if (!s) return;

    if (!audioDelay.matches(":active")) {
      audioDelay.value = String(s.audio_sync_offset_ms ?? 0);
      displayAudioDelay();
    }

    const enabled = !!s.enabled;
    const miplay = !!s.miplay_running;
    const dlna = !!s.dlna_running;
    const playing = !!s.playing;
    const session = !!s.session_active;
    const err = s.last_error || "";

    const topDot = document.getElementById("top-dot");
    topDot.className = "status-dot " + (playing ? "playing" : (miplay || dlna) ? "ready" : err ? "warn" : "");
    const ep = (s.endpoint || "").replace("/", " · ");
    document.getElementById("top-subtitle").textContent = enabled
      ? (playing && s.source_name ? `${s.source_name} 投送中` : (ep || (s.status || "接收中")))
      : "接收已停止";

    const topBtn = document.getElementById("top-toggle-btn");
    topBtn.textContent = enabled ? "停止接收" : "开启接收";
    topBtn.className = "pill-cta " + (enabled ? "stop" : "primary");

    document.getElementById("hero-status").textContent = enabled ? (s.status || "监听中") : "接收已停止";
    const subParts = [];
    subParts.push(ep ? `本机 ${ep}` : "未绑定局域网地址");
    if (s.source_name) subParts.push(`来源 ${s.source_name}`);
    if (s.format) subParts.push(s.format);
    document.getElementById("hero-sub").textContent = subParts.join(" · ");

    const cMi = document.getElementById("chip-miplay");
    cMi.className = "chip " + (miplay ? "ok" : err.includes("8899") ? "err" : "");
    cMi.querySelector("span:last-child").textContent = `小米妙播 · ${miplay ? "已就绪" : (enabled && err.includes("8899") ? "端口冲突" : "未开启")}`;

    const cDl = document.getElementById("chip-dlna");
    cDl.className = "chip " + (dlna ? "ok" : "");
    cDl.querySelector("span:last-child").textContent = `通用 DLNA · ${dlna ? "已就绪" : (s.enable_dlna === false ? "已关闭" : "未开启")}`;

    document.querySelector(".receiver-content").classList.toggle("is-idle", !session);
    const receiverDot = document.getElementById("receiver-dot");
    receiverDot.style.background = enabled && (miplay || dlna) ? "var(--sys-green-dot)" : "var(--label-4)";
    document.getElementById("receiver-label").textContent = enabled ? (playing ? "正在播放" : "等待连接") : "接收已关闭";
    if (!err) document.getElementById("hero-status").textContent = playing ? "音乐正在这里播放" : enabled ? "已准备好接收音乐" : "随时，听见好音乐";
    document.querySelectorAll(".transport-btn").forEach(button => { button.disabled = !session; });
    const errBanner = document.getElementById("error-banner");
    errBanner.classList.toggle("show", !!err);
    document.getElementById("error-text").textContent = err;
    document.getElementById("conflict-btn").style.display = err.includes("8899") ? "inline-flex" : "none";

    if (document.activeElement !== nameInput && s.device_name) nameInput.value = s.device_name;
    if (typeof s.device_type === "number") {
      selectedDevType = s.device_type;
      document.querySelectorAll("#devtype-seg button").forEach((b) => {
        b.setAttribute("aria-selected", String(Number(b.dataset.val) === selectedDevType));
      });
    }
    if (typeof s.enable_dlna === "boolean") {
      dlnaEnabled = s.enable_dlna;
      document.getElementById("sw-dlna").setAttribute("aria-checked", String(dlnaEnabled));
    }
    if (typeof s.auto_online_lyrics === "boolean") {
      syncOnlineToggleUi(s.auto_online_lyrics);
    }
    if (!seekingVol && typeof s.volume === "number") {
      volSlider.value = s.volume;
      volVal.textContent = s.volume + "%";
      setSliderFill(volSlider, s.volume);
    }

    if (!playing && !session) {
      setStageVisible(false);
    }
  },

  updateGuardStatus(batteryIgnored, rootSummary, cachedLrcCount) {
    const bDesc = document.getElementById("battery-desc");
    const bIcon = document.getElementById("battery-icon-box");
    bDesc.textContent = batteryIgnored ? "已豁免电池优化（推荐）" : "未豁免 · 建议点击允许无限制后台";
    bIcon.className = "row-icon " + (batteryIgnored ? "green" : "orange");
    document.getElementById("root-desc").textContent = rootSummary || "状态未知";
    const cDesc = document.getElementById("offline-cache-desc");
    if (cDesc && typeof cachedLrcCount === "number") {
      cDesc.textContent = `已离线保存 ${cachedLrcCount} 首完整歌词 · 实时片段不自动同步`;
    }
  },

  showLogSheet(text) {
    lastLogText = text || "暂无运行日志";
    openSheet({
      title: "最近运行诊断日志",
      body: lastLogText,
      mono: true,
      buttons: [
        { label: "导出文件", onClick: () => window.SoundFlowBridge?.exportLogs?.(lastLogText) },
        { label: "关闭", primary: true },
      ],
    });
  },

  showToast,

  handleBack() {
    if (sheetBackdrop.classList.contains("open")) {
      closeSheet();
      return true;
    }
    if (tunerOpen) {
      setTunerOpen(false);
      return true;
    }
    if (isFullscreen) {
      toggleFullscreen(false, true);
      return true;
    }
    return false;
  },
};

function applyGlassTint(value) {
  const tint = Math.max(0, Math.min(100, Number(value) || 0));
  localStorage.setItem("sf_glass_tint", String(tint));
  document.documentElement.style.setProperty("--glass-tint", (tint / 100).toFixed(2));
  document.getElementById("glass-tint-slider").value = tint;
  document.getElementById("glass-tint-val").textContent = Math.round(tint) + "%";
  setSliderFill(document.getElementById("glass-tint-slider"), tint);
}
document.getElementById("glass-tint-slider").addEventListener("input", (e) => applyGlassTint(e.target.value));
applyGlassTint(localStorage.getItem("sf_glass_tint") ?? 35);

function applyRippleSensitivity(value) {
  const sens = Math.max(0, Math.min(100, Math.round(Number(value) || 0)));
  localStorage.setItem("sf_ripple_sens", String(sens));
  for (const id of ["stage-ripple-sens-slider", "pref-ripple-sens-slider"]) {
    const el = document.getElementById(id);
    if (el) {
      el.value = String(sens);
      setSliderFill(el, sens);
    }
  }
  for (const id of ["stage-ripple-sens-val", "pref-ripple-sens-val"]) {
    const el = document.getElementById(id);
    if (el) el.textContent = sens + "%";
  }
  window.SoundFlowBridge?.setRippleSensitivity?.(sens);
}
for (const id of ["stage-ripple-sens-slider", "pref-ripple-sens-slider"]) {
  document.getElementById(id)?.addEventListener("input", (e) => applyRippleSensitivity(e.target.value));
}
applyRippleSensitivity(localStorage.getItem("sf_ripple_sens") ?? 55);

// Split by purpose, not by arbitrary row counts. All feature IDs and native
// bridge actions remain intact; tablets gain two readable columns.
const settingsPane = document.getElementById("pane-settings");
const settingsGroups = settingsPane.querySelectorAll(":scope > .list-group");
const settingsHeadings = settingsPane.querySelectorAll(":scope > .section-header");
const columns = document.createElement("div");
columns.className = "settings-columns";
const leftColumn = document.createElement("div");
const rightColumn = document.createElement("div");
const lyricsGroup = document.createElement("div");
lyricsGroup.className = "list-group";
for (const id of ["sw-lrc", "row-import-lrc", "btn-clear-cache"]) {
  lyricsGroup.appendChild(document.getElementById(id).closest(".list-row"));
}
function sectionHeading(text) {
  const el = document.createElement("div");
  el.className = "section-header";
  el.textContent = text;
  return el;
}
leftColumn.append(settingsHeadings[0], settingsGroups[0], sectionHeading("离线歌词"), lyricsGroup);
rightColumn.append(settingsHeadings[1], settingsGroups[1], settingsHeadings[2], settingsGroups[2]);
columns.append(leftColumn, rightColumn);
settingsPane.appendChild(columns);

paintGenerativeCover("SoundFlow");
applyLyricStyle(lyricStyle);
applyCenterRipple(centerRippleEnabled);
applySyncOffset(syncOffsetMs);
applyAnimSpeed(animSpeed);
syncOnlineToggleUi(lrcEnabled);
setSliderFill(volSlider, Number(volSlider.value));
syncTheme();
window.SoundFlowBridge?.onJsReady?.();
