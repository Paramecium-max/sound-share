// Schedules native speaker-time-aligned beat and band onsets onto the display
// timeline, applying a small scanout lead so the bright water crest emerges at
// the album rim on the exact acoustic downbeat.
const VISUAL_LEAD_MS = 10;

export class RippleOnsetQueue {
  constructor() {
    this.pending = [];
    this.lastId = 0;
    this.lastTime = -Infinity;
  }

  reset() {
    this.pending = [];
    this.lastTime = -Infinity;
  }

  accept(events, mode, now, elapsed = 0) {
    if (!Array.isArray(events)) return;
    const bridgeLag = Math.max(0, Number(elapsed) || 0);
    for (const event of events) {
      if (!Number.isFinite(event.id) || event.id <= this.lastId) continue;
      this.lastId = event.id;
      if (!["bass", "mid", "high", "drum"].includes(event.band)) continue;

      const strength = Math.max(0, Math.min(1, Number(event.strength) || 0));
      if (mode === "percussion") {
        if (event.band !== "drum" || !event.percussion) continue;
      } else if (mode === "classic") {
        const isDrum = event.band === "drum" && !!event.percussion;
        const isStrongGroove = (event.band === "bass" || event.band === "mid") && strength >= 0.34;
        if (!isDrum && !isStrongGroove) continue;
      } else if (event.band !== mode) {
        continue;
      }

      const delay = Number(event.delay);
      if (!Number.isFinite(delay)) continue;
      const at = now + delay - bridgeLag - VISUAL_LEAD_MS;
      if (at < now - 220 || at > now + 750) continue;

      const near = this.pending.find((other) => Math.abs(other.at - at) < 48);
      if (near) {
        const curIsDrum = event.band === "drum";
        const nearIsDrum = near.band === "drum";
        if ((curIsDrum && !nearIsDrum) || (curIsDrum === nearIsDrum && strength > near.strength)) {
          near.at = Math.min(near.at, at);
          near.band = event.band;
          near.strength = Math.max(near.strength, strength);
        }
      } else if (this.pending.length < 40) {
        const gap = mode === "percussion" ? 115 : mode === "classic" ? 110 : 95;
        this.pending.push({ at, band: event.band, strength, gap });
      }
    }
    this.pending.sort((a, b) => a.at - b.at);
  }

  takeDue(now) {
    let chosen = null;
    while (this.pending.length && this.pending[0].at <= now) {
      const candidate = this.pending.shift();
      if (now - candidate.at > 200) continue;
      if (candidate.at - this.lastTime < candidate.gap) continue;
      chosen = candidate;
    }
    if (!chosen) return null;
    this.lastTime = chosen.at;
    return chosen;
  }
}
