#[cfg(target_os = "android")]
fn main() {}

#[cfg(not(target_os = "android"))]
#[path = "../desktop_core.rs"]
mod desktop_core;

#[cfg(not(target_os = "android"))]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    desktop_core::run()
}
