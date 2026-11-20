// Rendered PCM analysis runs on the host analysis thread. The audio callback
// only copies samples into this preallocated single-producer ring.
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{LazyLock, Mutex};
use std::time::Instant;

const CAPACITY: usize = 32768;
const WINDOW: usize = 2048;
const HOP: usize = 512;
static SAMPLES: [AtomicU32; CAPACITY] = [const { AtomicU32::new(0) }; CAPACITY];
static TIMES: [AtomicU64; CAPACITY] = [const { AtomicU64::new(0) }; CAPACITY];
static WRITTEN: AtomicU64 = AtomicU64::new(0);
static RATE: AtomicU32 = AtomicU32::new(48000);
static SENSITIVITY: AtomicU32 = AtomicU32::new(55);
static ORIGIN: LazyLock<Instant> = LazyLock::new(Instant::now);
static ANALYSIS: LazyLock<Mutex<PcmAnalysis>> = LazyLock::new(|| Mutex::new(PcmAnalysis::new()));
static EXTERNAL: LazyLock<Mutex<Detector>> = LazyLock::new(|| Mutex::new(Detector::new()));

pub fn now_ms() -> f64 { ORIGIN.elapsed().as_secs_f64() * 1000.0 }

pub fn set_sensitivity(value: u32) {
    SENSITIVITY.store(value.min(100), Ordering::Relaxed);
}

fn sensitivity_norm() -> f32 {
    SENSITIVITY.load(Ordering::Relaxed).min(100) as f32 / 100.0
}

pub fn tap_output(frames: &[(f32, f32)], rate: u32, latency_ms: f64) {
    let start = WRITTEN.load(Ordering::Relaxed);
    let time = now_ms() + latency_ms.max(0.0);
    RATE.store(rate, Ordering::Relaxed);
    for (i, &(left, right)) in frames.iter().enumerate() {
        let at = (start as usize + i) % CAPACITY;
        SAMPLES[at].store((0.5 * (left + right)).to_bits(), Ordering::Relaxed);
        TIMES[at].store(((time + i as f64 * 1000.0 / rate.max(1) as f64) * 1000.0) as u64, Ordering::Relaxed);
    }
    WRITTEN.store(start + frames.len() as u64, Ordering::Release);
}

#[derive(Clone, Debug)]
pub struct OnsetEvent {
    pub band: usize,
    pub strength: f32,
    pub percussion: bool,
    pub time_ms: f64,
}

pub fn poll(active: bool) -> Vec<OnsetEvent> {
    let Ok(mut state) = ANALYSIS.lock() else { return Vec::new() };
    let end = WRITTEN.load(Ordering::Acquire);
    if !active {
        state.reset(end);
        return Vec::new();
    }
    let rate = RATE.load(Ordering::Relaxed);
    if rate != state.rate || end.saturating_sub(state.read) > (CAPACITY / 2) as u64 {
        state.reset(end);
        state.rate = rate;
        return Vec::new();
    }
    let mut events = Vec::new();
    while state.read < end {
        let at = state.read as usize % CAPACITY;
        let sample = f32::from_bits(SAMPLES[at].load(Ordering::Relaxed));
        let time = TIMES[at].load(Ordering::Relaxed) as f64 / 1000.0;
        state.read += 1;
        state.window.push_back(sample);
        if state.window.len() == WINDOW {
            let spectrum = spectrum(&state.window);
            events.extend(state.detector.process(&spectrum, rate, time - WINDOW as f64 * 500.0 / rate as f64));
            state.window.drain(..HOP);
        }
    }
    // If the producer overtook this copy, discard the possibly torn window.
    if WRITTEN.load(Ordering::Acquire).saturating_sub(end) >= (CAPACITY / 2) as u64 {
        let latest = WRITTEN.load(Ordering::Acquire);
        state.reset(latest);
        events.clear();
    }
    events
}

pub fn external_spectrum(magnitudes: &[f32], rate: u32) -> Vec<OnsetEvent> {
    let Ok(mut detector) = EXTERNAL.lock() else { return Vec::new() };
    detector.process(magnitudes, rate, now_ms())
}

struct PcmAnalysis {
    read: u64,
    rate: u32,
    window: VecDeque<f32>,
    detector: Detector,
}
impl PcmAnalysis {
    fn new() -> Self { Self { read: 0, rate: 48000, window: VecDeque::with_capacity(WINDOW), detector: Detector::new() } }
    fn reset(&mut self, end: u64) {
        self.read = end;
        self.window.clear();
        self.detector = Detector::new();
    }
}

// Radix-2 FFT over a Hann window. Allocations occur on the analysis thread.
fn spectrum(samples: &VecDeque<f32>) -> Vec<f32> {
    let n = samples.len();
    let mut real: Vec<f32> = samples.iter().enumerate().map(|(i, x)|
        x * (0.5 - 0.5 * (std::f32::consts::TAU * i as f32 / (n - 1) as f32).cos())).collect();
    let mut imag = vec![0.0; n];
    let mut j = 0;
    for i in 1..n {
        let mut bit = n >> 1;
        while j & bit != 0 { j ^= bit; bit >>= 1; }
        j ^= bit;
        if i < j { real.swap(i, j); }
    }
    let mut size = 2;
    while size <= n {
        let angle = -std::f32::consts::TAU / size as f32;
        let (wi, wr) = angle.sin_cos();
        for base in (0..n).step_by(size) {
            let (mut r, mut im) = (1.0, 0.0);
            for k in 0..size / 2 {
                let a = base + k;
                let b = a + size / 2;
                let tr = r * real[b] - im * imag[b];
                let ti = r * imag[b] + im * real[b];
                real[b] = real[a] - tr;
                imag[b] = imag[a] - ti;
                real[a] += tr;
                imag[a] += ti;
                (r, im) = (r * wr - im * wi, r * wi + im * wr);
            }
        }
        size *= 2;
    }
    (0..n / 2).map(|i| (real[i].hypot(imag[i]) * 2.0 / n as f32).max(0.0)).collect()
}

fn median(values: impl Iterator<Item = f32>) -> f32 {
    let mut storage = [0.0f32; 256];
    let mut count = 0;
    for value in values.take(storage.len()) {
        storage[count] = value;
        count += 1;
    }
    if count == 0 { return 0.0; }
    let sorted = &mut storage[..count];
    sorted.sort_unstable_by(f32::total_cmp);
    sorted[count / 2]
}

struct Detector {
    previous: Vec<f32>,
    harmonic: VecDeque<Vec<f32>>,
    history: [VecDeque<f32>; 3],
    last_hit: [f64; 3],
    armed: [bool; 3],
    release_since: [Option<f64>; 3],
    attack_energy: [f32; 3],
    pending: Option<([f32; 3], [bool; 3], f64, [f32; 3])>,
    last_time: f64,
    history_time: f64,
    drums: DrumDetector,
}
impl Detector {
    fn new() -> Self {
        Self { previous: Vec::new(), harmonic: VecDeque::new(),
            history: std::array::from_fn(|_| VecDeque::new()),
            last_hit: [-1000.0; 3], armed: [true; 3], release_since: [None; 3], attack_energy: [0.0; 3], pending: None, last_time: -1000.0, history_time: 0.0, drums: DrumDetector::new() }
    }

    fn process(&mut self, input: &[f32], rate: u32, time: f64) -> Vec<OnsetEvent> {
        if input.len() < 16 || rate == 0 { return Vec::new(); }
        if time - self.last_time > 500.0 || time < self.last_time || self.previous.len() != input.len() {
            *self = Self::new();
            self.previous = vec![0.0; input.len()];
        }
        self.last_time = time;
        let log: Vec<f32> = input.iter().map(|&x| (1.0 + 100.0 * x.max(0.0)).ln()).collect();
        let mut flux = [0.0f32; 3];
        let mut energy = [0.0f32; 3];
        let mut counts = [0usize; 3];
        for k in 1..input.len() - 1 {
            let hz = k as f32 * rate as f32 / (input.len() * 2) as f32;
            if !(35.0..12000.0).contains(&hz) { continue; }
            let band = if hz < 250.0 { 0 } else if hz < 2000.0 { 1 } else { 2 };
            // Frequency-axis max reference suppresses bin movement from vibrato.
            let reference = self.previous[k - 1].max(self.previous[k]).max(self.previous[k + 1]);
            let rise = (log[k] - reference).max(0.0);
            flux[band] += rise;
            energy[band] += input[k] * input[k];
            counts[band] += 1;
        }
        let mut accepted = [false; 3];
        let percussion = [false; 3];
        let mut scores = [0.0; 3];
        let sens = sensitivity_norm();
        let sens_scale = 1.45 - 0.85 * sens; // 0%=strict 1.45x, 55%=0.98x, 100%=wide 0.60x
        let mad_k = [2.2f32, 2.4, 4.2];
        let min_thresh = [0.005f32, 0.006, 0.012];
        let band_holdoff = [95.0f64, 100.0, 115.0];
        for band in 0..3 {
            if time - self.last_hit[band] < 65.0 {
                self.attack_energy[band] = self.attack_energy[band].max(energy[band]);
            }
            flux[band] /= counts[band].max(1) as f32;
            let floor = median(self.history[band].iter().copied());
            let mad = median(self.history[band].iter().map(|value| (value - floor).abs()));
            let threshold = (floor + mad_k[band] * sens_scale * mad).max(min_thresh[band] * sens_scale);
            let elapsed = (time - self.last_hit[band]).max(0.0);
            let tail_floor = self.attack_energy[band] * 0.07 * (-elapsed / 240.0).exp() as f32;
            if !self.armed[band] {
                // Re-arm once spectral flux drops back below threshold (never block
                // bass/mid indefinitely during sustained vocals or basslines).
                let released = flux[band] < threshold * 0.72;
                if released {
                    let since = self.release_since[band].get_or_insert(time);
                    if time - *since >= 15.0 { self.armed[band] = true; }
                } else if elapsed >= 220.0 {
                    self.armed[band] = true;
                } else {
                    self.release_since[band] = None;
                }
            }
            accepted[band] = self.armed[band] && flux[band] > threshold &&
                energy[band] > 0.000002f32.max(tail_floor);
            scores[band] = (flux[band] / (threshold * 3.6)).clamp(0.18, 1.0);
        }
        let mut events = Vec::new();
        if let Some((prior, flags, stamp, prior_energy)) = self.pending {
            for band in 0..3 {
                if self.armed[band] && prior[band] > 0.0 && scores[band] < prior[band] &&
                    stamp - self.last_hit[band] >= band_holdoff[band] {
                    self.last_hit[band] = stamp;
                    self.armed[band] = false;
                    self.release_since[band] = None;
                    self.attack_energy[band] = prior_energy[band];
                    events.push(OnsetEvent { band, strength: prior[band], percussion: flags[band], time_ms: stamp });
                }
            }
        }
        self.pending = Some((std::array::from_fn(|i| if accepted[i] { scores[i] } else { 0.0 }), percussion, time, energy));
        self.previous = log.clone();
        self.harmonic.push_back(log);
        while self.harmonic.len() > 9 { self.harmonic.pop_front(); }
        for band in 0..3 {
            self.history[band].push_back(flux[band]);
            // Roughly one second at the 48 kHz / 512 analysis hop.
            let hop_ms = (time - self.history_time).clamp(5.0, 100.0);
            let limit = (1000.0 / hop_ms) as usize;
            while self.history[band].len() > limit { self.history[band].pop_front(); }
        }
        self.history_time = time;
        // Drum events have one independent lane. A kick's bass, click and
        // resonant tail must never become three separate "percussion" events.
        if let Some(drum) = self.drums.process(input, rate, time) { events.push(drum); }
        events
    }
}

// Mix-aware kick/snare onset gate: operates on in-band sub-bass and snare-wire
// flux/energy so kicks and snares are caught reliably even under loud vocals
// and synths, while rejecting tonal notes, hi-hats, and room echoes.
// One 55 ms observation window and a 175 ms global holdoff guarantee 1 hit -> 1 wave.
#[derive(Clone, Copy, Default)]
struct DrumFeatures {
    energy: f32,
    sub: f32,
    low_mid: f32,
    snare_band: f32,
    high: f32,
    sub_centroid: f32,
    centroid: f32,
    snare_flatness: f32,
}
impl DrumFeatures {
    fn from_spectrum(input: &[f32], rate: u32) -> Self {
        let mut f = Self::default();
        let (mut sub_w, mut sub_c) = (0.0f32, 0.0f32);
        let (mut log_sum, mut noise_sum, mut bins) = (0.0f32, 0.0f32, 0usize);
        for (k, &value) in input.iter().enumerate().skip(1) {
            let hz = k as f32 * rate as f32 / (input.len() * 2) as f32;
            if !(35.0..11000.0).contains(&hz) { continue; }
            let power = value.max(0.0).powi(2);
            f.energy += power;
            f.centroid += power * hz;
            if hz < 150.0 { f.sub += power; }
            if hz < 220.0 { sub_w += power; sub_c += power * hz; }
            if (150.0..500.0).contains(&hz) { f.low_mid += power; }
            if (450.0..5500.0).contains(&hz) {
                f.snare_band += power;
                log_sum += (power + 1e-14).ln();
                noise_sum += power;
                bins += 1;
            }
            if hz >= 5500.0 { f.high += power; }
        }
        f.sub_centroid = sub_c / sub_w.max(1e-14);
        f.centroid /= f.energy.max(1e-14);
        if bins > 0 {
            f.snare_flatness = (log_sum / bins as f32).exp() / (noise_sum / bins as f32 + 1e-14);
        }
        f
    }
}
struct DrumCandidate {
    at: f64,
    peak: DrumFeatures,
    pre_sub: f32,
    pre_snare: f32,
    first_pitch: f32,
    attack_flatness: f32,
    attack_click: f32,
    last: DrumFeatures,
}
struct DrumDetector {
    previous: DrumFeatures,
    sub_history: VecDeque<f32>,
    candidate: Option<DrumCandidate>,
    last_attempt: f64,
    last_hit: f64,
    hit_sub: f32,
    hit_snare: f32,
    armed: bool,
}
impl DrumDetector {
    fn new() -> Self {
        Self {
            previous: DrumFeatures::default(),
            sub_history: VecDeque::with_capacity(64),
            candidate: None,
            last_attempt: -1000.0,
            last_hit: -1000.0,
            hit_sub: 0.0,
            hit_snare: 0.0,
            armed: true,
        }
    }
    fn process(&mut self, input: &[f32], rate: u32, time: f64) -> Option<OnsetEvent> {
        let f = DrumFeatures::from_spectrum(input, rate);
        let prior = self.previous;
        self.previous = f;
        let sub_floor = median(self.sub_history.iter().copied());
        self.sub_history.push_back(f.sub);
        while self.sub_history.len() > 48 { self.sub_history.pop_front(); }

        let since = (time - self.last_hit).max(0.0);
        if (f.sub < self.hit_sub * 0.55 && f.snare_band < self.hit_snare * 0.55) || since >= 190.0 {
            self.armed = true;
        }
        if let Some(c) = self.candidate.as_mut() {
            if f.sub > c.peak.sub {
                c.peak.sub = f.sub;
                c.peak.low_mid = c.peak.low_mid.max(f.low_mid);
                c.peak.sub_centroid = f.sub_centroid;
            }
            if f.snare_band > c.peak.snare_band {
                c.peak.snare_band = f.snare_band;
                c.peak.snare_flatness = c.peak.snare_flatness.max(f.snare_flatness);
                c.peak.centroid = f.centroid;
            }
            if f.energy > c.peak.energy {
                c.peak.energy = f.energy;
                c.peak.high = f.high;
            }
            if time - c.at < 24.0 {
                if f.sub + f.low_mid > 1e-9 {
                    c.first_pitch = c.first_pitch.max(f.sub_centroid);
                }
                c.attack_flatness = c.attack_flatness.max(f.snare_flatness);
                c.attack_click = c.attack_click.max(f.snare_band / f.energy.max(1e-12));
            }
            c.last = f;
            if time - c.at < 55.0 { return None; }
            let c = self.candidate.take().unwrap();
            let p = c.peak;
            let energy = p.energy.max(1e-12);
            let sub_decay = c.last.sub / p.sub.max(1e-12);
            let snare_decay = c.last.snare_band / p.snare_band.max(1e-12);

            let sens = sensitivity_norm();
            let sub_surge_ratio = 1.78 - 0.58 * sens; // 0%: 1.78, 55%: 1.46, 100%: 1.20
            let sub_floor_ratio = 1.65 - 0.50 * sens;
            let sub_share_min = 0.18 - 0.13 * sens;   // 0%: 0.18, 55%: 0.11, 100%: 0.05
            let snare_flat_min = 0.28 - 0.12 * sens;  // 0%: 0.28, 55%: 0.21, 100%: 0.16
            let snare_surge_ratio = 2.10 - 0.65 * sens;
            let holdoff_ms = (230.0 - 95.0 * sens) as f64;

            // Kick: works even under loud vocals/synths by checking in-band sub surge & sub decay.
            let sub_surge = p.sub > c.pre_sub.max(1e-9) * sub_surge_ratio && p.sub > sub_floor * sub_floor_ratio;
            let pitch_drop = c.first_pitch > 88.0 && c.last.sub_centroid < c.first_pitch * 0.86;
            let punch_kick = p.low_mid > p.sub * 0.06 && p.sub_centroid < 145.0
                && (c.attack_click > 0.018 || p.sub / energy > sub_share_min * 1.25);
            let kick = sub_surge
                && p.sub / energy > sub_share_min
                && p.sub_centroid < 160.0
                && (pitch_drop || punch_kick || sens > 0.72)
                && sub_decay > 0.015 && sub_decay < 0.90;

            // Snare: broadband mid/upper burst with fast in-band decay, rejecting pure tones & hi-hats.
            let snare_surge = p.snare_band > c.pre_snare.max(1e-9) * snare_surge_ratio;
            let snare = snare_surge
                && p.snare_flatness > snare_flat_min
                && (p.low_mid + p.snare_band) / energy > 0.22
                && p.high / energy < 0.48
                && (320.0..4800.0).contains(&p.centroid)
                && snare_decay < 0.74;

            if (kick || snare) && c.at - self.last_hit >= holdoff_ms {
                self.last_hit = c.at;
                self.hit_sub = p.sub;
                self.hit_snare = p.snare_band;
                self.armed = false;
                let driver = if kick { p.sub.sqrt() } else { p.snare_band.sqrt() };
                return Some(OnsetEvent {
                    band: 3,
                    percussion: true,
                    time_ms: c.at,
                    strength: (0.45 + driver * 2.2).clamp(0.45, 1.0),
                });
            }
            return None;
        }
        let sens = sensitivity_norm();
        let holdoff_ms = (230.0 - 95.0 * sens) as f64;
        let sub_rise_ratio = 1.68 - 0.50 * sens;   // 0%: 1.68, 55%: 1.40, 100%: 1.18
        let snare_rise_ratio = 2.15 - 0.70 * sens; // 0%: 2.15, 55%: 1.76, 100%: 1.45
        let sub_tail = self.hit_sub * 0.16 * (-since / 400.0).exp() as f32;
        let snare_tail = self.hit_snare * 0.16 * (-since / 360.0).exp() as f32;
        let sub_rise = f.sub > prior.sub * sub_rise_ratio && f.sub > 0.000003f32.max(sub_tail);
        let snare_rise = f.snare_band > prior.snare_band * snare_rise_ratio && f.snare_band > 0.000006f32.max(snare_tail);
        if self.armed && since >= holdoff_ms && time - self.last_attempt >= 75.0 && (sub_rise || snare_rise) {
            self.last_attempt = time;
            self.candidate = Some(DrumCandidate {
                at: time,
                peak: f,
                pre_sub: prior.sub,
                pre_snare: prior.snare_band,
                first_pitch: f.sub_centroid,
                attack_flatness: f.snare_flatness,
                attack_click: f.snare_band / f.energy.max(1e-12),
                last: f,
            });
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn analyse(frequency: f32, pulses: bool, length: usize) -> Vec<OnsetEvent> {
        let mut detector = Detector::new();
        let mut window = VecDeque::new();
        let mut events = Vec::new();
        for i in 0..length {
            let local = i % 6000; // sixteenth notes at 120 BPM (125 ms)
            let envelope = if pulses {
                if local < 2200 { (-(local as f32) / 480.0).exp() } else { 0.0 }
            } else { 0.4 };
            let x = envelope * (std::f32::consts::TAU * frequency * i as f32 / 48000.0).sin();
            window.push_back(x);
            if window.len() == WINDOW {
                events.extend(detector.process(&spectrum(&window), 48000, i as f64 / 48.0));
                window.drain(..HOP);
            }
        }
        events
    }

    #[test]
    fn one_strike_with_five_decay_echoes_stays_one_event() {
        for (frequency, band) in [(94.0, 0), (750.0, 1), (6000.0, 2)] {
            let mut detector = Detector::new();
            let mut window = VecDeque::new();
            let mut events = Vec::new();
            for i in 0..48000 {
                let mut envelope = 0.0;
                for hit in 0..6 {
                    let start = 2400 + hit * 4800;
                    if i >= start && i < start + 2000 {
                        let amplitude = if hit == 0 { 0.8 } else { 0.12 * (-(hit as f32 - 1.0) * 0.3).exp() };
                        envelope += amplitude * (-((i - start) as f32) / 480.0).exp();
                    }
                }
                window.push_back(envelope * (std::f32::consts::TAU * frequency * i as f32 / 48000.0).sin());
                if window.len() == WINDOW {
                    events.extend(detector.process(&spectrum(&window), 48000, i as f64 / 48.0));
                    window.drain(..HOP);
                }
            }
            let count = events.iter().filter(|e| e.band == band).count();
            assert_eq!(count, 1, "{frequency}Hz: {events:?}");
        }
    }

    #[test]
    fn fft_hann_resolves_a_low_tone() {
        let samples = (0..WINDOW).map(|i| (std::f32::consts::TAU * 93.75 * i as f32 / 48000.0).sin()).collect();
        let bins = spectrum(&samples);
        let index = bins.iter().enumerate().max_by(|a, b| a.1.total_cmp(b.1)).unwrap().0;
        assert_eq!(index, 4);
    }

    #[test]
    fn band_pulses_and_fast_repeated_notes() {
        for (frequency, band) in [(94.0, 0), (750.0, 1), (6000.0, 2)] {
            let events = analyse(frequency, true, 48000);
            let selected: Vec<_> = events.iter().filter(|e| e.band == band).collect();
            eprintln!("frequency {frequency}: {} onsets", selected.len());
            assert!(selected.len() >= 6 && selected.len() <= 9);
            assert!(selected.windows(2).any(|pair| pair[1].time_ms - pair[0].time_ms < 170.0));
        }
    }

    #[test]
    fn sustained_tone_and_silence_do_not_keep_triggering() {
        assert!(analyse(94.0, false, 48000).iter().filter(|e| e.time_ms > 200.0).count() <= 1);
        let mut detector = Detector::new();
        for i in 0..300 {
            assert!(detector.process(&vec![0.0; 1024], 48000, i as f64 * 10.67).is_empty());
        }
    }

    #[test]
    fn pitched_notes_are_not_drums() {
        for hz in [70.0, 110.0, 440.0, 750.0, 6000.0] {
            let count = analyse(hz, true, 48000).iter().filter(|e| e.percussion).count();
            assert_eq!(count, 0, "{hz}Hz falsely triggered {count} drum events");
        }
    }

    #[test]
    fn real_kick_and_snare_emit_one_event_per_hit() {
        let mut detector = Detector::new();
        let mut window = VecDeque::new();
        let mut events = Vec::new();
        let mut rng = 0x1234_5678u32;
        // 4 hits over 2 seconds: kick @ 0.15s, snare @ 0.65s, kick @ 1.15s, snare @ 1.65s
        // plus soft room echoes 140ms after each hit that must NOT re-trigger.
        let mut phase = 0.0f32;
        for i in 0..96000 {
            let t = i as f32 / 48000.0;
            let mut x = 0.0f32;
            for &(hit_t, is_kick) in &[(0.15, true), (0.65, false), (1.15, true), (1.65, false)] {
                for &(dt_off, gain) in &[(0.0f32, 1.0f32), (0.14, 0.18)] {
                    let lt = t - (hit_t + dt_off);
                    if lt >= 0.0 && lt < 0.16 {
                        if is_kick {
                            let f = 54.0 + 105.0 * (-lt / 0.028).exp();
                            phase += std::f32::consts::TAU * f / 48000.0;
                            let env = (-lt / 0.045).exp();
                            x += gain * 0.85 * env * phase.sin();
                        } else {
                            rng = rng.wrapping_mul(1664525).wrapping_add(1013904223);
                            let noise = ((rng >> 9) as f32 / ((1u32 << 23) as f32)) * 2.0 - 1.0;
                            let env = (-lt / 0.032).exp();
                            let body = (std::f32::consts::TAU * 210.0 * lt).sin() * (-lt / 0.025).exp();
                            x += gain * (0.55 * noise * env + 0.35 * body);
                        }
                    }
                }
            }
            window.push_back(x);
            if window.len() == WINDOW {
                events.extend(detector.process(&spectrum(&window), 48000, i as f64 / 48.0));
                window.drain(..HOP);
            }
        }
        let drums: Vec<_> = events.iter().filter(|e| e.percussion && e.band == 3).collect();
        assert_eq!(drums.len(), 4, "Expected exactly 4 drum hits, got {drums:?}");
    }

    #[test]
    fn kicks_inside_loud_vocal_mix_are_detected() {
        let mut detector = Detector::new();
        let mut window = VecDeque::new();
        let mut events = Vec::new();
        let mut phase = 0.0f32;
        // 4 kicks (both sweeping acoustic kicks and deeper 808-style kicks) buried under
        // loud continuous vocal/synth chord (330Hz + 554Hz + 880Hz).
        for i in 0..96000 {
            let t = i as f32 / 48000.0;
            let vocal = 0.42 * (std::f32::consts::TAU * 330.0 * t).sin()
                + 0.30 * (std::f32::consts::TAU * 554.0 * t).sin()
                + 0.22 * (std::f32::consts::TAU * 880.0 * t).sin();
            let mut kick = 0.0f32;
            for &(hit_t, start_f, end_f) in &[
                (0.20f32, 130.0f32, 55.0f32),
                (0.65, 95.0, 52.0),
                (1.10, 125.0, 58.0),
                (1.55, 88.0, 50.0),
            ] {
                let lt = t - hit_t;
                if lt >= 0.0 && lt < 0.14 {
                    let f = end_f + (start_f - end_f) * (-lt / 0.028).exp();
                    phase += std::f32::consts::TAU * f / 48000.0;
                    let env = (-lt / 0.048).exp();
                    let punch = 0.16 * (std::f32::consts::TAU * 220.0 * lt).sin() * (-lt / 0.012).exp();
                    kick += 0.65 * env * phase.sin() + punch;
                }
            }
            window.push_back(vocal + kick);
            if window.len() == WINDOW {
                events.extend(detector.process(&spectrum(&window), 48000, i as f64 / 48.0));
                window.drain(..HOP);
            }
        }
        let drums: Vec<_> = events.iter().filter(|e| e.percussion && e.band == 3).collect();
        assert_eq!(drums.len(), 4, "Expected all 4 kicks under vocal mix, got {drums:?}");
    }
}
