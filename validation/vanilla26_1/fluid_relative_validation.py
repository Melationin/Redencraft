"""Run W-02, M-03 and H-01 against the official Minecraft 26.1 server.

Each run gets a fresh workspace directory and loopback port. The server is
frozen, the fixture is built from commands, and states are sampled only at an
exact gameTime boundary. The checked-in golden is promoted only when requested;
the raw log, full candidate and manifest remain with the unique run.

H-01 is deliberately a real-container slice: both sources are placed through
the server, their scheduled fluid ticks make water spread into a dry rail, and
SimpleWaterloggedBlock.placeLiquid must perform the observed state transition.
No command directly writes ``rail[waterlogged=true]``. The setup still uses
setblock to construct the fixture, so it does not exercise a player item use or
bucket entity path.
"""

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import threading
import time
import uuid


ROOT = pathlib.Path(__file__).resolve().parent
JAR_SHA1 = "3872a7f07a1a595e651aef8b058dfc2bb3772f46"
JAVA = os.environ.get("JAVA_26", r"C:\Program Files\Zulu\zulu-26\bin\java.exe")


W02_CELLS = [
    (0, 83, 0), (0, 82, 0), (0, 81, 0), (0, 80, 0),
    (1, 80, 0), (0, 79, 0), (1, 79, 0), (-1, 80, 0),
    (0, 80, -1), (0, 80, 1), (2, 80, 0), (0, 84, 0),
]


M03_CASES = (
    ("basalt-blue", 10, "soul_soil", "blue_ice", None),
    ("missing-soul", 20, "stone", "blue_ice", None),
    ("missing-blue", 30, "soul_soil", None, None),
    ("blue-below", 40, "blue_ice", None, None),
    ("priority-blue", 50, "soul_soil", "blue_ice", "water"),
    ("priority-water", 60, "soul_soil", "water", "blue_ice"),
)


def m03_cells():
    cells = []
    for _, x, _, _, _ in M03_CASES:
        cells.extend(((x, 80, 0), (x, 79, 0), (x - 1, 80, 0), (x + 1, 80, 0)))
    return cells


H01_CELLS = [
    (0, 120, 0), (-1, 120, 0), (1, 120, 0), (0, 119, 0),
    (0, 121, 0), (10, 120, 0), (9, 120, 0), (10, 119, 0),
    (10, 121, 0),
]


def predicates(fixture):
    water = [("minecraft:water[level=%d]" % level,
              "water[level=%d]" % level) for level in range(16)]
    if fixture == "w02":
        return [("minecraft:air", "air"), ("minecraft:stone", "stone")] + water
    if fixture == "m03":
        lava = [("minecraft:lava[level=%d]" % level,
                 "lava[level=%d]" % level) for level in range(16)]
        return ([
            ("minecraft:air", "air"), ("minecraft:stone", "stone"),
            ("minecraft:soul_soil", "soul_soil"),
            ("minecraft:blue_ice", "blue_ice"),
            ("minecraft:obsidian", "obsidian"),
            ("minecraft:basalt[axis=x]", "basalt[axis=x]"),
            ("minecraft:basalt[axis=y]", "basalt[axis=y]"),
            ("minecraft:basalt[axis=z]", "basalt[axis=z]"),
        ] + water + lava)
    if fixture == "h01":
        return ([
            ("minecraft:air", "air"), ("minecraft:stone", "stone"),
            ("minecraft:rail[shape=north_south,waterlogged=false]",
             "rail[shape=north_south,waterlogged=false]"),
            ("minecraft:rail[shape=north_south,waterlogged=true]",
             "rail[shape=north_south,waterlogged=true]"),
        ] + water)
    raise ValueError(fixture)


def eula_ok():
    path = ROOT / "eula.txt"
    return path.exists() and "eula=true" in path.read_text(encoding="utf-8")


def jar_sha1():
    digest = hashlib.sha1()
    with (ROOT / "server.jar").open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pick_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Server:
    def __init__(self, run_dir, log, process):
        self.run_dir = run_dir
        self.log = log
        self.process = process
        self.counter = 0
        self.last_output = time.monotonic()

    def line(self, timeout=60):
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("server exited %s" % self.process.returncode)
        self.last_output = time.monotonic()
        self.log.write(line)
        self.log.flush()
        return line

    def send(self, commands):
        for command in commands:
            self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def marker(self, prefix):
        self.counter += 1
        return "__%s_%d_%s" % (prefix, self.counter, uuid.uuid4().hex[:8])

    def sync(self, commands, timeout=120):
        marker = self.marker("REL_SYNC")
        self.send(list(commands) + ["say " + marker])
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if marker in self.line(max(1.0, end - time.monotonic())):
                return
        raise RuntimeError("marker not observed: " + marker)

    def game_time(self):
        marker = self.marker("REL_GT")
        self.send(["time query gametime", "say " + marker])
        value = None
        while True:
            text = self.line()
            match = re.search(r"time is (\d+)", text, re.IGNORECASE)
            if match:
                value = int(match.group(1))
            if marker in text:
                if value is None:
                    raise RuntimeError("no gameTime in command output")
                return value

    def step(self, count=1):
        before = self.game_time()
        self.send(["tick step %d" % count])
        end = time.monotonic() + 60
        while time.monotonic() < end:
            current = self.game_time()
            if current == before + count:
                return
            if current > before + count:
                raise RuntimeError("gameTime overshot %d > %d" %
                                   (current, before + count))
        raise RuntimeError("gameTime did not reach %d" % (before + count))

    def capture(self, fixture, tag, cells):
        commands = []
        for index, (x, y, z) in enumerate(cells):
            for predicate, label in predicates(fixture):
                commands.append(
                    "execute if block %d %d %d %s run say REL %s %s %d %s" %
                    (x, y, z, predicate, fixture, tag, index, label))
        marker = self.marker("REL_END_" + fixture + "_" + tag)
        commands.append("say " + marker)
        self.send(commands)
        values = {}
        end = time.monotonic() + 120
        pattern = re.compile(r"REL %s %s (\d+) (\S+)" %
                             (re.escape(fixture), re.escape(tag)))
        while time.monotonic() < end:
            text = self.line(max(1.0, end - time.monotonic()))
            match = pattern.search(text)
            if match:
                index = int(match.group(1))
                if index in values:
                    raise RuntimeError("%s/%s duplicate cell %d" %
                                       (fixture, tag, index))
                values[index] = match.group(2)
            if marker in text:
                if len(values) != len(cells):
                    raise RuntimeError(
                        "%s/%s has %d of %d states" %
                        (fixture, tag, len(values), len(cells)))
                return [values[index] for index in range(len(cells))]
        raise RuntimeError("%s/%s snapshot timed out" % (fixture, tag))

    def close(self):
        if self.process.poll() is None:
            try:
                self.send(["stop"])
                self.process.wait(timeout=60)
            except (subprocess.TimeoutExpired, BrokenPipeError):
                self.process.kill()
        self.log.close()


def w02_layout():
    return [
        "fill -1 79 -1 2 84 1 minecraft:stone",
        "fill 0 80 0 0 83 0 minecraft:air",
        "fill 1 80 0 1 80 0 minecraft:air",
    ]


def m03_layout():
    commands = []
    for name, x, below, west, east in M03_CASES:
        commands.append("fill %d 79 -2 %d 81 2 minecraft:stone" %
                        (x - 1, x + 1))
        commands.append("setblock %d 80 0 minecraft:air" % x)
        commands.append("setblock %d 79 0 minecraft:%s" % (x, below))
        if west is not None:
            commands.append("setblock %d 80 0 minecraft:%s" % (x - 1, west))
        if east is not None:
            commands.append("setblock %d 80 0 minecraft:%s" % (x + 1, east))
    return commands


def m03_action():
    return ["setblock %d 80 0 minecraft:lava[level=0]" % x
            for _, x, _, _, _ in M03_CASES]


def h01_layout():
    return [
        "fill -2 119 -1 2 121 1 minecraft:stone",
        "fill -1 120 0 1 120 0 minecraft:air",
        "setblock 0 120 0 minecraft:rail[shape=north_south,waterlogged=false]",
        "fill 9 119 -1 12 121 1 minecraft:stone",
        "fill 9 120 0 10 120 0 minecraft:air",
        "setblock 10 120 0 minecraft:rail[shape=north_south,waterlogged=false]",
    ]


def run():
    base = ROOT.parents[1] / "build-fluid-validation" / "relative-runs"
    base.mkdir(parents=True, exist_ok=True)
    run_dir = base / (time.strftime("%Y%m%d-%H%M%S") +
                      "-%d-%s" % (os.getpid(), uuid.uuid4().hex[:8]))
    run_dir.mkdir(parents=True)
    port = pick_port()
    (run_dir / "eula.txt").write_text("eula=true\n", encoding="ascii")
    (run_dir / "server.properties").write_text(
        "server-ip=127.0.0.1\nserver-port=%d\nonline-mode=true\n"
        "spawn-protection=0\nview-distance=3\nsimulation-distance=3\n"
        "max-players=1\nlevel-name=relative-validation-world\n"
        "pause-when-empty-seconds=-1\n" % port, encoding="ascii")
    log_path = run_dir / "server.log"
    log = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        [JAVA, "-Xms512M", "-Xmx2G", "-jar", str(ROOT / "server.jar"),
         "nogui"], cwd=run_dir, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        bufsize=1, encoding="utf-8", errors="replace")
    server = Server(run_dir, log, process)

    def watchdog():
        while process.poll() is None:
            time.sleep(5)
            if time.monotonic() - server.last_output > 300:
                process.kill()
                return

    threading.Thread(target=watchdog, daemon=True).start()
    result = {"schema": 1, "jar_sha1": jar_sha1(), "fixtures": {}}
    absolute_t0 = {}
    try:
        while "Done (" not in server.line():
            pass
        server.sync([
            "tick freeze", "gamerule random_tick_speed 0",
            "gamerule spawn_mobs false", "gamerule advance_weather false",
            "gamerule water_source_conversion true",
            "gamerule lava_source_conversion false",
            "forceload add -16 -16 79 15",
        ])
        warm = server.game_time()
        server.step(100)
        if server.game_time() != warm + 100:
            raise RuntimeError("chunk warmup did not consume exactly 100 ticks")

        server.sync(w02_layout())
        states = {"T0": server.capture("w02", "T0", W02_CELLS)}
        absolute_t0["w02"] = server.game_time()
        server.sync(["setblock 0 83 0 minecraft:water[level=0]"])
        for tick in range(1, 21):
            server.step(1)
            states["T%d" % tick] = server.capture(
                "w02", "T%d" % tick, W02_CELLS)
        result["fixtures"]["w02"] = {
            "ticks": 20, "cells": [list(c) for c in W02_CELLS],
            "states": states,
        }

        server.sync(m03_layout())
        server.sync(m03_action())
        m03_cell_list = m03_cells()
        states = {"T0": server.capture("m03", "T0", m03_cell_list)}
        absolute_t0["m03"] = server.game_time()
        server.step(1)
        states["T1"] = server.capture("m03", "T1", m03_cell_list)
        result["fixtures"]["m03"] = {
            "ticks": 1, "cells": [list(c) for c in m03_cell_list],
            "states": states,
        }

        server.sync(h01_layout())
        server.sync([
            "setblock -1 120 0 minecraft:water[level=0]",
            "setblock 1 120 0 minecraft:water[level=0]",
            "setblock 9 120 0 minecraft:water[level=0]",
        ])
        states = {"T0": server.capture("h01", "T0", H01_CELLS)}
        absolute_t0["h01"] = server.game_time()
        for tick in range(1, 11):
            server.step(1)
            states["T%d" % tick] = server.capture(
                "h01", "T%d" % tick, H01_CELLS)
        result["fixtures"]["h01"] = {
            "ticks": 10, "cells": [list(c) for c in H01_CELLS],
            "states": states,
        }
        return result, absolute_t0, run_dir, log_path
    finally:
        server.close()


def validate(result):
    for fixture, block in result["fixtures"].items():
        cells = block["cells"]
        expected_tags = ["T%d" % i for i in range(block["ticks"] + 1)]
        if sorted(block["states"]) != sorted(expected_tags):
            raise RuntimeError("%s has wrong tick set" % fixture)
        for tag in expected_tags:
            if len(block["states"][tag]) != len(cells):
                raise RuntimeError("%s/%s has wrong cell count" %
                                   (fixture, tag))


def validate_semantics(result):
    """Pin the branches this fixture is meant to prove before promotion."""
    w02 = result["fixtures"]["w02"]["states"]
    expected_fall = {
        "T5": (1, "water[level=8]"),
        "T10": (2, "water[level=8]"),
        "T15": (3, "water[level=8]"),
        "T20": (4, "water[level=1]"),
    }
    for tag, (cell, state) in expected_fall.items():
        if w02[tag][cell] != state:
            raise RuntimeError("W-02 %s cell %d is %s, expected %s" %
                               (tag, cell, w02[tag][cell], state))
    if w02["T20"][3] != "water[level=8]":
        raise RuntimeError("W-02 did not reach the bottom as falling water")

    m03 = result["fixtures"]["m03"]["states"]
    expected_centres = {
        0: "basalt[axis=y]", 4: "lava[level=0]", 8: "lava[level=0]",
        12: "lava[level=0]", 16: "basalt[axis=y]", 20: "obsidian",
    }
    for cell, state in expected_centres.items():
        if m03["T0"][cell] != state or m03["T1"][cell] != state:
            raise RuntimeError("M-03 cell %d did not stay at %s" %
                               (cell, state))

    h01 = result["fixtures"]["h01"]["states"]
    dry = "rail[shape=north_south,waterlogged=false]"
    wet = "rail[shape=north_south,waterlogged=true]"
    for tag in ("T0", "T1", "T2", "T3", "T4"):
        if h01[tag][0] != dry:
            raise RuntimeError("H-01 rail filled before T5")
    for tick in range(5, 11):
        if h01["T%d" % tick][0] != wet:
            raise RuntimeError("H-01 two-source container did not fill at T5")
    for tick in range(0, 11):
        if h01["T%d" % tick][5] != dry:
            raise RuntimeError("H-01 flowing candidate filled the dry host")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--promote-golden", action="store_true")
    parser.add_argument("--force-golden", action="store_true")
    args = parser.parse_args()
    if not eula_ok():
        raise SystemExit("Accept the Minecraft EULA in this directory first.")
    digest = jar_sha1()
    if digest != JAR_SHA1:
        raise SystemExit("server.jar SHA-1 %s != %s" % (digest, JAR_SHA1))
    result, t0, run_dir, log_path = run()
    validate(result)
    validate_semantics(result)
    candidate = run_dir / "fluid-relative-golden.candidate.json"
    candidate.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    manifest = {
        "jar_sha1": digest,
        "java": JAVA,
        "relative_t0_game_time": t0,
        "fixtures": {
            name: {
                "ticks": block["ticks"], "cells": len(block["cells"]),
                "observations": len(block["cells"]) * (block["ticks"] + 1),
            }
            for name, block in result["fixtures"].items()
        },
        "candidate_sha256": hashlib.sha256(candidate.read_bytes()).hexdigest(),
        "log_sha256": hashlib.sha256(log_path.read_bytes()).hexdigest(),
        "run_dir": str(run_dir),
    }
    (run_dir / "run-manifest.json").write_text(
        json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    golden = ROOT / "fluid-relative-golden.json"
    if golden.exists():
        old = json.loads(golden.read_text(encoding="utf-8"))
        if old != result:
            raise SystemExit("candidate differs from the checked-in golden")
        print("candidate matches golden")
    if args.promote_golden:
        if golden.exists() and not args.force_golden:
            raise SystemExit("golden exists; pass --force-golden to replace it")
        shutil.copyfile(candidate, golden)
        print("promoted fluid-relative-golden.json")
    print("run artifacts:", run_dir)
    print(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    main()
