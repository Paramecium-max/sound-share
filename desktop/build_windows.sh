#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/core"
OUT="$CORE/build-soundflow-win"
ZIG="${SOUNDFLOW_ZIG:-$HOME/.local/zig/zig}"
TARGET=x86_64-pc-windows-gnu

mkdir -p "$OUT"
mapfile -t CORE_SOURCES < <(sed -n '/add_library(miplay_core STATIC/,/^)/s/^    \(src.*\.cpp\)/\1/p' "$CORE/CMakeLists.txt")
for i in "${!CORE_SOURCES[@]}"; do CORE_SOURCES[$i]="$CORE/${CORE_SOURCES[$i]}"; done
"$ZIG" c++ -target x86_64-windows-gnu -std=c++20 -O2 \
  -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
  -I"$CORE/include" -I"$CORE/src" "${CORE_SOURCES[@]}" "$CORE/src/sidecar/sender_ffi.cpp" \
  -lws2_32 -liphlpapi -lole32 -luuid -luser32 -static -shared -o "$OUT/soundflow_sender.dll"

export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER="${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-x86_64-w64-mingw32-gcc}"
export CC_x86_64_pc_windows_gnu="${CC_x86_64_pc_windows_gnu:-$CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER}"
cargo build --locked --release --target "$TARGET" --target-dir "$OUT/rust" \
  --manifest-path "$ROOT/soundflow-android/Cargo.toml" --bin soundflow-core
python3 "$ROOT/desktop/package_windows.py" \
  --rust-core "$OUT/rust/$TARGET/release/soundflow-core.exe" --sender-dll "$OUT/soundflow_sender.dll" \
  --output "$ROOT/release" "$@"
