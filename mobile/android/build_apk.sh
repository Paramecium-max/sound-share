#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
TOOLS="${TOOLS_DIR:-$(cd "$REPO/.." && pwd)/tools}"
if [ -n "${JAVA_HOME:-}" ] && [ -x "${JAVA_HOME}/bin/javac" ]; then
    JDK="$JAVA_HOME"
elif [ -x "$TOOLS/jdk17/usr/lib/jvm/java-17-openjdk-amd64/bin/javac" ]; then
    JDK="$TOOLS/jdk17/usr/lib/jvm/java-17-openjdk-amd64"
else
    JDK="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
fi
export JAVA_HOME="$JDK"
export PATH="$JDK/bin:$PATH"

if [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -d "${ANDROID_SDK_ROOT}" ]; then
    SDK="$ANDROID_SDK_ROOT"
elif [ -d "$TOOLS/android-sdk" ]; then
    SDK="$TOOLS/android-sdk"
else
    SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
fi
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
AAPT2="$SDK/build-tools/34.0.0/aapt2"
D8="$SDK/build-tools/34.0.0/d8"
ZIPALIGN="$SDK/build-tools/34.0.0/zipalign"
APKSIGNER="$SDK/build-tools/34.0.0/apksigner"

APP_MAIN="$ROOT/app/src/main"
WEB_DIST="$REPO/desktop/dist"
BUILD_DIR="$ROOT/build_tmp"
KEYSTORE="${KEYSTORE_PATH:-$ROOT/miplay_sender_release.jks}"
KEY_ALIAS="${KEYSTORE_ALIAS:-miplay}"
KEY_PASS="${KEYSTORE_PASS:-miplay123456}"
OUT_APK="${1:-$ROOT/build/sound-share-v0.2.0.apk}"

trap 'rm -rf "$BUILD_DIR"' EXIT

if [ ! -f "$WEB_DIST/index.html" ]; then
    echo "ERROR: $WEB_DIST/index.html not found. Run desktop build:renderer first." >&2
    exit 1
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/classes" "$BUILD_DIR/dex" "$BUILD_DIR/res_flat" "$BUILD_DIR/assets/www" "$BUILD_DIR/gen"

cp -r "$WEB_DIST"/* "$BUILD_DIR/assets/www/"

echo "=== [1/5] Compiling Android resources (aapt2) ==="
"$AAPT2" compile --dir "$APP_MAIN/res" -o "$BUILD_DIR/res_flat/res.zip"
"$AAPT2" link \
    -o "$BUILD_DIR/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$APP_MAIN/AndroidManifest.xml" \
    -A "$BUILD_DIR/assets" \
    --java "$BUILD_DIR/gen" \
    --min-sdk-version 29 \
    --target-sdk-version 34 \
    "$BUILD_DIR/res_flat/res.zip"

echo "=== [2/5] Compiling Java sources (javac --release 11) ==="
find "$APP_MAIN/java" "$BUILD_DIR/gen" -name "*.java" > "$BUILD_DIR/sources.list"
"$JDK/bin/javac" --release 11 -encoding UTF-8 -cp "$ANDROID_JAR" -d "$BUILD_DIR/classes" @"$BUILD_DIR/sources.list"

echo "=== [3/5] Converting bytecode to DEX (d8) ==="
find "$BUILD_DIR/classes" -name "*.class" > "$BUILD_DIR/classes.list"
JAVA_HOME="$JDK" "$D8" --min-api 29 --lib "$ANDROID_JAR" --output "$BUILD_DIR/dex" @"$BUILD_DIR/classes.list"

echo "=== [4/5] Packaging & aligning APK ==="
cp "$BUILD_DIR/base.apk" "$BUILD_DIR/unsigned.apk"
(cd "$BUILD_DIR/dex" && zip -q -u "$BUILD_DIR/unsigned.apk" classes*.dex)
"$ZIPALIGN" -f -p 4 "$BUILD_DIR/unsigned.apk" "$BUILD_DIR/aligned.apk"

echo "=== [5/5] Signing APK ==="
if [ ! -f "$KEYSTORE" ]; then
    "$JDK/bin/keytool" -genkeypair -v \
        -keystore "$KEYSTORE" \
        -alias "$KEY_ALIAS" \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass "$KEY_PASS" -keypass "$KEY_PASS" \
        -dname "CN=MiPlay MultiRoom, OU=SoundShare, O=Local, C=CN"
fi

mkdir -p "$(dirname "$OUT_APK")"
"$APKSIGNER" sign \
    --ks "$KEYSTORE" \
    --ks-key-alias "$KEY_ALIAS" \
    --ks-pass "pass:$KEY_PASS" \
    --key-pass "pass:$KEY_PASS" \
    --out "$OUT_APK" \
    "$BUILD_DIR/aligned.apk"

"$APKSIGNER" verify --verbose "$OUT_APK" > /dev/null
rm -f "${OUT_APK}.idsig"
echo "APK built & verified: $OUT_APK ($(du -h "$OUT_APK" | cut -f1))"
