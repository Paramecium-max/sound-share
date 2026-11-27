// SPDX-License-Identifier: AGPL-3.0-only
use rodio::{Decoder, DeviceSinkBuilder, MixerDeviceSink, Player, Source};
use serde_json::{Value, json};
use socket2::{Domain, Protocol, Socket, Type};
use std::{
    collections::VecDeque,
    io::{Cursor, Read, Write},
    net::{Ipv4Addr, SocketAddrV4, TcpListener, TcpStream, UdpSocket},
    sync::{Arc, Mutex, mpsc},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

const GROUP: Ipv4Addr = Ipv4Addr::new(239, 255, 255, 250);
const DEVICE: &str = "urn:schemas-upnp-org:device:MediaRenderer:1";
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
        let status = Arc::new(Mutex::new(Status::default()));
        let shared = Arc::clone(&status);
        let worker = thread::Builder::new().name("soundflow-dlna".into()).spawn(move || {
            let location = format!("http://{ip}:8901/device.xml");
            let targets = std::iter::once(identity.clone())
                .chain(["upnp:rootdevice".to_owned(), DEVICE.to_owned()])
                .chain(SERVICES.iter().map(|(name, _)| service_type(name)))
                .collect::<Vec<_>>();
            let mut media = Media { volume: initial_volume, ..Media::default() };
            let xml = description(&identity, &name);
            let mut buffer = [0_u8; 4096];
            let mut next_alive = Instant::now();
            while shutdown.try_recv().is_err() {
                if Instant::now() >= next_alive {
                    announce(&udp, &targets, &identity, &location, "ssdp:alive");
                    next_alive = Instant::now() + Duration::from_secs(120);
                }
                for _ in 0..16 {
                    let Ok((count, peer)) = udp.recv_from(&mut buffer) else { break };
                    let Ok(text) = std::str::from_utf8(&buffer[..count]) else { continue };
                    let Some(st) = search_target(text) else { continue };
                    for target in targets.iter().filter(|target| st.eq_ignore_ascii_case("ssdp:all") || target.eq_ignore_ascii_case(st)) {
                        let usn = usn(&identity, target);
                        let response = format!("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=300\r\nEXT:\r\nLOCATION: {location}\r\nSERVER: SoundFlow UPnP/1.0\r\nST: {target}\r\nUSN: {usn}\r\n\r\n");
                        let _ = udp.send_to(response.as_bytes(), peer);
                    }
                }
                let (blocked, action) = {
                    let mut state = shared.lock().unwrap();
                    (state.blocked, state.local_action.take())
                };
                if blocked { media.pause(&shared); }
                if let Some((action, value)) = action {
                    match action.as_str() {
                        "pause" => media.pause(&shared),
                        "resume" if !blocked => { let _ = media.play(&shared); }
                        "volume" => { media.volume = value.clamp(0, 100) as u8; media.apply_volume(); }
                        _ => {}
                    }
                }
                media.tick(&shared);
                if let Ok((stream, _)) = http.accept() {
                    handle_http(stream, &xml, &mut media, &shared, blocked);
                } else {
                    thread::sleep(Duration::from_millis(15));
                }
            }
            announce(&udp, &targets, &identity, &location, "ssdp:byebye");
        }).map_err(|e| e.to_string())?;
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

#[derive(Default)]
struct Media {
    player: Option<Player>,
    output: Option<MixerDeviceSink>,
    download: Option<mpsc::Receiver<Result<Vec<u8>, String>>>,
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

impl Media {
    fn play(&mut self, status: &Mutex<Status>) -> Result<(), u16> {
        if self.uri.is_empty() {
            return Err(701);
        }
        self.want_play = true;
        if let Some(player) = &self.player {
            player.play();
            self.set_playing(status, true);
        } else if self.download.is_none() {
            let uri = self.uri.clone();
            let (tx, rx) = mpsc::sync_channel(1);
            thread::Builder::new()
                .name("soundflow-dlna-download".into())
                .spawn(move || {
                    let _ = tx.send(download(&uri));
                })
                .map_err(|_| 501_u16)?;
            self.download = Some(rx);
        }
        Ok(())
    }

    fn pause(&mut self, status: &Mutex<Status>) {
        self.want_play = false;
        if let Some(player) = &self.player {
            player.pause();
        }
        self.set_playing(status, false);
    }

    fn stop(&mut self, status: &Mutex<Status>) {
        self.pause(status);
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
        if let Some(result) = self.download.as_ref().and_then(|rx| rx.try_recv().ok()) {
            self.download = None;
            if self.want_play {
                match result.and_then(|bytes| self.install(bytes)) {
                    Ok(()) => {
                        let mut status = state.lock().unwrap();
                        status.emit(json!({"event":"media_info","protocol":"dlna","title":self.title,
                            "artist":self.artist,"duration_ms":self.duration.as_millis(),"position_ms":0}));
                        drop(status);
                        self.set_playing(state, true);
                    }
                    Err(message) => {
                        self.want_play = false;
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
            state.lock().unwrap().emit(json!({"event":"progress","protocol":"dlna","position_ms":self.position().as_millis()}));
        }
    }

    fn install(&mut self, bytes: Vec<u8>) -> Result<(), String> {
        let decoded =
            Decoder::try_from(Cursor::new(bytes)).map_err(|e| format!("DLNA 音频解码失败: {e}"))?;
        let output = DeviceSinkBuilder::open_default_sink().map_err(|e| e.to_string())?;
        let player = Player::connect_new(output.mixer());
        self.duration = decoded.total_duration().unwrap_or_default();
        player.append(decoded);
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
        } else if self.download.is_some() && self.want_play {
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

fn download(uri: &str) -> Result<Vec<u8>, String> {
    use std::io::Read;
    const LIMIT: u64 = 128 * 1024 * 1024;
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(35)))
        .max_redirects(3)
        .build()
        .into();
    let mut response = agent
        .get(uri)
        .call()
        .map_err(|e| format!("DLNA 媒体读取失败: {e}"))?;
    let mut bytes = Vec::new();
    response
        .body_mut()
        .as_reader()
        .take(LIMIT + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() as u64 > LIMIT {
        return Err("DLNA 音频超过 128 MiB".into());
    }
    Ok(bytes)
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
            let uri = field("CurrentURI");
            if uri.len() > 4096 || !uri.starts_with("http://") && !uri.starts_with("https://") { return Err(714); }
            if media.download.is_some() { return Err(701); }
            media.stop(state);
            media.uri = uri.to_owned();
            media.duration = Duration::ZERO;
            let metadata = roxmltree::Document::parse(field("CurrentURIMetaData")).ok();
            let meta = |name| metadata.as_ref().and_then(|doc| doc.descendants().find(|n| n.tag_name().name() == name))
                .and_then(|n| n.text()).unwrap_or_default().chars().take(256).collect::<String>();
            media.title = meta("title");
            media.artist = meta("artist");
            if media.title.is_empty() { media.title = "DLNA 音频".into(); }
            String::new()
        }
        "Play" if !blocked => { media.play(state)?; String::new() }
        "Play" => return Err(701),
        "Pause" => { media.pause(state); String::new() }
        "Stop" => { media.stop(state); String::new() }
        "Seek" => {
            if !matches!(field("Unit"), "REL_TIME" | "ABS_TIME") { return Err(710); }
            let time = parse_clock(field("Target")).ok_or(711_u16)?;
            media.player.as_ref().ok_or(701_u16)?.try_seek(time).map_err(|_| 711_u16)?;
            String::new()
        }
        "GetTransportInfo" => format!("<CurrentTransportState>{}</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>", media.transport_state()),
        "GetPositionInfo" => format!("<Track>1</Track><TrackDuration>{}</TrackDuration><TrackMetaData></TrackMetaData><TrackURI>{}</TrackURI><RelTime>{}</RelTime><AbsTime>{}</AbsTime><RelCount>2147483647</RelCount><AbsCount>2147483647</AbsCount>",
            clock(media.duration), escape(&media.uri), clock(media.position()), clock(media.position())),
        "GetMediaInfo" => format!("<NrTracks>1</NrTracks><MediaDuration>{}</MediaDuration><CurrentURI>{}</CurrentURI><CurrentURIMetaData></CurrentURIMetaData><NextURI></NextURI><NextURIMetaData></NextURIMetaData><PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium><WriteStatus>NOT_IMPLEMENTED</WriteStatus>", clock(media.duration), escape(&media.uri)),
        "GetCurrentTransportActions" => "<Actions>Play,Pause,Stop,Seek</Actions>".into(),
        "GetVolume" => format!("<CurrentVolume>{}</CurrentVolume>", media.volume),
        "SetVolume" => { media.volume = field("DesiredVolume").parse::<u8>().ok().filter(|v| *v <= 100).ok_or(402_u16)?; media.apply_volume(); String::new() }
        "GetMute" => format!("<CurrentMute>{}</CurrentMute>", u8::from(media.mute)),
        "SetMute" => { media.mute = matches!(field("DesiredMute"), "1" | "true"); media.apply_volume(); String::new() }
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

fn handle_http(
    mut stream: TcpStream,
    xml: &str,
    media: &mut Media,
    state: &Mutex<Status>,
    blocked: bool,
) {
    let (status, body) = match request(&mut stream) {
        Some((method, url, content)) if method == "POST" => {
            let Some((service, actions)) =
                SERVICES.iter().find(|(name, _)| url == format!("/{name}"))
            else {
                respond(&mut stream, "404 Not Found", "");
                return;
            };
            let (status, payload) = match soap(service, actions, &content, media, state, blocked) {
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
