// A wave owns its travel distance. Lyric changes, speed controls and new beats
// cannot rewind it. Distances are fractions of the viewport's half diagonal.
export class RippleField {
  constructor() {
    this.waves = [];
    this.nextId = 1;
  }

  spawn(rgb, strength = null, elapsed = 0, percussive = false, startRadius = 0.04) {
    // Backpressure only drops NEW beats; never remove a wave already on screen.
    if (this.waves.length >= (strength === null ? 18 : 40)) return false;
    const weight = strength === null ? null : Math.max(0, Math.min(1, strength));
    const age = Math.max(0, Math.min(0.35, elapsed));
    const baseR = Math.max(0.015, Math.min(0.28, Number(startRadius) || 0.04));
    this.waves.push({
      id: this.nextId++, radius: baseR + age * 0.36, age, rgb,
      opacity: weight === null ? 1 : 0.74 + weight * 0.26,
      thickness: weight === null ? 1 : Math.round(
        (percussive ? 0.95 + weight * 0.35 : 0.74 + weight * 0.36) * 8
      ) / 8,
    });
    return true;
  }

  step(seconds) {
    const dt = Math.max(0, Math.min(0.1, seconds));
    for (const wave of this.waves) {
      wave.age += dt;
      wave.radius += dt * 0.36;
    }
    // Dispose only AFTER this wave's trailing capillary packet leaves the farthest corner.
    this.waves = this.waves.filter(wave => wave.radius <= 1.24);
  }

  clear() { this.waves = []; }
}
