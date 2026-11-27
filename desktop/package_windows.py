#!/usr/bin/env python3
"""Build, Authenticode-sign, and verify the Windows x64 SoundFlow portable ZIP & NSIS installer."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import secrets
import shutil
import stat
import subprocess
import tempfile
import urllib.request
import zipfile

DESKTOP = Path(__file__).resolve().parent
ROOT = DESKTOP.parent
ANDROID = ROOT / "soundflow-android"
VERSION = "1.2.2"


def run(args, **options):
    subprocess.run([str(arg) for arg in args], check=True, **options)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


class AuthenticodeSigner:
    """Signs and strictly verifies Windows PE executables/DLLs via signtool or osslsigncode."""

    def __init__(self, pfx_path: Path | None, pass_env: str, timestamp_url: str | None):
        self.timestamp_url = timestamp_url
        self.tool_kind, self.tool_bin, self.tool_env = self._discover_tool()
        self.pfx_path, self.pfx_pass, self.ca_cert, self.cert_thumbprint = self._prepare_credentials(
            pfx_path, pass_env
        )

    @staticmethod
    def _discover_tool():
        env = os.environ.copy()
        if os.name == "nt":
            signtool = shutil.which("signtool") or shutil.which("signtool.exe")
            if signtool:
                return ("signtool", Path(signtool), env)
        explicit = os.environ.get("OSSLSIGNCODE")
        candidates = [
            Path(explicit) if explicit else None,
            Path(shutil.which("osslsigncode")) if shutil.which("osslsigncode") else None,
            Path("/tmp/soundflow-sign/root/usr/bin/osslsigncode"),
        ]
        for cand in candidates:
            if cand and cand.is_file():
                lib_dir = cand.parent.parent / "lib/x86_64-linux-gnu"
                if lib_dir.is_dir():
                    cur = env.get("LD_LIBRARY_PATH", "")
                    env["LD_LIBRARY_PATH"] = f"{lib_dir}{os.pathsep}{cur}" if cur else str(lib_dir)
                return ("osslsigncode", cand, env)
        raise RuntimeError(
            "Missing Authenticode signing tool: install osslsigncode (or signtool.exe on Windows)"
        )

    @staticmethod
    def _prepare_credentials(pfx_arg: Path | None, pass_env: str):
        env_pfx = os.environ.get("SOUNDFLOW_WIN_PFX")
        pfx = pfx_arg or (Path(env_pfx) if env_pfx else None)
        ca_env = os.environ.get("SOUNDFLOW_WIN_CA_CERT")
        ca_cert = Path(ca_env) if ca_env else None
        if pfx:
            password = os.environ.get(pass_env, "") or os.environ.get("SOUNDFLOW_WIN_PFX_PASS", "")
            if not pfx.is_file() or not password:
                raise RuntimeError(f"Set {pass_env} for signing certificate {pfx}")
            thumb = hashlib.sha256(pfx.read_bytes()).hexdigest()[:40]
            return (pfx.resolve(), password, ca_cert.resolve() if ca_cert and ca_cert.is_file() else None, thumb)

        # Managed local code-signing CA + leaf PFX stored outside VCS in desktop/.keys (mode 0700).
        keys_dir = DESKTOP / ".keys"
        keys_dir.mkdir(mode=0o700, exist_ok=True)
        pfx_file = keys_dir / "soundflow-authenticode.pfx"
        ca_file = keys_dir / "soundflow-codesign-ca.crt"
        meta_file = keys_dir / "signing.json"

        if pfx_file.is_file() and ca_file.is_file() and meta_file.is_file():
            meta = json.loads(meta_file.read_text(encoding="utf-8"))
            return (pfx_file, meta["password"], ca_file, meta["sha256_fingerprint"])

        openssl = shutil.which("openssl")
        if not openssl:
            raise RuntimeError("openssl is required to provision local Authenticode keypair")

        password = secrets.token_urlsafe(24)
        with tempfile.TemporaryDirectory(prefix="sf-codesign-") as tmpdir:
            tmp = Path(tmpdir)
            ca_key = tmp / "ca.key"
            leaf_key = tmp / "leaf.key"
            leaf_csr = tmp / "leaf.csr"
            leaf_crt = tmp / "leaf.crt"
            ext_cfg = tmp / "codesign.ext"
            ext_cfg.write_text(
                "basicConstraints = critical, CA:FALSE\n"
                "keyUsage = critical, digitalSignature\n"
                "extendedKeyUsage = critical, codeSigning\n"
                "subjectKeyIdentifier = hash\n"
                "authorityKeyIdentifier = keyid,issuer\n",
                encoding="utf-8",
            )
            run([
                openssl, "req", "-x509", "-newkey", "rsa:3072", "-nodes",
                "-keyout", ca_key, "-out", ca_file, "-days", "3650", "-sha256",
                "-subj", "/CN=SoundFlow Local Code Signing Root CA/O=SoundFlow",
                "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            ])
            run([
                openssl, "req", "-newkey", "rsa:3072", "-nodes",
                "-keyout", leaf_key, "-out", leaf_csr, "-sha256",
                "-subj", "/CN=SoundFlow Desktop Publisher/O=SoundFlow",
            ])
            run([
                openssl, "x509", "-req", "-in", leaf_csr, "-CA", ca_file, "-CAkey", ca_key,
                "-CAcreateserial", "-out", leaf_crt, "-days", "1825", "-sha256",
                "-extfile", ext_cfg,
            ])
            sign_env = {**os.environ, "SF_PFX_PASS": password}
            run([
                openssl, "pkcs12", "-export", "-out", pfx_file,
                "-inkey", leaf_key, "-in", leaf_crt, "-certfile", ca_file,
                "-passout", "env:SF_PFX_PASS",
            ], env=sign_env)
            fp_out = subprocess.check_output(
                [openssl, "x509", "-in", str(leaf_crt), "-noout", "-fingerprint", "-sha256"],
                text=True,
            ).strip()
            fingerprint = fp_out.split("=")[-1].replace(":", "").lower()

        os.chmod(pfx_file, 0o600)
        meta_payload = {"password": password, "sha256_fingerprint": fingerprint}
        with os.fdopen(
            os.open(meta_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w", encoding="utf-8"
        ) as sec:
            json.dump(meta_payload, sec)
        return (pfx_file, password, ca_file, fingerprint)

    def sign_file(self, target: Path):
        target = target.resolve()
        if self.tool_kind == "signtool":
            env = {**self.tool_env}
            cmd = [
                self.tool_bin, "sign", "/fd", "SHA256",
                "/f", self.pfx_path, "/p", self.pfx_pass,
            ]
            if self.timestamp_url:
                cmd += ["/tr", self.timestamp_url, "/td", "SHA256"]
            cmd.append(target)
            run(cmd, env=env)
        else:
            with tempfile.TemporaryDirectory(prefix="sf-pesign-") as tmpdir:
                tmp = Path(tmpdir)
                stripped = tmp / "unsigned.pe"
                signed = tmp / "signed.pe"
                pass_file = tmp / "pfx.pass"
                pass_file.write_text(self.pfx_pass, encoding="utf-8")
                os.chmod(pass_file, 0o600)

                # Strip pre-existing vendor signature (e.g. stock electron.exe) before applying unified Authenticode.
                strip_res = subprocess.run(
                    [str(self.tool_bin), "remove-signature", "-in", str(target), "-out", str(stripped)],
                    env=self.tool_env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                src = stripped if (strip_res.returncode == 0 and stripped.is_file()) else target
                cmd = [
                    self.tool_bin, "sign",
                    "-pkcs12", self.pfx_path,
                    "-readpass", pass_file,
                    "-h", "sha256",
                    "-n", "SoundFlow All-in-One Audio",
                    "-in", src,
                    "-out", signed,
                ]
                if self.timestamp_url:
                    cmd += ["-ts", self.timestamp_url]
                run(cmd, env=self.tool_env)
                shutil.move(str(signed), str(target))
        self.verify_file(target)

    def verify_file(self, target: Path):
        target = target.resolve()
        if self.tool_kind == "signtool":
            cmd = [self.tool_bin, "verify", "/pa", target] if not self.ca_cert else [self.tool_bin, "verify", "/v", target]
            run(cmd, env=self.tool_env)
        else:
            cmd = [self.tool_bin, "verify"]
            if self.ca_cert and self.ca_cert.is_file():
                cmd += ["-CAfile", self.ca_cert]
            cmd += ["-in", target]
            run(cmd, env=self.tool_env)


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
        exe = next((stage / name for name in ("electron.exe", "MiPlayMultiRoom.exe", "SoundFlow.exe")
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
            with supplied.open("rb") as src:
                if hashlib.file_digest(src, "sha256").hexdigest() != expected:
                    raise RuntimeError("Electron archive checksum mismatch")
        with zipfile.ZipFile(supplied) as source:
            source.extractall(stage)
        exe = stage / "electron.exe"
    if not exe or not exe.is_file():
        raise RuntimeError("The Electron Windows distribution has no executable")
    target_exe = stage / "SoundFlow.exe"
    if exe != target_exe:
        exe.rename(target_exe)
    default_app = stage / "resources/default_app.asar"
    default_app.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust-core", type=Path)
    parser.add_argument("--sender-dll", type=Path)
    parser.add_argument("--electron-dist", type=Path, help="Electron win32-x64 ZIP or directory")
    parser.add_argument("--output", type=Path, default=DESKTOP / "release")
    parser.add_argument("--zip-only", action="store_true")
    parser.add_argument("--sign-pfx", type=Path, help="PKCS#12 (.pfx) Authenticode certificate")
    parser.add_argument("--sign-pass-env", default="SOUNDFLOW_WIN_PFX_PASS")
    parser.add_argument("--timestamp-url", default=os.environ.get("SOUNDFLOW_TIMESTAMP_URL"))
    parser.add_argument("--capture-uninst", nargs=2, type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.capture_uninst:
        shutil.copy2(args.capture_uninst[0], args.capture_uninst[1])
        return

    signer = AuthenticodeSigner(args.sign_pfx, args.sign_pass_env, args.timestamp_url)

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
        for file in [
            "soundflow-main.cjs",
            "soundflow-native.cjs",
            "soundflow-preload.cjs",
            "soundflow-lyrics.cjs",
            "smtc_bridge.cjs",
            "soundflow.ico",
        ]:
            shutil.copy2(DESKTOP / "electron" / file, host / file)
        shutil.copytree(ANDROID / "assets/ui", app / "soundflow-ui")
        for file in ["LICENSE.txt", "AMLL-LICENSE.txt"]:
            shutil.copy2(ANDROID / "assets" / file, app / file)
        binaries = stage / "resources/bin"
        binaries.mkdir()
        shutil.copy2(rust, binaries / "soundflow-core.exe")
        shutil.copy2(sender, binaries / "soundflow_sender.dll")

        # Sign and verify all staged first-party / host PE files before archiving & NSIS packaging.
        signed_targets = [
            stage / "SoundFlow.exe",
            binaries / "soundflow-core.exe",
            binaries / "soundflow_sender.dll",
        ]
        for pe in signed_targets:
            signer.sign_file(pe)

        archive = Path(shutil.make_archive(str(workspace / f"{name}-portable"), "zip", workspace, name))
        out_zip = output / archive.name
        shutil.move(str(archive), str(out_zip))

        out_installer = None
        if not args.zip_only:
            makensis = os.environ.get("MAKENSIS", "makensis")
            prefix = "/" if os.name == "nt" else "-"

            # Stage 1: Emit standalone NSIS uninstaller via !uninstfinalize before any outer PE signature,
            # then Authenticode-sign Uninstall.exe independently so its PE security directory stays intact.
            uninst_carrier = workspace / "uninst-carrier.exe"
            uninstaller_exe = workspace / "Uninstall.exe"
            capture_cmd = f'"{os.sys.executable}" "{Path(__file__).resolve()}" --capture-uninst'
            run([
                makensis,
                f"{prefix}DOUTPUT={uninst_carrier}",
                f"{prefix}DBUILD_UNINSTALLER=1",
                f"{prefix}DCAPTURE_CMD={capture_cmd}",
                f"{prefix}DUNINST_OUT={uninstaller_exe}",
                DESKTOP / "SoundFlow.nsi",
            ])
            if not uninstaller_exe.is_file():
                raise RuntimeError("Failed to capture standalone Uninstall.exe from NSIS !uninstfinalize")
            signer.sign_file(uninstaller_exe)

            # Stage 2: Build the full installer embedding the signed Uninstall.exe, then Authenticode-sign setup.exe.
            installer = workspace / f"{name}-setup.exe"
            run([
                makensis,
                f"{prefix}DOUTPUT={installer}",
                f"{prefix}DSTAGE={stage}",
                f"{prefix}DUNINSTALLER_EXE={uninstaller_exe}",
                f"{prefix}DVERSION={VERSION}",
                DESKTOP / "SoundFlow.nsi",
            ])
            signer.sign_file(installer)
            out_installer = output / installer.name
            shutil.move(str(installer), str(out_installer))

        if signer.ca_cert and signer.ca_cert.is_file():
            shutil.copy2(signer.ca_cert, output / "soundflow-codesign-ca.crt")

        manifest = {
            "version": VERSION,
            "authenticode": {
                "tool": signer.tool_kind,
                "hash": "SHA-256",
                "certificate_sha256": signer.cert_thumbprint,
                "signed_binaries": [
                    "SoundFlow.exe",
                    "resources/bin/soundflow-core.exe",
                    "resources/bin/soundflow_sender.dll",
                    *(["Uninstall.exe", out_installer.name] if out_installer else []),
                ],
            },
            "portable_zip": out_zip.name,
            "portable_zip_bytes": out_zip.stat().st_size,
            "portable_zip_sha256": sha256_file(out_zip),
        }
        if out_installer:
            manifest["installer_exe"] = out_installer.name
            manifest["installer_exe_bytes"] = out_installer.stat().st_size
            manifest["installer_exe_sha256"] = sha256_file(out_installer)
        (output / "build-manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    print(output)


if __name__ == "__main__":
    main()
