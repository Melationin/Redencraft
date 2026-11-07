"""Headless backpressure integration test; no Minecraft client or UI."""
import argparse
import json
import os
import re
import socket
import struct
import subprocess
import threading
from pathlib import Path

from socket_test import handshake, packet, position, send, string, vint


def load_ids(assets):
    ids = {}
    for line in (assets / "packet_ids.tsv").read_text(encoding="utf-8").splitlines():
        phase, flow, name, value = line.split("\t")
        ids[(phase, flow, name.removeprefix("minecraft:"))] = int(value)
    return ids


def run(server, assets):
    ids = load_ids(assets)
    def pid(phase, flow, name):
        return ids[(phase, flow, name)]

    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    ticks = 160
    process = subprocess.Popen([str(server), "--port", "0", "--ticks", str(ticks),
                                "--assets", str(assets)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, creationflags=flags)
    stop = threading.Event()
    worker = None
    sockets = []
    result = {"slow_requests": 0, "slow_send_stopped": "running", "healthy_ticks": []}
    try:
        line = process.stdout.readline()
        match = re.search(r"LISTENING 127\.0\.0\.1:(\d+)", line)
        assert match, f"server did not listen: {line!r}"
        port = int(match[1])

        def login(name):
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            sockets.append(s)
            handshake(s, 2, port)
            send(s, pid("login", "serverbound", "hello"), string(name) + bytes(16))
            assert packet(s)[0] == pid("login", "clientbound", "login_finished")
            send(s, pid("login", "serverbound", "login_acknowledged"))
            assert packet(s)[0] == pid("configuration", "clientbound", "select_known_packs")
            send(s, pid("configuration", "serverbound", "select_known_packs"), b"\0")
            for _ in range(100):
                if packet(s)[0] == pid("configuration", "clientbound", "finish_configuration"):
                    break
            else:
                raise AssertionError("configuration did not finish")
            send(s, pid("configuration", "serverbound", "finish_configuration"))
            for _ in range(300):
                ident, _ = packet(s)
                if ident == pid("play", "clientbound", "player_position"):
                    break
            else:
                raise AssertionError("play position missing")
            send(s, pid("play", "serverbound", "accept_teleportation"), vint(1))
            return s

        slow = login("SlowReader")
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        # Do not read slow after login. Batches remain below Session's
        # 4096-packet receive limit, with spacing to avoid a single huge read.
        ping_id = pid("play", "serverbound", "ping_request")
        request = vint(1 + 8) + vint(ping_id) + struct.pack(">q", 12345)
        slow.settimeout(.1)

        def flood():
            batch = request * 1024
            while not stop.is_set():
                try:
                    slow.sendall(batch)
                    result["slow_requests"] += 1024
                except (OSError, TimeoutError) as exc:
                    result["slow_send_stopped"] = type(exc).__name__
                    return
                stop.wait(.02)
            result["slow_send_stopped"] = "test_complete"

        worker = threading.Thread(target=flood, daemon=True)
        worker.start()
        healthy = login("HealthyReader")
        play = lambda name: pid("play", "clientbound", name)
        serverbound = lambda name: pid("play", "serverbound", name)
        seen_ticks = []

        def read_until(predicate):
            for _ in range(2000):
                ident, payload = packet(healthy)
                assert ident != play("disconnect"), f"healthy disconnected: {payload!r}"
                if ident == play("keep_alive"):
                    send(healthy, serverbound("keep_alive"), payload)
                if ident == play("system_chat"):
                    tick = re.search(rb"Tick (\d+):", payload)
                    if tick:
                        assert b"(1s avg," in payload
                        seen_ticks.append(int(tick.group(1)))
                if predicate(ident, payload):
                    return
            raise AssertionError("healthy response missing")

        nonce = struct.pack(">q", 998877)
        send(healthy, ping_id, nonce)
        read_until(lambda ident, payload: ident == play("pong_response") and payload == nonce)
        send(healthy, serverbound("use_item_on"), vint(0) + position(8, 64, 8) + vint(1) +
             struct.pack(">fff??", .5, 1, .5, False, False) + vint(1))
        updates = []
        def placed(ident, payload):
            if ident == play("block_update"):
                updates.append(payload)
            return ident == play("block_changed_ack") and payload == vint(1)
        read_until(placed)
        assert any(p[:8] == position(8, 65, 8) and p[8:] != b"\0" for p in updates)
        send(healthy, serverbound("player_action"), vint(0) + position(8, 65, 8) + b"\1" + vint(2))
        updates.clear()
        def broken(ident, payload):
            if ident == play("block_update"):
                updates.append(payload)
            return ident == play("block_changed_ack") and payload == vint(2)
        read_until(broken)
        assert position(8, 65, 8) + b"\0" in updates
        while len(set(seen_ticks)) < 3:
            read_until(lambda ident, payload: ident == play("system_chat") and b"Tick " in payload)
        assert all(a < b for a, b in zip(seen_ticks, seen_ticks[1:])), seen_ticks
        assert result["slow_requests"] > 0
        result["healthy_ticks"] = seen_ticks
        result["healthy_ping_place_break"] = True
        # Keep the slow connection in place until the finite server exits.
        stdout_tail, stderr = process.communicate(timeout=20)
        assert process.returncode == 0, stderr
        assert re.search(rf"STOPPED ticks={ticks}\b", stdout_tail), stdout_tail
        result["server_exit_code"] = process.returncode
        result["server_ticks"] = ticks
        overflow = re.search(r"backpressure_disconnects=(\d+)", stdout_tail)
        result["output_backpressure_disconnects"] = int(overflow.group(1)) if overflow else None
        result["server_stderr"] = stderr.strip()
        stop.set()
        worker.join(timeout=2)
        print(json.dumps(result, sort_keys=True))
    finally:
        stop.set()
        for s in sockets:
            s.close()
        if worker:
            worker.join(timeout=2)
        if process.poll() is None:
            process.terminate()
        process.communicate(timeout=10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--assets", type=Path,
                        default=Path(__file__).resolve().parents[2] / "assets" / "protocol775")
    args = parser.parse_args()
    run(args.server.resolve(), args.assets.resolve())
