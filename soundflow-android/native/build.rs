fn main() {
    let target = std::env::var("CARGO_CFG_TARGET_OS").unwrap();
    if !matches!(target.as_str(), "android" | "linux") {
        return;
    }
    let core = std::path::Path::new("../../core");
    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++20")
        .include(core.join("include"))
        .include(core.join("src"));
    for file in [
        "src/crypto/mini_crypto.cpp",
        "src/crypto/mini_crypto.h",
        "src/util/json.cpp",
        "src/util/json.h",
        "src/util/utf8.h",
        "src/platform/net_platform.cpp",
        "src/platform/net_platform.h",
        "src/platform/deadline_write.h",
        "src/sync/master_clock.cpp",
        "src/sync/master_clock.h",
        "src/sync/timer_service.cpp",
        "src/sync/timer_service.h",
        "src/discovery/mdns_discovery.cpp",
        "src/discovery/mdns_discovery.h",
        "src/discovery/probe_burst.h",
        "src/cmd/cmd_codec.cpp",
        "src/cmd/cmd_codec.h",
        "src/cmd/cmd_session.cpp",
        "src/cmd/cmd_session.h",
        "src/media/ts_packetizer.cpp",
        "src/media/ts_packetizer.h",
        "src/media/rtp_sender.cpp",
        "src/media/rtp_sender.h",
        "src/media/rtp_framing.cpp",
        "src/media/aac_encoder.cpp",
        "src/media/aac_encoder.h",
        "src/media/aac_timeline.h",
        "src/media/audio_pipeline.cpp",
        "src/media/audio_pipeline.h",
        "src/media/process_loopback.cpp",
        "src/media/process_loopback.h",
        "src/media/per_app_route.cpp",
        "src/media/per_app_route.h",
        "src/media/media_control.cpp",
        "src/media/media_control.h",
        "src/rtsp/wfd_rtsp_server.cpp",
        "src/rtsp/wfd_rtsp_server.h",
        "src/engine/multiroom_engine.cpp",
        "src/engine/multiroom_engine.h",
        "src/sidecar/sender_rpc.cpp",
        "src/sidecar/sender_rpc.h",
        "src/sidecar/sender_ffi.cpp",
    ] {
        let path = core.join(file);
        println!("cargo:rerun-if-changed={}", path.display());
        if file.ends_with(".cpp") {
            build.file(path);
        }
    }
    if target == "android" {
        build.cpp_link_stdlib("c++_static");
        println!("cargo:rustc-link-lib=mediandk");
    }
    build.compile("soundflow_sender_android");
}
