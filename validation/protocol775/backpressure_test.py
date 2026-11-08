"""Headless backpressure integration test; no Minecraft client or UI.

The server runs a real fluid fixture (sealed shafts plus surface pools) while one
client never reads and floods ping requests, and a second client reads normally.
The slow connection must hit the server's OutboundQueue limit; the healthy one
must keep decoding block/section updates, see water in BOTH the source section
and the lower section, and observe the tick clock advance monotonically.
"""
import argparse
import json
import os
import re
import socket
import struct
import subprocess
import threading
import time
from pathlib import Path

from socket_test import handshake, packet, position, send, string, vint


def load_ids(assets):
    ids = {}
    for line in (assets / "packet_ids.tsv").read_text(encoding="utf-8").splitlines():
        phase, flow, name, value = line.split("\t")
        ids[(phase, flow, name.removeprefix("minecraft:"))] = int(value)
    return ids


def load_water_ids(assets):
    ids = set()
    for line in (assets / "block_states.tsv").read_text(encoding="utf-8").splitlines():
        state, block, *_ = line.split("\t")
        if block == "minecraft:water":
            ids.add(int(state))
    assert ids, "no water states in the 26.1 export"
    return ids


def sign_extend(value, bits):
    mask = (1 << bits) - 1
    value &= mask
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def read_varint(data, pos):
    value = 0
    for i in range(10):
        b = data[pos]
        pos += 1
        value |= (b & 0x7F) << (7 * i)
        if not b & 0x80:
            return value, pos
    raise AssertionError("bad VarInt/VarLong")


def decode_position(payload):
    packed = struct.unpack(">Q", payload[:8])[0]
    return sign_extend(packed >> 38, 26), sign_extend(packed, 12), sign_extend(
        packed >> 12, 26)


def section_origin(section):
    packed = section & ((1 << 64) - 1)
    return (sign_extend(packed >> 42, 22), sign_extend(packed, 20),
            sign_extend(packed >> 20, 22))


def decode_updates(ident, payload, water_ids, ids, water_cells, sections):
    """Decode one block_update / section_blocks_update payload."""
    if ident == ids[("play", "clientbound", "block_update")]:
        x, y, z = decode_position(payload)
        state, end = read_varint(payload, 8)
        assert end == len(payload), "block_update has trailing bytes"
        if state in water_ids:
            water_cells.add((x, y, z))
            sections.add(y // 16)
        return 1
    if ident == ids[("play", "clientbound", "section_blocks_update")]:
        section = struct.unpack(">q", payload[:8])[0]
        sx, sy, sz = section_origin(section)
        count, pos = read_varint(payload, 8)
        assert count > 0
        added = 0
        for _ in range(count):
            packed, pos = read_varint(payload, pos)
            local = packed & 0xFFF
            state = packed >> 12
            # Production packSectionLocal: x << 8 | z << 4 | y.
            x = sx * 16 + ((local >> 8) & 15)
            y = sy * 16 + (local & 15)
            z = sz * 16 + ((local >> 4) & 15)
            assert -16 <= x <= 31 and -16 <= z <= 31 and 48 <= y <= 79, (
                "fluid update outside the fixture", x, y, z)
            if state in water_ids:
                water_cells.add((x, y, z))
                sections.add(sy)
                added += 1
        assert pos == len(payload), "section_blocks_update has trailing bytes"
        return added
    return 0


def parse_frames(data, allow_partial=False):
    frames = []
    pos = 0
    while pos < len(data):
        try:
            n, after = read_varint(data, pos)
        except (IndexError, AssertionError):
            if allow_partial:
                break
            raise
        if not 0 < n < 2097152 or after + n > len(data):
            if allow_partial:
                break
            raise AssertionError("bad TCP frame")
        body = data[after:after + n]
        pos = after + n
        ident, body_pos = read_varint(body, 0)
        frames.append((ident, body[body_pos:]))
    return frames


def drain_socket(sock, timeout=1.0):
    sock.settimeout(timeout)
    data = bytearray()
    while True:
        try:
            chunk = sock.recv(65536)
        except (socket.timeout, TimeoutError):
            break
        except OSError:
            break
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def run(server, assets):
    ids = load_ids(assets)
    water_ids = load_water_ids(assets)

    def pid(phase, flow, name):
        return ids[(phase, flow, name)]

    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    ticks = 320
    process = subprocess.Popen([str(server), "--port", "0", "--ticks", str(ticks),
                                "--assets", str(assets), "--fluid-basin"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, creationflags=flags)
    stop = threading.Event()
    worker = None
    sockets = []
    result = {
        "slow_requests": 0,
        "slow_send_stopped": "running",
        "healthy_ticks": [],
        "fluid_updates": 0,
        "fluid_sections": [],
        "lower_water_cells": 0,
        "fluid_slow_frames": 0,
        "fluid_slow_water_cells": 0,
        "fluid_slow_sections": [],
    }
    try:
        lines = []
        for _ in range(20):
            line = process.stdout.readline()
            lines.append(line)
            if "LISTENING" in line:
                break
        match = re.search(r"LISTENING 127\.0\.0\.1:(\d+)", "".join(lines))
        assert match, f"server did not listen: {lines!r}"
        port = int(match[1])
        assert any("FLUID_FIXTURE" in line for line in lines), lines

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
        # A second slow peer: it logs in and then does not read while the fluid
        # fixture fills both sections. Its frames are drained later, proving the
        # real broadcast stream reached a non-reading session.
        fluid_slow = login("SlowFluidReader")
        healthy = login("HealthyReader")
        play = lambda name: pid("play", "clientbound", name)
        serverbound = lambda name: pid("play", "serverbound", name)
        block_update = play("block_update")
        section_update = play("section_blocks_update")
        seen_ticks = []
        water_cells = set()
        water_sections = set()

        def read_until(predicate):
            for _ in range(4000):
                ident, payload = packet(healthy)
                assert ident != play("disconnect"), f"healthy disconnected: {payload!r}"
                if ident == play("keep_alive"):
                    send(healthy, serverbound("keep_alive"), payload)
                if ident == play("system_chat"):
                    tick = re.search(rb"Tick (\d+):", payload)
                    if tick:
                        assert b"(1s avg," in payload
                        value = int(tick.group(1))
                        if seen_ticks:
                            assert value >= seen_ticks[-1], (seen_ticks, value)
                        seen_ticks.append(value)
                if ident in (block_update, section_update):
                    added = decode_updates(ident, payload, water_ids, ids,
                                           water_cells, water_sections)
                    result["fluid_updates"] += added
                if predicate(ident, payload):
                    return
            raise AssertionError("healthy response missing")

        nonce = struct.pack(">q", 998877)
        send(healthy, ping_id, nonce)
        read_until(lambda ident, payload: ident == play("pong_response") and payload == nonce)
        # The healthy peer must stay interactive while the slow queue fills.
        send(healthy, serverbound("use_item_on"), vint(0) + position(4, 64, 8) + vint(1) +
             struct.pack(">fff??", .5, 1, .5, False, False) + vint(1))
        updates = []
        def placed(ident, payload):
            if ident == block_update:
                updates.append(payload)
            return ident == play("block_changed_ack") and payload == vint(1)
        read_until(placed)
        assert any(p[:8] == position(4, 65, 8) and p[8:] != b"\0" for p in updates)
        send(healthy, serverbound("player_action"), vint(0) + position(4, 65, 8) + b"\1" + vint(2))
        updates.clear()
        def broken(ident, payload):
            if ident == block_update:
                updates.append(payload)
            return ident == play("block_changed_ack") and payload == vint(2)
        read_until(broken)
        assert position(4, 65, 8) + b"\0" in updates
        # Wait for real cross-section fluid updates, not just the static chunks.
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if (len(seen_ticks) >= 4 and {3, 4}.issubset(water_sections)
                    and any(y < 64 for _, y, _ in water_cells)):
                break
            # One tick chat per simulation tick; all block/section updates read
            # on the way are decoded by read_until's handler.
            read_until(lambda ident, payload: ident == play("system_chat") and b"Tick " in payload)
        assert {3, 4}.issubset(water_sections), (
            "expected fluid writes in source and lower sections", water_sections)
        assert any(y < 64 for _, y, _ in water_cells), "no water below the source"
        assert all(a <= b for a, b in zip(seen_ticks, seen_ticks[1:])), seen_ticks
        result["healthy_ticks"] = seen_ticks
        result["fluid_sections"] = sorted(water_sections)
        result["lower_water_cells"] = sum(1 for _, y, _ in water_cells if y < 64)
        result["healthy_ping_place_break"] = True
        # Drain the non-reading fluid peer while the server is still up. Its
        # frames were really queued for a session that did not read during the
        # whole observation window, and include both Y sections.
        fluid_frames = parse_frames(drain_socket(fluid_slow, 2.0),
                                    allow_partial=True)
        fluid_cells = set()
        fluid_sections = set()
        result["fluid_slow_frames"] = len(fluid_frames)
        for ident, payload in fluid_frames:
            if ident in (block_update, section_update):
                decode_updates(ident, payload, water_ids, ids, fluid_cells,
                               fluid_sections)
        result["fluid_slow_water_cells"] = len(fluid_cells)
        result["fluid_slow_sections"] = sorted(fluid_sections)
        assert result["fluid_slow_frames"] > 0, "fluid slow peer got no frames"
        assert result["fluid_slow_water_cells"] > 0, (
            "fluid slow peer got no water update")
        assert {3, 4}.issubset(fluid_sections), (
            "fluid slow peer did not see both sections", fluid_sections)
        # The ping-flood peer additionally crosses the outbox threshold. Its
        # queue is intentionally never read, so the server must disconnect it.
        stdout_tail, stderr = process.communicate(timeout=30)
        assert process.returncode == 0, stderr
        assert re.search(rf"STOPPED ticks={ticks}\b", stdout_tail), stdout_tail
        overflow = re.search(r"backpressure_disconnects=(\d+)", stdout_tail)
        result["output_backpressure_disconnects"] = int(overflow.group(1)) if overflow else None
        assert result["output_backpressure_disconnects"] and \
            result["output_backpressure_disconnects"] >= 1, stdout_tail
        result["server_exit_code"] = process.returncode
        result["server_ticks"] = ticks
        result["server_stderr"] = stderr.strip()
        stop.set()
        if worker:
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
