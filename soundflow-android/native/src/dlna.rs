// SPDX-License-Identifier: AGPL-3.0-only
use fusionplay_miplay_sdk::{report_external_output_pcm, reset_live_output_peak};
use rodio::{ChannelCount, Decoder, DeviceSinkBuilder, MixerDeviceSink, Player, SampleRate, Source};
use serde_json::{Value, json};
use socket2::{Domain, Protocol, Socket, Type};
use std::{
    collections::VecDeque,
    io::{self, Read, Seek, SeekFrom, Write},
    net::{IpAddr, Ipv4Addr, SocketAddrV4, TcpListener, TcpStream, ToSocketAddrs, UdpSocket},
    num::NonZero,
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

const GROUP: Ipv4Addr = Ipv4Addr::new(239, 255, 255, 250);
const DEVICE: &str = "urn:schemas-upnp-org:device:MediaRenderer:1";
const MAX_MEDIA_BYTES: usize = 128 * 1024 * 1024;
const PREBUFFER_BYTES: usize = 128 * 1024;
const MAX_HTTP_CLIENTS: usize = 8;
const MAX_REDIRECTS: usize = 3;

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
    local_action: Option<(String, i32)>,
}

impl Status {
    fn emit(&mut self, event: Value) {
        if self.events.len() == 32 {
            self.events.pop_front();
        }
        self.events.push_back(event);
    }
}

struct SoapJob {
    service: String,
    actions: &'static [&'static str],
    body: String,
    reply: mpsc::SyncSender<Result<String, u16>>,
}

pub struct DlnaRenderer {
    status: Arc<Mutex<Status>>,
    stop: mpsc::Sender<()>,
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
        let (stop, shutdown) = mpsc::channel();
        let (soap_tx, soap_rx) = mpsc::sync_channel::<SoapJob>(16);
        let status = Arc::new(Mutex::new(Status::default()));
        let shared = Arc::clone(&status);
        let active_http = Arc::new(AtomicUsize::new(0));
        let xml = Arc::new(description(&identity, &name));

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
                    ..Media::default()
                };
                let mut buffer = [0_u8; 4096];
                let mut next_alive = Instant::now();
                while shutdown.try_recv().is_err() {
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
                        let tx_ref = soap_tx.clone();
                        let counter = Arc::clone(&active_http);
                        if thread::Builder::new()
                            .name("soundflow-dlna-http".into())
                            .spawn(move || {
                                let _guard = HttpCounterGuard(counter);
                                handle_http_worker(stream, &xml_ref, &tx_ref);
                            })
                            .is_err()
                        {
                            active_http.fetch_sub(1, Ordering::AcqRel);
                        }
                    }

                    let (blocked, action) = {
                        let mut state = shared.lock().unwrap();
                        (state.blocked, state.local_action.take())
                    };
                    if blocked {
                        media.pause(&shared);
                    }
                    if let Some((action, value)) = action {
                        match action.as_str() {
                            "pause" => media.pause(&shared),
                            "resume" if !blocked => {
                                let _ = media.play(&shared);
                            }
                            "volume" => {
                                media.volume = value.clamp(0, 100) as u8;
                                media.apply_volume();
                            }
                            _ => {}
                        }
                    }
                    while let Ok(job) = soap_rx.try_recv() {
                        let blocked_now = shared.lock().unwrap().blocked;
                        let res = soap(
                            &job.service,
                            job.actions,
                            &job.body,
                            &mut media,
                            &shared,
                            blocked_now,
                        );
                        let _ = job.reply.send(res);
                    }
                    media.tick(&shared);
                    thread::sleep(Duration::from_millis(12));
                }
                media.stop(&shared);
                announce(&udp, &targets, &identity, &location, "ssdp:byebye");
            })
            .map_err(|e| e.to_string())?;

        Ok(Self {
            status,
            stop,
            worker: Some(worker),
        })
    }

    pub fn active(&self) -> bool {
        self.status.lock().unwrap().active
    }

    pub fn set_blocked(&self, blocked: bool) {
        self.status.lock().unwrap().blocked = blocked;
    }

    pub fn control(&self, action: &str, value: i32) {
        self.status.lock().unwrap().local_action = Some((action.to_owned(), value));
    }

    pub fn poll(&self) -> Vec<Value> {
        self.status.lock().unwrap().events.drain(..).collect()
    }
}

impl Drop for DlnaRenderer {
    fn drop(&mut self) {
        let _ = self.stop.send(());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

struct HttpCounterGuard(Arc<AtomicUsize>);
impl Drop for HttpCounterGuard {
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
}

impl SharedStream {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(StreamState::default()),
            cv: Condvar::new(),
            cancelled: AtomicBool::new(false),
        })
    }

    fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
        self.cv.notify_all();
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
        let deadline = Instant::now() + Duration::from_secs(12);
        let mut guard = self.shared.state.lock().unwrap();
        loop {
            if self.shared.cancelled.load(Ordering::Acquire) {
                return Err(io::Error::new(io::ErrorKind::Interrupted, "DLNA stream cancelled"));
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
        let deadline = Instant::now() + Duration::from_secs(8);
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
                return Err(io::Error::new(io::ErrorKind::Interrupted, "DLNA stream cancelled"));
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
        }
    }

    fn flush_meter(&mut self) {
        if self.block_len > 0 {
            report_external_output_pcm(
                &self.block[..self.block_len],
                self.sample_rate,
                35.0,
                &mut self.lp_state,
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
        self.inner.try_seek(pos)
    }
}

struct Media {
    player: Option<Player>,
    output: Option<MixerDeviceSink>,
    active_stream: Option<Arc<SharedStream>>,
    pending_ready: Option<mpsc::Receiver<Result<StreamReader, String>>>,
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
    fn play(&mut self, status: &Mutex<Status>) -> Result<(), u16> {
        if self.uri.is_empty() {
            return Err(701);
        }
        self.want_play = true;
        if let Some(player) = &self.player {
            player.play();
            self.set_playing(status, true);
        } else if self.pending_ready.is_none() {
            let uri = self.uri.clone();
            let self_ip = self.local_ip;
            let shared_stream = SharedStream::new();
            let stream_for_worker = Arc::clone(&shared_stream);
            let (tx, rx) = mpsc::sync_channel(1);
            thread::Builder::new()
                .name("soundflow-dlna-stream".into())
                .spawn(move || {
                    stream_download(&uri, self_ip, stream_for_worker, tx);
                })
                .map_err(|_| 501_u16)?;
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

    fn apply_volume(&self) {
        if let Some(player) = &self.player {
            player.set_volume(if self.mute {
                0.0
            } else {
                f32::from(self.volume) / 100.0
            });
        }
    }

    fn tick(&mut self, state: &Mutex<Status>) {
        if let Some(result) = self.pending_ready.as_ref().and_then(|rx| rx.try_recv().ok()) {
            self.pending_ready = None;
            if self.want_play {
                match result.and_then(|reader| self.install(reader)) {
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

    fn install(&mut self, reader: StreamReader) -> Result<(), String> {
        let decoded = Decoder::new(reader).map_err(|e| format!("DLNA 音频解码失败: {e}"))?;
        let output = DeviceSinkBuilder::open_default_sink().map_err(|e| e.to_string())?;
        let player = Player::connect_new(output.mixer());
        self.duration = decoded.total_duration().unwrap_or_default();
        player.append(MeteredSource::new(decoded));
        self.player = Some(player);
        self.output = Some(output);
        self.apply_volume();
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

#[derive(Debug, PartialEq, Eq)]
struct ParsedHttpUrl {
    scheme: String,
    host: String,
    port: u16,
}

fn is_allowed_media_ip(ip: IpAddr, self_ip: Ipv4Addr) -> bool {
    match ip {
        IpAddr::V4(v4) => {
            !v4.is_loopback()
                && !v4.is_unspecified()
                && !v4.is_link_local()
                && !v4.is_multicast()
                && !v4.is_broadcast()
                && (self_ip.is_unspecified() || v4 != self_ip)
        }
        IpAddr::V6(v6) => {
            if let Some(mapped) = v6.to_ipv4_mapped() {
                return is_allowed_media_ip(IpAddr::V4(mapped), self_ip);
            }
            let seg0 = v6.segments()[0];
            let is_link_local_v6 = (seg0 & 0xffc0) == 0xfe80;
            !v6.is_loopback()
                && !v6.is_unspecified()
                && !v6.is_multicast()
                && !is_link_local_v6
        }
    }
}

fn parse_and_validate_url_syntax(uri: &str, self_ip: Ipv4Addr) -> Result<ParsedHttpUrl, String> {
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
        && !is_allowed_media_ip(ip, self_ip)
    {
        return Err("禁止访问本机回环、链路本地或多播地址".into());
    }
    Ok(ParsedHttpUrl {
        scheme: scheme.to_owned(),
        host: host_raw.to_owned(),
        port,
    })
}

fn validate_and_resolve_media_uri(uri: &str, self_ip: Ipv4Addr) -> Result<(), String> {
    let parsed = parse_and_validate_url_syntax(uri, self_ip)?;
    if let Ok(ip) = parsed.host.parse::<IpAddr>() {
        if !is_allowed_media_ip(ip, self_ip) {
            return Err("禁止访问受限 IP 地址".into());
        }
        return Ok(());
    }
    let addrs = (parsed.host.as_str(), parsed.port)
        .to_socket_addrs()
        .map_err(|_| "无法解析媒体服务器主机名".to_string())?
        .collect::<Vec<_>>();
    if addrs.is_empty() {
        return Err("媒体服务器主机无可用地址".into());
    }
    if addrs.iter().any(|sa| !is_allowed_media_ip(sa.ip(), self_ip)) {
        return Err("媒体主机解析到了受限的回环或链路本地地址".into());
    }
    Ok(())
}

fn resolve_redirect_url(base_uri: &str, location: &str) -> Result<String, String> {
    let loc = location.trim();
    if loc.starts_with("http://") || loc.starts_with("https://") {
        return Ok(loc.to_owned());
    }
    let parsed = parse_and_validate_url_syntax(base_uri, Ipv4Addr::UNSPECIFIED)?;
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

fn open_validated_response(initial_uri: &str, self_ip: Ipv4Addr) -> Result<ureq::http::Response<ureq::Body>, String> {
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_connect(Some(Duration::from_secs(6)))
        .timeout_recv_response(Some(Duration::from_secs(10)))
        .max_redirects(0)
        .http_status_as_error(false)
        .build()
        .into();

    let mut current = initial_uri.to_owned();
    for hop in 0..=MAX_REDIRECTS {
        validate_and_resolve_media_uri(&current, self_ip)?;
        let response = agent
            .get(&current)
            .call()
            .map_err(|e| format!("DLNA 媒体连接失败: {e}"))?;
        let status = response.status().as_u16();
        if matches!(status, 301 | 302 | 303 | 307 | 308) {
            if hop == MAX_REDIRECTS {
                return Err("DLNA 媒体重定向次数过多".into());
            }
            let loc = response
                .headers()
                .get("location")
                .and_then(|v| v.to_str().ok())
                .ok_or_else(|| "DLNA 重定向缺少 Location".to_string())?;
            current = resolve_redirect_url(&current, loc)?;
            continue;
        }
        if !(200..300).contains(&status) {
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
    let mut response = match open_validated_response(uri, self_ip) {
        Ok(r) => r,
        Err(err) => {
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
    let prebuffer_goal = content_len.map_or(PREBUFFER_BYTES, |len| len.min(PREBUFFER_BYTES));

    loop {
        if shared.cancelled.load(Ordering::Acquire) {
            return;
        }
        match reader.read(&mut chunk) {
            Ok(0) => {
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
                        shared.cv.notify_all();
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
            if parse_and_validate_url_syntax(uri, media.local_ip).is_err() {
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
            media.play(state)?;
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
        "Seek" => {
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
            media.apply_volume();
            String::new()
        }
        "GetMute" => format!("<CurrentMute>{}</CurrentMute>", u8::from(media.mute)),
        "SetMute" => {
            media.mute = matches!(field("DesiredMute"), "1" | "true");
            media.apply_volume();
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

fn handle_http_worker(mut stream: TcpStream, xml: &str, soap_tx: &mpsc::SyncSender<SoapJob>) {
    let (status, body) = match request(&mut stream) {
        Some((method, url, content)) if method == "POST" => {
            let Some((service, actions)) =
                SERVICES.iter().find(|(name, _)| url == format!("/{name}"))
            else {
                respond(&mut stream, "404 Not Found", "");
                return;
            };
            let (reply_tx, reply_rx) = mpsc::sync_channel(1);
            if soap_tx
                .try_send(SoapJob {
                    service: (*service).to_owned(),
                    actions,
                    body: content,
                    reply: reply_tx,
                })
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
    fn dlna_ssrf_guard_rejects_loopback_link_local_multicast_and_self_ip() {
        let self_ip = Ipv4Addr::new(192, 168, 5, 10);
        for blocked in [
            "http://127.0.0.1:8080/a.mp3",
            "http://127.1.2.3/a.mp3",
            "http://localhost:8000/a.flac",
            "https://sub.localhost/a.wav",
            "http://0.0.0.0:80/a.mp3",
            "http://169.254.169.254/latest/meta-data/",
            "http://224.0.0.251:5353/a.mp3",
            "http://255.255.255.255/a.mp3",
            "http://[::1]:8080/a.mp3",
            "http://[::ffff:127.0.0.1]:8080/a.mp3",
            "http://[fe80::1]:8080/a.mp3",
            "http://user:pass@192.168.5.20/a.mp3",
            "file:///etc/passwd",
            "http://192.168.5.10:8901/device.xml",
        ] {
            assert!(
                parse_and_validate_url_syntax(blocked, self_ip).is_err(),
                "expected {blocked} to be rejected"
            );
        }

        for allowed in [
            "http://192.168.5.84:9000/music/track.flac",
            "https://10.0.0.12:8443/stream.aac",
            "http://[2001:4860:4860::8888]:8080/live.mp3",
        ] {
            assert!(
                parse_and_validate_url_syntax(allowed, self_ip).is_ok(),
                "expected {allowed} to be accepted"
            );
        }
    }

    #[test]
    fn dlna_redirect_to_loopback_is_blocked() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        // Even if an allowed LAN URL redirected to 127.0.0.1, resolve_redirect_url + syntax check rejects it.
        let redirected =
            resolve_redirect_url("http://192.168.5.99:8080/play", &format!("http://127.0.0.1:{port}/secret"))
                .unwrap();
        assert!(parse_and_validate_url_syntax(&redirected, Ipv4Addr::new(192, 168, 5, 10)).is_err());
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
