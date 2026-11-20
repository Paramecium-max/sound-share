// A delayed bridge batch must not paint several new rings in one frame.
// Keep the freshest due attack and require visible spacing between launches.
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
    if (mode === "classic" || !Array.isArray(events)) return;
    for (const event of events) {
      if (!Number.isFinite(event.id) || event.id <= this.lastId) continue;
      this.lastId = event.id;
      if (!["bass", "mid", "high", "drum"].includes(event.band)) continue;
      if (mode === "percussion" ? event.band !== "drum" || !event.percussion : event.band !== mode) continue;
      const delay = Number(event.delay);
      if (!Number.isFinite(delay)) continue;
      const at = now + delay - Math.max(0, elapsed);
      if (at < now - 160 || at > now + 600) continue;
      const strength = Math.max(0, Math.min(1, Number(event.strength) || 0));
      const near = this.pending.find(other => Math.abs(other.at - at) < 70);
      if (near) {
        if (strength > near.strength) Object.assign(near, {band: event.band, strength});
      } else if (this.pending.length < 32) {
        this.pending.push({at, band: event.band, strength, gap: mode === "percussion" ? 180 : 120});
      }
    }
    this.pending.sort((a, b) => a.at - b.at);
  }

  takeDue(now) {
    let latest = null;
    while (this.pending.length && this.pending[0].at <= now) {
      latest = this.pending.shift();
    }
    if (!latest || now - latest.at > 160 || latest.at - this.lastTime < latest.gap) return null;
    this.lastTime = latest.at;
    return latest;
  }
}
