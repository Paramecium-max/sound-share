#!/usr/bin/env python3
"""Build SoundFlow All-in-One Receiver & Multi-Room Sender APK and companion Magisk ZIP."""
from pathlib import Path
import hashlib
import json
import os
import platform
import sys
import shutil
import secrets
import struct
import subprocess
import zipfile
import urllib.request

PROJ = Path(__file__).resolve().parent
ROOT = next((p for p in (PROJ.parent, PROJ.parent.parent) if (p / "拆包内容/tools").is_dir()), PROJ.parent)
TOOLS = Path(os.environ.get("SOUNDFLOW_TOOLS_DIR", str(ROOT / "拆包内容/tools")))
LOCAL_SDK = Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or (Path.home() / "Android/Sdk"))


def _pick_existing(*candidates: Path) -> Path:
    for c in candidates:
        if c and c.exists():
            return c
    return candidates[0]


_javac_sys = shutil.which("javac")
JAVA_BIN = _pick_existing(
    TOOLS / "jdk17/usr/lib/jvm/java-17-openjdk-amd64/bin",
    Path(_javac_sys).resolve().parent if _javac_sys else Path("/usr/bin"),
)
AAPT2 = _pick_existing(
    TOOLS / "aapt2/aapt2",
    LOCAL_SDK / "build-tools/35.0.0/aapt2",
    LOCAL_SDK / "build-tools/34.0.0/aapt2",
)
SDK_JAR = _pick_existing(
    Path(os.environ.get("SOUNDFLOW_ANDROID_JAR", TOOLS / "android-sdk/android-35-public.jar")),
    LOCAL_SDK / "platforms/android-35/android.jar",
    LOCAL_SDK / "platforms/android-34/android.jar",
)
R8_JAR = _pick_existing(
    TOOLS / "r8-8.3.37.jar",
    LOCAL_SDK / "build-tools/35.0.0/lib/d8.jar",
    LOCAL_SDK / "build-tools/34.0.0/lib/d8.jar",
)
APKSIGNER_JAR = _pick_existing(
    TOOLS / "apksigner-local/usr/share/java/apksigner-31.0.2.jar",
    LOCAL_SDK / "build-tools/35.0.0/lib/apksigner.jar",
    LOCAL_SDK / "build-tools/34.0.0/lib/apksigner.jar",
)

BUILD = Path(os.environ.get("SOUNDFLOW_BUILD_DIR", PROJ / "build")).resolve()
BUILD.mkdir(parents=True, exist_ok=True)
OUT_ROOT = Path(os.environ.get("SOUNDFLOW_OUT_DIR", str(ROOT / "输出内容")))
OUT_APP = OUT_ROOT / "应用"
OUT_ZIP = OUT_ROOT / "刷机包"
DEBUG_DIR = Path(os.environ.get("SOUNDFLOW_DEBUG_DIR", str(ROOT / "调试文件/音流独立接收器")))

VERSION = "1.2.4"


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


def verify_elf_alignment(path: Path):
    with path.open("rb") as elf:
        header = elf.read(64)
        if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01":
            raise ValueError(f"Expected a little-endian ELF64 library: {path}")
        kind, machine = struct.unpack_from("<HH", header, 16)
        if (kind, machine) != (3, 183):
            raise ValueError(f"Expected an AArch64 shared library: {path}")
        offset = struct.unpack_from("<Q", header, 32)[0]
        size, count = struct.unpack_from("<HH", header, 54)
        if size != 56 or not count:
            raise ValueError(f"Invalid ELF program header table: {path}")
        elf.seek(offset)
        loads = 0
        for _ in range(count):
            kind, _, file_offset, address, _, _, _, align = struct.unpack("<IIQQQQQQ", elf.read(size))
            if kind == 1:
                loads += 1
                if align != 0x4000 or (address - file_offset) % align:
                    raise ValueError(f"ELF LOAD is not 16 KiB aligned: {path}")
        if not loads:
            raise ValueError(f"No loadable ELF segments: {path}")


def build_rust_so() -> Path:
    prebuilt = os.environ.get("SOUNDFLOW_PREBUILT_SO")
    if prebuilt:
        so = Path(prebuilt)
        verify_elf_alignment(so)
        return so

    env = os.environ.copy()
    rust = TOOLS / "rust"
    bundled_cargo = rust / "cargo/bin/cargo"
    cargo = shutil.which("cargo")
    if not cargo and bundled_cargo.is_file():
        cargo = str(bundled_cargo)
        env.setdefault("CARGO_HOME", str(rust / "cargo"))
        env.setdefault("RUSTUP_HOME", str(rust / "rustup"))
    if not cargo:
        raise RuntimeError("Install Cargo and the aarch64-linux-android Rust target")

    ndk_override = env.get("ANDROID_NDK_HOME") or env.get("ANDROID_NDK_ROOT")
    if ndk_override:
        candidates = [Path(ndk_override)]
    else:
        sdk_ndks = LOCAL_SDK / "ndk"
        def ndk_version(path):
            return tuple(int(part) if part.isdigit() else 0 for part in path.name.split("."))
        candidates = sorted(sdk_ndks.glob("*"), key=ndk_version, reverse=True)
        candidates += sorted(TOOLS.glob("android-ndk-*"), reverse=True)
        candidates += [LOCAL_SDK / "ndk-bundle"]

    windows = platform.system() == "Windows"
    clang = "aarch64-linux-android28-clang" + (".cmd" if windows else "")
    tool_bin = next((bin_dir for ndk in candidates
                     for bin_dir in ndk.glob("toolchains/llvm/prebuilt/*/bin")
                     if (bin_dir / clang).is_file()), None)
    if tool_bin is None:
        raise RuntimeError("Install an Android NDK or set ANDROID_NDK_HOME")

    env["PATH"] = os.pathsep.join([str(Path(cargo).parent), str(tool_bin), env.get("PATH", "")])
    env["CARGO_TARGET_DIR"] = str(BUILD / "rust-target")
    env["CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER"] = str(tool_bin / clang)
    env["CC_aarch64_linux_android"] = str(tool_bin / clang)
    env["CXX_aarch64_linux_android"] = str(tool_bin / clang.replace("clang", "clang++"))
    env["AR_aarch64_linux_android"] = str(tool_bin / ("llvm-ar.exe" if windows else "llvm-ar"))
    env["RUSTFLAGS"] = (env.get("RUSTFLAGS", "") +
                        " -C link-arg=-Wl,-z,max-page-size=16384 -C link-arg=-lc++abi -C link-arg=-Wl,-z,defs").strip()
    run(cargo, "build", "--locked", "--manifest-path", PROJ / "Cargo.toml",
        "-p", "soundflow-receiver", "--lib", "--release", "--target", "aarch64-linux-android", env=env)
    so = BUILD / "rust-target/aarch64-linux-android/release/libsoundflow.so"
    verify_elf_alignment(so)
    return so


def add_zip_entry(out: zipfile.ZipFile, name: str, data: bytes, stored: bool = False, align: int = 4):
    info = zipfile.ZipInfo(name, (2026, 11, 17, 14, 0, 0))
    info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    if stored:
        base = out.fp.tell() + 30 + len(name.encode("utf-8"))
        padding = (-(base + 4)) % align
        info.extra = struct.pack("<HH", 0xD935, padding) + (b"\0" * padding)
    out.writestr(info, data)


ADB_DEPENDENCIES = (
    ("libadb-3.1.1.aar",
     "https://jitpack.io/com/github/MuntashirAkon/libadb-android/3.1.1/libadb-android-3.1.1.aar",
     "e6fcb495a20a507ca4a125afc9c554e838a84b3a15fbcc29f7deafd0f88bbb58"),
    ("spake2-android-2.2.1.aar",
     "https://jitpack.io/com/github/MuntashirAkon/spake2-java/spake2-android/2.2.1/spake2-android-2.2.1.aar",
     "8798fb6c04b5d53a6307ed9481e9afe563227abd4e8b9e917c8514fa850071bb"),
    ("conscrypt-android-2.5.3.aar",
     "https://repo.maven.apache.org/maven2/org/conscrypt/conscrypt-android/2.5.3/conscrypt-android-2.5.3.aar",
     "551ae4e301c571760d1791e647db6ed1dcb10d34dcae7aa12b67f220f2ce98d1"),
    ("bcprov-jdk15to18-1.81.jar",
     "https://repo.maven.apache.org/maven2/org/bouncycastle/bcprov-jdk15to18/1.81/bcprov-jdk15to18-1.81.jar",
     "0ef9c4d9536719aaa1c92a7c694d87981fa9d4d64dd9479cc322a08aebbc651e"),
)


def prepare_adb_dependencies():
    cache = BUILD / "adb-dependencies"
    cache.mkdir(parents=True, exist_ok=True)
    jars = []
    libraries = {}
    for filename, url, digest in ADB_DEPENDENCIES:
        archive = cache / filename
        if not archive.is_file():
            with urllib.request.urlopen(url, timeout=25) as response:
                payload = response.read(16 * 1024 * 1024)
            if hashlib.sha256(payload).hexdigest() != digest:
                raise ValueError(f"Checksum mismatch for {filename}")
            archive.write_bytes(payload)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Checksum mismatch for cached {filename}")
        if archive.suffix == ".jar":
            jars.append(archive)
            continue
        classes = archive.with_suffix(".jar")
        with zipfile.ZipFile(archive) as component:
            classes.write_bytes(component.read("classes.jar"))
            for name in component.namelist():
                if name.startswith("jni/arm64-v8a/") and name.endswith(".so"):
                    library = cache / Path(name).name
                    library.write_bytes(component.read(name))
                    verify_elf_alignment(library)
                    libraries[name.replace("jni/", "lib/", 1)] = library
        jars.append(classes)
    return jars, libraries


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

    stub_srcs = sorted((PROJ / "stubs").rglob("*.java"))
    stub_classes = BUILD / "stub_classes"
    extra_cp = []
    d8_libs = ["--lib", SDK_JAR]
    if stub_srcs:
        stub_classes.mkdir(parents=True, exist_ok=True)
        run(
            JAVA_BIN / "javac",
            "-encoding",
            "UTF-8",
            "-source",
            "11",
            "-target",
            "11",
            "-cp",
            str(SDK_JAR),
            "-d",
            stub_classes,
            *stub_srcs,
        )
        extra_cp.append(str(stub_classes))
        d8_libs.extend(["--lib", stub_classes])

    java_srcs = sorted((PROJ / "src").rglob("*.java")) + sorted((BUILD / "gen").rglob("*.java"))
    adb_jars, adb_libraries = prepare_adb_dependencies()
    lib_jars = sorted((PROJ / "libs").glob("*.jar")) + adb_jars
    javac_cp = os.pathsep.join([str(SDK_JAR), *extra_cp, *(str(j) for j in lib_jars)])
    run(
        JAVA_BIN / "javac",
        "-encoding",
        "UTF-8",
        "-source",
        "11",
        "-target",
        "11",
        "-cp",
        javac_cp,
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
        *d8_libs,
        "--output",
        BUILD / "dex",
        *class_files,
        *lib_jars,
    )

    unsigned_apk = BUILD / "unsigned.apk"
    with zipfile.ZipFile(BUILD / "resources.apk") as zin, zipfile.ZipFile(unsigned_apk, "w") as zout:
        for item in zin.infolist():
            add_zip_entry(zout, item.filename, zin.read(item.filename), item.filename == "resources.arsc", 4)
        for dex in sorted((BUILD / "dex").glob("*.dex")):
            add_zip_entry(zout, dex.name, dex.read_bytes())
        add_zip_entry(zout, "lib/arm64-v8a/libsoundflow.so", so_path.read_bytes(), stored=True, align=16384)
        for name, library in adb_libraries.items():
            add_zip_entry(zout, name, library.read_bytes(), stored=True, align=16384)

    keys = PROJ / "keys"
    external_key = os.environ.get("SOUNDFLOW_KS_FILE")
    key_file = Path(external_key) if external_key else keys / "soundflow.jks"
    pass_file = keys / "signing.json"
    ks_alias = os.environ.get("SOUNDFLOW_KS_ALIAS", "")
    ks_pass = os.environ.get("SOUNDFLOW_KS_PASS", "")
    if not ks_pass and not external_key and pass_file.is_file():
        ks_pass = json.loads(pass_file.read_text(encoding="utf-8"))["password"]
    if not external_key and not key_file.exists() and not ks_pass:
        keys.mkdir(mode=0o700, exist_ok=True)
        ks_pass = secrets.token_urlsafe(24)
        ks_alias = ks_alias or "soundflow"
        key_env = {**os.environ, "SF_KEY_PASS": ks_pass}
        run(JAVA_BIN / "keytool", "-genkeypair", "-keystore", key_file, "-alias", ks_alias,
            "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
            "-storepass:env", "SF_KEY_PASS", "-keypass:env", "SF_KEY_PASS",
            "-dname", "CN=SoundFlow, O=Local", env=key_env)
        with os.fdopen(os.open(pass_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as secret:
            json.dump({"password": ks_pass}, secret)
    if not key_file.is_file() or not ks_pass:
        raise RuntimeError("Set SOUNDFLOW_KS_FILE and SOUNDFLOW_KS_PASS to sign with an existing key")
    sign_env = {**os.environ, "SF_KEY_PASS": ks_pass}
    tmp_signed = BUILD / "candidate.apk"
    sign_cmd = [
        JAVA_BIN / "java",
        "-jar",
        APKSIGNER_JAR,
        "sign",
        "--ks",
        key_file,
    ]
    if ks_alias:
        sign_cmd += ["--ks-key-alias", ks_alias]
    sign_cmd += [
        "--ks-pass",
        "env:SF_KEY_PASS",
        "--key-pass",
        "env:SF_KEY_PASS",
        "--out",
        tmp_signed,
        unsigned_apk,
    ]
    run(*sign_cmd, env=sign_env)
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
        # RECORD_AUDIO is strictly for DLNA session Visualizer and AudioPlaybackCapture internal loopback; microphone capture is forbidden.
        forbidden_mic_apis = ("new AudioRecord(", "new MediaRecorder(", "MediaRecorder.AudioSource", "setAudioSource(")
        for p in (PROJ / "src").rglob("*.java"):
            src_txt = p.read_text(encoding="utf-8")
            assert all(tok not in src_txt for tok in forbidden_mic_apis), f"Forbidden mic API in {p}"

    out_apk = OUT_APP / f"音流全屋收发一体_v{VERSION}.apk"
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
    if sys.argv[1:] == ["--ui-only"]:
        return
    for directory in (OUT_APP, OUT_ZIP, DEBUG_DIR):
        directory.mkdir(parents=True, exist_ok=True)
    so = build_rust_so()
    apk = build_apk(so)
    magisk_zip = build_magisk_zip(apk)
    meta = {
        "version": VERSION,
        "package": "io.soundflow.receiver",
        "apk": os.path.relpath(apk, ROOT),
        "apk_bytes": apk.stat().st_size,
        "apk_sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
        "magisk_zip": os.path.relpath(magisk_zip, ROOT),
        "magisk_zip_bytes": magisk_zip.stat().st_size,
        "magisk_zip_sha256": hashlib.sha256(magisk_zip.read_bytes()).hexdigest(),
    }
    manifest = json.dumps(meta, ensure_ascii=False, indent=2) + "\n"
    for directory in dict.fromkeys((OUT_ROOT.resolve(), DEBUG_DIR.resolve())):
        temporary = directory / "build-manifest.json.tmp"
        temporary.write_text(manifest, encoding="utf-8")
        temporary.replace(directory / "build-manifest.json")
    print(json.dumps(meta, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
