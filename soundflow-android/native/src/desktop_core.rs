use fusionplay_miplay_sdk::{live_output_peak, live_output_spectrum_take, onset};
use serde_json::{Value, json};
use std::{
    ffi::{CStr, CString, c_char, c_void},
    io::{self, BufRead, Write},
};

#[path = "dlna.rs"]
mod dlna;

type Create = unsafe extern "C" fn() -> *mut c_void;
type Destroy = unsafe extern "C" fn(*mut c_void);
type Request = unsafe extern "C" fn(*mut c_void, *const c_char) -> *const c_char;

struct Sender {
    context: *mut c_void,
    request: Request,
    destroy: Destroy,
    _library: libloading::Library,
}

impl Sender {
    fn new() -> Result<Self, Box<dyn std::error::Error>> {
        let name = format!(
            "{}soundflow_sender{}",
            std::env::consts::DLL_PREFIX,
            std::env::consts::DLL_SUFFIX,
        );
        let file = std::env::current_exe()?.with_file_name(name);
        // The packaged library lives beside this executable, outside renderer control.
        unsafe {
            let library = libloading::Library::new(file)?;
            let create = library.get::<Create>(b"soundflow_sender_create")?;
            let request = *library.get::<Request>(b"soundflow_sender_request")?;
            let destroy = *library.get::<Destroy>(b"soundflow_sender_destroy")?;
            let context = create();
            if context.is_null() {
                return Err("Could not initialize the sender engine".into());
            }
            Ok(Self {
                context,
                request,
                destroy,
                _library: library,
            })
        }
    }

    fn request(&mut self, body: Option<&Value>) -> Result<Value, String> {
        let serialized = body.map(|body| CString::new(body.to_string()).expect("JSON escapes NUL"));
        let pointer = serialized.as_ref().map_or(std::ptr::null(), |s| s.as_ptr());
        let response = unsafe { (self.request)(self.context, pointer) };
        if response.is_null() {
            return Err("发送内核请求失败".into());
        }
        // The response belongs to this context and stays alive until the next request.
        serde_json::from_slice(unsafe { CStr::from_ptr(response) }.to_bytes())
            .map_err(|err| err.to_string())
    }
}

impl Drop for Sender {
    fn drop(&mut self) {
        unsafe { (self.destroy)(self.context) };
    }
}

struct Host {
    sender: Sender,
    dlna: Option<dlna::DlnaRenderer>,
    playing: bool,
    receiver_blocked: bool,
    capture_onsets: bool,
    onset_sequence: u64,
}

impl Host {
    fn set_receiver_blocked(&mut self, blocked: bool) {
        self.receiver_blocked = blocked;
        if blocked && self.playing {
            let _ = soundflow::receiver_action("pause", 0);
        }
        let _ = soundflow::receiver_action(if blocked { "suspend" } else { "output_resume" }, 0);
        if let Some(dlna) = &self.dlna {
            dlna.set_blocked(blocked);
        }
    }

    fn request(&mut self, command: &Value) -> Result<Value, String> {
        let args = &command["args"];
        let number = |key: &str| args[key].as_i64().unwrap_or_default().clamp(0, 100) as i32;
        match command["method"].as_str().unwrap_or_default() {
            "sender" => {
                let start = args["method"] == "multiroom.start";
                if start && !self.receiver_blocked {
                    self.set_receiver_blocked(true);
                }
                let response = self.sender.request(Some(args));
                if (start && response.as_ref().map_or(true, |r| r.get("error").is_some()))
                    || args["method"] == "multiroom.stop"
                {
                    self.set_receiver_blocked(false);
                }
                response
            }
            "receiver.start" => {
                soundflow::start_receiver(&args.to_string())?;
                self.playing = false;
                let dlna_error = if args["enable_dlna"] == true {
                    match dlna::DlnaRenderer::start(args) {
                        Ok(renderer) => {
                            self.dlna = Some(renderer);
                            None
                        }
                        Err(err) => Some(err),
                    }
                } else {
                    None
                };
                self.set_receiver_blocked(self.receiver_blocked);
                Ok(
                    json!({"miplay_ready": true, "dlna_ready": self.dlna.is_some(),
                    "last_error": dlna_error, "endpoint": args["ip"]}),
                )
            }
            "receiver.stop" => {
                self.dlna = None;
                self.playing = false;
                soundflow::stop_receiver();
                Ok(Value::Null)
            }
            "receiver.control" => {
                let action = args["action"].as_str().ok_or("缺少控制命令")?;
                if self.receiver_blocked && action == "resume" {
                    return Err("全屋发送期间，本机接收播放已暂停".into());
                }
                if let Some(dlna) = &self.dlna {
                    if dlna.active() || action == "volume" {
                        dlna.control(action, number("value"));
                        if dlna.active() && action != "volume" {
                            return Ok(Value::Null);
                        }
                    }
                }
                soundflow::receiver_action(action, number("value"))?;
                Ok(Value::Null)
            }
            "receiver.visual" => {
                self.capture_onsets = args["onsets"].as_bool().unwrap_or(false);
                onset::set_capture(self.capture_onsets);
                Ok(Value::Null)
            }
            "receiver.syncOffset" => {
                let value = args["value"].as_i64().unwrap_or(0).clamp(-400, 400) as i32;
                fusionplay_miplay_sdk::set_audio_sync_offset_ms(value);
                Ok(Value::Null)
            }
            "receiver.sensitivity" => {
                onset::set_sensitivity(number("value") as u32);
                Ok(Value::Null)
            }
            "poll" => {
                let mut events = soundflow::poll_receiver();
                for event in &events {
                    match event["event"].as_str().unwrap_or_default() {
                        "stream_started" | "audio_output_signal_started" => self.playing = true,
                        "stream_stopped" | "media_channel_closed" => self.playing = false,
                        "playback_state" => self.playing = event["playing"] == true,
                        _ => {}
                    }
                }
                if let Some(dlna) = &self.dlna {
                    events.extend(dlna.poll());
                }
                let now = onset::now_ms();
                let onsets = onset::poll(self.capture_onsets).into_iter().filter(|e| {
                    e.time_ms.is_finite() && e.strength.is_finite() && now - e.time_ms < 100.0
                }).map(|e| {
                    self.onset_sequence += 1;
                    json!({"id": self.onset_sequence, "band": (["bass", "mid", "high", "drum"][e.band]),
                        "strength": e.strength, "percussion": e.percussion, "delay": e.time_ms - now})
                }).collect::<Vec<_>>();
                let spectrum = live_output_spectrum_take();
                Ok(json!({
                    "receiver": events, "onsets": onsets, "level": live_output_peak(),
                    "bands": [(spectrum >> 16) & 255, (spectrum >> 8) & 255, spectrum & 255],
                    "sender": if args["sender"] == true { self.sender.request(None)? } else { Value::Null },
                }))
            }
            _ => Err("不支持的核心命令".into()),
        }
    }
}

pub fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut host = Host {
        sender: Sender::new()?,
        dlna: None,
        playing: false,
        receiver_blocked: false,
        capture_onsets: false,
        onset_sequence: 0,
    };
    let mut output = io::BufWriter::new(io::stdout().lock());
    writeln!(output, "{{\"ready\":true}}")?;
    output.flush()?;
    for line in io::stdin().lock().lines() {
        let request: Value = match serde_json::from_str(&line?) {
            Ok(request) => request,
            Err(_) => continue,
        };
        let response = match host.request(&request) {
            Ok(value) => json!({"id": request["id"], "result": value}),
            Err(error) => json!({"id": request["id"], "error": error}),
        };
        serde_json::to_writer(&mut output, &response)?;
        writeln!(output)?;
        output.flush()?;
    }
    soundflow::stop_receiver();
    Ok(())
}
