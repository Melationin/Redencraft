import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
CACHE = RUNTIME / "mods_cache"
GEN_DIR = RUNTIME / "gen_server"
MASTER_DIR = RUNTIME / "world_master"


def copy_mods():
    mods_dir = GEN_DIR / "mods"
    if mods_dir.exists():
        shutil.rmtree(mods_dir)
    mods_dir.mkdir(parents=True, exist_ok=True)
    for jar in [
        "fabric-api-0.155.3+26.1.2.jar",
        "Chunky-Fabric-1.5.66.jar",
        "c2me-fabric-mc26.1-0.3.7+alpha.0.61.jar",
        "zfastnoise-1.1.1+26.1.jar",
        "zconfig-1.0.0+26.x.jar",
    ]:
        shutil.copy2(CACHE / jar, mods_dir / jar)


def main():
    if MASTER_DIR.exists() and (MASTER_DIR / "level.dat").exists():
        print("world_master already exists at", MASTER_DIR)
        return

    if GEN_DIR.exists():
        shutil.rmtree(GEN_DIR)
    GEN_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copy2(CACHE / "fabric-server-26.1.jar", GEN_DIR / "fabric-server-26.1.jar")
    copy_mods()

    (GEN_DIR / "eula.txt").write_text("eula=true\n", encoding="utf-8")
    (GEN_DIR / "server.properties").write_text(
        "\n".join(
            [
                "online-mode=false",
                "level-seed=20261",
                "level-type=minecraft\\:normal",
                "difficulty=peaceful",
                "spawn-protection=0",
                "view-distance=10",
                "simulation-distance=10",
                "max-tick-time=-1",
                "pause-when-empty-seconds=0",
                "enable-status=false",
                "server-port=25575",
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    jdk_java = RUNTIME / "jdk" / "bin" / "java"
    java_bin = os.environ.get("JAVA_BIN") or (str(jdk_java) if jdk_java.exists() else "java")

    cmd = [
        java_bin,
        "-Xms4G",
        "-Xmx8G",
        "-XX:+UseG1GC",
        "-jar",
        "fabric-server-26.1.jar",
        "nogui",
    ]
    print("Launching generator server in", GEN_DIR)
    t0 = time.monotonic()
    proc = subprocess.Popen(
        cmd,
        cwd=str(GEN_DIR),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )

    started = False
    chunky_started = False
    chunky_done = False
    stopping = False

    def send(line):
        assert proc.stdin is not None
        print(f">>> {line}", flush=True)
        proc.stdin.write(line + "\n")
        proc.stdin.flush()

    assert proc.stdout is not None
    for line in proc.stdout:
        line_s = line.rstrip("\r\n")
        print(line_s, flush=True)
        if not started and "Done (" in line_s and "For help, type" in line_s:
            started = True
            send("gamerule minecraft:random_tick_speed 0")
            send("gamerule minecraft:spawn_mobs false")
            send("gamerule minecraft:advance_time false")
            send("gamerule minecraft:advance_weather false")
            send("chunky quiet 5")
            send("chunky world minecraft:overworld")
            send("chunky shape square")
            send("chunky center 0 0")
            send("chunky radius 768")
            send("chunky start")
            send("chunky confirm")
            chunky_started = True
        elif chunky_started and not chunky_done and "Task finished for" in line_s:
            chunky_done = True
            send("save-all flush")
        elif chunky_done and not stopping and "Saved the game" in line_s:
            stopping = True
            send("stop")

    rc = proc.wait()
    elapsed = time.monotonic() - t0
    print(f"Generator exited rc={rc} in {elapsed:.1f}s")
    if rc != 0 or not chunky_done:
        sys.exit(1)

    if MASTER_DIR.exists():
        shutil.rmtree(MASTER_DIR)
    shutil.copytree(GEN_DIR / "world", MASTER_DIR)
    print("Saved immutable world snapshot to", MASTER_DIR)


if __name__ == "__main__":
    main()
