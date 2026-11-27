fn main() {
    let target = std::env::var("CARGO_CFG_TARGET_OS").unwrap();
    if !matches!(target.as_str(), "android" | "linux") {
        return;
    }
    let core = std::path::Path::new("../../core");
    let mut build = cc::Build::new();
    build.cpp(true).std("c++20").include(core.join("src"));
    for file in [
        "src/media/packetizer_ffi.cpp",
        "src/media/ts_packetizer.cpp",
        "src/media/ts_packetizer.h",
        "src/media/rtp_framing.cpp",
        "src/media/rtp_sender.h",
        "src/sync/timer_ffi.cpp",
        "src/sync/timer_service.cpp",
        "src/sync/timer_service.h",
        "src/sync/master_clock.cpp",
        "src/sync/master_clock.h",
        "src/platform/net_platform.cpp",
        "src/platform/net_platform.h",
        "src/platform/deadline_write.h",
        "src/util/json.cpp",
        "src/util/json.h",
    ] {
        let path = core.join(file);
        println!("cargo:rerun-if-changed={}", path.display());
        if file.ends_with(".cpp") {
            build.file(path);
        }
    }
    if target == "android" {
        build.cpp_link_stdlib("c++_static");
    }
    build.compile("soundflow_sender_android");
}
