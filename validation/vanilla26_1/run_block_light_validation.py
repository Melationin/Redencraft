"""Run the isolated official 26.1 block-light fixture on loopback."""

import argparse
import hashlib
import json
import os
import pathlib
import queue
import re
import shutil
import socket
import subprocess
import threading
import time
import uuid


ROOT = pathlib.Path(__file__).resolve().parent
REPO = ROOT.parents[1]
JAVA = os.environ.get("JAVA_26", r"C:\Program Files\Zulu\zulu-26\bin\java.exe")
JAR_SHA1 = "3872a7f07a1a595e651aef8b058dfc2bb3772f46"
STAGES = {
    1: [
        "fill -15 198 -1 15 202 1 minecraft:air",
        "fill 5 199 -1 5 201 1 minecraft:stone",
        "setblock 8 200 0 minecraft:glass",
        "setblock -5 200 0 minecraft:piston[facing=north,extended=true]",
        "setblock 0 200 0 minecraft:redstone_lamp[lit=true]",
        "setblock 0 250 0 minecraft:note_block[note=1]",
    ],
    2: [
        "setblock 5 200 0 minecraft:air",
        "setblock -5 200 0 minecraft:piston[facing=north,extended=false]",
        "setblock -10 200 0 minecraft:redstone_lamp[lit=true]",
        "setblock 0 250 0 minecraft:note_block[note=2]",
    ],
    3: [
        "setblock 0 200 0 minecraft:air",
        "setblock -10 200 0 minecraft:air",
        "setblock 8 200 0 minecraft:air",
        "setblock 0 250 0 minecraft:note_block[note=3]",
    ],
    4: [
        "setblock 4 -64 4 minecraft:redstone_lamp[lit=true]",
        "setblock 4 319 4 minecraft:redstone_lamp[lit=true]",
        "setblock 0 250 0 minecraft:note_block[note=4]",
    ],
    5: [
        "setblock 4 -64 4 minecraft:air",
        "setblock 4 319 4 minecraft:air",
        "setblock 0 250 0 minecraft:note_block[note=5]",
    ],
}


def sha1(path):
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--promote-golden", action="store_true")
    args = parser.parse_args()
    jar = ROOT / "server.jar"
    if sha1(jar) != JAR_SHA1:
        raise SystemExit("unexpected server.jar SHA-1")
    run_dir = (REPO / "build-light-validation" / "block-light-runs" /
               (time.strftime("%Y%m%d-%H%M%S") + "-" +
                str(os.getpid()) + "-" + uuid.uuid4().hex[:8]))
    run_dir.mkdir(parents=True)
    (run_dir / "eula.txt").write_text("eula=true\n", encoding="ascii")
    (run_dir / "server.properties").write_text(
        "server-ip=127.0.0.1\nserver-port=%d\nonline-mode=true\n"
        "spawn-protection=0\nview-distance=3\nsimulation-distance=3\n"
        "max-players=1\nlevel-name=block-light-world\n"
        "pause-when-empty-seconds=-1\n" % port(), encoding="ascii")

    classes = run_dir / "agent-classes"
    classes.mkdir()
    versions = ROOT / "versions" / "26.1" / "server-26.1.jar"
    libs = [str(path) for path in (ROOT / "libraries").rglob("*.jar")]
    compile_cp = os.pathsep.join([str(versions), *libs])
    javac = pathlib.Path(JAVA).with_name("javac.exe")
    subprocess.run([str(javac), "-cp", compile_cp, "-d", str(classes),
                    str(ROOT / "LightDiffAgent.java")], check=True)
    manifest = run_dir / "MANIFEST.MF"
    manifest.write_text("Manifest-Version: 1.0\nPremain-Class: LightDiffAgent\n\n",
                        encoding="ascii")
    agent = run_dir / "light-diff-agent.jar"
    subprocess.run([str(pathlib.Path(JAVA).with_name("jar.exe")), "cfm",
                    str(agent), str(manifest), "-C", str(classes), "."],
                   check=True)

    classpath = os.pathsep.join([str(versions), *libs, str(agent)])
    log_path = run_dir / "server.log"
    log = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        [JAVA, "-Xms512M", "-Xmx2G", "-javaagent:" + str(agent),
         "-cp", classpath, "net.minecraft.server.Main", "nogui"],
        cwd=run_dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1, encoding="utf-8",
        errors="replace")
    lines = queue.Queue()

    def reader():
        for line in process.stdout:
            log.write(line)
            log.flush()
            lines.put(line)

    threading.Thread(target=reader, daemon=True).start()

    def wait_line(pattern, timeout=300):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if process.poll() is not None:
                raise RuntimeError("server exited " + str(process.returncode))
            try:
                line = lines.get(timeout=1)
            except queue.Empty:
                continue
            match = re.search(pattern, line)
            if match:
                return match
        raise RuntimeError("timed out waiting for " + pattern)

    result = {"schema": 1, "jar_sha1": JAR_SHA1, "stages": {}}
    try:
        wait_line(r"Done \(", 600)
        process.stdin.write(
            "tick freeze\n"
            "gamerule random_tick_speed 0\n"
            "gamerule spawn_mobs false\n"
            "forceload add -2 -2 2 2\n")
        process.stdin.flush()
        for stage, commands in STAGES.items():
            process.stdin.write("\n".join(commands) + "\n")
            process.stdin.flush()
            match = wait_line(r"BLOCKLIGHT_JSON (\{.*\})")
            payload = json.loads(match.group(1))
            if payload["stage"] != stage:
                raise RuntimeError("unexpected light stage " + str(payload))
            result["stages"][str(stage)] = payload["block_light"]
        result["commands"] = {str(stage): commands
                              for stage, commands in STAGES.items()}
    finally:
        if process.poll() is None:
            try:
                process.stdin.write("stop\n")
                process.stdin.flush()
                process.wait(timeout=60)
            except Exception:
                process.kill()
        log.close()

    candidate = run_dir / "block-light-candidate.json"
    candidate.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    run_manifest = {
        "jar_sha1": JAR_SHA1,
        "run_dir": str(run_dir),
        "candidate_sha256": hashlib.sha256(candidate.read_bytes()).hexdigest(),
        "server_log_sha256": hashlib.sha256(log_path.read_bytes()).hexdigest(),
        "stages": len(result["stages"]),
        "cells": len(result["stages"]["1"]),
    }
    (run_dir / "run-manifest.json").write_text(
        json.dumps(run_manifest, indent=1) + "\n", encoding="utf-8")
    golden = ROOT / "block-light-golden.json"
    if args.promote_golden:
        shutil.copyfile(candidate, golden)
    elif golden.exists():
        if json.loads(golden.read_text(encoding="utf-8")) != result:
            raise SystemExit("block-light candidate differs from golden")
    print(json.dumps(run_manifest, indent=1))


if __name__ == "__main__":
    main()
