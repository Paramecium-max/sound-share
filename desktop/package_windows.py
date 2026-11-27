#!/usr/bin/env python3
"""Build the Windows x64 SoundFlow portable directory (default for manual testing), with optional ZIP & NSIS installer."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
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


def sha256_stream(stream) -> str:
    digest = hashlib.sha256()
    for chunk in iter(lambda: stream.read(65536), b""):
        digest.update(chunk)
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return sha256_stream(stream)


class AuthenticodeSigner:
    """Signs and strictly verifies Windows PE executables/DLLs via signtool or osslsigncode."""

    def __init__(self, pfx_path: Path | None, pass_env: str, timestamp_url: str | None,
                 development: bool):
        self.development = development
        self.timestamp_url = timestamp_url
        self.store_sha1 = os.environ.get("SOUNDFLOW_WIN_CERT_SHA1", "").upper()
        pfx_env = os.environ.get("SOUNDFLOW_WIN_PFX")
        pfx = pfx_path or (Path(pfx_env) if pfx_env else None)
        if not development and not timestamp_url:
            raise RuntimeError("Production signing requires an RFC 3161 timestamp URL")
        if self.store_sha1 and (pfx or development or not re.fullmatch(r"[0-9A-F]{40}", self.store_sha1)):
            raise RuntimeError("Set SOUNDFLOW_WIN_CERT_SHA1 to a CurrentUser\\My certificate SHA-1 ID by itself")
        if not (pfx or self.store_sha1 or development):
            raise RuntimeError("Production signing requires a publisher certificate")
        if development and pfx:
            raise RuntimeError("Development signing uses the managed local CA")
        self.tool_kind, self.tool_bin, self.tool_env = self._discover_tool(self.store_sha1)
        self.ca_cert = None
        if self.store_sha1:
            script = (
                f"$cert = Get-Item -LiteralPath 'Cert:\\CurrentUser\\My\\{self.store_sha1}' -ErrorAction Stop; "
                "$sha = [Security.Cryptography.SHA256]::Create(); "
                "[BitConverter]::ToString($sha.ComputeHash($cert.RawData)).Replace('-', '').ToLowerInvariant()"
            )
            self.cert_thumbprint = subprocess.check_output(
                ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                text=True,
            ).strip()
        else:
            self.pfx_path, self.pfx_pass, self.ca_cert, self.cert_thumbprint = self._prepare_credentials(
                pfx, pass_env
            )
        if not re.fullmatch(r"[0-9a-f]{64}", self.cert_thumbprint):
            raise RuntimeError("Unable to determine the publisher certificate SHA-256 fingerprint")

    @staticmethod
    def _discover_tool(store_sha1):
        env = os.environ.copy()
        if store_sha1:
            signtool = shutil.which("signtool") or shutil.which("signtool.exe")
            if signtool:
                return ("signtool", Path(signtool), env)
            raise RuntimeError("signtool.exe is required for certificate-store signing")
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
        if pfx_arg:
            password = os.environ.get(pass_env, "")
            if not pfx_arg.is_file() or not password:
                raise RuntimeError(f"Set {pass_env} for signing certificate {pfx_arg}")
            openssl = shutil.which("openssl")
            if not openssl:
                raise RuntimeError("openssl is required to read the PKCS#12 signer certificate")
            leaf_pem = subprocess.check_output(
                [openssl, "pkcs12", "-in", str(pfx_arg), "-clcerts", "-nokeys",
                 "-passin", f"env:{pass_env}"],
            )
            leaf = subprocess.run(
                [openssl, "x509", "-outform", "DER"], check=True, input=leaf_pem, stdout=subprocess.PIPE,
            ).stdout
            return (pfx_arg.resolve(), password, None, hashlib.sha256(leaf).hexdigest())

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
            cmd = [self.tool_bin, "sign", "/fd", "SHA256", "/sha1", self.store_sha1]
            if self.timestamp_url:
                cmd += ["/tr", self.timestamp_url, "/td", "SHA256"]
            cmd.append(target)
            run(cmd, env=self.tool_env)
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
            run([self.tool_bin, "verify", "/pa", "/all", "/v", "/tw", target], env=self.tool_env)
            return

        cmd = [self.tool_bin, "verify", "-require-leaf-hash", f"sha256:{self.cert_thumbprint}"]
        if self.ca_cert:
            cmd += ["-CAfile", self.ca_cert]
        cmd += ["-in", target]
        result = subprocess.run(
            [str(arg) for arg in cmd], env=self.tool_env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True,
        )
        print(result.stdout, end="")
        result.check_returncode()
        if not re.search(r"^Message digest algorithm\s*:\s*SHA256\s*$", result.stdout, re.MULTILINE):
            raise RuntimeError(f"{target.name} does not have a SHA-256 Authenticode signature")
        if self.timestamp_url and "Timestamp Server Signature verification: ok" not in result.stdout:
            raise RuntimeError(f"{target.name} does not have a verified timestamp")


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
    package = json.loads((DESKTOP / "package-lock.json").read_text(encoding="utf-8"))
    version = package["packages"]["node_modules/electron"]["version"]
    archive = f"electron-v{version}-win32-x64.zip"
    url = f"https://github.com/electron/electron/releases/download/v{version}/"
    if supplied is not None and not supplied.is_file():
        raise RuntimeError("Provide the official Electron win32-x64 release ZIP")
    with urllib.request.urlopen(url + "SHASUMS256.txt", timeout=30) as published:
        checksums = (line.split() for line in published.read().decode().splitlines())
        expected = next((row[0] for row in checksums if len(row) == 2 and row[1].lstrip("*") == archive), "")
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise RuntimeError(f"Missing official Electron checksum for {archive}")
    if supplied is None:
        supplied = workspace / archive
        with urllib.request.urlopen(url + archive, timeout=60) as source, supplied.open("wb") as out:
            shutil.copyfileobj(source, out)
    with supplied.open("rb") as src:
        if sha256_stream(src) != expected:
            raise RuntimeError("Electron archive checksum mismatch")
        src.seek(0)
        with zipfile.ZipFile(src) as source:
            source.extractall(stage)
    exe = stage / "electron.exe"
    if not exe.is_file() or (stage / "version").read_text().strip().removeprefix("v") != version:
        raise RuntimeError("The Electron Windows distribution does not match the locked version")
    exe.rename(stage / "SoundFlow.exe")
    (stage / "resources/default_app.asar").unlink(missing_ok=True)
    return {"version": version, "archive_sha256": expected}


def write_uninstall_files(stage, include):
    files = sorted(path.relative_to(stage) for path in stage.rglob("*") if path.is_file())
    dirs = sorted(
        (path.relative_to(stage) for path in stage.rglob("*") if path.is_dir()),
        key=lambda path: (-len(path.parts), path),
    )

    def nsis_path(path):
        if any(not re.fullmatch(r"[A-Za-z0-9_. -]+", part) or part in (".", "..") for part in path.parts):
            raise RuntimeError(f"Unsupported NSIS payload path: {path}")
        return "$INSTDIR\\" + "\\".join(path.parts)

    lines = ["!macro SoundFlowVerifyOwnedDirs InvalidLabel"]
    for folder in [Path(), *dirs]:
        path = nsis_path(folder).rstrip("\\")
        lines.extend([
            f"""  System::Call 'kernel32::GetFileAttributesW(w "{path}") i.r0'""",
            '  ${If} $0 != -1',
            '    IntOp $0 $0 & 0x410',
            '    ${If} $0 != 0x10',
            '      Goto ${InvalidLabel}',
            '    ${EndIf}',
            '  ${EndIf}',
        ])
    lines.extend(["!macroend", "!macro SoundFlowDeleteOwnedFiles"])
    lines.extend(f'  Delete "{nsis_path(path)}"' for path in files)
    lines.extend(f'  RMDir "{nsis_path(path)}"' for path in dirs)
    lines.extend(["!macroend", ""])
    include.write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust-core", type=Path)
    parser.add_argument("--sender-dll", type=Path)
    parser.add_argument("--electron-dist", type=Path, help="Official Electron win32-x64 release ZIP")
    parser.add_argument("--output", type=Path, default=DESKTOP / "release")
    parser.add_argument("--zip", action="store_true", help="Also package portable ZIP archive")
    parser.add_argument("--installer", action="store_true", help="Also build NSIS setup installer")
    parser.add_argument("--zip-only", action="store_true", help="Alias for --zip without --installer")
    parser.add_argument("--sign-pfx", type=Path, help="PKCS#12 (.pfx) Authenticode certificate")
    parser.add_argument("--sign-pass-env", default="SOUNDFLOW_WIN_PFX_PASS")
    parser.add_argument("--timestamp-url", default=os.environ.get("SOUNDFLOW_TIMESTAMP_URL"))
    parser.add_argument("--development-signing", action="store_true")
    parser.add_argument("--capture-uninst", nargs=2, type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.capture_uninst:
        shutil.copy2(args.capture_uninst[0], args.capture_uninst[1])
        return

    build_zip = args.zip or args.zip_only
    build_installer = args.installer and not args.zip_only
    has_sign_config = bool(
        args.sign_pfx
        or args.development_signing
        or os.environ.get("SOUNDFLOW_WIN_PFX")
        or os.environ.get("SOUNDFLOW_WIN_CERT_SHA1")
    )
    need_signing = build_zip or build_installer or has_sign_config
    signer = (
        AuthenticodeSigner(
            args.sign_pfx, args.sign_pass_env, args.timestamp_url, args.development_signing
        )
        if need_signing
        else None
    )

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
        electron = electron_runtime(stage, workspace, args.electron_dist)
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

        signed_targets = [
            stage / "SoundFlow.exe",
            binaries / "soundflow-core.exe",
            binaries / "soundflow_sender.dll",
        ]
        if signer is not None:
            for pe in signed_targets:
                signer.sign_file(pe)

        out_portable_dir = output / name
        if out_portable_dir.exists():
            shutil.rmtree(out_portable_dir)
        shutil.copytree(stage, out_portable_dir)

        out_zip = None
        if build_zip:
            archive = Path(shutil.make_archive(str(workspace / f"{name}-portable"), "zip", workspace, name))
            out_zip = output / archive.name
            shutil.move(str(archive), str(out_zip))

        out_installer = None
        if build_installer:
            makensis = os.environ.get("MAKENSIS", "makensis")
            prefix = "/" if os.name == "nt" else "-"

            # Stage 1: Emit standalone NSIS uninstaller via !uninstfinalize before any outer PE signature,
            # then Authenticode-sign Uninstall.exe independently so its PE security directory stays intact.
            uninstall_files = workspace / "uninstall-files.nsh"
            write_uninstall_files(stage, uninstall_files)
            uninst_carrier = workspace / "uninst-carrier.exe"
            uninstaller_exe = workspace / "Uninstall.exe"
            capture_cmd = f'"{os.sys.executable}" "{Path(__file__).resolve()}" --capture-uninst'
            run([
                makensis,
                f"{prefix}DOUTPUT={uninst_carrier}",
                f"{prefix}DBUILD_UNINSTALLER=1",
                f"{prefix}DCAPTURE_CMD={capture_cmd}",
                f"{prefix}DUNINST_OUT={uninstaller_exe}",
                f"{prefix}DOWNED_FILES={uninstall_files}",
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
                f"{prefix}DOWNED_FILES={uninstall_files}",
                DESKTOP / "SoundFlow.nsi",
            ])
            signer.sign_file(installer)
            out_installer = output / installer.name
            shutil.move(str(installer), str(out_installer))

        if signer is not None and signer.ca_cert and signer.ca_cert.is_file():
            shutil.copy2(signer.ca_cert, output / "soundflow-codesign-ca.crt")

        manifest = {
            "version": VERSION,
            "mode": "release" if (out_zip or out_installer) else "portable_dir",
            "electron": electron,
            "portable_dir": out_portable_dir.name,
            "authenticode": (
                {
                    "signed": True,
                    "trust_scope": "development_local_ca" if signer.development else "publisher",
                    "rfc3161_timestamp": bool(signer.timestamp_url),
                    "tool": signer.tool_kind,
                    "hash": "SHA-256",
                    "certificate_sha256": signer.cert_thumbprint,
                    "signed_binaries": [
                        "SoundFlow.exe",
                        "resources/bin/soundflow-core.exe",
                        "resources/bin/soundflow_sender.dll",
                        *(["Uninstall.exe", out_installer.name] if out_installer else []),
                    ],
                }
                if signer is not None
                else {
                    "signed": False,
                    "trust_scope": "unsigned_portable_test",
                }
            ),
        }
        if out_zip:
            manifest["portable_zip"] = out_zip.name
            manifest["portable_zip_bytes"] = out_zip.stat().st_size
            manifest["portable_zip_sha256"] = sha256_file(out_zip)
        if out_installer:
            manifest["installer_exe"] = out_installer.name
            manifest["installer_exe_bytes"] = out_installer.stat().st_size
            manifest["installer_exe_sha256"] = sha256_file(out_installer)
        (output / "build-manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    print(out_portable_dir if not (out_zip or out_installer) else output)


if __name__ == "__main__":
    main()
