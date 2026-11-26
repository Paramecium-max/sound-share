// Rendered PCM analysis runs on the host analysis thread. The audio callback
// only copies samples into this preallocated single-producer ring.
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{LazyLock, Mutex};
use std::time::Instant;

const CAPACITY: usize = 32768;
const WINDOW: usize = 2048;
const HOP: usize = 256;
const MAX_HOPS_PER_POLL: usize = 12;
const MAX_BINS: usize = 4096;
static CAPTURING: AtomicBool = AtomicBool::new(!cfg!(target_os = "android"));
static TAP_BUSY: AtomicBool = AtomicBool::new(false);
static STREAM: AtomicU64 = AtomicU64::new(0);
static RESET_AT: AtomicU64 = AtomicU64::new(0);
#[cfg(target_os = "android")]
static MONOTONIC_BIAS: AtomicU64 = AtomicU64::new(0);
static SAMPLES: [AtomicU32; CAPACITY] = [const { AtomicU32::new(0) }; CAPACITY];
static TIMES: [AtomicU64; CAPACITY] = [const { AtomicU64::new(0) }; CAPACITY];
static WRITTEN: AtomicU64 = AtomicU64::new(0);
static RATE: AtomicU32 = AtomicU32::new(48000);
static SENSITIVITY: AtomicU32 = AtomicU32::new(55);
static ORIGIN: LazyLock<Instant> = LazyLock::new(Instant::now);
static ANALYSIS: LazyLock<Mutex<PcmAnalysis>> = LazyLock::new(|| Mutex::new(PcmAnalysis::new()));
static EXTERNAL: LazyLock<Mutex<Detector>> = LazyLock::new(|| Mutex::new(Detector::new()));

static HANN_TABLE: LazyLock<[f32; WINDOW]> = LazyLock::new(|| {
    std::array::from_fn(|i| {
        0.5 - 0.5 * (std::f32::consts::TAU * i as f32 / (WINDOW - 1) as f32).cos()
    })
});

static BIT_REV_TABLE: LazyLock<[usize; WINDOW]> = LazyLock::new(|| {
    let mut table = [0usize; WINDOW];
    let mut j = 0usize;
    for i in 1..WINDOW {
        let mut bit = WINDOW >> 1;
        while j & bit != 0 {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        table[i] = j;
    }
    table
});

pub fn now_ms() -> f64 {
    ORIGIN.elapsed().as_secs_f64() * 1000.0
}

pub fn set_sensitivity(value: u32) {
    SENSITIVITY.store(value.min(100), Ordering::Relaxed);
}

fn sensitivity_norm() -> f32 {
    SENSITIVITY.load(Ordering::Relaxed).min(100) as f32 / 100.0
}

#[cfg(target_os = "android")]
pub(crate) fn capture_enabled() -> bool {
    CAPTURING.load(Ordering::Relaxed)
}

pub fn set_capture(enabled: bool) {
    CAPTURING.store(enabled, Ordering::Release);
    if !enabled {
        poll_into(false, &mut Vec::new());
        if let Ok(mut detector) = EXTERNAL.lock() {
            detector.reset();
        }
    }
}

#[cfg(target_os = "android")]
pub fn align_monotonic_clock(nanos: i64) {
    MONOTONIC_BIAS.store((nanos as f64 / 1e6 - now_ms()).to_bits(), Ordering::Release);
}

#[cfg(target_os = "android")]
pub fn monotonic_bias() -> f64 {
    f64::from_bits(MONOTONIC_BIAS.load(Ordering::Acquire))
}

#[cfg(target_os = "android")]
pub(crate) fn begin_output_stream() -> u64 {
    STREAM.fetch_add(1, Ordering::AcqRel) + 1
}

pub fn tap_output(frames: &[(f32, f32)], rate: u32, latency_ms: f64) {
    tap_output_at(frames, rate, now_ms() + latency_ms.max(0.0), 0, false);
}

pub(crate) fn tap_output_at(
    frames: &[(f32, f32)], rate: u32, time: f64, stream: u64, discontinuity: bool,
) {
    if frames.is_empty() || !CAPTURING.load(Ordering::Relaxed)
        || TAP_BUSY.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed).is_err()
    {
        return;
    }
    if stream != STREAM.load(Ordering::Acquire) {
        TAP_BUSY.store(false, Ordering::Release);
        return;
    }
    let start = WRITTEN.load(Ordering::Relaxed);
    if discontinuity {
        RESET_AT.store(start + 1, Ordering::Release);
    }
    RATE.store(rate.max(1), Ordering::Relaxed);
    let inv_rate_ms = 1000.0 / rate.max(1) as f64;
    for (i, &(left, right)) in frames.iter().enumerate() {
        let at = (start as usize + i) % CAPACITY;
        SAMPLES[at].store((0.5 * (left + right)).to_bits(), Ordering::Relaxed);
        TIMES[at].store(
            ((time + i as f64 * inv_rate_ms) * 1000.0).max(0.0) as u64,
            Ordering::Relaxed,
        );
    }
    WRITTEN.store(start + frames.len() as u64, Ordering::Release);
    TAP_BUSY.store(false, Ordering::Release);
}

#[derive(Clone, Debug)]
pub struct OnsetEvent {
    pub band: usize,
    pub strength: f32,
    pub percussion: bool,
    pub time_ms: f64,
}

pub fn poll(active: bool) -> Vec<OnsetEvent> {
    let mut events = Vec::new();
    poll_into(active, &mut events);
    events
}

pub fn poll_into(active: bool, events: &mut Vec<OnsetEvent>) {
    events.clear();
    let Ok(mut state) = ANALYSIS.lock() else {
        return;
    };
    let end = WRITTEN.load(Ordering::Acquire);
    if !active {
        state.reset(end);
        return;
    }
    let rate = RATE.load(Ordering::Relaxed).max(1);
    let marker = RESET_AT.load(Ordering::Acquire);
    if marker > state.reset_marker && marker <= end {
        let start = state.read.max(marker - 1);
        state.reset(start);
        state.reset_marker = marker;
    }
    if rate != state.rate {
        let start = state.read;
        state.reset(start);
        state.rate = rate;
    }
    if end.saturating_sub(state.read) > (WINDOW + HOP * MAX_HOPS_PER_POLL) as u64 {
        state.reset(end - (WINDOW + HOP * (MAX_HOPS_PER_POLL - 1)) as u64);
    }
    let mut remaining_hops = MAX_HOPS_PER_POLL;
    let state = &mut *state;
    // SuperFlux log-difference peaks once the transient advances ~3/8 of the Hann window
    // past the right edge (~16 ms at 48 kHz), aligning time_ms directly to the attack start.
    let window_attack_lag_ms = (WINDOW * 3 / 8) as f64 * 1000.0 / rate as f64;
    while state.read < end && remaining_hops != 0 {
        let at = state.read as usize % CAPACITY;
        let sample = f32::from_bits(SAMPLES[at].load(Ordering::Relaxed));
        let time = TIMES[at].load(Ordering::Relaxed) as f64 / 1000.0;
        state.read += 1;
        state.window.push_back(sample);
        if state.window.len() == WINDOW {
            let spec = state.fft.spectrum(&state.window);
            // Detector windows and refractory periods live on the PCM sample clock.
            // Updating the speaker timestamp anchor must not rewind detector time.
            let sample_time = (state.read - 1) as f64 * 1000.0 / rate as f64;
            let first_event = events.len();
            state.detector.process_into(spec, rate, sample_time - window_attack_lag_ms, events);
            for event in &mut events[first_event..] {
                event.time_ms += time - sample_time;
            }
            remaining_hops -= 1;
            state.window.drain(..HOP);
        }
    }
    // If the producer overtook this copy, discard the possibly torn window.
    if WRITTEN.load(Ordering::Acquire).saturating_sub(end) >= (CAPACITY / 2) as u64 {
        let latest = WRITTEN.load(Ordering::Acquire);
        state.reset(latest);
        events.clear();
    }
}

pub fn external_spectrum(magnitudes: &[f32], rate: u32) -> Vec<OnsetEvent> {
    let mut events = Vec::new();
    external_spectrum_into(magnitudes, rate, now_ms(), &mut events);
    events
}

pub fn external_spectrum_into(magnitudes: &[f32], rate: u32, time: f64, events: &mut Vec<OnsetEvent>) {
    events.clear();
    if let Ok(mut detector) = EXTERNAL.lock() {
        detector.process_into(magnitudes, rate, time, events);
    }
}

struct PcmAnalysis {
    read: u64,
    reset_marker: u64,
    rate: u32,
    window: VecDeque<f32>,
    fft: Fft,
    detector: Detector,
}

impl PcmAnalysis {
    fn new() -> Self {
        Self {
            read: 0,
            reset_marker: 0,
            rate: 48000,
            window: VecDeque::with_capacity(WINDOW),
            fft: Fft::new(),
            detector: Detector::new(),
        }
    }

    fn reset(&mut self, end: u64) {
        self.read = end;
        self.window.clear();
        self.detector.reset();
    }
}

struct Fft {
    real: Vec<f32>,
    imag: Vec<f32>,
    magnitudes: Vec<f32>,
    twiddles: Vec<(f32, f32)>,
}

impl Fft {
    fn new() -> Self {
        Self {
            real: vec![0.0; WINDOW],
            imag: vec![0.0; WINDOW],
            magnitudes: vec![0.0; WINDOW / 2],
            twiddles: (0..WINDOW / 2).map(|i| {
                (-std::f32::consts::TAU * i as f32 / WINDOW as f32).sin_cos()
            }).collect(),
        }
    }

    fn spectrum(&mut self, samples: &VecDeque<f32>) -> &[f32] {
        let hann = &*HANN_TABLE;
        let reverse = &*BIT_REV_TABLE;
        for (i, &sample) in samples.iter().enumerate() {
            self.real[reverse[i]] = sample * hann[i];
        }
        self.imag.fill(0.0);
        let mut size = 2;
        while size <= WINDOW {
            let step = WINDOW / size;
            for base in (0..WINDOW).step_by(size) {
                for k in 0..size / 2 {
                    let (im, re) = self.twiddles[k * step];
                    let a = base + k;
                    let b = a + size / 2;
                    let tr = re * self.real[b] - im * self.imag[b];
                    let ti = re * self.imag[b] + im * self.real[b];
                    self.real[b] = self.real[a] - tr;
                    self.imag[b] = self.imag[a] - ti;
                    self.real[a] += tr;
                    self.imag[a] += ti;
                }
            }
            size *= 2;
        }
        for (i, bin) in self.magnitudes.iter_mut().enumerate() {
            let (re, im) = (self.real[i], self.imag[i]);
            *bin = (re * re + im * im).sqrt() * (2.0 / WINDOW as f32);
        }
        &self.magnitudes
    }
}

#[cfg(test)]
fn spectrum(samples: &VecDeque<f32>) -> Vec<f32> {
    Fft::new().spectrum(samples).to_vec()
}

fn median(values: impl Iterator<Item = f32>) -> f32 {
    let mut storage = [0.0f32; 256];
    let mut count = 0;
    for value in values.take(storage.len()) {
        storage[count] = value;
        count += 1;
    }
    if count == 0 {
        return 0.0;
    }
    let sorted = &mut storage[..count];
    *sorted.select_nth_unstable_by(count / 2, f32::total_cmp).1
}

#[derive(Clone, Copy)]
struct PendingBandPeak {
    raw_ratio: f32,
    strength: f32,
    stamp: f64,
    energy: f32,
}

struct Detector {
    ref_frames: [(f64, Vec<f32>); 4],
    ref_count: usize,
    ref_next: usize,
    bins: usize,
    log: Vec<f32>,
    history: [VecDeque<f32>; 3],
    last_hit: [f64; 3],
    armed: [bool; 3],
    release_since: [Option<f64>; 3],
    attack_energy: [f32; 3],
    pending: [Option<PendingBandPeak>; 3],
    last_time: f64,
    history_time: Option<f64>,
    drums: PercussionDetector,
}

impl Detector {
    fn new() -> Self {
        Self {
            ref_frames: std::array::from_fn(|_| (0.0, vec![0.0; MAX_BINS])),
            ref_count: 0, ref_next: 0, bins: 0, log: vec![0.0; MAX_BINS],
            history: std::array::from_fn(|_| VecDeque::with_capacity(221)),
            last_hit: [-1000.0; 3],
            armed: [true; 3],
            release_since: [None; 3],
            attack_energy: [0.0; 3],
            pending: [None; 3],
            last_time: -1000.0,
            history_time: None,
            drums: PercussionDetector::new(),
        }
    }

    fn reset(&mut self) {
        self.ref_count = 0;
        self.ref_next = 0;
        self.bins = 0;
        for history in &mut self.history { history.clear(); }
        self.last_hit = [-1000.0; 3];
        self.armed = [true; 3];
        self.release_since = [None; 3];
        self.attack_energy = [0.0; 3];
        self.pending = [None; 3];
        self.last_time = -1000.0;
        self.history_time = None;
        self.drums.reset();
    }

    #[cfg(test)]
    fn process(&mut self, input: &[f32], rate: u32, time: f64) -> Vec<OnsetEvent> {
        let mut events = Vec::new();
        self.process_into(input, rate, time, &mut events);
        events
    }

    fn process_into(&mut self, input: &[f32], rate: u32, time: f64, events: &mut Vec<OnsetEvent>) {
        if !(16..=MAX_BINS).contains(&input.len()) || rate == 0 { return; }
        if time - self.last_time > 500.0 || time < self.last_time || self.bins != input.len() {
            self.reset();
            self.bins = input.len();
            self.ref_frames[0].0 = time - 10.67;
            self.ref_frames[0].1[..self.bins].fill(0.0);
            self.ref_count = 1;
            self.ref_next = 1;
        }
        self.last_time = time;
        let log = &mut self.log[..self.bins];
        for (out, &sample) in log.iter_mut().zip(input) {
            *out = (1.0 + 100.0 * sample.max(0.0)).ln();
        }
        let oldest = (self.ref_next + 4 - self.ref_count) % 4;
        let reference = (0..self.ref_count).rev()
            .map(|i| (oldest + i) % 4)
            .find(|&i| time - self.ref_frames[i].0 >= 8.5)
            .unwrap_or(oldest);
        let ref_spec = &self.ref_frames[reference].1[..self.bins];

        let mut flux = [0.0f32; 3];
        let mut energy = [0.0f32; 3];
        let mut counts = [0usize; 3];
        {
            for k in 1..input.len() - 1 {
                let hz = k as f32 * rate as f32 / (input.len() * 2) as f32;
                if !(22.0..12500.0).contains(&hz) {
                    continue;
                }
                let band = if hz < 250.0 {
                    0
                } else if hz < 2000.0 {
                    1
                } else {
                    2
                };
                // Frequency-axis max reference (SuperFlux) suppresses vibrato bin drift.
                let reference = ref_spec[k - 1].max(ref_spec[k]).max(ref_spec[k + 1]);
                let rise = (log[k] - reference).max(0.0);
                flux[band] += rise;
                energy[band] += input[k] * input[k];
                counts[band] += 1;
            }
        }

        let sens = sensitivity_norm();
        let sens_scale = 1.45 - 0.85 * sens;
        let mad_k = [2.2f32, 2.4, 4.0];
        let min_thresh = [0.0048f32, 0.0058, 0.0115];
        let band_holdoff = [92.0f64, 96.0, 110.0];

        for band in 0..3 {
            if time - self.last_hit[band] < 65.0 {
                self.attack_energy[band] = self.attack_energy[band].max(energy[band]);
            }
            flux[band] /= counts[band].max(1) as f32;
            let floor = median(self.history[band].iter().copied());
            let mad = median(self.history[band].iter().map(|value| (value - floor).abs()));
            let threshold =
                (floor + mad_k[band] * sens_scale * mad).max(min_thresh[band] * sens_scale);
            let elapsed = (time - self.last_hit[band]).max(0.0);
            let tail_floor = self.attack_energy[band] * 0.07 * (-elapsed / 240.0).exp() as f32;

            if !self.armed[band] {
                let released = flux[band] < threshold * 0.72;
                if released {
                    let since = self.release_since[band].get_or_insert(time);
                    if time - *since >= 14.0 {
                        self.armed[band] = true;
                    }
                } else if elapsed >= 210.0 {
                    self.armed[band] = true;
                } else {
                    self.release_since[band] = None;
                }
            }

            let raw_ratio = flux[band] / threshold.max(1e-6);
            let accepted = self.armed[band]
                && flux[band] > threshold
                && energy[band] > 0.000002f32.max(tail_floor);
            let strength = (raw_ratio / 3.4).clamp(0.18, 1.0);

            if let Some(prev) = self.pending[band] {
                if !self.armed[band] {
                    self.pending[band] = None;
                } else if accepted && raw_ratio >= prev.raw_ratio && time - prev.stamp <= 24.0 {
                    self.pending[band] = Some(PendingBandPeak {
                        raw_ratio,
                        strength: strength.max(prev.strength),
                        stamp: prev.stamp,
                        energy: energy[band].max(prev.energy),
                    });
                } else {
                    if prev.stamp - self.last_hit[band] >= band_holdoff[band] {
                        self.last_hit[band] = prev.stamp;
                        self.armed[band] = false;
                        self.release_since[band] = None;
                        self.attack_energy[band] = prev.energy;
                        events.push(OnsetEvent {
                            band,
                            strength: prev.strength,
                            percussion: false,
                            time_ms: prev.stamp,
                        });
                    }
                    self.pending[band] = None;
                }
            } else if accepted {
                self.pending[band] = Some(PendingBandPeak {
                    raw_ratio,
                    strength,
                    stamp: time,
                    energy: energy[band],
                });
            }
        }

        let hop_ms = self
            .history_time
            .map(|prev| (time - prev).clamp(4.0, 100.0))
            .unwrap_or(10.67);
        self.history_time = Some(time);
        let limit = ((1000.0 / hop_ms) as usize).clamp(16, 220);
        for band in 0..3 {
            self.history[band].push_back(flux[band]);
            while self.history[band].len() > limit {
                self.history[band].pop_front();
            }
        }

        if let Some(drum) = self.drums.process(input, log, ref_spec, rate, time) {
            events.push(drum);
        }
        self.ref_frames[self.ref_next].0 = time;
        self.ref_frames[self.ref_next].1[..self.bins].copy_from_slice(log);
        self.ref_next = (self.ref_next + 1) % 4;
        self.ref_count = (self.ref_count + 1).min(4);
    }
}

// Causal low-latency multi-lane percussion detector (Kick / 808 / Tom / Snare / Clap /
// Rimshot / Woodblock / Accented Percussion) with <= 10.7 ms confirmation latency.
#[derive(Clone, Copy, Default)]
struct PercFrame {
    total_power: f32,
    sub_power: f32,
    tom_power: f32,
    body_power: f32,
    crack_power: f32,
    air_power: f32,
    sub_flux: f32,
    tom_flux: f32,
    body_flux: f32,
    crack_flux: f32,
    air_flux: f32,
    sub_centroid: f32,
    sub_peak_hz: f32,
    broadband_centroid: f32,
    mid_flatness: f32,
    active_mid_ratio: f32,
    active_high_ratio: f32,
}

impl PercFrame {
    fn extract(input: &[f32], log: &[f32], ref_log: &[f32], rate: u32) -> Self {
        let mut f = Self::default();
        let (mut sub_w, mut sub_c) = (0.0f32, 0.0f32);
        let (mut best_sub_k, mut best_sub_mag) = (0usize, 0.0f32);
        let (mut sub_n, mut tom_n, mut body_n, mut crack_n, mut air_n) = (0usize, 0, 0, 0, 0);
        let (mut flat_log_sum, mut flat_pow_sum, mut flat_bins) = (0.0f32, 0.0f32, 0usize);
        let (mut mid_rising, mut mid_total) = (0usize, 0usize);
        let (mut high_rising, mut high_total) = (0usize, 0usize);

        let len = input.len().min(log.len()).min(ref_log.len());
        let hz_per_bin = rate as f32 / (input.len() * 2).max(1) as f32;
        for k in 1..len.saturating_sub(1) {
            let hz = k as f32 * hz_per_bin;
            if !(22.0..12000.0).contains(&hz) {
                continue;
            }
            let mag = input[k].max(0.0);
            let power = mag * mag;
            let reference = ref_log[k - 1].max(ref_log[k]).max(ref_log[k + 1]);
            let rise = (log[k] - reference).max(0.0);

            f.total_power += power;
            f.broadband_centroid += power * hz;

            if hz < 165.0 {
                f.sub_power += power;
                f.sub_flux += rise;
                sub_n += 1;
            }
            if hz < 210.0 {
                sub_w += power;
                sub_c += power * hz;
                if mag > best_sub_mag {
                    best_sub_mag = mag;
                    best_sub_k = k;
                }
            }
            if (115.0..340.0).contains(&hz) {
                f.tom_power += power;
                f.tom_flux += rise;
                tom_n += 1;
            }
            if (185.0..850.0).contains(&hz) {
                f.body_power += power;
                f.body_flux += rise;
                body_n += 1;
            }
            if (850.0..3800.0).contains(&hz) {
                f.crack_power += power;
                f.crack_flux += rise;
                crack_n += 1;
            }
            if hz >= 3800.0 {
                f.air_power += power;
                f.air_flux += rise;
                air_n += 1;
                high_total += 1;
                if rise > 0.085 {
                    high_rising += 1;
                }
            }
            if (320.0..7500.0).contains(&hz) {
                flat_log_sum += (power + 1e-14).ln();
                flat_pow_sum += power;
                flat_bins += 1;
                mid_total += 1;
                if rise > 0.075 {
                    mid_rising += 1;
                }
            }
        }

        f.sub_flux /= sub_n.max(1) as f32;
        f.tom_flux /= tom_n.max(1) as f32;
        f.body_flux /= body_n.max(1) as f32;
        f.crack_flux /= crack_n.max(1) as f32;
        f.air_flux /= air_n.max(1) as f32;
        f.sub_centroid = sub_c / sub_w.max(1e-14);
        f.broadband_centroid /= f.total_power.max(1e-14);
        if best_sub_k > 0 && best_sub_k + 1 < len {
            let a = input[best_sub_k - 1].max(0.0);
            let b = input[best_sub_k].max(0.0);
            let c = input[best_sub_k + 1].max(0.0);
            let denom = (2.0 * b - a - c).max(1e-6);
            let delta = (0.5 * (c - a) / denom).clamp(-0.5, 0.5);
            f.sub_peak_hz = (best_sub_k as f32 + delta) * hz_per_bin;
        }
        if flat_bins > 0 {
            f.mid_flatness =
                (flat_log_sum / flat_bins as f32).exp() / (flat_pow_sum / flat_bins as f32 + 1e-14);
        }
        if mid_total > 0 {
            f.active_mid_ratio = mid_rising as f32 / mid_total as f32;
        }
        if high_total > 0 {
            f.active_high_ratio = high_rising as f32 / high_total as f32;
        }
        f
    }
}

#[derive(Clone, Copy)]
struct PendingKick {
    at: f64,
    init_peak_hz: f32,
    hop1_peak_hz: f32,
    peak_sub: f32,
    peak_upper: f32,
    peak_high: f32,
    peak_flux: f32,
    sustained_beater: bool,
    mixed_attack_at: Option<f64>,
    hops: u8,
}

#[derive(Clone, Copy)]
struct PendingImpact {
    at: f64,
    score: f32,
    strength: f32,
    sub_power: f32,
    perc_power: f32,
    is_kick: bool,
    wait_hops: u8,
}

struct PercussionDetector {
    frames_seen: usize,
    sub_flux_hist: VecDeque<f32>,
    sub_pow_hist: VecDeque<f32>,
    perc_flux_hist: VecDeque<f32>,
    air_flux_hist: VecDeque<f32>,
    ref_powers: VecDeque<(f64, f32, f32, f32)>,
    pending_kick: Option<PendingKick>,
    pending_impact: Option<PendingImpact>,
    last_hit: f64,
    hit_sub_power: f32,
    hit_perc_power: f32,
}

impl PercussionDetector {
    fn new() -> Self {
        Self {
            frames_seen: 0,
            sub_flux_hist: VecDeque::with_capacity(96),
            sub_pow_hist: VecDeque::with_capacity(96),
            perc_flux_hist: VecDeque::with_capacity(96),
            air_flux_hist: VecDeque::with_capacity(96),
            ref_powers: VecDeque::with_capacity(6),
            pending_kick: None,
            pending_impact: None,
            last_hit: -1000.0,
            hit_sub_power: 0.0,
            hit_perc_power: 0.0,
        }
    }

    fn reset(&mut self) {
        self.frames_seen = 0;
        self.sub_flux_hist.clear();
        self.sub_pow_hist.clear();
        self.perc_flux_hist.clear();
        self.air_flux_hist.clear();
        self.ref_powers.clear();
        self.pending_kick = None;
        self.pending_impact = None;
        self.last_hit = -1000.0;
        self.hit_sub_power = 0.0;
        self.hit_perc_power = 0.0;
    }

    fn adaptive_thresh(hist: &VecDeque<f32>, k: f32, min_val: f32) -> f32 {
        let floor = median(hist.iter().copied());
        let mad = median(hist.iter().map(|&v| (v - floor).abs()));
        (floor + k * mad).max(min_val)
    }

    fn process(
        &mut self,
        input: &[f32],
        log: &[f32],
        ref_log: &[f32],
        rate: u32,
        time: f64,
    ) -> Option<OnsetEvent> {
        let f = PercFrame::extract(input, log, ref_log, rate);
        self.frames_seen = self.frames_seen.saturating_add(1);
        let sens = sensitivity_norm();
        let strict = 1.38 - 0.72 * sens; // 0%: 1.38, 55%: 0.98, 100%: 0.66
        let holdoff_ms = (98.0 - 30.0 * sens) as f64;
        let since_hit = (time - self.last_hit).max(0.0);
        let perc_power = f.body_power + f.crack_power + f.air_power;
        let high_impact_power = f.crack_power + f.air_power;

        if since_hit < 55.0 {
            self.hit_sub_power = self.hit_sub_power.max(f.sub_power);
            self.hit_perc_power = self.hit_perc_power.max(perc_power);
        }

        let sub_tail_mask = self.hit_sub_power * 0.25 * (-since_hit / 145.0).exp() as f32;
        let perc_tail_mask = self.hit_perc_power * 0.18 * (-since_hit / 140.0).exp() as f32;

        let sub_flux_th = Self::adaptive_thresh(&self.sub_flux_hist, 2.35 * strict, 0.032 * strict);
        let sub_pow_floor = median(self.sub_pow_hist.iter().copied());
        let perc_flux = 0.38 * f.body_flux + 0.62 * f.crack_flux;
        let perc_flux_th =
            Self::adaptive_thresh(&self.perc_flux_hist, 2.65 * strict, 0.020 * strict);
        let air_flux_th = Self::adaptive_thresh(&self.air_flux_hist, 3.00 * strict, 0.024 * strict);

        let (ref_sub_pow, ref_body_pow, ref_high_pow) = self
            .ref_powers
            .iter()
            .rev()
            .find(|&&(t, _, _, _)| time - t >= 8.0)
            .or_else(|| self.ref_powers.front())
            .map(|&(_, sp, bp, hp)| (sp, bp, hp))
            .unwrap_or((f.sub_power, f.body_power, high_impact_power));
        let ref_perc_pow = ref_body_pow + ref_high_pow;

        let sub_jump = (f.sub_power - ref_sub_pow).max(0.0);
        let body_jump = (f.body_power - ref_body_pow).max(0.0);
        let high_jump = (high_impact_power - ref_high_pow).max(0.0);
        let upper_jump = body_jump + high_jump;
        let mid_burst = (f.body_flux + f.crack_flux) > perc_flux_th * 1.10
            && (f.active_mid_ratio > 0.16 || f.body_flux > sub_flux_th * 0.65);

        // Distinguish a renewed attack from beating between the old kick and
        // the bass bed. A falling instantaneous power alone cannot reject it.
        let mixed_attack = since_hit < 220.0
            && sub_pow_floor > 0.002
            && mid_burst
            && upper_jump > 0.00018
            && f.sub_flux > (sub_flux_th * 1.15).max(0.18 * strict)
            && f.sub_power > sub_pow_floor * 0.78;

        let mut fired: Option<PendingImpact> = None;
        if self.frames_seen >= 3 {
            // --- Lane A: Kick / 808 / Low-Tom (causal 3-hop = 16.0 ms confirmation) ---
            if let Some(mut pk) = self.pending_kick.take() {
                pk.hops += 1;
                pk.peak_sub = pk.peak_sub.max(f.sub_power);
                pk.peak_upper = pk.peak_upper.max(upper_jump);
                pk.peak_high = pk.peak_high.max(high_jump);
                pk.peak_flux = pk.peak_flux.max(f.sub_flux);
                pk.init_peak_hz = pk.init_peak_hz.max(f.sub_centroid);
                if mixed_attack && pk.mixed_attack_at.is_none() {
                    pk.mixed_attack_at = Some(time);
                }
                if pk.hops == 1 {
                    pk.hop1_peak_hz = f.sub_peak_hz;
                }
                if mid_burst && upper_jump > f.sub_power * 0.065 {
                    pk.sustained_beater = true;
                }
                if pk.hops >= 3 {
                    let stationary_bass_note = f.sub_peak_hz < 165.0
                        && (f.sub_peak_hz - pk.hop1_peak_hz).abs() < 9.0
                        && pk.peak_high < pk.peak_sub * 0.12;
                    let pitch_swept = !stationary_bass_note
                        && pk.init_peak_hz > 86.0
                        && f.sub_power > pk.peak_sub * 0.18
                        && f.sub_centroid < pk.init_peak_hz * 0.86;
                    let beater_kick = !stationary_bass_note
                        && pk.sustained_beater
                        && pk.peak_upper > pk.peak_sub * 0.065
                        && f.sub_centroid < 165.0;
                    let renewed_attack = pk.mixed_attack_at.is_some()
                        && pk.init_peak_hz > 79.0
                        && pk.peak_upper > pk.peak_sub * 0.020;
                    if pitch_swept || beater_kick || renewed_attack {
                        let strength = (0.48
                            + (pk.peak_flux / (sub_flux_th * 3.0)) * 0.42
                            + pk.peak_sub.sqrt() * 0.35)
                            .clamp(0.46, 1.0);
                        fired = Some(PendingImpact {
                            at: pk.mixed_attack_at.filter(|_| renewed_attack).unwrap_or(pk.at),
                            score: pk.peak_flux / sub_flux_th.max(1e-6),
                            strength,
                            sub_power: pk.peak_sub,
                            perc_power: pk.peak_upper,
                            is_kick: true,
                            wait_hops: 0,
                        });
                    }
                } else {
                    self.pending_kick = Some(pk);
                }
            }

            let sub_power_rising = sub_jump > body_jump * 0.15
                && f.sub_power > ref_sub_pow * (1.24 - 0.12 * sens)
                && f.sub_power > sub_pow_floor * (1.35 - 0.28 * sens);
            let sub_surge = f.sub_flux > sub_flux_th
                && f.sub_power > 0.0012f32.max(sub_tail_mask)
                && f.sub_power > f.body_power * 0.12
                && f.sub_centroid < 178.0
                && (sub_power_rising || mixed_attack);

            if fired.is_none() && self.pending_kick.is_none() && sub_surge {
                self.pending_kick = Some(PendingKick {
                    at: time,
                    init_peak_hz: f.sub_centroid,
                    hop1_peak_hz: f.sub_peak_hz,
                    peak_sub: f.sub_power,
                    peak_upper: upper_jump,
                    peak_high: high_jump,
                    peak_flux: f.sub_flux,
                    sustained_beater: mid_burst && upper_jump > f.sub_power * 0.065,
                    mixed_attack_at: mixed_attack.then_some(time),
                    hops: 0,
                });
            }

            // --- Lane B: Snare / Clap / Rimshot / Woodblock (in-band jump, mix-immune) ---
            let perc_power_rising = (body_jump + high_jump) > 0.00018
                && (perc_power > ref_perc_pow * (1.12 - 0.06 * sens)
                    || perc_flux > perc_flux_th * 1.55);
            let snare_or_clap = perc_power_rising
                && perc_flux > perc_flux_th
                && perc_power > 0.00025f32.max(perc_tail_mask)
                && high_impact_power > f.body_power * 0.06
                && high_jump > 0.00018
                && high_jump > body_jump * 0.42
                && high_jump > sub_jump * 0.24
                && f.mid_flatness > 0.015
                && f.active_mid_ratio > (0.15 - 0.05 * sens)
                && (f.mid_flatness > (0.14 - 0.05 * sens)
                    || (f.crack_flux > perc_flux_th * 1.35 && high_jump > 0.0006))
                && (260.0..5200.0).contains(&f.broadband_centroid);

            // --- Lane C: Crisp High Percussion (Shaker / Tambourine / Rim / Hi-Hat Accent) ---
            let crisp_perc = perc_power_rising
                && f.air_flux > air_flux_th * 1.12
                && high_impact_power > f.body_power * 0.06
                && high_jump > 0.00018
                && high_impact_power > 0.00022f32.max(perc_tail_mask * 0.85)
                && high_jump > body_jump * 0.42
                && high_jump > sub_jump * 0.24
                && f.mid_flatness > 0.015
                && (f.crack_power > f.air_power * 0.08 || f.mid_flatness > 0.15)
                && f.active_high_ratio > (0.28 - 0.08 * sens)
                && (f.crack_flux > perc_flux_th * 0.55 || f.mid_flatness > 0.16);

            if snare_or_clap || crisp_perc {
                let ratio = if snare_or_clap {
                    perc_flux / perc_flux_th.max(1e-6)
                } else {
                    f.air_flux / air_flux_th.max(1e-6)
                };
                let strength =
                    (0.44 + (ratio / 3.4) * 0.42 + perc_power.sqrt() * 0.45).clamp(0.44, 1.0);
                let impact = PendingImpact {
                    at: time,
                    score: ratio,
                    strength,
                    sub_power: f.sub_power,
                    perc_power,
                    is_kick: false,
                    wait_hops: 2,
                };
                if fired.as_ref().is_none_or(|k| impact.score > k.score) {
                    fired = Some(impact);
                }
            }
        }

        // Update rolling baselines AFTER thresholding so an attack never raises its own floor.
        self.sub_flux_hist.push_back(f.sub_flux);
        self.sub_pow_hist.push_back(f.sub_power);
        self.perc_flux_hist.push_back(perc_flux);
        self.air_flux_hist.push_back(f.air_flux);
        for h in [
            &mut self.sub_flux_hist,
            &mut self.sub_pow_hist,
            &mut self.perc_flux_hist,
            &mut self.air_flux_hist,
        ] {
            while h.len() > 80 {
                h.pop_front();
            }
        }
        self.ref_powers
            .push_back((time, f.sub_power, f.body_power, high_impact_power));
        while self.ref_powers.len() > 5 {
            self.ref_powers.pop_front();
        }

        // 1..2-hop confirmation merges simultaneous kick+snare layers and rejects
        // hard step-click discontinuities on low/mid pitched notes once >=10 ms enters the window.
        let mut emitted = None;
        if let Some(mut prev) = self.pending_impact.take() {
            let step_click_artifact = !prev.is_kick && high_jump < sub_jump * 0.35;
            if !step_click_artifact {
                if let Some(cur) = fired {
                    if cur.is_kick || (cur.score > prev.score && cur.at - prev.at <= 20.0) {
                        prev = PendingImpact {
                            at: prev.at,
                            score: cur.score.max(prev.score),
                            strength: cur.strength.max(prev.strength),
                            sub_power: cur.sub_power.max(prev.sub_power),
                            perc_power: cur.perc_power.max(prev.perc_power),
                            is_kick: prev.is_kick || cur.is_kick,
                            wait_hops: if prev.is_kick || cur.is_kick {
                                0
                            } else {
                                prev.wait_hops.saturating_sub(1)
                            },
                        };
                    } else {
                        prev.wait_hops = prev.wait_hops.saturating_sub(1);
                    }
                    if prev.wait_hops > 0 {
                        self.pending_impact = Some(prev);
                    } else if prev.at - self.last_hit >= holdoff_ms {
                        self.last_hit = prev.at;
                        self.hit_sub_power = prev.sub_power;
                        self.hit_perc_power = prev.perc_power;
                        emitted = Some(OnsetEvent {
                            band: 3,
                            percussion: true,
                            time_ms: prev.at,
                            strength: prev.strength,
                        });
                    }
                } else if prev.wait_hops > 0 {
                    prev.wait_hops -= 1;
                    self.pending_impact = Some(prev);
                } else if prev.at - self.last_hit >= holdoff_ms {
                    self.last_hit = prev.at;
                    self.hit_sub_power = prev.sub_power;
                    self.hit_perc_power = prev.perc_power;
                    emitted = Some(OnsetEvent {
                        band: 3,
                        percussion: true,
                        time_ms: prev.at,
                        strength: prev.strength,
                    });
                }
            }
        } else if let Some(cur) = fired {
            if cur.at - self.last_hit >= holdoff_ms {
                self.pending_impact = Some(cur);
            }
        }

        emitted
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
                if local < 2200 {
                    (-(local as f32) / 480.0).exp()
                } else {
                    0.0
                }
            } else {
                0.4
            };
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
                        let amplitude = if hit == 0 {
                            0.8
                        } else {
                            0.12 * (-(hit as f32 - 1.0) * 0.3).exp()
                        };
                        envelope += amplitude * (-((i - start) as f32) / 480.0).exp();
                    }
                }
                window.push_back(
                    envelope * (std::f32::consts::TAU * frequency * i as f32 / 48000.0).sin(),
                );
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
        let samples = (0..WINDOW)
            .map(|i| (std::f32::consts::TAU * 93.75 * i as f32 / 48000.0).sin())
            .collect();
        let bins = spectrum(&samples);
        let index = bins
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .unwrap()
            .0;
        assert_eq!(index, 4);
    }

    #[test]
    fn band_pulses_and_fast_repeated_notes() {
        for (frequency, band) in [(94.0, 0), (750.0, 1), (6000.0, 2)] {
            let events = analyse(frequency, true, 48000);
            let selected: Vec<_> = events.iter().filter(|e| e.band == band).collect();
            assert!(
                (6..=9).contains(&selected.len()),
                "frequency {frequency}: got {} events: {selected:?}",
                selected.len()
            );
            assert!(selected
                .windows(2)
                .any(|pair| pair[1].time_ms - pair[0].time_ms < 170.0));
        }
    }

    #[test]
    fn sustained_tone_and_silence_do_not_keep_triggering() {
        assert!(
            analyse(94.0, false, 48000)
                .iter()
                .filter(|e| e.time_ms > 200.0)
                .count()
                <= 1
        );
        let mut detector = Detector::new();
        for i in 0..300 {
            assert!(detector
                .process(&vec![0.0; 1024], 48000, i as f64 * 10.67)
                .is_empty());
        }
    }

    #[test]
    fn pitched_notes_are_not_drums() {
        for hz in [70.0, 110.0, 440.0, 750.0, 6000.0] {
            let drums: Vec<_> = analyse(hz, true, 48000)
                .into_iter()
                .filter(|e| e.percussion)
                .collect();
            assert_eq!(drums.len(), 0, "{hz}Hz falsely triggered {}: {drums:?}", drums.len());
        }
    }

    #[test]
    fn real_kick_and_snare_emit_one_event_per_hit_with_tight_timing() {
        let mut detector = Detector::new();
        let mut window = VecDeque::new();
        let mut events = Vec::new();
        let mut rng = 0x1234_5678u32;
        let expected_ms = [150.0, 650.0, 1150.0, 1650.0];
        let mut phase = 0.0f32;
        for i in 0..96000 {
            let t = i as f32 / 48000.0;
            let mut x = 0.0f32;
            for &(hit_t, is_kick) in &[(0.15, true), (0.65, false), (1.15, true), (1.65, false)] {
                for &(dt_off, gain) in &[(0.0f32, 1.0f32), (0.14, 0.18)] {
                    let lt = t - (hit_t + dt_off);
                    if (0.0..0.16).contains(&lt) {
                        if is_kick {
                            let f = 54.0 + 105.0 * (-lt / 0.028).exp();
                            phase += std::f32::consts::TAU * f / 48000.0;
                            let env = (-lt / 0.045).exp();
                            x += gain * 0.85 * env * phase.sin();
                        } else {
                            rng = rng.wrapping_mul(1664525).wrapping_add(1013904223);
                            let noise =
                                ((rng >> 9) as f32 / ((1u32 << 23) as f32)) * 2.0 - 1.0;
                            let env = (-lt / 0.032).exp();
                            let body =
                                (std::f32::consts::TAU * 210.0 * lt).sin() * (-lt / 0.025).exp();
                            x += gain * (0.55 * noise * env + 0.35 * body);
                        }
                    }
                }
            }
            window.push_back(x);
            if window.len() == WINDOW {
                let stamp = i as f64 / 48.0 - (WINDOW * 3 / 8) as f64 / 48.0;
                events.extend(detector.process(&spectrum(&window), 48000, stamp));
                window.drain(..HOP);
            }
        }
        let drums: Vec<_> = events
            .iter()
            .filter(|e| e.percussion && e.band == 3)
            .collect();
        assert_eq!(drums.len(), 4, "Expected exactly 4 drum hits, got {drums:?}");
        for (ev, &target) in drums.iter().zip(&expected_ms) {
            assert!(
                (ev.time_ms - target).abs() <= 16.0,
                "Drum timestamp {} drifted too far from actual hit {}ms",
                ev.time_ms,
                target
            );
        }
    }

    #[test]
    fn kicks_and_percussion_inside_loud_vocal_and_bass_mix_are_detected() {
        let mut detector = Detector::new();
        let mut window = VecDeque::new();
        let mut events = Vec::new();
        let mut phase = 0.0f32;
        let mut rng = 0x9E37_79B9u32;
        let schedule = [
            (0.20f32, 0u8), // deep 44Hz 808 kick
            (0.55, 1),      // sustained compressed pop kick
            (0.90, 2),      // crisp clap / woodblock percussion
            (1.25, 1),      // syncopated kick 1
            (1.362, 1),     // syncopated kick 2 (112 ms later)
        ];
        for i in 0..96000 {
            let t = i as f32 / 48000.0;
            let mix_bed = 0.24 * (std::f32::consts::TAU * 82.4 * t).sin()
                + 0.40 * (std::f32::consts::TAU * 330.0 * t).sin()
                + 0.28 * (std::f32::consts::TAU * 554.0 * t).sin()
                + 0.20 * (std::f32::consts::TAU * 880.0 * t).sin();
            let mut perc = 0.0f32;
            for &(hit_t, kind) in &schedule {
                let lt = t - hit_t;
                if (0.0..0.42).contains(&lt) {
                    match kind {
                        0 => {
                            let f = 44.0 + 68.0 * (-lt / 0.022).exp();
                            phase += std::f32::consts::TAU * f / 48000.0;
                            perc += 0.68 * (-lt / 0.075).exp() * phase.sin();
                        }
                        1 => {
                            let f = 52.0 + 78.0 * (-lt / 0.024).exp();
                            phase += std::f32::consts::TAU * f / 48000.0;
                            let env = (-lt / 0.110).exp();
                            let click =
                                0.18 * (std::f32::consts::TAU * 260.0 * lt).sin() * (-lt / 0.009).exp();
                            perc += 0.64 * env * phase.sin() + click;
                        }
                        _ => {
                            rng = rng.wrapping_mul(1664525).wrapping_add(1013904223);
                            let noise =
                                ((rng >> 9) as f32 / ((1u32 << 23) as f32)) * 2.0 - 1.0;
                            let wood = (std::f32::consts::TAU * 1450.0 * lt).sin()
                                + 0.6 * (std::f32::consts::TAU * 2320.0 * lt).sin();
                            perc += (0.45 * noise + 0.35 * wood) * (-lt / 0.016).exp();
                        }
                    }
                }
            }
            window.push_back(mix_bed + perc);
            if window.len() == WINDOW {
                let stamp = i as f64 / 48.0 - (WINDOW * 3 / 8) as f64 / 48.0;
                events.extend(detector.process(&spectrum(&window), 48000, stamp));
                window.drain(..HOP);
            }
        }
        let drums: Vec<_> = events
            .iter()
            .filter(|e| e.percussion && e.band == 3)
            .collect();
        assert_eq!(
            drums.len(),
            schedule.len(),
            "Expected all {} percussion hits under bass+vocal mix, got {drums:?}",
            schedule.len()
        );
    }

    #[test]
    fn mixed_double_kicks_are_kept_and_quieter_echoes_are_rejected() {
        fn detect(schedule: &[(f32, f32)]) -> Vec<f64> {
            let mut detector = Detector::new();
            let mut window = VecDeque::new();
            let mut times = Vec::new();
            let tau = std::f32::consts::TAU;
            for i in 0..57600 {
                let t = i as f32 / 48000.0;
                let bed = 0.24 * (tau * 82.4 * t).sin()
                    + 0.40 * (tau * 330.0 * t).sin()
                    + 0.28 * (tau * 554.0 * t).sin()
                    + 0.20 * (tau * 880.0 * t).sin();
                let drum = schedule.iter().map(|&(start, amplitude)| {
                    let dt = t - start;
                    if !(0.0..0.42).contains(&dt) {
                        return 0.0;
                    }
                    let phase = tau * (52.0 * dt + 78.0 * 0.024 * (1.0 - (-dt / 0.024).exp()));
                    amplitude * (0.64 * (-dt / 0.11).exp() * phase.sin()
                        + 0.18 * (tau * 260.0 * dt).sin() * (-dt / 0.009).exp())
                }).sum::<f32>();
                window.push_back(bed + drum);
                if window.len() == WINDOW {
                    let stamp = i as f64 / 48.0 - (WINDOW * 3 / 8) as f64 / 48.0;
                    times.extend(detector.process(&spectrum(&window), 48000, stamp)
                        .into_iter().filter(|event| event.percussion).map(|event| event.time_ms));
                    window.drain(..HOP);
                }
            }
            times
        }

        for interval in [90.0, 112.0, 125.0, 140.0] {
            let times = detect(&[(0.42, 1.0), (0.42 + interval as f32 / 1000.0, 1.0)]);
            assert_eq!(times.len(), 2, "{interval} ms: {times:?}");
            for (actual, expected) in times.iter().zip([420.0, 420.0 + interval]) {
                assert!((actual - expected).abs() <= 15.0, "{interval} ms: {times:?}");
            }
        }
        let times = detect(&[(0.42, 1.0), (0.475, 0.35), (0.53, 0.22)]);
        assert_eq!(times.len(), 1, "echoes: {times:?}");
        assert!((times[0] - 420.0).abs() <= 15.0, "{times:?}");
    }

    #[test]
    fn tap_output_and_poll_preserve_acoustic_speaker_time_within_12ms() {
        set_capture(true);
        let _ = poll(false);
        set_sensitivity(55);
        let latency_ms = 65.0;
        let base_ms = now_ms();
        let mut phase = 0.0f32;
        let mut collected = Vec::new();
        for block in 0..80 {
            let mut frames = [(0.0f32, 0.0f32); 240];
            for (j, sample) in frames.iter_mut().enumerate() {
                let idx = block * 240 + j;
                let t = idx as f32 / 48000.0;
                let lt = t - 0.180;
                let v = if (0.0..0.14).contains(&lt) {
                    let f = 52.0 + 96.0 * (-lt / 0.025).exp();
                    phase += std::f32::consts::TAU * f / 48000.0;
                    let click =
                        0.18 * (std::f32::consts::TAU * 280.0 * lt).sin() * (-lt / 0.010).exp();
                    0.75 * (-lt / 0.055).exp() * phase.sin() + click
                } else {
                    0.0
                };
                *sample = (v, v);
            }
            let synthetic_latency = (base_ms + block as f64 * 5.0 + latency_ms) - now_ms();
            tap_output(&frames, 48000, synthetic_latency);
            collected.extend(poll(true));
        }
        let drum = collected
            .iter()
            .find(|e| e.percussion && e.band == 3)
            .expect("expected kick onset from tap_output -> poll");
        let expected_speaker_ms = base_ms + 180.0 + latency_ms;
        assert!(
            (drum.time_ms - expected_speaker_ms).abs() <= 12.0,
            "Expected speaker time ~{expected_speaker_ms:.1}ms, got {:.1}ms",
            drum.time_ms
        );
    }
}
