import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main(core_tests):
    root = Path(__file__).resolve().parent
    java = root.parent / "app/src/main/java/com/mishare/miplay"
    fixtures = root.parents[2] / "core/tests/fixtures"
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or Path.home() / "Android/Sdk")
    android = sdk / "platforms/android-34/android.jar"
    with tempfile.TemporaryDirectory() as out:
        subprocess.run(["javac", "--release", "11", "-encoding", "UTF-8", "-cp", str(android), "-d", out,
                        str(root / "OfflineRegression.java"), *[str(java / (name + ".java")) for name in
                        ("AudioCaptureEngine", "CmdSession", "ConnectionWriter", "MdnsDiscovery", "TimerService", "TsAndRtpSender")]],
                       check=True)
        command = ["java", "-cp", os.pathsep.join((out, str(android))), "com.mishare.miplay.OfflineRegression",
                   str(fixtures)]
        subprocess.run(command, check=True, timeout=25)
        java_wire = subprocess.check_output(command + ["--dump-transport-fixtures"], timeout=10)
        cpp_wire = subprocess.check_output([core_tests, "--dump-transport-fixtures"], timeout=10)
        assert java_wire == cpp_wire, "Android and C++ transport fixtures produced different bytes"
        print("[PASS] shared transport fixtures matched byte for byte")


if __name__ == "__main__":
    main(sys.argv[1])
