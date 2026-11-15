import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
CACHE = RUNTIME / "mods_cache"
MASTER_DIR = RUNTIME / "world_master"
RTBENCH_JAR = ROOT / "mod" / "build" / "libs" / "modid-1.0.0.jar"


def run_profile(profile: str, mods: list[Path]) -> str:
    if not MASTER_DIR.exists() or not (MASTER_DIR / "level.dat").exists():
        raise RuntimeError(f"Missing world_master at {MASTER_DIR}; run run_gen.py first")
    if not RTBENCH_JAR.exists():
        raise RuntimeError(f"Missing built mod jar at {RTBENCH_JAR}")

    run_dir = RUNTIME / f"bench_{profile}"
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy2(CACHE / "fabric-server-26.1.jar", run_dir / "fabric-server-26.1.jar")
    mods_dir = run_dir / "mods"
    mods_dir.mkdir(parents=True, exist_ok=True)
    for jar in mods:
        shutil.copy2(jar, mods_dir / jar.name)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]

    shutil.copytree(MASTER_DIR, run_dir / "world")
    (run_dir / "eula.txt").write_text("eula=true\n", encoding="utf-8")
    (run_dir / "server.properties").write_text(
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
                f"server-port={port}",
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
        "-Drtbench.enabled=true",
        "-Drtbench.stopEntities=true",
        "-Drtbench.chunkCount=9000",
        "-Drtbench.warmupTicks=100",
        "-Drtbench.measureTicks=1000",
        "-Drtbench.randomTickSpeed=3",
        "-jar",
        "fabric-server-26.1.jar",
        "nogui",
    ]
    print(f"\n=== Running Java benchmark profile: {profile} ===", flush=True)
    t0 = time.monotonic()
    proc = subprocess.Popen(
        cmd,
        cwd=str(run_dir),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )

    result_line = ""
    killed_after_result = False
    shutdown_timer = None

    def force_stop_if_hung():
        nonlocal killed_after_result
        if proc.poll() is None:
            killed_after_result = True
            proc.terminate()

    assert proc.stdout is not None
    try:
        for line in proc.stdout:
            line_s = line.rstrip("\r\n")
            if any(
                k in line_s
                for k in (
                    "RTBENCH",
                    "Sprint completed",
                    "Starting minecraft server",
                    "Loaded",
                    "Stopping server",
                    "ERROR",
                    "Exception",
                    "Caused by",
                    "Mixin",
                )
            ):
                print(f"[{profile}] {line_s}", flush=True)
            if "RTBENCH_RESULT " in line_s and not result_line:
                result_line = line_s[line_s.index("RTBENCH_RESULT ") :]
                shutdown_timer = threading.Timer(8.0, force_stop_if_hung)
                shutdown_timer.daemon = True
                shutdown_timer.start()
            if result_line and "Exception stopping the server" in line_s:
                killed_after_result = True
                proc.terminate()
                break
    finally:
        if shutdown_timer is not None:
            shutdown_timer.cancel()

    if killed_after_result:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        rc = 0
    else:
        rc = proc.wait()
    elapsed = time.monotonic() - t0
    print(f"[{profile}] exited rc={rc} wall={elapsed:.1f}s", flush=True)
    if rc != 0 or not result_line:
        raise RuntimeError(f"Profile {profile} failed (rc={rc})")
    return result_line


def main():
    profiles = sys.argv[1:] if len(sys.argv) > 1 else ["vanilla", "lithium"]
    results = {}
    for p in profiles:
        if p == "vanilla":
            mods = [RTBENCH_JAR]
        elif p == "lithium":
            mods = [RTBENCH_JAR, CACHE / "lithium-fabric-0.24.7+mc26.1.2.jar"]
        else:
            raise ValueError(f"Unknown profile: {p}")
        results[p] = run_profile(p, mods)

    print("\n=== Summary ===")
    for p, line in results.items():
        print(f"{p}: {line}")


if __name__ == "__main__":
    main()
