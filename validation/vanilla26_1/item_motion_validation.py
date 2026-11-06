"""Compare the carpet-duper item entity trajectory with a real 26.1 server.

This probe mirrors the piston/slime structure in run_validation.py, then reads
the item entity Pos/Motion after each frozen tick. The post-hit X velocity
sequence is deterministic; the tick on which the queued retraction event hits
the item can vary with command/tick scheduling, and Y/Z include random spawn and
stack velocities. C++ therefore asserts the launch direction and the local
one-tick -1.0 launch, not a single server-run absolute position.

Run with Java 26 at the default Zulu path or set JAVA_26. Accept the EULA in
this directory first. The script overwrites item-motion-validation.log.
"""
import os
import pathlib
import queue
import subprocess
import threading
import time

root = pathlib.Path(__file__).resolve().parent
if not (root / "eula.txt").exists() or "eula=true" not in (
    root / "eula.txt"
).read_text(encoding="utf-8"):
    raise SystemExit("Accept the Minecraft EULA in this directory before running this test.")
(root / "server.properties").write_text(
    "server-ip=127.0.0.1\nserver-port=25597\nonline-mode=true\n"
    "spawn-protection=0\nview-distance=3\nsimulation-distance=3\n"
    "max-players=1\nlevel-name=carpet-validation-world\n",
    encoding="ascii",
)
java = os.environ.get("JAVA_26", r"C:\Program Files\Zulu\zulu-26\bin\java.exe")
log_path = root / "item-motion-validation.log"
process = subprocess.Popen(
    [java, "-Xms512M", "-Xmx2G", "-jar", "server.jar", "nogui"],
    cwd=root,
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    bufsize=1,
    encoding="utf-8",
    errors="replace",
)
lines = queue.Queue()
log = log_path.open("w", encoding="utf-8")


def reader():
    for line in process.stdout:
        print(line, end="", flush=True)
        log.write(line)
        log.flush()
        lines.put(line)


threading.Thread(target=reader, daemon=True).start()


def wait_for(text, timeout=300):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if process.poll() is not None:
            raise RuntimeError(f"server exited {process.returncode}")
        try:
            line = lines.get(timeout=1)
        except queue.Empty:
            continue
        if text in line:
            return
    raise RuntimeError("Timed out waiting for " + text)


def command(text, wait=0.4):
    log.write("COMMAND: " + text + "\n")
    log.flush()
    process.stdin.write(text + "\n")
    process.stdin.flush()
    time.sleep(wait)


def item_data(tag):
    command("data get entity @e[type=minecraft:item,limit=1] Pos")
    command("data get entity @e[type=minecraft:item,limit=1] Motion")
    end = time.monotonic() + 0.5
    found = []
    while time.monotonic() < end:
        try:
            found.append(lines.get(timeout=0.05).rstrip())
        except queue.Empty:
            pass
    marker = f"ITEM_{tag} " + " | ".join(
        line for line in found if "entity data" in line or "No entity" in line
    )
    print(marker, flush=True)
    log.write(marker + "\n")
    log.flush()


try:
    wait_for("Done (")
    command("tick freeze")
    command("forceload add -16 -16 15 15")
    command("fill -3 99 -3 5 104 3 minecraft:air replace")
    command("kill @e[type=minecraft:item]")
    command("setblock 0 100 0 minecraft:sticky_piston[facing=east,extended=false]")
    command("setblock 1 100 0 minecraft:slime_block")
    command("setblock 1 101 0 minecraft:slime_block")
    command("setblock -1 100 0 minecraft:redstone_block")
    command("tick step 3", 0.8)
    command("setblock 1 101 0 minecraft:white_carpet")
    command("setblock -1 100 0 minecraft:air")
    for tick in range(1, 6):
        command("tick step 1", 0.8)
        item_data(f"TICK{tick}")
    command("say ITEM_MOTION_VALIDATION_END")
    wait_for("ITEM_MOTION_VALIDATION_END", 30)
finally:
    if process.poll() is None:
        command("stop")
        try:
            process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            process.kill()
    log.close()
