// SPDX-License-Identifier: AGPL-3.0-only
use fusionplay_miplay_sdk::{
    begin_external_output_stream, report_external_output_pcm_stream, reset_live_output_peak,
};
use rodio::{
    ChannelCount, Decoder, DeviceSinkBuilder, MixerDeviceSink, Player, SampleRate, Source,
};
use serde_json::{Value, json};
use socket2::{Domain, Protocol, Socket, Type};
use std::{
    collections::{HashSet, VecDeque},
    io::{self, Read, Seek, SeekFrom, Write},
    net::{IpAddr, Ipv4Addr, SocketAddr, SocketAddrV4, TcpListener, TcpStream, ToSocketAddrs, UdpSocket},
    num::NonZero,
    sync::{
        Arc, Condvar, Mutex, Weak,
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};
use ureq::unversioned::{
    resolver::{ResolvedSocketAddrs, Resolver},
    transport::{DefaultConnector, NextTimeout},
};

const GROUP: Ipv4Addr = Ipv4Addr::new(239, 255, 255, 250);
const DEVICE: &str = "urn:schemas-upnp-org:device:MediaRenderer:1";
const MAX_MEDIA_BYTES: usize = 128 * 1024 * 1024;
const PREBUFFER_BYTES: usize = 128 * 1024;
const MAX_HTTP_CLIENTS: usize = 8;
const MAX_MEDIA_DOWNLOADS: usize = 2;
const MAX_REDIRECTS: usize = 3;
static ACTIVE_DNS: AtomicUsize = AtomicUsize::new(0);

const SERVICES: &[(&str, &[&str])] = &[
    (
        "AVTransport",
        &[
            "SetAVTransportURI",
            "Play",
            "Pause",
            "Stop",
            "Seek",
            "GetTransportInfo",
            "GetPositionInfo",
            "GetMediaInfo",
            "GetCurrentTransportActions",
        ],
    ),
    (
        "RenderingControl",
        &["GetVolume", "SetVolume", "GetMute", "SetMute"],
    ),
    (
        "ConnectionManager",
        &[
            "GetProtocolInfo",
            "GetCurrentConnectionIDs",
            "GetCurrentConnectionInfo",
        ],
    ),
];
const PROTOCOLS: &str = "http-get:*:audio/mpeg:*,http-get:*:audio/flac:*,http-get:*:audio/mp4:*,http-get:*:audio/aac:*,http-get:*:audio/wav:*";

#[derive(Default)]
struct Status {
    events: VecDeque<Value>,
    active: bool,
    blocked: bool,
    block_generation: u64,
    playback_stream: Option<Weak<SharedStream>>,
}

impl Status {
    fn emit(&mut self, event: Value) {
        if self.events.len() == 32 {
            self.events.pop_front();
        }
        self.events.push_back(event);
    }
}

enum WorkerCmd {
    Shutdown,
    SetBlocked {
        blocked: bool,
        generation: u64,
        ack: mpsc::SyncSender<()>,
    },
    LocalAction {
        action: String,
        value: i32,
    },
    Soap(SoapJob),
}

struct SoapJob {
    service: String,
    actions: &'static [&'static str],
    body: String,
    reply: mpsc::SyncSender<Result<String, u16>>,
}

pub struct DlnaRenderer {
    status: Arc<Mutex<Status>>,
    cmd_tx: mpsc::SyncSender<WorkerCmd>,
    worker: Option<JoinHandle<()>>,
}

impl DlnaRenderer {
    pub fn start(config: &Value) -> Result<Self, String> {
        let ip: Ipv4Addr = config["ip"]
            .as_str()
            .unwrap_or_default()
            .parse()
            .map_err(|_| "无效的 DLNA IPv4")?;
        if ip.is_loopback() || ip.is_unspecified() || ip.is_link_local() || ip.is_multicast() {
            return Err("无效的 DLNA 局域网绑定地址".into());
        }
        let socket = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP))
            .map_err(|e| e.to_string())?;
        socket.set_reuse_address(true).map_err(|e| e.to_string())?;
        socket
            .bind(&SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 1900).into())
            .map_err(|e| format!("DLNA UDP 1900: {e}"))?;
        socket
            .join_multicast_v4(&GROUP, &ip)
            .map_err(|e| format!("DLNA SSDP: {e}"))?;
        socket.set_multicast_if_v4(&ip).map_err(|e| e.to_string())?;
        socket.set_nonblocking(true).map_err(|e| e.to_string())?;
        let udp: UdpSocket = socket.into();
        let http = TcpListener::bind(SocketAddrV4::new(ip, 8901))
            .map_err(|e| format!("DLNA TCP 8901: {e}"))?;
        http.set_nonblocking(true).map_err(|e| e.to_string())?;
        let identity = format!("uuid:{}", config["uuid"].as_str().unwrap_or_default());
        let name = config["name"].as_str().unwrap_or("SoundFlow").to_owned();
        let initial_volume = config["volume"].as_u64().unwrap_or(45).min(100) as u8;
        let (cmd_tx, cmd_rx) = mpsc::sync_channel::<WorkerCmd>(32);
        let status = Arc::new(Mutex::new(Status::default()));
        let shared = Arc::clone(&status);
        let active_http = Arc::new(AtomicUsize::new(0));
        let active_downloads = Arc::new(AtomicUsize::new(0));
        let xml = Arc::new(description(&identity, &name));
        let http_cmd_tx = cmd_tx.clone();

        let worker = thread::Builder::new()
            .name("soundflow-dlna".into())
            .spawn(move || {
                let location = format!("http://{ip}:8901/device.xml");
                let targets = std::iter::once(identity.clone())
                    .chain(["upnp:rootdevice".to_owned(), DEVICE.to_owned()])
                    .chain(SERVICES.iter().map(|(name, _)| service_type(name)))
                    .collect::<Vec<_>>();
                let mut media = Media {
                    volume: initial_volume,
                    local_ip: ip,
                    active_downloads,
                    ..Media::default()
                };
                let mut buffer = [0_u8; 4096];
                let mut next_alive = Instant::now();
                let mut running = true;
                while running {
                    if Instant::now() >= next_alive {
                        announce(&udp, &targets, &identity, &location, "ssdp:alive");
                        next_alive = Instant::now() + Duration::from_secs(120);
                    }
                    for _ in 0..16 {
                        let Ok((count, peer)) = udp.recv_from(&mut buffer) else {
                            break;
                        };
                        let Ok(text) = std::str::from_utf8(&buffer[..count]) else {
                            continue;
                        };
                        let Some(st) = search_target(text) else {
                            continue;
                        };
                        for target in targets.iter().filter(|target| {
                            st.eq_ignore_ascii_case("ssdp:all") || target.eq_ignore_ascii_case(st)
                        }) {
                            let usn = usn(&identity, target);
                            let response = format!(
                                "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=300\r\nEXT:\r\nLOCATION: {location}\r\nSERVER: SoundFlow UPnP/1.0\r\nST: {target}\r\nUSN: {usn}\r\n\r\n"
                            );
                            let _ = udp.send_to(response.as_bytes(), peer);
                        }
                    }

                    // Accept HTTP connections onto bounded short-lived workers so slow/stalled
                    // clients never block SSDP discovery or media playback state updates.
                    for _ in 0..8 {
                        let Ok((stream, _)) = http.accept() else {
                            break;
                        };
                        if active_http.fetch_add(1, Ordering::AcqRel) >= MAX_HTTP_CLIENTS {
                            active_http.fetch_sub(1, Ordering::AcqRel);
                            drop(stream);
                            continue;
                        }
                        let xml_ref = Arc::clone(&xml);
                        let tx_ref = http_cmd_tx.clone();
                        let counter = Arc::clone(&active_http);
                        if thread::Builder::new()
                            .name("soundflow-dlna-http".into())
                            .spawn(move || {
                                let _guard = CounterGuard(counter);
                                handle_http_worker(stream, &xml_ref, &tx_ref);
                            })
                            .is_err()
                        {
                            active_http.fetch_sub(1, Ordering::AcqRel);
                        }
                    }

                    let process_cmd = |cmd: WorkerCmd, media: &mut Media| {
                        match cmd {
                            WorkerCmd::Shutdown => return false,
                            WorkerCmd::SetBlocked { blocked, generation, ack } => {
                                {
                                    let mut st = shared.lock().unwrap();
                                    if st.block_generation != generation {
                                        return true;
                                    }
                                    st.blocked = blocked;
                                }
                                if blocked {
                                    media.silence_for_block(&shared);
                                }
                                let _ = ack.send(());
                            }
                            WorkerCmd::LocalAction { action, value } => {
                                let blocked = shared.lock().unwrap().blocked;
                                match action.as_str() {
                                    "pause" => media.pause(&shared),
                                    "resume" if !blocked => {
                                        let _ = media.play(&shared, blocked);
                                    }
                                    "volume" => {
                                        media.volume = value.clamp(0, 100) as u8;
                                        media.apply_volume(blocked);
                                    }
                                    _ => {}
                                }
                            }
                            WorkerCmd::Soap(job) => {
                                let blocked_now = shared.lock().unwrap().blocked;
                                let res = soap(
                                    &job.service,
                                    job.actions,
                                    &job.body,
                                    media,
                                    &shared,
                                    blocked_now,
                                );
                                let _ = job.reply.send(res);
                            }
                        }
                        true
                    };

                    while running {
                        match cmd_rx.try_recv() {
                            Ok(cmd) => running = process_cmd(cmd, &mut media),
                            Err(_) => break,
                        }
                    }
                    if !running {
                        break;
                    }
                    media.tick(&shared);
                    if let Ok(cmd) = cmd_rx.recv_timeout(Duration::from_millis(12)) {
                        running = process_cmd(cmd, &mut media);
                        while running {
                            match cmd_rx.try_recv() {
                                Ok(next) => running = process_cmd(next, &mut media),
                                Err(_) => break,
                            }
                        }
                    }
                }
                media.stop(&shared);
                announce(&udp, &targets, &identity, &location, "ssdp:byebye");
            })
            .map_err(|e| e.to_string())?;

        Ok(Self {
            status,
            cmd_tx,
            worker: Some(worker),
        })
    }

    pub fn active(&self) -> bool {
        self.status.lock().unwrap().active
    }

    /// Synchronously updates the receiver block gate and waits until the DLNA worker
    /// has paused and muted any active or pending audio output before returning.
    pub fn set_blocked(&self, blocked: bool) -> Result<(), String> {
        let generation = self.request_block_state(blocked);
        let (ack_tx, ack_rx) = mpsc::sync_channel(1);
        self.cmd_tx
            .try_send(WorkerCmd::SetBlocked { blocked, generation, ack: ack_tx })
            .map_err(|_| "DLNA 控制队列不可用".to_owned())?;
        ack_rx.recv_timeout(Duration::from_secs(2))
            .map_err(|_| "无法确认 DLNA 输出已切换".to_owned())
    }

    fn request_block_state(&self, blocked: bool) -> u64 {
        let (generation, stream) = {
            let mut state = self.status.lock().unwrap();
            state.block_generation = state.block_generation.wrapping_add(1);
            let stream = if blocked {
                state.blocked = true;
                state.playback_stream.as_ref().and_then(Weak::upgrade)
            } else {
                None
            };
            (state.block_generation, stream)
        };
        if let Some(stream) = stream {
            stream.cancel();
        }
        generation
    }

    pub fn control(&self, action: &str, value: i32) {
        let _ = self.cmd_tx.try_send(WorkerCmd::LocalAction {
            action: action.to_owned(),
            value,
        });
    }

    pub fn poll(&self) -> Vec<Value> {
        self.status.lock().unwrap().events.drain(..).collect()
    }
}

impl Drop for DlnaRenderer {
    fn drop(&mut self) {
        self.request_block_state(true);
        let _ = self.cmd_tx.send(WorkerCmd::Shutdown);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

struct CounterGuard(Arc<AtomicUsize>);
impl Drop for CounterGuard {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::AcqRel);
    }
}

#[derive(Default)]
struct StreamState {
    bytes: Vec<u8>,
    total_len: Option<usize>,
    eof: bool,
    error: Option<String>,
}

struct SharedStream {
    state: Mutex<StreamState>,
    cv: Condvar,
    cancelled: AtomicBool,
    active_agent: Mutex<Option<ureq::Agent>>,
}

impl SharedStream {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(StreamState::default()),
            cv: Condvar::new(),
            cancelled: AtomicBool::new(false),
            active_agent: Mutex::new(None),
        })
    }

    fn set_agent(&self, agent: Option<ureq::Agent>) {
        if self.cancelled.load(Ordering::Acquire) {
            if let Some(a) = agent {
                a.shutdown();
            }
            return;
        }
        let mut guard = self.active_agent.lock().unwrap();
        if self.cancelled.load(Ordering::Acquire) {
            if let Some(a) = agent {
                a.shutdown();
            }
            return;
        }
        *guard = agent;
    }

    fn cancel(&self) {
        {
            let _state = self.state.lock().unwrap();
            self.cancelled.store(true, Ordering::Release);
            self.cv.notify_all();
        }
        if let Some(agent) = self.active_agent.lock().unwrap().take() {
            agent.shutdown();
        }
    }
}

#[derive(Clone)]
struct StreamReader {
    shared: Arc<SharedStream>,
    pos: usize,
}

impl Read for StreamReader {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if out.is_empty() {
            return Ok(0);
        }
        let deadline = Instant::now() + Duration::from_secs(10);
        let mut guard = self.shared.state.lock().unwrap();
        loop {
            if self.shared.cancelled.load(Ordering::Acquire) {
                return Err(io::Error::new(io::ErrorKind::ConnectionAborted, "DLNA stream cancelled"));
            }
            if self.pos < guard.bytes.len() {
                let avail = (guard.bytes.len() - self.pos).min(out.len());
                out[..avail].copy_from_slice(&guard.bytes[self.pos..self.pos + avail]);
                self.pos += avail;
                return Ok(avail);
            }
            if let Some(err) = &guard.error {
                return Err(io::Error::other(err.clone()));
            }
            if guard.eof {
                return Ok(0);
            }
            let Some(rem) = deadline.checked_duration_since(Instant::now()) else {
                return Err(io::Error::new(io::ErrorKind::TimedOut, "DLNA stream read timed out"));
            };
            let (next_guard, wait_res) = self.shared.cv.wait_timeout(guard, rem).unwrap();
            guard = next_guard;
            if wait_res.timed_out() && self.pos >= guard.bytes.len() && !guard.eof {
                return Err(io::Error::new(io::ErrorKind::TimedOut, "DLNA stream read timed out"));
            }
        }
    }
}

impl Seek for StreamReader {
    fn seek(&mut self, style: SeekFrom) -> io::Result<u64> {
        let deadline = Instant::now() + Duration::from_secs(6);
        let mut guard = self.shared.state.lock().unwrap();
        let target: i128 = match style {
            SeekFrom::Start(off) => i128::from(off),
            SeekFrom::Current(delta) => self.pos as i128 + i128::from(delta),
            SeekFrom::End(delta) => {
                let end = if guard.eof {
                    Some(guard.bytes.len())
                } else {
                    guard.total_len
                };
                let Some(len) = end else {
                    return Err(io::Error::new(
                        io::ErrorKind::Unsupported,
                        "stream length unknown",
                    ));
                };
                len as i128 + i128::from(delta)
            }
        };
        if target < 0 || target > MAX_MEDIA_BYTES as i128 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "seek out of range"));
        }
        let target_usize = target as usize;
        while target_usize > guard.bytes.len() && !guard.eof && guard.error.is_none() {
            if self.shared.cancelled.load(Ordering::Acquire) {
                return Err(io::Error::new(io::ErrorKind::ConnectionAborted, "DLNA stream cancelled"));
            }
            let Some(rem) = deadline.checked_duration_since(Instant::now()) else {
                return Err(io::Error::new(io::ErrorKind::TimedOut, "DLNA seek timed out"));
            };
            let (next_guard, _) = self.shared.cv.wait_timeout(guard, rem).unwrap();
            guard = next_guard;
        }
        if let Some(err) = &guard.error {
            return Err(io::Error::other(err.clone()));
        }
        if guard.eof && target_usize > guard.bytes.len() {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "seek past EOF"));
        }
        self.pos = target_usize;
        Ok(self.pos as u64)
    }
}

struct MeteredSource<S> {
    inner: S,
    channels: usize,
    sample_rate: u32,
    ch_cursor: usize,
    pending_left: f32,
    block: [(f32, f32); 256],
    block_len: usize,
    lp_state: (f32, f32),
    stream_id: u64,
    discontinuity: bool,
}

impl<S: Source<Item = f32>> MeteredSource<S> {
    fn new(inner: S) -> Self {
        let channels = inner.channels().get().max(1) as usize;
        let sample_rate = inner.sample_rate().get().max(8_000);
        Self {
            inner,
            channels,
            sample_rate,
            ch_cursor: 0,
            pending_left: 0.0,
            block: [(0.0, 0.0); 256],
            block_len: 0,
            lp_state: (0.0, 0.0),
            stream_id: begin_external_output_stream(),
            discontinuity: true,
        }
    }

    fn flush_meter(&mut self) {
        if self.block_len > 0 {
            report_external_output_pcm_stream(
                &self.block[..self.block_len],
                self.sample_rate,
                35.0,
                &mut self.lp_state,
                self.stream_id,
                std::mem::replace(&mut self.discontinuity, false),
            );
            self.block_len = 0;
        }
    }
}

impl<S: Source<Item = f32>> Iterator for MeteredSource<S> {
    type Item = f32;

    fn next(&mut self) -> Option<Self::Item> {
        let Some(sample) = self.inner.next() else {
            self.flush_meter();
            return None;
        };
        let ch = self.ch_cursor;
        self.ch_cursor += 1;
        if self.ch_cursor >= self.channels {
            self.ch_cursor = 0;
        }
        if self.channels == 1 {
            self.block[self.block_len] = (sample, sample);
            self.block_len += 1;
        } else if ch == 0 {
            self.pending_left = sample;
        } else if ch == 1 {
            self.block[self.block_len] = (self.pending_left, sample);
            self.block_len += 1;
        }
        if self.block_len == self.block.len() {
            self.flush_meter();
        }
        Some(sample)
    }
}

impl<S: Source<Item = f32>> Source for MeteredSource<S> {
    fn current_span_len(&self) -> Option<usize> {
        let next_span = self.inner.current_span_len()?;
        let next_channels = self.inner.channels().get().max(1) as usize;
        let next_rate = self.inner.sample_rate().get().max(8_000);
        if next_channels != self.channels || next_rate != self.sample_rate {
            return Some(0);
        }
        Some(next_span)
    }

    fn channels(&self) -> ChannelCount {
        NonZero::new(self.channels as u16).unwrap_or(NonZero::new(2).unwrap())
    }

    fn sample_rate(&self) -> SampleRate {
        NonZero::new(self.sample_rate).unwrap_or(NonZero::new(48_000).unwrap())
    }

    fn total_duration(&self) -> Option<Duration> {
        self.inner.total_duration()
    }

    fn try_seek(&mut self, pos: Duration) -> Result<(), rodio::source::SeekError> {
        self.ch_cursor = 0;
        self.block_len = 0;
        self.discontinuity = true;
        self.inner.try_seek(pos)
    }
}

struct Media {
    player: Option<Player>,
    output: Option<MixerDeviceSink>,
    active_stream: Option<Arc<SharedStream>>,
    pending_ready: Option<mpsc::Receiver<Result<StreamReader, String>>>,
    active_downloads: Arc<AtomicUsize>,
    local_ip: Ipv4Addr,
    uri: String,
    title: String,
    artist: String,
    volume: u8,
    mute: bool,
    playing: bool,
    want_play: bool,
    duration: Duration,
    next_progress: Option<Instant>,
}

impl Default for Media {
    fn default() -> Self {
        Self {
            player: None,
            output: None,
            active_stream: None,
            pending_ready: None,
            active_downloads: Arc::new(AtomicUsize::new(0)),
            local_ip: Ipv4Addr::UNSPECIFIED,
            uri: String::new(),
            title: String::new(),
            artist: String::new(),
            volume: 45,
            mute: false,
            playing: false,
            want_play: false,
            duration: Duration::ZERO,
            next_progress: None,
        }
    }
}

impl Media {
    fn play(&mut self, status: &Mutex<Status>, blocked: bool) -> Result<(), u16> {
        if blocked || status.lock().unwrap().blocked || self.uri.is_empty() {
            return Err(701);
        }
        self.want_play = true;
        if let Some(player) = &self.player {
            self.apply_volume(false);
            player.play();
            self.set_playing(status, true);
        } else if self.pending_ready.is_none() {
            if let Some(prev) = self.active_stream.take() {
                prev.cancel();
            }
            if !acquire_download_slot(&self.active_downloads, Duration::from_millis(200)) {
                self.want_play = false;
                return Err(701);
            }
            let uri = self.uri.clone();
            let self_ip = self.local_ip;
            let shared_stream = SharedStream::new();
            {
                let mut state = status.lock().unwrap();
                if state.blocked {
                    self.active_downloads.fetch_sub(1, Ordering::AcqRel);
                    self.want_play = false;
                    return Err(701);
                }
                state.playback_stream = Some(Arc::downgrade(&shared_stream));
            }
            let stream_for_worker = Arc::clone(&shared_stream);
            let counter = Arc::clone(&self.active_downloads);
            let (tx, rx) = mpsc::sync_channel(1);
            if thread::Builder::new()
                .name("soundflow-dlna-stream".into())
                .spawn(move || {
                    let _guard = CounterGuard(counter);
                    stream_download(&uri, self_ip, stream_for_worker, tx);
                })
                .is_err()
            {
                self.active_downloads.fetch_sub(1, Ordering::AcqRel);
                self.want_play = false;
                return Err(501);
            }
            self.active_stream = Some(shared_stream);
            self.pending_ready = Some(rx);
        }
        Ok(())
    }

    fn pause(&mut self, status: &Mutex<Status>) {
        self.want_play = false;
        if let Some(player) = &self.player {
            player.pause();
        }
        reset_live_output_peak();
        self.set_playing(status, false);
    }

    fn silence_for_block(&mut self, status: &Mutex<Status>) {
        self.want_play = false;
        if let Some(s) = self.active_stream.take() {
            s.cancel();
        }
        self.pending_ready = None;
        if let Some(player) = &self.player {
            player.set_volume(0.0);
            player.pause();
        }
        self.player = None;
        self.output = None;
        reset_live_output_peak();
        self.set_playing(status, false);
    }

    fn stop(&mut self, status: &Mutex<Status>) {
        self.pause(status);
        if let Some(s) = self.active_stream.take() {
            s.cancel();
        }
        self.pending_ready = None;
        let mut state = status.lock().unwrap();
        if state.active {
            state.emit(json!({"event":"stream_stopped","protocol":"dlna"}));
        }
        state.active = false;
        self.player = None;
        self.output = None;
        self.next_progress = None;
    }

    fn set_playing(&mut self, state: &Mutex<Status>, playing: bool) {
        if playing == self.playing {
            return;
        }
        self.playing = playing;
        let mut state = state.lock().unwrap();
        state.active |= playing;
        let active = state.active;
        state.emit(json!({"event":"playback_state","protocol":"dlna","playing":playing,"session_active":active}));
    }

    fn apply_volume(&self, blocked: bool) {
        if let Some(player) = &self.player {
            player.set_volume(if self.mute || blocked {
                0.0
            } else {
                f32::from(self.volume) / 100.0
            });
        }
    }

    fn tick(&mut self, state: &Mutex<Status>) {
        let blocked_now = state.lock().unwrap().blocked;
        if blocked_now {
            if self.playing || self.want_play || self.pending_ready.is_some() || self.player.is_some() {
                self.silence_for_block(state);
            }
            return;
        }
        if let Some(result) = self.pending_ready.as_ref().and_then(|rx| rx.try_recv().ok()) {
            self.pending_ready = None;
            if self.want_play && !state.lock().unwrap().blocked {
                match result.and_then(|reader| self.install(reader, state)) {
                    Ok(()) => {
                        let mut status = state.lock().unwrap();
                        status.emit(json!({
                            "event":"media_info",
                            "protocol":"dlna",
                            "title":self.title,
                            "artist":self.artist,
                            "duration_ms":self.duration.as_millis(),
                            "position_ms":0
                        }));
                        drop(status);
                        self.set_playing(state, true);
                    }
                    Err(message) => {
                        self.want_play = false;
                        if let Some(s) = self.active_stream.take() {
                            s.cancel();
                        }
                        state
                            .lock()
                            .unwrap()
                            .emit(json!({"event":"error","protocol":"dlna","message":message}));
                    }
                }
            } else if let Some(s) = self.active_stream.take() {
                s.cancel();
            }
        }
        if self.player.as_ref().is_some_and(Player::empty) {
            self.stop(state);
        }
        if self.playing && self.next_progress.is_none_or(|next| Instant::now() >= next) {
            self.next_progress = Some(Instant::now() + Duration::from_millis(500));
            state
                .lock()
                .unwrap()
                .emit(json!({"event":"progress","protocol":"dlna","position_ms":self.position().as_millis()}));
        }
    }

    fn install(&mut self, reader: StreamReader, state: &Mutex<Status>) -> Result<(), String> {
        let decoded = Decoder::new(reader).map_err(|e| format!("DLNA 音频解码失败: {e}"))?;
        if state.lock().unwrap().blocked {
            return Err("全屋发送运行中，已阻止本机 DLNA 音频输出".into());
        }
        let output = DeviceSinkBuilder::open_default_sink().map_err(|e| e.to_string())?;
        let player = Player::connect_new(output.mixer());
        self.duration = decoded.total_duration().unwrap_or_default();
        player.append(MeteredSource::new(decoded));
        if state.lock().unwrap().blocked {
            player.pause();
            return Err("全屋发送运行中，已中止本机 DLNA 音频输出".into());
        }
        self.player = Some(player);
        self.output = Some(output);
        self.apply_volume(false);
        Ok(())
    }

    fn position(&self) -> Duration {
        self.player.as_ref().map_or(Duration::ZERO, Player::get_pos)
    }

    fn transport_state(&self) -> &str {
        if self.playing {
            "PLAYING"
        } else if self.pending_ready.is_some() && self.want_play {
            "TRANSITIONING"
        } else if self.player.is_some() {
            "PAUSED_PLAYBACK"
        } else if self.uri.is_empty() {
            "NO_MEDIA_PRESENT"
        } else {
            "STOPPED"
        }
    }
}

fn acquire_download_slot(counter: &AtomicUsize, wait_budget: Duration) -> bool {
    let deadline = Instant::now() + wait_budget;
    loop {
        let cur = counter.load(Ordering::Acquire);
        if cur < MAX_MEDIA_DOWNLOADS {
            if counter
                .compare_exchange_weak(cur, cur + 1, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
            {
                return true;
            }
            continue;
        }
        if Instant::now() >= deadline {
            return false;
        }
        thread::sleep(Duration::from_millis(8));
    }
}

#[derive(Debug, PartialEq, Eq)]
struct ParsedHttpUrl {
    scheme: String,
    host: String,
    port: u16,
}

fn normalize_ip(ip: IpAddr) -> IpAddr {
    match ip {
        IpAddr::V6(v6) => match v6.to_ipv4_mapped() {
            Some(v4) => IpAddr::V4(v4),
            None => IpAddr::V6(v6),
        },
        v4 => v4,
    }
}

fn collect_local_ips(extra: Ipv4Addr) -> HashSet<IpAddr> {
    let mut set = HashSet::new();
    if !extra.is_unspecified() {
        set.insert(IpAddr::V4(extra));
    }
    if let Ok(ifaces) = if_addrs::get_if_addrs() {
        for iface in ifaces {
            set.insert(normalize_ip(iface.ip()));
        }
    }
    set
}

fn is_private_lan_ip(ip: IpAddr) -> bool {
    match normalize_ip(ip) {
        IpAddr::V4(v4) => {
            let oct = v4.octets();
            v4.is_private() || (oct[0] == 100 && (oct[1] & 0xc0) == 64)
        }
        IpAddr::V6(v6) => (v6.segments()[0] & 0xfe00) == 0xfc00,
    }
}

fn is_allowed_media_ip_with_locals(ip: IpAddr, local_ips: &HashSet<IpAddr>) -> bool {
    let norm = normalize_ip(ip);
    if local_ips.contains(&norm) {
        return false;
    }
    match norm {
        IpAddr::V4(v4) => {
            let b0 = v4.octets()[0];
            b0 != 0
                && !v4.is_loopback()
                && !v4.is_unspecified()
                && !v4.is_link_local()
                && !v4.is_multicast()
                && !v4.is_broadcast()
        }
        IpAddr::V6(v6) => {
            let seg0 = v6.segments()[0];
            let is_link_local_v6 = (seg0 & 0xffc0) == 0xfe80;
            let is_doc_v6 = seg0 == 0x2001 && v6.segments()[1] == 0x0db8;
            !v6.is_loopback()
                && !v6.is_unspecified()
                && !v6.is_multicast()
                && !is_link_local_v6
                && !is_doc_v6
        }
    }
}

fn parse_and_validate_url_syntax(
    uri: &str,
    local_ips: &HashSet<IpAddr>,
) -> Result<ParsedHttpUrl, String> {
    if uri.is_empty() || uri.len() > 4096 {
        return Err("无效的媒体 URL 长度".into());
    }
    if uri.bytes().any(|b| b <= 0x20 || b == 0x7f) {
        return Err("媒体 URL 含非法控制字符".into());
    }
    let (scheme, rest) = if let Some(r) = uri.strip_prefix("http://") {
        ("http", r)
    } else if let Some(r) = uri.strip_prefix("https://") {
        ("https", r)
    } else {
        return Err("仅支持 HTTP/HTTPS 媒体流".into());
    };
    let authority_end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    let authority = &rest[..authority_end];
    if authority.is_empty() || authority.contains('@') || authority.contains('\\') {
        return Err("媒体 URL 包含不安全的认证或路径字符".into());
    }
    let default_port = if scheme == "https" { 443 } else { 80 };
    let (host_raw, port) = if let Some(stripped) = authority.strip_prefix('[') {
        let Some((v6_str, after)) = stripped.split_once(']') else {
            return Err("无效的 IPv6 主机格式".into());
        };
        let p = if after.is_empty() {
            default_port
        } else if let Some(p_str) = after.strip_prefix(':') {
            p_str.parse::<u16>().ok().filter(|p| *p > 0).ok_or("无效端口")?
        } else {
            return Err("无效的 IPv6 端口语法".into());
        };
        (v6_str, p)
    } else if let Some((h, p_str)) = authority.rsplit_once(':') {
        let p = p_str.parse::<u16>().ok().filter(|p| *p > 0).ok_or("无效端口")?;
        (h, p)
    } else {
        (authority, default_port)
    };

    if host_raw.is_empty() {
        return Err("媒体 URL 缺少主机名".into());
    }
    let lower = host_raw.to_ascii_lowercase();
    if lower == "localhost"
        || lower.ends_with(".localhost")
        || lower.ends_with(".localdomain")
        || lower == "ip6-localhost"
        || lower == "ip6-loopback"
    {
        return Err("禁止访问本机回环主机".into());
    }
    if let Ok(ip) = host_raw.parse::<IpAddr>()
        && !is_allowed_media_ip_with_locals(ip, local_ips)
    {
        return Err("禁止访问本机接口、回环、链路本地或多播地址".into());
    }
    Ok(ParsedHttpUrl {
        scheme: scheme.to_owned(),
        host: host_raw.to_owned(),
        port,
    })
}

fn resolve_and_pin_socket_addrs(
    uri: &str,
    local_ips: &HashSet<IpAddr>,
) -> Result<(ParsedHttpUrl, Vec<SocketAddr>), String> {
    let parsed = parse_and_validate_url_syntax(uri, local_ips)?;
    if let Ok(ip) = parsed.host.parse::<IpAddr>() {
        if !is_allowed_media_ip_with_locals(ip, local_ips) {
            return Err("禁止访问受限 IP 地址".into());
        }
        let addr = SocketAddr::new(normalize_ip(ip), parsed.port);
        return Ok((parsed, vec![addr]));
    }

    let host = parsed.host.clone();
    let port = parsed.port;
    let (tx, rx) = mpsc::sync_channel(1);
    struct DnsSlot;
    impl Drop for DnsSlot {
        fn drop(&mut self) {
            ACTIVE_DNS.fetch_sub(1, Ordering::AcqRel);
        }
    }

    if !acquire_download_slot(&ACTIVE_DNS, Duration::ZERO) {
        return Err("DLNA 媒体主机解析繁忙".into());
    }
    let slot = DnsSlot;
    thread::Builder::new()
        .name("soundflow-dlna-dns".into())
        .spawn(move || {
            let _slot = slot;
            let res = (host.as_str(), port)
                .to_socket_addrs()
                .map(|it| it.collect::<Vec<_>>());
            let _ = tx.send(res);
        })
        .map_err(|_| "无法启动媒体主机解析".to_string())?;

    let addrs = rx
        .recv_timeout(Duration::from_secs(4))
        .map_err(|_| "解析媒体服务器主机名超时".to_string())?
        .map_err(|_| "无法解析媒体服务器主机名".to_string())?;
    if addrs.is_empty() {
        return Err("媒体服务器主机无可用地址".into());
    }
    let mut verified = Vec::with_capacity(addrs.len());
    for sa in addrs {
        let norm = normalize_ip(sa.ip());
        if !is_allowed_media_ip_with_locals(norm, local_ips) {
            return Err("媒体主机解析到了受限的本机、回环或链路本地地址".into());
        }
        verified.push(SocketAddr::new(norm, sa.port()));
    }
    Ok((parsed, verified))
}

#[derive(Debug)]
struct PinnedResolver {
    expected_host: String,
    expected_port: u16,
    addrs: Vec<SocketAddr>,
}

impl Resolver for PinnedResolver {
    fn resolve(
        &self,
        uri: &ureq::http::Uri,
        _config: &ureq::config::Config,
        _timeout: NextTimeout,
    ) -> Result<ResolvedSocketAddrs, ureq::Error> {
        let req_host = uri
            .host()
            .unwrap_or_default()
            .trim_start_matches('[')
            .trim_end_matches(']');
        let default_port = if uri.scheme_str() == Some("https") { 443 } else { 80 };
        let req_port = uri.port_u16().unwrap_or(default_port);
        if !req_host.eq_ignore_ascii_case(&self.expected_host)
            || req_port != self.expected_port
            || self.addrs.is_empty()
        {
            return Err(ureq::Error::Io(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "unverified redirect or host mismatch",
            )));
        }
        let mut out = self.empty();
        for &sa in &self.addrs {
            out.push(sa);
        }
        Ok(out)
    }
}

fn resolve_redirect_url(base_uri: &str, location: &str) -> Result<String, String> {
    let loc = location.trim();
    if loc.starts_with("http://") || loc.starts_with("https://") {
        return Ok(loc.to_owned());
    }
    let empty_locals = HashSet::new();
    let parsed = parse_and_validate_url_syntax(base_uri, &empty_locals)?;
    let default_port = if parsed.scheme == "https" { 443 } else { 80 };
    let host_fmt = if parsed.host.contains(':') {
        format!("[{}]", parsed.host)
    } else {
        parsed.host
    };
    let origin = if parsed.port == default_port {
        format!("{}://{}", parsed.scheme, host_fmt)
    } else {
        format!("{}://{}:{}", parsed.scheme, host_fmt, parsed.port)
    };
    if loc.starts_with('/') {
        Ok(format!("{origin}{loc}"))
    } else {
        Ok(format!("{origin}/{loc}"))
    }
}

fn build_pinned_agent(parsed: &ParsedHttpUrl, pinned_addrs: Vec<SocketAddr>) -> ureq::Agent {
    let config = ureq::Agent::config_builder()
        .proxy(None)
        .max_redirects(0)
        .http_status_as_error(false)
        .timeout_resolve(Some(Duration::from_secs(4)))
        .timeout_connect(Some(Duration::from_secs(5)))
        .timeout_send_request(Some(Duration::from_secs(5)))
        .timeout_recv_response(Some(Duration::from_secs(8)))
        .timeout_per_read(Some(Duration::from_secs(5)))
        .timeout_per_write(Some(Duration::from_secs(5)))
        .build();
    ureq::Agent::with_parts(
        config,
        DefaultConnector::default(),
        PinnedResolver {
            expected_host: parsed.host.clone(),
            expected_port: parsed.port,
            addrs: pinned_addrs,
        },
    )
}

fn open_validated_response_with_locals(
    initial_uri: &str,
    local_ips: &HashSet<IpAddr>,
    shared: &SharedStream,
) -> Result<ureq::http::Response<ureq::Body>, String> {
    let mut current = initial_uri.to_owned();
    let mut previous: Vec<IpAddr> = Vec::new();

    for hop in 0..=MAX_REDIRECTS {
        if shared.cancelled.load(Ordering::Acquire) {
            return Err("DLNA 媒体流已取消".into());
        }
        let (parsed, pinned_addrs) = resolve_and_pin_socket_addrs(&current, local_ips)?;
        let addresses: Vec<_> = pinned_addrs.iter().map(SocketAddr::ip).collect();
        let private = is_private_lan_ip(addresses[0]);
        if addresses.iter().any(|&ip| is_private_lan_ip(ip) != private) {
            return Err("媒体主机同时解析到公网和局域网地址".into());
        }
        if hop > 0
            && (private != is_private_lan_ip(previous[0])
                || (private && addresses.iter().any(|ip| !previous.contains(ip))))
        {
            return Err("禁止媒体重定向跨越网络边界或更换局域网主机".into());
        }
        previous = addresses;

        let agent = build_pinned_agent(&parsed, pinned_addrs);
        shared.set_agent(Some(agent.clone()));

        let response = agent
            .get(&current)
            .call()
            .map_err(|e| format!("DLNA 媒体连接失败: {e}"))?;
        let status = response.status().as_u16();
        if matches!(status, 301 | 302 | 303 | 307 | 308) {
            if hop == MAX_REDIRECTS {
                agent.shutdown();
                return Err("DLNA 媒体重定向次数过多".into());
            }
            let loc = response
                .headers()
                .get("location")
                .and_then(|v| v.to_str().ok())
                .ok_or_else(|| "DLNA 重定向缺少 Location".to_string())?
                .to_owned();
            agent.shutdown();
            current = resolve_redirect_url(&current, &loc)?;
            continue;
        }
        if !(200..300).contains(&status) {
            agent.shutdown();
            return Err(format!("DLNA 媒体服务器返回 HTTP {status}"));
        }
        return Ok(response);
    }
    Err("DLNA 媒体重定向次数过多".into())
}

fn stream_download(
    uri: &str,
    self_ip: Ipv4Addr,
    shared: Arc<SharedStream>,
    ready_tx: mpsc::SyncSender<Result<StreamReader, String>>,
) {
    let local_ips = collect_local_ips(self_ip);
    stream_download_with_locals(uri, &local_ips, shared, ready_tx);
}

fn stream_download_with_locals(
    uri: &str,
    local_ips: &HashSet<IpAddr>,
    shared: Arc<SharedStream>,
    ready_tx: mpsc::SyncSender<Result<StreamReader, String>>,
) {
    let mut response = match open_validated_response_with_locals(uri, local_ips, &shared) {
        Ok(r) => r,
        Err(err) => {
            shared.set_agent(None);
            let _ = ready_tx.send(Err(err));
            return;
        }
    };

    let content_len = response
        .headers()
        .get("content-length")
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.trim().parse::<usize>().ok());

    if let Some(len) = content_len {
        if len > MAX_MEDIA_BYTES {
            shared.cancel();
            let _ = ready_tx.send(Err("DLNA 音频超过 128 MiB 限制".into()));
            return;
        }
        let mut st = shared.state.lock().unwrap();
        st.total_len = Some(len);
        st.bytes.reserve(len.min(4 * 1024 * 1024));
    }

    let mut reader = response.body_mut().as_reader();
    let mut chunk = [0u8; 16 * 1024];
    let mut ready_sent = false;
    let prebuffer_deadline = Instant::now() + Duration::from_secs(30);
    let prebuffer_goal = content_len.map_or(PREBUFFER_BYTES, |len| len.min(PREBUFFER_BYTES));

    loop {
        if shared.cancelled.load(Ordering::Acquire) {
            shared.set_agent(None);
            return;
        }
        if !ready_sent && Instant::now() >= prebuffer_deadline {
            shared.cancel();
            let _ = ready_tx.send(Err("DLNA 媒体预缓冲超时".into()));
            return;
        }
        match reader.read(&mut chunk) {
            Ok(0) => {
                shared.set_agent(None);
                let mut st = shared.state.lock().unwrap();
                st.eof = true;
                let has_data = !st.bytes.is_empty();
                drop(st);
                shared.cv.notify_all();
                if !ready_sent {
                    let res = if has_data {
                        Ok(StreamReader {
                            shared: Arc::clone(&shared),
                            pos: 0,
                        })
                    } else {
                        Err("DLNA 媒体响应为空".into())
                    };
                    let _ = ready_tx.send(res);
                }
                return;
            }
            Ok(n) => {
                let should_announce_ready = {
                    let mut st = shared.state.lock().unwrap();
                    if st.bytes.len() + n > MAX_MEDIA_BYTES {
                        st.error = Some("DLNA 音频流超过 128 MiB 上限".into());
                        drop(st);
                        shared.cancel();
                        if !ready_sent {
                            let _ = ready_tx.send(Err("DLNA 音频流超过 128 MiB 上限".into()));
                        }
                        return;
                    }
                    st.bytes.extend_from_slice(&chunk[..n]);
                    !ready_sent && st.bytes.len() >= prebuffer_goal
                };
                shared.cv.notify_all();
                if should_announce_ready {
                    ready_sent = true;
                    let _ = ready_tx.send(Ok(StreamReader {
                        shared: Arc::clone(&shared),
                        pos: 0,
                    }));
                }
            }
            Err(err) => {
                shared.set_agent(None);
                let msg = format!("DLNA 媒体流读取中断: {err}");
                let mut st = shared.state.lock().unwrap();
                st.error = Some(msg.clone());
                drop(st);
                shared.cv.notify_all();
                if !ready_sent {
                    let _ = ready_tx.send(Err(msg));
                }
                return;
            }
        }
    }
}

fn service_type(service: &str) -> String {
    format!("urn:schemas-upnp-org:service:{service}:1")
}

fn usn(identity: &str, target: &str) -> String {
    if target == identity {
        identity.to_owned()
    } else {
        format!("{identity}::{target}")
    }
}

fn announce(udp: &UdpSocket, targets: &[String], identity: &str, location: &str, nts: &str) {
    for target in targets {
        let usn = usn(identity, target);
        let body = format!(
            "NOTIFY * HTTP/1.1\r\nHOST: {GROUP}:1900\r\nCACHE-CONTROL: max-age=300\r\nLOCATION: {location}\r\nNT: {target}\r\nNTS: {nts}\r\nSERVER: SoundFlow UPnP/1.0\r\nUSN: {usn}\r\n\r\n"
        );
        let _ = udp.send_to(body.as_bytes(), (GROUP, 1900));
    }
}

fn search_target(text: &str) -> Option<&str> {
    let mut lines = text.lines();
    if !lines.next()?.eq_ignore_ascii_case("M-SEARCH * HTTP/1.1") {
        return None;
    }
    let fields = lines
        .filter_map(|line| line.split_once(':'))
        .collect::<Vec<_>>();
    if !fields.iter().any(|(k, v)| {
        k.eq_ignore_ascii_case("MAN") && v.trim().eq_ignore_ascii_case("\"ssdp:discover\"")
    }) {
        return None;
    }
    fields
        .into_iter()
        .find_map(|(k, v)| k.eq_ignore_ascii_case("ST").then(|| v.trim()))
}

fn escape(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}

fn description(identity: &str, name: &str) -> String {
    let list = SERVICES.iter().map(|(service, _)| format!(
        "<service><serviceType>{}</serviceType><serviceId>urn:upnp-org:serviceId:{service}</serviceId><SCPDURL>/{service}.xml</SCPDURL><controlURL>/{service}</controlURL><eventSubURL>/{service}/event</eventSubURL></service>",
        service_type(service))).collect::<String>();
    format!(
        "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><device><deviceType>{DEVICE}</deviceType><friendlyName>{}</friendlyName><manufacturer>SoundFlow</manufacturer><modelName>SoundFlow</modelName><UDN>{}</UDN><serviceList>{list}</serviceList></device></root>",
        escape(name),
        escape(identity)
    )
}

fn scpd(actions: &[&str]) -> String {
    let list = actions
        .iter()
        .map(|action| format!("<action><name>{action}</name></action>"))
        .collect::<String>();
    format!(
        "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><actionList>{list}</actionList><serviceStateTable/></scpd>"
    )
}

fn clock(duration: Duration) -> String {
    let seconds = duration.as_secs();
    format!(
        "{:02}:{:02}:{:02}",
        seconds / 3600,
        (seconds / 60) % 60,
        seconds % 60
    )
}

fn parse_clock(value: &str) -> Option<Duration> {
    let mut fields = value.split(':');
    let h: u64 = fields.next()?.parse().ok()?;
    let m: u64 = fields.next()?.parse().ok()?;
    let s: f64 = fields.next()?.parse().ok()?;
    if fields.next().is_some() || h > 1000 || m >= 60 || !(0.0..60.0).contains(&s) {
        return None;
    }
    Some(Duration::from_secs_f64((h * 3600 + m * 60) as f64 + s))
}

fn soap(
    service: &str,
    actions: &[&str],
    body: &str,
    media: &mut Media,
    state: &Mutex<Status>,
    blocked: bool,
) -> Result<String, u16> {
    let xml = roxmltree::Document::parse(body).map_err(|_| 402_u16)?;
    let action = xml
        .descendants()
        .find(|n| n.has_tag_name(("http://schemas.xmlsoap.org/soap/envelope/", "Body")))
        .and_then(|n| n.children().find(|n| n.is_element()))
        .ok_or(402_u16)?;
    let name = action.tag_name().name();
    if action.tag_name().namespace() != Some(service_type(service).as_str())
        || !actions.contains(&name)
    {
        return Err(401);
    }
    let field = |name| {
        action
            .children()
            .find(|n| n.tag_name().name() == name)
            .and_then(|n| n.text())
            .unwrap_or_default()
    };
    if !matches!(field("InstanceID"), "" | "0") {
        return Err(718);
    }
    let values = match name {
        "SetAVTransportURI" => {
            let uri = field("CurrentURI").trim();
            let locals = collect_local_ips(media.local_ip);
            if parse_and_validate_url_syntax(uri, &locals).is_err() {
                return Err(714);
            }
            media.stop(state);
            media.uri = uri.to_owned();
            media.duration = Duration::ZERO;
            let metadata = roxmltree::Document::parse(field("CurrentURIMetaData")).ok();
            let meta = |name| {
                metadata
                    .as_ref()
                    .and_then(|doc| doc.descendants().find(|n| n.tag_name().name() == name))
                    .and_then(|n| n.text())
                    .unwrap_or_default()
                    .chars()
                    .take(256)
                    .collect::<String>()
            };
            media.title = meta("title");
            media.artist = meta("artist");
            if media.title.is_empty() {
                media.title = "DLNA 音频".into();
            }
            String::new()
        }
        "Play" if !blocked => {
            media.play(state, false)?;
            String::new()
        }
        "Play" => return Err(701),
        "Pause" => {
            media.pause(state);
            String::new()
        }
        "Stop" => {
            media.stop(state);
            String::new()
        }
        "Seek" if !blocked => {
            if !matches!(field("Unit"), "REL_TIME" | "ABS_TIME") {
                return Err(710);
            }
            let time = parse_clock(field("Target")).ok_or(711_u16)?;
            media
                .player
                .as_ref()
                .ok_or(701_u16)?
                .try_seek(time)
                .map_err(|_| 711_u16)?;
            String::new()
        }
        "Seek" => return Err(701),
        "GetTransportInfo" => format!(
            "<CurrentTransportState>{}</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>",
            media.transport_state()
        ),
        "GetPositionInfo" => format!(
            "<Track>1</Track><TrackDuration>{}</TrackDuration><TrackMetaData></TrackMetaData><TrackURI>{}</TrackURI><RelTime>{}</RelTime><AbsTime>{}</AbsTime><RelCount>2147483647</RelCount><AbsCount>2147483647</AbsCount>",
            clock(media.duration),
            escape(&media.uri),
            clock(media.position()),
            clock(media.position())
        ),
        "GetMediaInfo" => format!(
            "<NrTracks>1</NrTracks><MediaDuration>{}</MediaDuration><CurrentURI>{}</CurrentURI><CurrentURIMetaData></CurrentURIMetaData><NextURI></NextURI><NextURIMetaData></NextURIMetaData><PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium><WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
            clock(media.duration),
            escape(&media.uri)
        ),
        "GetCurrentTransportActions" => "<Actions>Play,Pause,Stop,Seek</Actions>".into(),
        "GetVolume" => format!("<CurrentVolume>{}</CurrentVolume>", media.volume),
        "SetVolume" => {
            media.volume = field("DesiredVolume")
                .parse::<u8>()
                .ok()
                .filter(|v| *v <= 100)
                .ok_or(402_u16)?;
            media.apply_volume(blocked);
            String::new()
        }
        "GetMute" => format!("<CurrentMute>{}</CurrentMute>", u8::from(media.mute)),
        "SetMute" => {
            media.mute = matches!(field("DesiredMute"), "1" | "true");
            media.apply_volume(blocked);
            String::new()
        }
        "GetProtocolInfo" => format!("<Source></Source><Sink>{PROTOCOLS}</Sink>"),
        "GetCurrentConnectionIDs" => "<ConnectionIDs>0</ConnectionIDs>".into(),
        "GetCurrentConnectionInfo" => "<RcsID>0</RcsID><AVTransportID>0</AVTransportID><ProtocolInfo></ProtocolInfo><PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID><Direction>Input</Direction><Status>OK</Status>".into(),
        _ => return Err(401),
    };
    Ok(format!(
        "<u:{name}Response xmlns:u=\"{}\">{values}</u:{name}Response>",
        service_type(service)
    ))
}

fn request(stream: &mut TcpStream) -> Option<(String, String, String)> {
    const MAX_HEADER: usize = 8 * 1024;
    const MAX_BODY: usize = 64 * 1024;
    let _ = stream.set_nonblocking(false);
    let deadline = Instant::now() + Duration::from_millis(900);
    let mut bytes = Vec::with_capacity(2048);
    let mut chunk = [0; 2048];
    let mut read = |bytes: &mut Vec<u8>| {
        let remaining = deadline.checked_duration_since(Instant::now())?;
        stream
            .set_read_timeout(Some(remaining.max(Duration::from_millis(1))))
            .ok()?;
        let count = stream.read(&mut chunk).ok()?;
        if count == 0 {
            return None;
        }
        bytes.extend_from_slice(&chunk[..count]);
        Some(())
    };
    let end = loop {
        if let Some(end) = bytes.windows(4).position(|part| part == b"\r\n\r\n") {
            break end;
        }
        if bytes.len() >= MAX_HEADER {
            return None;
        }
        read(&mut bytes)?;
    };
    if end > MAX_HEADER {
        return None;
    }
    let header = std::str::from_utf8(&bytes[..end]).ok()?;
    let mut lines = header.split("\r\n");
    let mut fields = lines.next()?.split_ascii_whitespace();
    let method = fields.next()?.to_owned();
    let path = fields.next()?.to_owned();
    if !matches!(fields.next()?, "HTTP/1.0" | "HTTP/1.1") {
        return None;
    }
    let mut length = None;
    for header in lines {
        let (name, value) = header.split_once(':')?;
        if name.eq_ignore_ascii_case("content-length") {
            if length.is_some() {
                return None;
            }
            length = Some(value.trim().parse::<usize>().ok()?);
        } else if name.eq_ignore_ascii_case("transfer-encoding") {
            return None;
        }
    }
    let length = length.unwrap_or(0);
    if length > MAX_BODY {
        return None;
    }
    let start = end + 4;
    let end = start + length;
    while bytes.len() < end {
        read(&mut bytes)?;
    }
    let body = std::str::from_utf8(&bytes[start..end]).ok()?.to_owned();
    Some((method, path, body))
}

fn handle_http_worker(mut stream: TcpStream, xml: &str, cmd_tx: &mpsc::SyncSender<WorkerCmd>) {
    let (status, body) = match request(&mut stream) {
        Some((method, url, content)) if method == "POST" => {
            let Some((service, actions)) =
                SERVICES.iter().find(|(name, _)| url == format!("/{name}"))
            else {
                respond(&mut stream, "404 Not Found", "");
                return;
            };
            let (reply_tx, reply_rx) = mpsc::sync_channel(1);
            if cmd_tx
                .try_send(WorkerCmd::Soap(SoapJob {
                    service: (*service).to_owned(),
                    actions,
                    body: content,
                    reply: reply_tx,
                }))
                .is_err()
            {
                respond(&mut stream, "503 Service Unavailable", "");
                return;
            }
            let Ok(soap_res) = reply_rx.recv_timeout(Duration::from_secs(3)) else {
                respond(&mut stream, "504 Gateway Timeout", "");
                return;
            };
            let (status, payload) = match soap_res {
                Ok(value) => ("200 OK", value),
                Err(err) => (
                    "500 Internal Server Error",
                    format!(
                        "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>{err}</errorCode></UPnPError></detail></s:Fault>"
                    ),
                ),
            };
            (
                status,
                format!(
                    "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>{payload}</s:Body></s:Envelope>"
                ),
            )
        }
        Some((method, url, _)) if method == "GET" => {
            if url == "/device.xml" {
                ("200 OK", xml.to_owned())
            } else if let Some((_, actions)) = SERVICES
                .iter()
                .find(|(name, _)| url == format!("/{name}.xml"))
            {
                ("200 OK", scpd(actions))
            } else {
                ("404 Not Found", String::new())
            }
        }
        Some(_) => ("405 Method Not Allowed", String::new()),
        None => ("400 Bad Request", String::new()),
    };
    respond(&mut stream, status, &body);
}

fn respond(stream: &mut TcpStream, status: &str, body: &str) {
    if stream
        .set_write_timeout(Some(Duration::from_millis(400)))
        .is_ok()
    {
        let _ = write!(
            stream,
            "HTTP/1.1 {status}\r\nContent-Type: text/xml; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dlna_ssrf_guard_rejects_all_local_loopback_zero_net_and_cross_tier_redirects() {
        let mut locals = collect_local_ips(Ipv4Addr::new(192, 168, 5, 10));
        locals.insert(IpAddr::V4(Ipv4Addr::new(10, 20, 30, 40)));
        locals.insert("fd12:3456::1".parse().unwrap());

        for blocked in [
            "http://127.0.0.1:8080/a.mp3",
            "http://127.1.2.3/a.mp3",
            "http://localhost:8000/a.flac",
            "https://sub.localhost/a.wav",
            "http://0.0.0.0:80/a.mp3",
            "http://0.1.2.3:80/a.mp3",
            "http://169.254.169.254/latest/meta-data/",
            "http://224.0.0.251:5353/a.mp3",
            "http://255.255.255.255/a.mp3",
            "http://[::1]:8080/a.mp3",
            "http://[::ffff:127.0.0.1]:8080/a.mp3",
            "http://[::ffff:10.20.30.40]:8080/a.mp3",
            "http://[fe80::1]:8080/a.mp3",
            "http://[fd12:3456::1]:8080/a.mp3",
            "http://user:pass@192.168.5.20/a.mp3",
            "file:///etc/passwd",
            "http://192.168.5.10:8901/device.xml",
            "http://10.20.30.40:8901/device.xml",
        ] {
            assert!(
                parse_and_validate_url_syntax(blocked, &locals).is_err(),
                "expected {blocked} to be rejected"
            );
        }

        for allowed in [
            "http://192.168.5.84:9000/music/track.flac",
            "https://10.0.0.12:8443/stream.aac",
            "http://[2001:4860:4860::8888]:8080/live.mp3",
        ] {
            assert!(
                parse_and_validate_url_syntax(allowed, &locals).is_ok(),
                "expected {allowed} to be accepted"
            );
        }
    }

    #[test]
    fn pinned_agent_connects_to_verified_socket_and_cancel_interrupts_stalled_read() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();

        let (accepted_tx, accepted_rx) = mpsc::channel();
        let server = thread::spawn(move || {
            let (mut sock, _) = listener.accept().unwrap();
            let mut req_buf = [0u8; 512];
            let n = sock.read(&mut req_buf).unwrap();
            let req_txt = String::from_utf8_lossy(&req_buf[..n]).into_owned();
            // Send partial body and hold socket open forever until client cancels.
            sock.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n\r\nabcd")
                .unwrap();
            accepted_tx.send((req_txt, sock)).unwrap();
        });

        // Verify PinnedResolver connects exclusively to `addr` while preserving original Host header.
        let parsed = ParsedHttpUrl {
            scheme: "http".into(),
            host: "media-nas.lan".into(),
            port: addr.port(),
        };
        let shared = SharedStream::new();
        let agent = build_pinned_agent(&parsed, vec![addr]);
        shared.set_agent(Some(agent.clone()));

        let url = format!("http://media-nas.lan:{}/song.flac", addr.port());
        let shared_dl = Arc::clone(&shared);
        let (done_tx, done_rx) = mpsc::channel();
        let dl_worker = thread::spawn(move || {
            let mut resp = agent.get(&url).call().unwrap();
            let mut body = resp.body_mut().as_reader();
            let mut buf = [0u8; 64];
            let _ = body.read(&mut buf);
            // Next read blocks on silent socket until shared.cancel() shuts down underlying TCP stream.
            let res = body.read(&mut buf);
            shared_dl.set_agent(None);
            done_tx.send(res.is_err()).unwrap();
        });

        let (req_txt, _held_sock) = accepted_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        assert!(req_txt.to_ascii_lowercase().contains(&format!("host: media-nas.lan:{}", addr.port())));

        shared.cancel();
        assert!(
            done_rx.recv_timeout(Duration::from_millis(800)).unwrap(),
            "cancel() must immediately interrupt blocked body read"
        );
        dl_worker.join().unwrap();
        server.join().unwrap();
    }

    #[test]
    fn cancelled_reader_stops_read_exact_without_retrying() {
        let stream = SharedStream::new();
        stream.cancel();
        let mut reader = StreamReader { shared: stream, pos: 0 };
        let error = reader.read_exact(&mut [0; 4]).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::ConnectionAborted);
    }

    #[test]
    fn download_slot_limiter_caps_in_flight_media_tasks() {
        let counter = AtomicUsize::new(0);
        assert!(acquire_download_slot(&counter, Duration::from_millis(20)));
        assert!(acquire_download_slot(&counter, Duration::from_millis(20)));
        assert!(!acquire_download_slot(&counter, Duration::from_millis(30)));
        counter.fetch_sub(1, Ordering::AcqRel);
        assert!(acquire_download_slot(&counter, Duration::from_millis(20)));
    }

    #[test]
    fn dlna_external_onset_stream_survives_prior_miplay_stream_generations() {
        use fusionplay_miplay_sdk::onset;
        onset::set_capture(true);
        // Simulate two prior MiPlay output sessions incrementing STREAM > 0.
        let _ = begin_external_output_stream();
        let _ = begin_external_output_stream();

        let dlna_stream = begin_external_output_stream();
        let mut lp = (0.0_f32, 0.0_f32);
        let silence = [(0.0_f32, 0.0_f32); 256];
        for i in 0..16 {
            report_external_output_pcm_stream(&silence, 48_000, 0.0, &mut lp, dlna_stream, i == 0);
        }
        // Feed sharp transient drum/bass burst so onset detector triggers.
        let mut burst = [(0.0_f32, 0.0_f32); 256];
        for (idx, frame) in burst.iter_mut().enumerate() {
            let s = ((idx as f32 * 0.22).sin() * 0.92).clamp(-1.0, 1.0);
            *frame = (s, s);
        }
        for _ in 0..12 {
            report_external_output_pcm_stream(&burst, 48_000, 0.0, &mut lp, dlna_stream, false);
        }
        let events = onset::poll(true);
        assert!(
            !events.is_empty(),
            "DLNA external stream must produce onset events even after prior MiPlay sessions"
        );
    }

    #[test]
    fn stream_reader_supports_incremental_read_and_seek() {
        let shared = SharedStream::new();
        let writer = Arc::clone(&shared);
        let handle = thread::spawn(move || {
            {
                let mut st = writer.state.lock().unwrap();
                st.total_len = Some(10);
                st.bytes.extend_from_slice(b"hello");
            }
            writer.cv.notify_all();
            thread::sleep(Duration::from_millis(15));
            {
                let mut st = writer.state.lock().unwrap();
                st.bytes.extend_from_slice(b"world");
                st.eof = true;
            }
            writer.cv.notify_all();
        });

        let mut reader = StreamReader { shared, pos: 0 };
        let mut first = [0u8; 5];
        reader.read_exact(&mut first).unwrap();
        assert_eq!(&first, b"hello");

        assert_eq!(reader.seek(SeekFrom::Start(7)).unwrap(), 7);
        let mut tail = Vec::new();
        reader.read_to_end(&mut tail).unwrap();
        assert_eq!(&tail, b"rld");

        assert_eq!(reader.seek(SeekFrom::End(-5)).unwrap(), 5);
        let mut second = [0u8; 5];
        reader.read_exact(&mut second).unwrap();
        assert_eq!(&second, b"world");
        handle.join().unwrap();
    }
}
