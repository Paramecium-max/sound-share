#!/usr/bin/env bash
set -euo pipefail

DEVICE="${ADB_SERIAL:-192.168.5.84:5555}"
IFACE="${CAPTURE_IFACE:-wlan0}"
DURATION="${1:-45}"
TAG="${2:-session}"
OUT_DIR="$(cd "$(dirname "$0")/.." && pwd)/captures"
TS="$(date +%Y%m%d_%H%M%S)"
PCAP_LOCAL="${OUT_DIR}/${TS}_${TAG}.pcap"
LOG_LOCAL="${OUT_DIR}/${TS}_${TAG}_logcat.txt"
PCAP_REMOTE="/data/local/tmp/miplay_${TS}.pcap"

mkdir -p "${OUT_DIR}"

echo "[*] 目标设备: ${DEVICE}"
echo "[*] 抓包网卡: ${IFACE}, 时长: ${DURATION}s, 标签: ${TAG}"

adb -s "${DEVICE}" shell "rm -f ${PCAP_REMOTE}"
AUDIO_PID=$(adb -s "${DEVICE}" shell "pidof com.milink.service:audio 2>/dev/null || ps -ef | grep 'com.milink.service:audio' | grep -v grep | awk '{print \$2}' | head -n 1" | tr -d '\r\n')
echo "[*] com.milink.service:audio PID: ${AUDIO_PID:-unknown}"

adb -s "${DEVICE}" logcat -c

if [[ -n "${AUDIO_PID}" ]]; then
  adb -s "${DEVICE}" logcat -v threadtime --pid="${AUDIO_PID}" > "${LOG_LOCAL}" 2>&1 &
else
  adb -s "${DEVICE}" logcat -v threadtime > "${LOG_LOCAL}" 2>&1 &
fi
LOG_PID=$!

cleanup() {
  kill "${LOG_PID}" 2>/dev/null || true
  adb -s "${DEVICE}" shell "pkill -INT tcpdump 2>/dev/null || true"
}
trap cleanup EXIT

echo "[*] 开始抓包 (${DURATION}s)..."
adb -s "${DEVICE}" shell "timeout -s INT ${DURATION} /system/bin/tcpdump -i ${IFACE} -s 0 -U -w ${PCAP_REMOTE} 'not port 5555'" || true

kill "${LOG_PID}" 2>/dev/null || true
wait "${LOG_PID}" 2>/dev/null || true

echo "[*] 拉取抓包文件到本地..."
adb -s "${DEVICE}" pull "${PCAP_REMOTE}" "${PCAP_LOCAL}"
adb -s "${DEVICE}" shell "rm -f ${PCAP_REMOTE}"

echo "[+] 抓包完成:"
echo "    PCAP: ${PCAP_LOCAL}"
echo "    LOG : ${LOG_LOCAL}"
