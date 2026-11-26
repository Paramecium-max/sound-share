#!/usr/bin/env python3
"""Build SoundFlow Standalone Audio Receiver APK and companion Magisk Systemization ZIP."""
from pathlib import Path
import hashlib
import json
import os
import secrets
import shutil
import struct
import subprocess
import zipfile

PROJ = Path(__file__).resolve().parent
ROOT = next((p for p in (PROJ.parent, PROJ.parent.parent) if (p / "拆包内容/tools").is_dir()), PROJ.parent)
TOOLS = Path(os.environ.get("SOUNDFLOW_TOOLS_DIR", str(ROOT / "拆包内容/tools")))
JAVA_BIN = TOOLS / "jdk17/usr/lib/jvm/java-17-openjdk-amd64/bin"
AAPT2 = TOOLS / "aapt2/aapt2"
SDK_JAR = TOOLS / "android-sdk/android-35-public.jar"
R8_JAR = TOOLS / "r8-8.3.37.jar"
APKSIGNER_JAR = TOOLS / "apksigner-local/usr/share/java/apksigner-31.0.2.jar"

BUILD = PROJ / "build"
BUILD.mkdir(exist_ok=True)
OUT_ROOT = Path(os.environ.get("SOUNDFLOW_OUT_DIR", str(ROOT / "输出内容")))
OUT_APP = OUT_ROOT / "应用"
OUT_ZIP = OUT_ROOT / "刷机包"
DEBUG_DIR = Path(os.environ.get("SOUNDFLOW_DEBUG_DIR", str(ROOT / "调试文件/音流独立接收器")))
OUT_APP.mkdir(parents=True, exist_ok=True)
OUT_ZIP.mkdir(parents=True, exist_ok=True)
DEBUG_DIR.mkdir(parents=True, exist_ok=True)

VERSION = "1.1.20"


def run(*args, **kw):
    print("+", *(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], check=True, **kw)


def build_ui_bundle():
    node_bin = TOOLS / "node/bin"
    esbuild = PROJ / "web/node_modules/.bin/esbuild"
    style_src = PROJ / "web/node_modules/@applemusic-like-lyrics/core/dist/style.css"
    assert esbuild.is_file(), f"Missing {esbuild}; run npm install in web/"
    assert style_src.is_file(), f"Missing {style_src}"
    ui_dir = PROJ / "assets/ui"
    ui_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(style_src, ui_dir / "amll-style.css")
    env = os.environ.copy()
    env["PATH"] = f"{node_bin}{os.pathsep}{env.get('PATH', '')}"
    run(
        esbuild,
        PROJ / "web/src/stage-app.js",
        "--bundle",
        "--minify",
        "--format=iife",
        "--platform=browser",
        "--target=chrome120",
        f"--outfile={ui_dir / 'app.bundle.js'}",
        env=env,
    )


def build_rust_so() -> Path:
    env = os.environ.copy()
    rust = TOOLS / "rust"
    ndk = TOOLS / "android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin"
    env["CARGO_HOME"] = str(rust / "cargo")
    env["RUSTUP_HOME"] = str(rust / "rustup")
    env["PATH"] = f"{rust / 'cargo/bin'}{os.pathsep}{ndk}{os.pathsep}{env.get('PATH', '')}"
    env["CARGO_TARGET_DIR"] = str(BUILD / "rust-target")
    env["CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER"] = str(ndk / "aarch64-linux-android28-clang")
    env["CC_aarch64_linux_android"] = str(ndk / "aarch64-linux-android28-clang")
    env["CXX_aarch64_linux_android"] = str(ndk / "aarch64-linux-android28-clang++")
    env["AR_aarch64_linux_android"] = str(ndk / "llvm-ar")
    env["RUSTFLAGS"] = "-C link-arg=-Wl,-z,max-page-size=16384 -C link-arg=-lc++abi -C link-arg=-Wl,-z,defs"

    run(
        rust / "cargo/bin/cargo",
        "build",
        "--offline",
        "--manifest-path",
        PROJ / "native/Cargo.toml",
        "--release",
        "--target",
        "aarch64-linux-android",
        env=env,
    )
    so = BUILD / "rust-target/aarch64-linux-android/release/libsoundflow.so"
    assert so.is_file(), f"Missing {so}"
    return so


def add_zip_entry(out: zipfile.ZipFile, name: str, data: bytes, stored: bool = False, align: int = 4):
    info = zipfile.ZipInfo(name, (2026, 11, 17, 14, 0, 0))
    info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    if stored:
        base = out.fp.tell() + 30 + len(name.encode("utf-8"))
        padding = (-(base + 4)) % align
        info.extra = struct.pack("<HH", 0xD935, padding) + (b"\0" * padding)
    out.writestr(info, data)


def build_apk(so_path: Path) -> Path:
    for d in ("classes", "dex", "gen"):
        shutil.rmtree(BUILD / d, ignore_errors=True)
        (BUILD / d).mkdir()

    run(AAPT2, "compile", "--dir", PROJ / "res", "-o", BUILD / "compiled-res.zip")
    run(
        AAPT2,
        "link",
        "-o",
        BUILD / "resources.apk",
        "--manifest",
        PROJ / "AndroidManifest.xml",
        "--java",
        BUILD / "gen",
        "-A",
        PROJ / "assets",
        "-I",
        SDK_JAR,
        BUILD / "compiled-res.zip",
    )

    java_srcs = sorted((PROJ / "src").rglob("*.java")) + sorted((BUILD / "gen").rglob("*.java"))
    run(
        JAVA_BIN / "javac",
        "-encoding",
        "UTF-8",
        "-source",
        "8",
        "-target",
        "8",
        "-cp",
        SDK_JAR,
        "-d",
        BUILD / "classes",
        *java_srcs,
    )

    class_files = sorted((BUILD / "classes").rglob("*.class"))
    run(
        JAVA_BIN / "java",
        "-cp",
        R8_JAR,
        "com.android.tools.r8.D8",
        "--min-api",
        "29",
        "--lib",
        SDK_JAR,
        "--output",
        BUILD / "dex",
        *class_files,
    )

    unsigned_apk = BUILD / "unsigned.apk"
    with zipfile.ZipFile(BUILD / "resources.apk") as zin, zipfile.ZipFile(unsigned_apk, "w") as zout:
        for item in zin.infolist():
            add_zip_entry(zout, item.filename, zin.read(item.filename), item.filename == "resources.arsc", 4)
        for dex in sorted((BUILD / "dex").glob("*.dex")):
            add_zip_entry(zout, dex.name, dex.read_bytes())
        add_zip_entry(zout, "lib/arm64-v8a/libsoundflow.so", so_path.read_bytes(), stored=True, align=16384)

    key_file = PROJ / "keys/soundflow.jks"
    pass_file = PROJ / "keys/signing.json"
    if not key_file.exists():
        pwd = secrets.token_urlsafe(24)
        pass_file.write_text(json.dumps({"password": pwd}), encoding="utf-8")
        pass_file.chmod(0o600)
        kenv = os.environ.copy()
        kenv["SF_KEY_PASS"] = pwd
        run(
            JAVA_BIN / "keytool",
            "-genkeypair",
            "-keystore",
            key_file,
            "-storepass:env",
            "SF_KEY_PASS",
            "-keypass:env",
            "SF_KEY_PASS",
            "-keyalg",
            "RSA",
            "-keysize",
            "3072",
            "-validity",
            "10000",
            "-alias",
            "soundflow",
            "-dname",
            "CN=SoundFlow Standalone Receiver",
            env=kenv,
        )
        key_file.chmod(0o600)

    sign_env = os.environ.copy()
    sign_env["SF_KEY_PASS"] = json.loads(pass_file.read_text(encoding="utf-8"))["password"]
    tmp_signed = BUILD / "candidate.apk"
    run(
        JAVA_BIN / "java",
        "-jar",
        APKSIGNER_JAR,
        "sign",
        "--ks",
        key_file,
        "--ks-pass",
        "env:SF_KEY_PASS",
        "--key-pass",
        "env:SF_KEY_PASS",
        "--out",
        tmp_signed,
        unsigned_apk,
        env=sign_env,
    )
    run(JAVA_BIN / "java", "-jar", APKSIGNER_JAR, "verify", "--verbose", tmp_signed)

    with zipfile.ZipFile(tmp_signed) as z:
        assert z.testzip() is None
        names = set(z.namelist())
        assert {
            "classes.dex",
            "AndroidManifest.xml",
            "resources.arsc",
            "lib/arm64-v8a/libsoundflow.so",
            "assets/ui/index.html",
            "assets/ui/app.bundle.js",
            "assets/ui/app.css",
            "assets/ui/redesign.css",
            "assets/ui/tokens.css",
            "assets/ui/amll-style.css",
            "assets/ui/refraction.js",
            "assets/magisk/module.prop",
            "assets/LICENSE.txt",
        }.issubset(names)
        for item in z.infolist():
            if item.compress_type == zipfile.ZIP_STORED:
                req_align = 16384 if item.filename.endswith(".so") else 4
                with tmp_signed.open("rb") as fp:
                    fp.seek(item.header_offset + 26)
                    fn_len, ex_len = struct.unpack("<HH", fp.read(4))
                    data_off = item.header_offset + 30 + fn_len + ex_len
                    assert data_off % req_align == 0, f"{item.filename} misaligned: {data_off} % {req_align}"
        # Optional Visualizer permission is for our DLNA session, not a mic input.
        assert all("new AudioRecord(" not in p.read_text() and "new MediaRecorder(" not in p.read_text()
                   for p in (PROJ / "src").rglob("*.java"))

    out_apk = OUT_APP / f"音流独立音频接收器_v{VERSION}.apk"
    shutil.move(str(tmp_signed), str(out_apk))
    return out_apk


def build_magisk_zip(apk_path: Path) -> Path:
    tmp_zip = BUILD / "magisk_candidate.zip"
    mod_priv_apk = "system/product/priv-app/SoundFlowReceiver/SoundFlowReceiver.apk"
    perm_rel = "system/product/etc/permissions/privapp-permissions-io.soundflow.receiver.xml"

    with zipfile.ZipFile(tmp_zip, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.write(PROJ / "assets/magisk/module.prop", "module.prop")
        z.write(PROJ / "magisk-module/customize.sh", "customize.sh")
        z.write(PROJ / "assets/magisk/common/service.sh", "service.sh")
        z.write(PROJ / "assets/magisk/system/product/etc/permissions/privapp-permissions-io.soundflow.receiver.xml", perm_rel)
        z.write(apk_path, mod_priv_apk)

    with zipfile.ZipFile(tmp_zip) as z:
        assert z.testzip() is None
        assert {"module.prop", "customize.sh", "service.sh", perm_rel, mod_priv_apk}.issubset(z.namelist())

    out_zip = OUT_ZIP / f"J716F_SoundFlow_独立音频接收器系统固化_v{VERSION}.zip"
    shutil.move(str(tmp_zip), str(out_zip))
    return out_zip


def main():
    build_ui_bundle()
    so = build_rust_so()
    apk = build_apk(so)
    magisk_zip = build_magisk_zip(apk)
    meta = {
        "version": VERSION,
        "package": "io.soundflow.receiver",
        "apk": str(apk.relative_to(ROOT)),
        "apk_bytes": apk.stat().st_size,
        "apk_sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
        "magisk_zip": str(magisk_zip.relative_to(ROOT)),
        "magisk_zip_bytes": magisk_zip.stat().st_size,
        "magisk_zip_sha256": hashlib.sha256(magisk_zip.read_bytes()).hexdigest(),
    }
    (DEBUG_DIR / "build-manifest.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(meta, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
