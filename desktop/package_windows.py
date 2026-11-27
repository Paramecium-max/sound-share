#!/usr/bin/env python3
"""Build the Windows x64 host or package cross-compiled native binaries."""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

DESKTOP = Path(__file__).resolve().parent
ROOT = DESKTOP.parent
ANDROID = ROOT / "soundflow-android"
VERSION = "1.2.1"


def run(args, **options):
    subprocess.run([str(arg) for arg in args], check=True, **options)


def native_binaries():
    if os.name != "nt":
        raise RuntimeError("Pass --rust-core and --sender-dll when packaging a cross-compiled build")
    cmake_dir = ROOT / "core/build-soundflow-win"
    run(["cmake", "-S", ROOT / "core", "-B", cmake_dir, "-A", "x64"])
    run(["cmake", "--build", cmake_dir, "--config", "Release", "--target", "soundflow_sender"])
    run(["cargo", "build", "--locked", "--release", "--manifest-path", ANDROID / "Cargo.toml",
         "--target", "x86_64-pc-windows-msvc", "--target-dir", ANDROID / "build/desktop-target",
         "--bin", "soundflow-core"])
    return (
        ANDROID / "build/desktop-target/x86_64-pc-windows-msvc/release/soundflow-core.exe",
        cmake_dir / "Release/soundflow_sender.dll",
    )


def electron_runtime(stage, workspace, supplied):
    if supplied and supplied.is_dir():
        shutil.copytree(supplied, stage, dirs_exist_ok=True, ignore=shutil.ignore_patterns("resources"))
        exe = next((stage / name for name in ("electron.exe", "MiPlayMultiRoom.exe")
                    if (stage / name).is_file()), None)
    else:
        if not supplied:
            package = json.loads((DESKTOP / "package-lock.json").read_text(encoding="utf-8"))
            version = package["packages"]["node_modules/electron"]["version"]
            archive = f"electron-v{version}-win32-x64.zip"
            supplied = workspace / archive
            url = f"https://github.com/electron/electron/releases/download/v{version}/"
            urllib.request.urlretrieve(url + archive, supplied)
            checksums = urllib.request.urlopen(url + "SHASUMS256.txt", timeout=30).read().decode()
            expected = next(row.split()[0] for row in checksums.splitlines() if row.split()[-1] == archive)
            import hashlib
            with supplied.open("rb") as src:
                if hashlib.file_digest(src, "sha256").hexdigest() != expected:
                    raise RuntimeError("Electron archive checksum mismatch")
        with zipfile.ZipFile(supplied) as source:
            source.extractall(stage)
        exe = stage / "electron.exe"
    if not exe or not exe.is_file():
        raise RuntimeError("The Electron Windows distribution has no executable")
    exe.rename(stage / "SoundFlow.exe")
    default_app = stage / "resources/default_app.asar"
    default_app.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust-core", type=Path)
    parser.add_argument("--sender-dll", type=Path)
    parser.add_argument("--electron-dist", type=Path, help="Electron win32-x64 ZIP or directory")
    parser.add_argument("--output", type=Path, default=DESKTOP / "release")
    parser.add_argument("--zip-only", action="store_true")
    args = parser.parse_args()
    if bool(args.rust_core) != bool(args.sender_dll):
        parser.error("provide both --rust-core and --sender-dll")
    rust, sender = (args.rust_core, args.sender_dll) if args.rust_core else native_binaries()
    if not rust.is_file() or not sender.is_file():
        parser.error(f"missing native binaries: {rust}, {sender}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    run([os.sys.executable, ANDROID / "build.py", "--ui-only"])
    name = f"SoundFlow-{VERSION}-win-x64"
    with tempfile.TemporaryDirectory(prefix="soundflow-windows-") as temporary:
        workspace = Path(temporary)
        stage = workspace / name
        stage.mkdir()
        electron_runtime(stage, workspace, args.electron_dist)
        app = stage / "resources/app"
        app.mkdir(parents=True)
        (app / "package.json").write_text(json.dumps({
            "name": "soundflow", "version": VERSION, "main": "electron/soundflow-main.cjs",
        }), encoding="utf-8")
        host = app / "electron"
        host.mkdir()
        for file in ["soundflow-main.cjs", "soundflow-native.cjs", "soundflow-preload.cjs", "soundflow-lyrics.cjs", "smtc_bridge.cjs", "soundflow.ico"]:
            shutil.copy2(DESKTOP / "electron" / file, host / file)
        shutil.copytree(ANDROID / "assets/ui", app / "soundflow-ui")
        for file in ["LICENSE.txt", "AMLL-LICENSE.txt"]:
            shutil.copy2(ANDROID / "assets" / file, app / file)
        binaries = stage / "resources/bin"
        binaries.mkdir()
        shutil.copy2(rust, binaries / "soundflow-core.exe")
        shutil.copy2(sender, binaries / "soundflow_sender.dll")
        archive = shutil.make_archive(str(workspace / f"{name}-portable"), "zip", workspace, name)
        shutil.move(archive, output / Path(archive).name)
        if not args.zip_only:
            installer = workspace / f"{name}-setup.exe"
            prefix = "/" if os.name == "nt" else "-"
            run([os.environ.get("MAKENSIS", "makensis"), f"{prefix}DOUTPUT={installer}",
                 f"{prefix}DSTAGE={stage}", f"{prefix}DVERSION={VERSION}", DESKTOP / "SoundFlow.nsi"])
            shutil.move(installer, output / installer.name)
    print(output)


if __name__ == "__main__":
    main()
