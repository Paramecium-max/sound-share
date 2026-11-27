const { createHash } = require('node:crypto');
const fsSync = require('node:fs');
const fs = require('node:fs/promises');
const path = require('node:path');

const isTimed = (text) => typeof text === 'string' && text.length <= 1024 * 1024 &&
  /^\[\d+:\d\d(?:[.:]\d{1,3})?\]/m.test(text.trim());
const cachePattern = /^[0-9a-f]{64}\.lrc$/;
const KUWO_XOR_KEY = Buffer.from('yeelion', 'ascii');

function parseLrcTagMs(tag) {
  const colon = tag.indexOf(':');
  if (colon <= 0 || colon + 1 >= tag.length) return -1;
  const mins = Number(tag.slice(0, colon));
  if (!Number.isInteger(mins) || mins < 0 || mins > 9999) return -1;
  const rest = tag.slice(colon + 1);
  let dot = rest.indexOf('.');
  if (dot < 0) dot = rest.indexOf(':');
  let secs = 0;
  let fracMs = 0;
  if (dot < 0) {
    secs = Number(rest);
  } else {
    secs = Number(rest.slice(0, dot));
    const f = rest.slice(dot + 1);
    if (!f || !/^\d+$/.test(f)) return -1;
    if (f.length === 1) fracMs = Number(f) * 100;
    else if (f.length === 2) fracMs = Number(f) * 10;
    else fracMs = Number(f.slice(0, 3));
  }
  if (!Number.isInteger(secs) || secs < 0 || secs >= 60) return -1;
  return (mins * 60 + secs) * 1000 + fracMs;
}

function parseLrcLines(rawText) {
  const result = [];
  if (typeof rawText !== 'string' || !rawText) return result;
  const cleaned = rawText.startsWith('\uFEFF') ? rawText.slice(1) : rawText;
  let offsetMs = 0;
  const rawEntries = [];
  for (const line of cleaned.split(/\r?\n/)) {
    if (!line) continue;
    const times = [];
    let pos = 0;
    while (pos < line.length && line[pos] === '[') {
      const close = line.indexOf(']', pos + 1);
      if (close < 0) break;
      const tag = line.slice(pos + 1, close);
      const ts = parseLrcTagMs(tag);
      if (ts >= 0) {
        times.push(ts);
      } else if (tag.toLowerCase().startsWith('offset:')) {
        const parsedOff = Number(tag.slice(7).trim());
        if (Number.isFinite(parsedOff)) offsetMs = parsedOff;
      }
      pos = close + 1;
    }
    if (!times.length) continue;
    const lyric = line.slice(pos).replace(/<-?\d+,-?\d+>/g, '').trim();
    for (const t of times) rawEntries.push({ timeMs: t, text: lyric });
  }
  for (const e of rawEntries) {
    result.push({ timeMs: Math.max(0, e.timeMs - offsetMs), text: e.text });
  }
  result.sort((a, b) => a.timeMs - b.timeMs);
  return result;
}

function lrcLineAt(lines, positionMs) {
  if (!Array.isArray(lines) || !lines.length) return '';
  const pos = Math.max(0, Number(positionMs) || 0);
  let lo = 0;
  let hi = lines.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (lines[mid].timeMs <= pos) lo = mid + 1;
    else hi = mid;
  }
  for (let idx = lo - 1; idx >= 0; idx--) {
    if (lines[idx].text) return lines[idx].text;
  }
  return '';
}

function formatLrcTimestamp(secondsFloat) {
  const totalMs = Math.max(0, Math.round((Number(secondsFloat) || 0) * 1000));
  const mins = Math.floor(totalMs / 60000);
  const secs = Math.floor((totalMs % 60000) / 1000);
  const centis = Math.floor((totalMs % 1000) / 10);
  return `[${String(mins).padStart(2, '0')}:${String(secs).padStart(2, '0')}.${String(centis).padStart(2, '0')}]`;
}

function decodeKuwoSearchLyricDat(rawContent) {
  try {
    const b64 = String(rawContent || '').trim();
    if (!b64 || b64.length > 512 * 1024) return null;
    const raw = Buffer.from(b64, 'base64');
    if (!raw.length) return null;
    const out = Buffer.allocUnsafe(raw.length);
    for (let i = 0; i < raw.length; i++) {
      out[i] = raw[i] ^ KUWO_XOR_KEY[i % KUWO_XOR_KEY.length];
    }
    const text = new TextDecoder('gb18030').decode(out);
    const cleaned = text.replace(/<-?\d+,-?\d+>/g, '');
    if (!isTimed(cleaned)) return null;
    const tiMatch = cleaned.match(/^\[ti:([^\]]*)\]/im);
    const arMatch = cleaned.match(/^\[ar:([^\]]*)\]/im);
    const alMatch = cleaned.match(/^\[al:([^\]]*)\]/im);
    return {
      title: tiMatch ? tiMatch[1].trim() : '',
      artist: arMatch ? arMatch[1].trim() : '',
      album: alMatch ? alMatch[1].trim() : '',
      lyrics: cleaned,
    };
  } catch {
    return null;
  }
}

function findKuwoDataRoot() {
  const candidates = [
    process.env.ALLUSERSPROFILE ? path.join(process.env.ALLUSERSPROFILE, 'Kuwodata', 'kwmusic2013') : '',
    'B:\\ProgramData\\Kuwodata\\kwmusic2013',
    'C:\\ProgramData\\Kuwodata\\kwmusic2013',
    'D:\\ProgramData\\Kuwodata\\kwmusic2013',
  ].filter(Boolean);
  return candidates.find((dir) => fsSync.existsSync(dir)) || '';
}

function normalizeName(s) {
  return String(s || '').trim().toLowerCase().replace(/\s+/g, '');
}

async function lookupKuwoLocalCache(track) {
  if (process.platform !== 'win32' || !track?.title) return null;
  const root = findKuwoDataRoot();
  if (!root) return null;
  const lyricDir = path.join(root, 'Res', 'cache', 'KW_SEARCH_LYRIC');
  try {
    const names = await fs.readdir(lyricDir);
    const datFiles = [];
    for (const name of names) {
      if (!name.toLowerCase().endsWith('.dat')) continue;
      const full = path.join(lyricDir, name);
      try {
        const st = await fs.stat(full);
        if (st.isFile() && st.size > 32 && st.size <= 256 * 1024) {
          datFiles.push({ full, mtimeMs: st.mtimeMs });
        }
      } catch {}
    }
    datFiles.sort((a, b) => b.mtimeMs - a.mtimeMs);
    const wantTitle = normalizeName(track.title);
    const wantArtist = normalizeName(track.artist);
    for (const item of datFiles.slice(0, 50)) {
      try {
        const content = await fs.readFile(item.full, 'ascii');
        const decoded = decodeKuwoSearchLyricDat(content);
        if (!decoded) continue;
        const gotTitle = normalizeName(decoded.title);
        const gotArtist = normalizeName(decoded.artist);
        if (gotTitle && gotTitle === wantTitle && (!wantArtist || !gotArtist || gotArtist.includes(wantArtist) || wantArtist.includes(gotArtist))) {
          return decoded;
        }
      } catch {}
    }
  } catch {}
  return null;
}

async function fetchJsonWithLimit(url, options, maxBytes = 1024 * 1024) {
  const response = await fetch(url, options);
  if (!response.ok) return null;
  const reader = response.body.getReader();
  let length = 0;
  const chunks = [];
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      length += value.byteLength;
      if (length > maxBytes) return null;
      chunks.push(value);
    }
  } finally {
    await reader.cancel();
  }
  return JSON.parse(Buffer.concat(chunks).toString('utf8'));
}

async function lookupKuwoH5ByMusicId(musicId, track, signal) {
  const cleanId = String(musicId || '').replace(/^MUSIC_/i, '').trim();
  if (!/^\d+$/.test(cleanId)) return null;
  for (const scheme of ['https', 'http']) {
    try {
      const url = `${scheme}://m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=${cleanId}&httpsStatus=1`;
      const parsed = await fetchJsonWithLimit(url, {
        signal,
        headers: { 'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) SoundFlow/1.2.2' },
      });
      const data = parsed?.data;
      if (!data) continue;
      const info = data.songinfo || {};
      const lrclist = Array.isArray(data.lrclist) ? data.lrclist : [];
      const infoTitle = String(info.songName || '').trim();
      const infoArtist = String(info.artist || '').trim();
      const infoAlbum = String(info.album || '').trim();
      const durationMs = Number(info.duration) > 0 ? Number(info.duration) * 1000 : 0;

      // Ensure the cached MUSICRID matches the track if track.title is already known
      if (track?.title && infoTitle) {
        const want = normalizeName(track.title);
        const got = normalizeName(infoTitle);
        if (want && got && !got.includes(want) && !want.includes(got)) {
          return null;
        }
      }

      let lyrics = '';
      if (lrclist.length > 0) {
        const lines = [];
        if (infoTitle || track?.title) lines.push(`[ti:${infoTitle || track.title}]`);
        if (infoArtist || track?.artist) lines.push(`[ar:${infoArtist || track.artist}]`);
        if (infoAlbum) lines.push(`[al:${infoAlbum}]`);
        for (const item of lrclist) {
          const text = String(item?.lineLyric || '').trim();
          const t = Number(item?.time);
          if (Number.isFinite(t) && t >= 0) {
            lines.push(`${formatLrcTimestamp(t)}${text}`);
          }
        }
        const candidate = lines.join('\n');
        if (isTimed(candidate)) lyrics = candidate;
      }
      if (lyrics || durationMs > 0 || infoAlbum) {
        return {
          title: infoTitle || track?.title || '',
          artist: infoArtist || track?.artist || '',
          album: infoAlbum || track?.album || '',
          durationMs,
          lyrics,
        };
      }
    } catch {}
  }
  return null;
}

async function lookupNeteaseOnline(track, signal) {
  if (!track?.title?.trim()) return null;
  try {
    const q = [track.title.trim(), (track.artist || '').trim()].filter(Boolean).join(' ');
    const searchUrl = `https://music.163.com/api/cloudsearch/pc?s=${encodeURIComponent(q)}&type=1&limit=5`;
    const searchJson = await fetchJsonWithLimit(searchUrl, {
      method: 'POST',
      signal,
      headers: {
        Referer: 'https://music.163.com',
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)',
      },
    });
    const songs = searchJson?.result?.songs;
    if (!Array.isArray(songs) || !songs.length) return null;
    const wantTitle = normalizeName(track.title);
    const wantArtist = normalizeName(track.artist);
    const matched = songs.find((s) => {
      const sTitle = normalizeName(s?.name);
      const sArtist = Array.isArray(s?.ar) ? s.ar.map((a) => normalizeName(a?.name)).join(',') : '';
      return sTitle === wantTitle && (!wantArtist || sArtist.includes(wantArtist));
    }) || songs.find((s) => normalizeName(s?.name) === wantTitle) || null;
    if (!matched?.id) return null;

    const lyricUrl = `https://music.163.com/api/song/lyric?os=pc&id=${matched.id}&lv=-1&tv=-1`;
    const lyricJson = await fetchJsonWithLimit(lyricUrl, {
      signal,
      headers: {
        Referer: 'https://music.163.com',
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)',
      },
    });
    const rawLrc = lyricJson?.lrc?.lyric;
    if (!isTimed(rawLrc)) return null;
    return {
      title: matched.name || track.title,
      artist: (Array.isArray(matched.ar) && matched.ar[0]?.name) || track.artist || '',
      album: matched.al?.name || track.album || '',
      durationMs: Number(matched.dt) > 0 ? Number(matched.dt) : 0,
      lyrics: rawLrc,
    };
  } catch {
    return null;
  }
}

class DesktopLyrics {
  constructor(directory) {
    this.directory = directory;
    this.mutation = Promise.resolve();
  }

  file(track) {
    if (!track.title?.trim()) return null;
    const normalized = [track.title, track.artist || ''].map((part) => part.trim().toLowerCase());
    const key = createHash('sha256').update(JSON.stringify(normalized)).digest('hex');
    return path.join(this.directory, key + '.lrc');
  }

  async read(track) {
    const file = this.file(track);
    if (!file) return '';
    try {
      if ((await fs.stat(file)).size > 1024 * 1024) return '';
      const text = await fs.readFile(file, 'utf8');
      return isTimed(text) ? text : '';
    } catch (error) {
      if (error.code === 'ENOENT') return '';
      throw error;
    }
  }

  mutate(action) {
    const result = this.mutation.then(action);
    this.mutation = result.catch(() => {});
    return result;
  }

  async write(track, text) {
    if (!isTimed(text)) throw new Error('没有找到有效的 LRC 时间戳');
    const file = this.file(track);
    if (!file) return;
    return this.mutate(async () => {
      await fs.mkdir(this.directory, { recursive: true });
      const temporary = file + '.tmp';
      await fs.writeFile(temporary, text, 'utf8');
      await fs.rename(temporary, file);
    });
  }

  async files() {
    try { return (await fs.readdir(this.directory)).filter((name) => cachePattern.test(name)); }
    catch (error) { if (error.code === 'ENOENT') return []; throw error; }
  }

  async clear() {
    return this.mutate(async () => {
      for (const file of await this.files()) await fs.unlink(path.join(this.directory, file));
    });
  }

  async lookupRich(track, signal) {
    if (!track?.title?.trim() && !track?.kuwoMusicId) return null;

    // 1. Local Kuwo KW_SEARCH_LYRIC encrypted .dat cache (offline instant)
    const localKuwo = await lookupKuwoLocalCache(track);
    if (localKuwo?.lyrics) {
      if (track.kuwoMusicId) {
        const h5Meta = await lookupKuwoH5ByMusicId(track.kuwoMusicId, track, signal);
        if (h5Meta) {
          return {
            ...h5Meta,
            lyrics: localKuwo.lyrics,
            album: localKuwo.album || h5Meta.album || '',
          };
        }
      }
      return localKuwo;
    }

    // 2. Official Kuwo H5 songinfoandlrc API by MUSICRID (exact match for Kuwo PC v9.1.0.0)
    if (track.kuwoMusicId) {
      const h5 = await lookupKuwoH5ByMusicId(track.kuwoMusicId, track, signal);
      if (h5?.lyrics) return h5;
    }

    // 3. NetEase Cloud Music search + lyric fallback
    const netease = await lookupNeteaseOnline(track, signal);
    if (netease?.lyrics) return netease;

    // 4. International lrclib.net fallback
    const lrclibText = await this.lookupLrclib(track, signal);
    if (lrclibText) {
      return {
        title: track.title || '',
        artist: track.artist || '',
        album: track.album || '',
        durationMs: track.durationMs || 0,
        lyrics: lrclibText,
      };
    }
    return null;
  }

  async lookupLrclib(track, signal) {
    if (!track.title?.trim() || !track.artist?.trim()) return '';
    try {
      const url = new URL('https://lrclib.net/api/get');
      url.searchParams.set('track_name', track.title.slice(0, 256));
      url.searchParams.set('artist_name', track.artist.slice(0, 256));
      if (track.album) url.searchParams.set('album_name', track.album.slice(0, 256));
      if (track.durationMs > 10000) url.searchParams.set('duration', String(Math.round(track.durationMs / 1000)));
      const parsed = await fetchJsonWithLimit(url, {
        signal, redirect: 'error', headers: { 'User-Agent': 'SoundFlow/1.2.2' },
      });
      const value = parsed?.syncedLyrics;
      return isTimed(value) ? value : '';
    } catch {
      return '';
    }
  }

  async lookup(track, signal) {
    const rich = await this.lookupRich(track, signal);
    return rich?.lyrics && isTimed(rich.lyrics) ? rich.lyrics : '';
  }
}

module.exports = {
  DesktopLyrics,
  isTimed,
  parseLrcLines,
  lrcLineAt,
  decodeKuwoSearchLyricDat,
};
