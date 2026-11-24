#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
APP_DIR="${ROOT_DIR}/mobile/android/app"
SDK_DIR="${ANDROID_HOME:-$HOME/Android/Sdk}"
BT="${SDK_DIR}/build-tools/34.0.0"
ANDROID_JAR="${SDK_DIR}/platforms/android-34/android.jar"
BUILD_DIR="${ROOT_DIR}/mobile/android/build"
KEYSTORE="${BUILD_DIR}/debug.keystore"
APK_OUT="${BUILD_DIR}/miplay-multiroom-debug.apk"
DEVICE="${ADB_SERIAL:-192.168.5.84:5555}"

echo "[1/7] 同步最新桌面端全屋控制台 UI -> Android assets/www..."
(cd "${ROOT_DIR}/desktop" && npm run build:renderer)
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}/res_compiled" "${BUILD_DIR}/gen" "${BUILD_DIR}/classes" "${BUILD_DIR}/dex" "${BUILD_DIR}/assets/www"
cp -r "${ROOT_DIR}/desktop/dist/"* "${BUILD_DIR}/assets/www/"

# 在 index.html 头部提前注入同步 shim，确保 React 首次 mount 时 window.miplayBridge 已就绪
python3 - <<PY
p = "${BUILD_DIR}/assets/www/index.html"
with open(p, "r", encoding="utf-8") as f:
    html = f.read()
shim = """<script>
if (window._androidMiPlayBridge && !window.miplayBridge) {
  window.miplayBridge = {
    platform: 'android',
    getSidecarConfig: async () => JSON.parse(window._androidMiPlayBridge.getSidecarConfigJson()),
    openWavFile: async () => { window._androidMiPlayBridge.openWavPicker(); return null; }
  };
}
</script>"""
html = html.replace("<head>", "<head>" + shim, 1)
with open(p, "w", encoding="utf-8") as f:
    f.write(html)
PY

echo "[2/7] aapt2 编译资源..."
"${BT}/aapt2" compile --dir "${APP_DIR}/src/main/res" -o "${BUILD_DIR}/res_compiled/res.zip"

echo "[3/7] aapt2 链接生成 R.java 与未打包资源 APK..."
"${BT}/aapt2" link \
  -I "${ANDROID_JAR}" \
  --manifest "${APP_DIR}/src/main/AndroidManifest.xml" \
  -A "${BUILD_DIR}/assets" \
  --java "${BUILD_DIR}/gen" \
  -o "${BUILD_DIR}/unsigned_unaligned.apk" \
  "${BUILD_DIR}/res_compiled/res.zip"

echo "[4/7] javac 编译 Android Java 源码..."
JAVA_FILES=$(find "${APP_DIR}/src/main/java" "${BUILD_DIR}/gen" -name "*.java")
javac --release 11 -encoding UTF-8 \
  -classpath "${ANDROID_JAR}" \
  -d "${BUILD_DIR}/classes" \
  ${JAVA_FILES}

echo "[5/7] d8 转换为 classes.dex..."
CLASS_FILES=$(find "${BUILD_DIR}/classes" -name "*.class")
"${BT}/d8" --min-api 29 --lib "${ANDROID_JAR}" --output "${BUILD_DIR}/dex" ${CLASS_FILES}

echo "[6/7] 合并 DEX、zipalign 对齐并 apksigner 签名..."
(cd "${BUILD_DIR}/dex" && zip -q -u "${BUILD_DIR}/unsigned_unaligned.apk" classes.dex)
"${BT}/zipalign" -f -p 4 "${BUILD_DIR}/unsigned_unaligned.apk" "${BUILD_DIR}/aligned.apk"

if [[ ! -f "${KEYSTORE}" ]]; then
  keytool -genkeypair -v -keystore "${KEYSTORE}" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=MiPlayDebug,O=MiShare,C=CN" >/dev/null 2>&1
fi

"${BT}/apksigner" sign \
  --ks "${KEYSTORE}" \
  --ks-pass pass:android \
  --ks-key-alias androiddebugkey \
  --key-pass pass:android \
  --out "${APK_OUT}" \
  "${BUILD_DIR}/aligned.apk"

"${BT}/apksigner" verify "${APK_OUT}"
echo "[+] APK 构建并签名成功: ${APK_OUT} ($(du -h "${APK_OUT}" | awk '{print $1}'))"

echo "[7/7] 通过 ADB 安装到红米 K90 (${DEVICE}) 并启动验证..."
adb connect "${DEVICE}" >/dev/null 2>&1 || true
adb -s "${DEVICE}" install -r -g "${APK_OUT}"
adb -s "${DEVICE}" shell am start -n com.mishare.miplay/.MainActivity
sleep 2
adb -s "${DEVICE}" shell "ps -ef | grep com.mishare.miplay | grep -v grep"
echo "[+] 已成功安装并启动「全屋妙播 (com.mishare.miplay)」在红米 K90 上！"
