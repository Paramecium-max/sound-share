function parseLrc(content) {
  const lines = [];
  const offset = Number(content.match(/^\[offset:([+-]?\d+)\]/mi)?.[1] || 0);
  for (const row of content.split(/\r?\n/)) {
    const stamps = [...row.matchAll(/\[(\d+):(\d{2})(?:[.:](\d{1,3}))?\]/g)];
    const last = stamps.at(-1);
    if (!last) continue;
    const text = row.slice(last.index + last[0].length).trim();
    if (!text) continue;
    for (const [, minutes, seconds, fraction = ""] of stamps) {
      const startTime = Math.max(0, Number(minutes) * 60000 + Number(seconds) * 1000 +
        Number(fraction.padEnd(3, "0")) + offset);
      lines.push({ startTime, text });
    }
  }
  return lines.sort((a, b) => a.startTime - b.startTime);
}

export function attachDesktopStage(bridge, stage, app) {
  if (bridge?.platform !== "win32") return;
  document.documentElement.classList.add("soundflow-windows");
  document.documentElement.dataset.soundflowPlatform = "windows";
  const theme = matchMedia("(prefers-color-scheme: dark)");
  const syncTheme = () => app.setSafeInsets(12, 0, 12, 0, theme.matches);
  theme.addEventListener("change", syncTheme);
  syncTheme();
  const rows = {
    "row-autostart": ["登录时启动音流", "随 Windows 登录自动启动"],
    "row-battery": ["系统托盘常驻", "最小化或关闭窗口时继续接收和发送"],
    "row-notif-access": ["Windows 防火墙", "检查音流监听端口的入站规则"],
  };
  for (const [id, [title, description]] of Object.entries(rows)) {
    const row = document.getElementById(id);
    row.querySelector(".row-title").textContent = title;
    row.querySelector(".row-desc").textContent = description;
  }
  document.getElementById("row-battery").onclick = () => bridge.setTrayEnabled();
  document.getElementById("row-notif-access").onclick = () => bridge.checkFirewall();
  document.getElementById("row-notif-access").querySelector("button").textContent = "检查";
  document.querySelector("#pane-receiver .page-heading p").textContent = "让电脑成为你的无线音箱。";
  document.querySelectorAll("[data-android-only]").forEach((element) => { element.hidden = true; });

  let revision, trackKey, lyricsText, lyricLine, artwork;
  bridge.onHostEvent(({ type, data }) => {
    switch (type) {
      case "snapshot": app.applySnapshot(data); break;
      case "senderReply": window.SoundFlowSender?.onRpcReply(data); break;
      case "senderEvent": window.SoundFlowSender?.onEvent(data); break;
      case "playing": stage.setPlaying(data); break;
      case "position": stage.syncPosition(data, false); break;
      case "track": {
        const key = `${data.title || ""}::${data.artist || ""}`;
        if (revision !== data.revision || trackKey !== key || lyricsText !== data.lyrics) {
          revision = data.revision;
          trackKey = key;
          lyricsText = data.lyrics;
          lyricLine = "";
          artwork = "";
          stage.pushRealtimeLine("", true);
        }
        stage.setTrackHeader(data.title, data.artist, data.mode, data.mode);
        if (data.lyrics) stage.setFullLyrics(parseLrc(data.lyrics), data.positionMs || 0, "完整同步歌词");
        if (data.lyricLine && lyricLine !== data.lyricLine) {
          stage.pushRealtimeLine(data.lyricLine, false);
          lyricLine = data.lyricLine;
        }
        if (artwork !== data.artwork && data.artwork) {
          artwork = data.artwork;
          stage.setArtworkBase64(artwork);
        }
        stage.syncPosition(data.positionMs || 0, false);
        break;
      }
      case "audio":
        if (!data.preview) stage.onAudioOnsets(data.onsets);
        stage.onTick(data.level, -1, ...data.bands, data.preview === true);
        break;
      case "fullscreen": stage.expandFullscreen(); break;
      case "logSheet": app.showLogSheet(data); break;
      case "toast": app.showToast(data); break;
      case "lyricCount":
        document.getElementById("offline-cache-desc").textContent = `已离线保存 ${data} 首完整歌词`;
        break;
      case "system":
        for (const [id, enabled] of [["row-autostart", data.autostart], ["row-battery", data.tray]]) {
          const button = document.querySelector(`#${id} button`);
          button.textContent = enabled ? "已开启" : "已关闭";
          button.setAttribute("aria-pressed", String(enabled));
        }
        break;
      default: break;
    }
  });
}
