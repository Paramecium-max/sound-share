import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import wave


def read_frame(io):
    first, second = io.read(2)
    assert first == 0x81
    size = second & 127
    if size == 126:
        size, = struct.unpack("!H", io.read(2))
    elif size == 127:
        size, = struct.unpack("!Q", io.read(8))
    return json.loads(io.read(size))


def send_frame(conn, payload):
    length = len(payload)
    header = bytes([0x81, 0x80 | length]) if length < 126 else b"\x81\xfe" + struct.pack("!H", length)
    mask = b"\x13\x37\x55\xc1"
    body = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    conn.sendall(header + mask + body)


def main(executable):
    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / ('校准 "路径" \\ 示例.wav' if os.name != "nt" else "校准 示例.wav")
        with wave.open(str(wav), "wb") as file:
            file.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
            file.writeframes(bytes(480 * 4))

        with subprocess.Popen([executable], stdout=subprocess.PIPE, text=True, encoding="utf-8") as engine:
            try:
                ready = engine.stdout.readline()
                assert ready.startswith("READY "), ready
                config = json.loads(ready[6:])
                key = "bWlwbGF5LW9mZmxpbmUtdA=="
                with socket.create_connection(("127.0.0.1", config["port"]), timeout=5) as conn:
                    conn.sendall((
                        f"GET /?token={config['token']} HTTP/1.1\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\n\r\n"
                    ).encode("ascii"))
                    with conn.makefile("rb") as io:
                        assert io.readline().startswith(b"HTTP/1.1 101 ")
                        headers = {}
                        while (line := io.readline()) != b"\r\n":
                            name, value = line.split(b":", 1)
                            headers[name.lower()] = value.strip()
                        expected = base64.b64encode(hashlib.sha1(
                            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
                        assert headers[b"sec-websocket-accept"] == expected

                        def rpc(identifier, method, **params):
                            envelope = {"id": identifier, "method": method, **params}
                            send_frame(conn, json.dumps(envelope).encode("utf-8"))
                            while (reply := read_frame(io))["type"] != "rpc":
                                pass
                            assert type(reply["id"]) == type(identifier)
                            assert reply["id"] == identifier
                            return reply

                        identifier = 'id"\\\t\x00 中文 😃'
                        name = '音箱 "客厅" \\ \t\x00 😃'
                        reply = rpc(identifier, "device.addManual", name=name, ip="127.0.0.3", cmdPort=8899)
                        device = reply["result"]
                        assert rpc(9007199254740997, "device.addManual",
                                   name=name, ip="127.0.0.3", cmdPort=8899)["result"] == device
                        for port in [0, 65536, "8899", True, 8899.2]:
                            assert rpc(identifier, "device.addManual", ip="127.0.0.4", cmdPort=port)["error"]
                        assert rpc(identifier, "source.setMode", mode=1, param=str(wav))["result"] is True
                        source = rpc(identifier, "state.get")["result"]
                        assert source["sourceParam"] == str(wav)
                        assert source["track"]["title"] == wav.name
                        assert [(d["id"], d["name"]) for d in source["devices"] if d["isManual"]] == [(device, name)]
                        assert rpc(identifier, identifier)["error"] == "unknown method: " + identifier
                        assert "NUL" in rpc(identifier, "source.setMode", mode=1, param=str(wav) + "\x00")["error"]
                        malformed = b'{"id":4,"method":"state.get","x":"\xC0\xAF"}'
                        send_frame(conn, malformed)
                        while (reply := read_frame(io))["type"] != "rpc":
                            pass
                        assert reply["id"] is None and reply["error"]
            finally:
                engine.terminate()
                engine.wait(timeout=8)


if __name__ == "__main__":
    main(sys.argv[1])
