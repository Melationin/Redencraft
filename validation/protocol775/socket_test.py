"""Headless TCP client, not a Minecraft client or desktop/UI test."""
import argparse
import json
import os
import re
import socket
import struct
import subprocess
from pathlib import Path


def vint(n):
    n &= 0xFFFFFFFF
    out = bytearray()
    while True:
        b = n & 127
        n >>= 7
        out.append(b | (128 if n else 0))
        if not n:
            return bytes(out)


def string(s):
    b = s.encode()
    return vint(len(b)) + b


def send(s, ident, payload=b""):
    b = vint(ident) + payload
    s.sendall(vint(len(b)) + b)


def exact(s, n):
    out = bytearray()
    while len(out) < n:
        b = s.recv(n - len(out))
        assert b, "unexpected EOF"
        out.extend(b)
    return bytes(out)


def read_int(s):
    n = 0
    for i in range(5):
        b = exact(s, 1)[0]
        n |= (b & 127) << (7 * i)
        if not b & 128:
            return n
    raise AssertionError("bad VarInt")


def packet(s):
    n = read_int(s)
    assert 0 < n < 2097152
    body = exact(s, n)
    pos = 0
    ident = 0
    for i, b in enumerate(body):
        ident |= (b & 127) << (7 * i)
        pos += 1
        if not b & 128:
            return ident, body[pos:]
    raise AssertionError("bad packet ID")


def position(x, y, z):
    return struct.pack(">Q", ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 4095))


def until(s, target, seen=None):
    for _ in range(300):
        ident, payload = packet(s)
        if seen is not None:
            seen.add(ident)
        if ident == 32:
            raise AssertionError("server disconnected: " + repr(payload))
        if ident == 44:
            send(s, 28, payload)
        if ident == target:
            return payload
    raise AssertionError(f"packet {target} missing")


def handshake(s, state, port):
    send(s, 0, vint(775) + string("localhost") + struct.pack(">H", port) + vint(state))


def run(server):
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    process = subprocess.Popen([str(server), "--port", "0", "--ticks", "160",
                                "--command-permission-level", "4"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, creationflags=flags)
    try:
        line = process.stdout.readline()
        match = re.search(r"LISTENING 127\.0\.0\.1:(\d+)", line)
        assert match, line + process.stderr.read() if process.poll() is not None else line
        port = int(match[1])
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            handshake(s, 1, port)
            send(s, 0)
            ident, payload = packet(s)
            assert ident == 0
            # JSON string length prefix may span two bytes.
            i = 0
            while payload[i] & 128:
                i += 1
            info = json.loads(payload[i + 1:])
            assert info["version"]["protocol"] == 775
            nonce = struct.pack(">q", 987654321)
            send(s, 1, nonce)
            assert packet(s) == (1, nonce)
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            handshake(s, 2, port)
            send(s, 0, string("TcpTester") + bytes(16))
            assert packet(s)[0] == 2
            send(s, 3)
            assert packet(s) == (14, b"\0")
            send(s, 7, b"\0")
            count = 0
            while True:
                ident, payload = packet(s)
                if ident == 3:
                    break
                count += 1
            assert count == 30
            send(s, 3)
            assert packet(s)[0] == 49
            seen = set()
            until(s, 72, seen)
            # The join sequence carries the command tree (/tick) and the initial
            # ticking state.
            assert 16 in seen, "commands packet missing"


            def read_varint(payload, index):
                value = 0
                for shift in range(0, 35, 7):
                    byte = payload[index]
                    index += 1
                    value |= (byte & 127) << shift
                    if not byte & 128:
                        return value, index
                raise AssertionError("bad varint")


            # Completion for "/tick f" must report the replacement range and the
            # matching subcommand. The client sends the slash and expects the
            # offsets back in the same coordinates.
            send(s, 15, vint(7) + string("/tick f"))
            while True:
                ident, payload = packet(s)
                assert ident != 32, "disconnected after suggestion request"
                if ident == 15:
                    break
            request_id, index = read_varint(payload, 0)
            assert request_id == 7, payload
            start, index = read_varint(payload, index)
            length, index = read_varint(payload, index)
            count, index = read_varint(payload, index)
            assert (start, length, count) == (6, 1, 1), (start, length, count)
            size, index = read_varint(payload, index)
            text = payload[index:index + size].decode()
            assert text == "freeze", text

            send(s, 7, string("tick freeze"))
            while True:
                ident, payload = packet(s)
                assert ident != 32, "disconnected after tick command"
                if ident == 127:
                    tick_rate, frozen = struct.unpack(">f?", payload)
                    assert frozen is True, payload
                    assert abs(tick_rate - 20.0) < 1e-4, payload
                    break
            send(s, 7, string("/tick step 5"))
            while True:
                ident, payload = packet(s)
                assert ident != 32, "disconnected after tick step"
                if ident == 128:
                    steps, _ = read_varint(payload, 0)
                    assert steps == 5, steps
                    break
            send(s, 7, string("tick step stop"))
            while True:
                ident, payload = packet(s)
                assert ident != 32, "disconnected after tick step stop"
                if ident == 128:
                    steps, _ = read_varint(payload, 0)
                    assert steps == 0, steps
                    break
            # The world is frozen: no further tick-driven packets are required,
            # so unfreeze again and check the state round-trips.
            send(s, 7, string("tick unfreeze"))
            while True:
                ident, payload = packet(s)
                assert ident != 32, "disconnected after tick unfreeze"
                if ident == 127:
                    assert struct.unpack(">f?", payload)[1] is False, payload
                    break
            send(s, 0, vint(1))
            # Place selected smooth stone, then break. Wait for prediction ACK
            # and require the corresponding authoritative block update.
            send(s, 66, vint(0) + position(8, 64, 8) + vint(1) +
                 struct.pack(">fff??", .5, 1, .5, False, False) + vint(1))
            updates = []
            while True:
                ident, payload = packet(s)
                if ident == 8:
                    updates.append(payload)
                if ident == 4:
                    assert payload == vint(1)
                    break
            assert any(p[:8] == position(8, 65, 8) and p[8:] != b"\0" for p in updates)
            send(s, 41, vint(0) + position(8, 65, 8) + b"\1" + vint(2))
            updates = []
            while True:
                ident, payload = packet(s)
                if ident == 8:
                    updates.append(payload)
                if ident == 4:
                    assert payload == vint(2)
                    break
            assert position(8, 65, 8) + b"\0" in updates
        assert process.poll() is None
        print("TCP status/login/configuration/chunks/place/break passed")
    finally:
        if process.poll() is None:
            process.terminate()
        process.communicate(timeout=10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    run(parser.parse_args().server.resolve())
