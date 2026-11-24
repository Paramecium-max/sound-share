#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CORE_DIR="${ROOT_DIR}/core"
DESKTOP_DIR="${ROOT_DIR}/desktop"
OUT_ROOT="${ROOT_DIR}/release"
STAGE_DIR="${OUT_ROOT}/MiPlay-MultiRoom-win32-x64"
ZIP_OUT="${OUT_ROOT}/MiPlay-MultiRoom-win32-x64.zip"
ZIG="${HOME}/.local/zig/zig"
ELECTRON_ZIP="/tmp/electron-win32-x64.zip"

if [[ ! -x "${ZIG}" ]]; then
  echo "ERROR: Zig cross-compiler not found at ${ZIG}" >&2
  exit 1
fi
if [[ ! -f "${ELECTRON_ZIP}" ]]; then
  echo "ERROR: Electron win32 zip not found at ${ELECTRON_ZIP}" >&2
  exit 1
fi

echo "[1/4] 交叉编译 Windows x64 原生引擎 miplay_core_sidecar.exe (含 WASAPI 环回抓音, 零 DLL 依赖)..."
mkdir -p "${CORE_DIR}/build-win64"
"${ZIG}" c++ -target x86_64-windows-gnu -std=c++20 -O2 \
  -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
  -I"${CORE_DIR}/include" -I"${CORE_DIR}/src" \
  "${CORE_DIR}/src/crypto/mini_crypto.cpp" \
  "${CORE_DIR}/src/platform/net_platform.cpp" \
  "${CORE_DIR}/src/sync/master_clock.cpp" \
  "${CORE_DIR}/src/sync/timer_service.cpp" \
  "${CORE_DIR}/src/discovery/mdns_discovery.cpp" \
  "${CORE_DIR}/src/cmd/cmd_codec.cpp" \
  "${CORE_DIR}/src/cmd/cmd_session.cpp" \
  "${CORE_DIR}/src/media/ts_packetizer.cpp" \
  "${CORE_DIR}/src/media/rtp_sender.cpp" \
  "${CORE_DIR}/src/media/aac_encoder.cpp" \
  "${CORE_DIR}/src/media/audio_pipeline.cpp" \
  "${CORE_DIR}/src/rtsp/wfd_rtsp_server.cpp" \
  "${CORE_DIR}/src/engine/multiroom_engine.cpp" \
  "${CORE_DIR}/src/c_api/miplay_c_api.cpp" \
  "${CORE_DIR}/src/sidecar/ws_server.cpp" \
  "${CORE_DIR}/src/sidecar/main_sidecar.cpp" \
  -lws2_32 -liphlpapi -lole32 -luuid \
  -static \
  -o "${CORE_DIR}/build-win64/miplay_core_sidecar.exe"

file "${CORE_DIR}/build-win64/miplay_core_sidecar.exe"
ls -lh "${CORE_DIR}/build-win64/miplay_core_sidecar.exe"

echo "[2/4] 构建最新 Electron 前端..."
(cd "${DESKTOP_DIR}" && npm run build:renderer)

echo "[3/4] 组装 Windows x64 Electron 绿色便携版..."
rm -rf "${STAGE_DIR}" "${ZIP_OUT}"
mkdir -p "${STAGE_DIR}"
unzip -q "${ELECTRON_ZIP}" -d "${STAGE_DIR}"

# 重命名启动程序为 MiPlayMultiRoom.exe
mv "${STAGE_DIR}/electron.exe" "${STAGE_DIR}/MiPlayMultiRoom.exe"

# 移除默认 demo app，装入正式前端 + Electron 主进程 + 原生 Sidecar.exe
rm -rf "${STAGE_DIR}/resources/default_app.asar"
mkdir -p "${STAGE_DIR}/resources/app/electron" "${STAGE_DIR}/resources/app/dist" "${STAGE_DIR}/resources/bin"
cp "${DESKTOP_DIR}/package.json" "${STAGE_DIR}/resources/app/"
cp "${DESKTOP_DIR}/electron/main.cjs" "${DESKTOP_DIR}/electron/preload.cjs" "${STAGE_DIR}/resources/app/electron/"
cp -r "${DESKTOP_DIR}/dist/"* "${STAGE_DIR}/resources/app/dist/"
cp "${CORE_DIR}/build-win64/miplay_core_sidecar.exe" "${STAGE_DIR}/resources/bin/miplay_core_sidecar.exe"

echo "[4/4] 打包便携版 ZIP..."
(cd "${OUT_ROOT}" && zip -q -r "${ZIP_OUT}" "MiPlay-MultiRoom-win32-x64")

echo "[+] Windows x64 构建完成!"
echo "    便携目录: ${STAGE_DIR}"
echo "    便携 ZIP: ${ZIP_OUT} ($(du -h "${ZIP_OUT}" | awk '{print $1}'))"
echo "    独立核心: ${CORE_DIR}/build-win64/miplay_core_sidecar.exe"
