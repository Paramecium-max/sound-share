const { createHash } = require('node:crypto');
const fs = require('node:fs/promises');
const path = require('node:path');

const isTimed = (text) => typeof text === 'string' && text.length <= 1024 * 1024 &&
  /^\[\d+:\d\d(?:[.:]\d{1,3})?\]/m.test(text.trim());
const cachePattern = /^[0-9a-f]{64}\.lrc$/;

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

  async lookup(track, signal) {
    if (!track.title?.trim() || !track.artist?.trim()) return '';
    const url = new URL('https://lrclib.net/api/get');
    url.searchParams.set('track_name', track.title.slice(0, 256));
    url.searchParams.set('artist_name', track.artist.slice(0, 256));
    if (track.album) url.searchParams.set('album_name', track.album.slice(0, 256));
    if (track.durationMs > 10000) url.searchParams.set('duration', String(Math.round(track.durationMs / 1000)));
    const response = await fetch(url, {
      signal, redirect: 'error', headers: { 'User-Agent': 'SoundFlow/1.2.1' },
    });
    if (!response.ok) return '';
    const reader = response.body.getReader();
    let length = 0;
    const chunks = [];
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        length += value.byteLength;
        if (length > 1024 * 1024) return '';
        chunks.push(value);
      }
    } finally {
      await reader.cancel();
    }
    const value = JSON.parse(Buffer.concat(chunks).toString('utf8')).syncedLyrics;
    return isTimed(value) ? value : '';
  }
}

module.exports = { DesktopLyrics, isTimed };
