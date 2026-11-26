// Physical water-surface wave renderer inspired by WebGL Water (evanw) and
// jquery.ripples (sirxemic): analytical multi-wave superposition heightfield,
// exact 2D surface-normal gradients, trough shadowing, directional Blinn-Phong
// caustics, and radial energy attenuation — with automatic 2D Canvas fallback.

const MAX_WAVES = 16;

const VERT_SRC = `
attribute vec2 aPos;
varying vec2 vUv;
void main() {
  vUv = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}
`;

const FRAG_SRC = `
precision highp float;
varying vec2 vUv;
uniform vec2 uResolution;
uniform vec2 uOrigin;
uniform float uMaxDist;
uniform int uCount;
uniform vec4 uWaves[${MAX_WAVES}];   // x=radius(norm), y=amp, z=width(norm), w=age
uniform vec3 uColors[${MAX_WAVES}];

void main() {
  vec2 px = vec2(vUv.x * uResolution.x, (1.0 - vUv.y) * uResolution.y);
  vec2 originPx = uOrigin * uResolution;
  vec2 delta = px - originPx;
  float distPx = length(delta);
  vec2 dir = distPx > 0.001 ? delta / distPx : vec2(1.0, 0.0);
  float d = distPx / max(uMaxDist, 1.0);

  vec2 grad = vec2(0.0);
  float totalEnergy = 0.0;
  vec3 tintAccum = vec3(0.0);
  float tintWeight = 0.0;

  for (int i = 0; i < ${MAX_WAVES}; i++) {
    if (i >= uCount) break;
    vec4 w = uWaves[i];
    float R = w.x;
    float A = w.y;
    float width = max(w.z, 0.012);
    float age = w.w;
    float s = (d - R) / width;
    if (s < -4.2 || s > 2.2) continue;

    // Clean single primary water crest + soft concave trough behind it (avoids
    // multi-ring clutter so 1 drum beat clearly reads as 1 physical water wave).
    float k = s < 0.0 ? 0.56 : 1.18;
    float env = exp(-k * s * s);
    float phase = 2.05 * s;
    float c = cos(phase);
    float sn = sin(phase);
    float dWave = env * (-2.05 * sn - 2.0 * k * s * c);

    float fadeIn = smoothstep(0.0, 0.065, age);
    float radialAtten = 1.0 / pow(1.0 + d * 2.4, 0.65);
    float edgeFade = 1.0 - smoothstep(0.94, 1.20, d);
    float amp = A * fadeIn * radialAtten * edgeFade;

    float slopeMag = amp * dWave / width;
    grad += slopeMag * dir;
    float e = abs(amp * dWave);
    totalEnergy += e;
    tintAccum += e * uColors[i];
    tintWeight += e;
  }

  if (totalEnergy < 0.002) {
    gl_FragColor = vec4(0.0);
    return;
  }

  vec3 waterTint = tintWeight > 0.0001 ? (tintAccum / tintWeight) : vec3(0.75, 0.91, 1.0);

  // 3D water surface normal from analytical heightfield gradient.
  vec3 N = normalize(vec3(-grad * 0.022, 1.0));
  float slope = length(N.xy);

  vec3 L1 = normalize(vec3(-0.52, -0.66, 0.54));
  vec3 L2 = normalize(vec3(0.56, 0.48, 0.67));
  vec3 V = vec3(0.0, 0.0, 1.0);
  vec3 H1 = normalize(L1 + V);
  vec3 H2 = normalize(L2 + V);

  float ndl1 = dot(N, L1);
  // Subtle translucent concave trough shadow + crisp liquid specular caustic.
  float shadow = clamp((L1.z - ndl1) * 1.15, 0.0, 0.30) * smoothstep(0.02, 0.16, slope);
  float spec1 = pow(max(dot(N, H1), 0.0), 46.0) * smoothstep(0.02, 0.18, slope);
  float spec2 = pow(max(dot(N, H2), 0.0), 18.0) * smoothstep(0.03, 0.22, slope);
  float fresnel = smoothstep(0.02, 0.36, slope) * 0.16;

  // Premultiplied translucent liquid glass overlay (keeps lyrics crisp).
  vec3 bright = waterTint * (fresnel + spec2 * 0.38) + vec3(0.95, 0.98, 1.0) * (spec1 * 0.68);
  vec3 dark = vec3(0.01, 0.03, 0.08) * shadow;
  float alpha = clamp(shadow * 0.54 + fresnel * 0.56 + spec2 * 0.32 + spec1 * 0.62, 0.0, 0.56);
  vec3 outRgb = clamp(bright + dark, 0.0, 1.0);
  gl_FragColor = vec4(outRgb, alpha);
}
`;

function parseRgb(rgbStr) {
  const parts = String(rgbStr || "180, 224, 255").split(",").map(Number);
  return [
    (Number.isFinite(parts[0]) ? parts[0] : 180) / 255,
    (Number.isFinite(parts[1]) ? parts[1] : 224) / 255,
    (Number.isFinite(parts[2]) ? parts[2] : 255) / 255,
  ];
}

export class WaterRippleRenderer {
  constructor(canvas) {
    this.canvas = canvas;
    this.gl = null;
    this.program = null;
    this.uniforms = null;
    this.sprites = new Map();
    this.colorCache = new Map();
    this.waveData = new Float32Array(MAX_WAVES * 4);
    this.colorData = new Float32Array(MAX_WAVES * 3);
    if (this.canvas) {
      this.canvas.addEventListener("webglcontextlost", (e) => {
        e.preventDefault();
        this.gl = null;
        this.program = null;
        this.uniforms = null;
      });
      this.canvas.addEventListener("webglcontextrestored", () => {
        this.initWebGL();
      });
    }
    this.initWebGL();
  }

  initWebGL() {
    if (!this.canvas) return;
    try {
      const gl = this.canvas.getContext("webgl", {
        alpha: true,
        antialias: false,
        depth: false,
        stencil: false,
        premultipliedAlpha: true,
        preserveDrawingBuffer: false,
        powerPreference: "high-performance",
      });
      if (!gl) return;
      const compile = (type, src) => {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
          gl.deleteShader(s);
          return null;
        }
        return s;
      };
      const vs = compile(gl.VERTEX_SHADER, VERT_SRC);
      const fs = compile(gl.FRAGMENT_SHADER, FRAG_SRC);
      if (!vs || !fs) return;
      const prog = gl.createProgram();
      gl.attachShader(prog, vs);
      gl.attachShader(prog, fs);
      gl.linkProgram(prog);
      if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) return;
      gl.useProgram(prog);

      const buf = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, buf);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
      const loc = gl.getAttribLocation(prog, "aPos");
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);

      this.gl = gl;
      this.program = prog;
      this.uniforms = {
        res: gl.getUniformLocation(prog, "uResolution"),
        origin: gl.getUniformLocation(prog, "uOrigin"),
        maxDist: gl.getUniformLocation(prog, "uMaxDist"),
        count: gl.getUniformLocation(prog, "uCount"),
        waves: gl.getUniformLocation(prog, "uWaves[0]"),
        colors: gl.getUniformLocation(prog, "uColors[0]"),
      };
    } catch (_) {
      this.gl = null;
    }
  }

  clear(width, height) {
    if (this.gl) {
      const gl = this.gl;
      gl.viewport(0, 0, width, height);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
    } else if (this.canvas) {
      const ctx = this.canvas.getContext("2d");
      ctx?.clearRect(0, 0, width, height);
    }
  }

  render(waves, width, height, originX, originY) {
    if (!waves.length) {
      this.clear(width, height);
      return;
    }
    const cx = originX * width;
    const cy = originY * height;
    const maxDist = Math.max(
      Math.hypot(cx, cy),
      Math.hypot(width - cx, cy),
      Math.hypot(cx, height - cy),
      Math.hypot(width - cx, height - cy),
    );
    if (this.gl) {
      this.renderWebGL(waves, width, height, originX, originY, maxDist);
    } else {
      this.renderFallback2D(waves, width, height, cx, cy, maxDist);
    }
  }

  renderWebGL(waves, width, height, originX, originY, maxDist) {
    const gl = this.gl;
    if (gl.isContextLost && gl.isContextLost()) {
      this.gl = null;
      this.renderFallback2D(waves, width, height, originX * width, originY * height, maxDist);
      return;
    }
    gl.viewport(0, 0, width, height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    const start = Math.max(0, waves.length - MAX_WAVES);
    const count = waves.length - start;
    const waveData = this.waveData;
    const colorData = this.colorData;
    waveData.fill(0);
    colorData.fill(0);
    for (let i = 0; i < count; i++) {
      const w = waves[start + i];
      const amp = (0.58 + 0.52 * w.opacity) * (w.thickness >= 1.1 ? 1.15 : 0.92);
      const bandWidth = (0.036 + 0.018 * w.thickness) * (1 + w.radius * 0.35);
      waveData[i * 4] = w.radius;
      waveData[i * 4 + 1] = amp;
      waveData[i * 4 + 2] = bandWidth;
      waveData[i * 4 + 3] = w.age;
      let rgb = this.colorCache.get(w.rgb);
      if (!rgb) {
        rgb = parseRgb(w.rgb);
        this.colorCache.set(w.rgb, rgb);
      }
      colorData[i * 3] = rgb[0];
      colorData[i * 3 + 1] = rgb[1];
      colorData[i * 3 + 2] = rgb[2];
    }

    gl.useProgram(this.program);
    gl.uniform2f(this.uniforms.res, width, height);
    gl.uniform2f(this.uniforms.origin, originX, originY);
    gl.uniform1f(this.uniforms.maxDist, maxDist);
    gl.uniform1i(this.uniforms.count, count);
    gl.uniform4fv(this.uniforms.waves, waveData);
    gl.uniform3fv(this.uniforms.colors, colorData);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }

  fallbackSprite(rgb, thickness) {
    const key = rgb + ":" + thickness;
    if (this.sprites.has(key)) return this.sprites.get(key);
    const sprite = document.createElement("canvas");
    sprite.width = sprite.height = 384;
    const ctx = sprite.getContext("2d");
    const inner = Math.max(40, 191 - 44 * thickness);
    const grad = ctx.createRadialGradient(184, 184, inner, 192, 192, 191);
    grad.addColorStop(0, "rgba(0,0,0,0)");
    grad.addColorStop(0.22, "rgba(3,9,24,0.22)");
    grad.addColorStop(0.46, `rgba(${rgb},0.10)`);
    grad.addColorStop(0.68, "rgba(3,9,24,0.26)");
    grad.addColorStop(0.84, "rgba(255,255,255,0.34)");
    grad.addColorStop(0.93, `rgba(${rgb},0.14)`);
    grad.addColorStop(1, "rgba(255,255,255,0)");
    ctx.fillStyle = grad;
    ctx.beginPath();
    ctx.arc(192, 192, 191, 0, Math.PI * 2);
    ctx.arc(192, 192, inner, 0, Math.PI * 2, true);
    ctx.fill();
    this.sprites.set(key, sprite);
    return sprite;
  }

  renderFallback2D(waves, width, height, cx, cy, maxDist) {
    const ctx = this.canvas.getContext("2d");
    if (!ctx) return;
    ctx.clearRect(0, 0, width, height);
    for (const wave of waves) {
      const radius = wave.radius * maxDist;
      ctx.globalAlpha = Math.min(1, wave.age * 9) * wave.opacity;
      ctx.drawImage(this.fallbackSprite(wave.rgb, wave.thickness), cx - radius, cy - radius, radius * 2, radius * 2);
    }
    ctx.globalAlpha = 1;
  }
}

