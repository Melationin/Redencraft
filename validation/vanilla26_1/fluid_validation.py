"""F-7 fluid differential harness.

Runs one sealed 1-wide water channel on the official 26.1 dedicated server and
records the channel cells after each tick, so the C++ side can be compared
against the same fixture and observation points.

RNG streams are independent on the two sides. This fixture has no random input
(water has a fixed delay, and no lava surface rise occurs), so that
independence cannot perturb the compared values.
"""
import json, os, pathlib, queue, re, subprocess, sys, threading, time

root = pathlib.Path(__file__).resolve().parent
TICKS = int(os.environ.get('FLUID_TICKS', '12'))  # must match the CTest value
PORT = 25597


def eula_ok():
    f = root / 'eula.txt'
    return f.exists() and 'eula=true' in f.read_text(encoding='utf-8')


# Sealed channel at y=80, x in [-2, 6], z = 0.
X0, X1, Y, Z = -2, 6, 80, 0
CELLS = [(x, Y, Z) for x in range(X0, X1 + 1)]


def layout_commands():
    cmds = []
    for x in range(X0, X1 + 1):
        cmds.append(f'fill {x} {Y-1} {Z} {x} {Y-1} {Z} minecraft:stone')
        cmds.append(f'fill {x} {Y+1} {Z} {x} {Y+1} {Z} minecraft:stone')
        cmds.append(f'fill {x} {Y} {Z-1} {x} {Y} {Z-1} minecraft:stone')
        cmds.append(f'fill {x} {Y} {Z+1} {x} {Y} {Z+1} minecraft:stone')
        cmds.append(f'fill {x} {Y} {Z} {x} {Y} {Z} minecraft:air')
    cmds.append(f'fill {X0} {Y} {Z} {X0} {Y} {Z} minecraft:stone')
    cmds.append(f'fill {X1} {Y} {Z} {X1} {Y} {Z} minecraft:stone')
    return cmds


def snapshot_commands(tag):
    # One marker per cell carrying its level, so the log itself is the record.
    cmds = []
    for i, (x, y, z) in enumerate(CELLS):
        cmds.append(
            f'execute if block {x} {y} {z} minecraft:air run say SNAP {tag} {i} air')
        for level in range(0, 16):
            cmds.append(
                f'execute if block {x} {y} {z} minecraft:water[level={level}] '
                f'run say SNAP {tag} {i} water{level}')
        cmds.append(
            f'execute if block {x} {y} {z} minecraft:stone run say SNAP {tag} {i} stone')
    cmds.append(f'say SNAP_END {tag}')
    return cmds


def run_vanilla(ticks):
    java = os.environ.get('JAVA_26', r'C:\Program Files\Zulu\zulu-26\bin\java.exe')
    # Each run gets its OWN directory and port so it cannot inherit state from a
    # previous run, and so two runs can never share a world or a socket.
    run_dir = root / 'fluid-runs' / time.strftime('%Y%m%d-%H%M%S')
    run_dir.mkdir(parents=True, exist_ok=True)
    port = PORT + (int(time.time()) % 200)
    (run_dir / 'eula.txt').write_text('eula=true\n', encoding='ascii')
    (run_dir / 'server.properties').write_text(
        'server-ip=127.0.0.1\nserver-port=%d\nonline-mode=true\n'
        'spawn-protection=0\nview-distance=3\nsimulation-distance=3\n'
        'max-players=1\nlevel-name=fluid-validation-world\n'
        'pause-when-empty-seconds=-1\n' % port,
        encoding='ascii')
    p = subprocess.Popen(
        [java, '-Xms512M', '-Xmx2G', '-jar', str(root / 'server.jar'), 'nogui'],
        cwd=run_dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1, encoding='utf-8',
        errors='replace')
    lines = queue.Queue()
    recorded = []
    log = (root / 'fluid-vanilla.log').open('w', encoding='utf-8')

    def reader():
        for line in p.stdout:
            log.write(line)
            log.flush()
            lines.put(line)

    threading.Thread(target=reader, daemon=True).start()

    def wait_for(text, timeout=300):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if p.poll() is not None:
                raise RuntimeError(f'server exited {p.returncode}')
            try:
                line = lines.get(timeout=1)
            except queue.Empty:
                continue
            if text in line:
                return
        raise RuntimeError('Timed out waiting for ' + text)

    def command(s):
        p.stdin.write(s + '\n')
        p.stdin.flush()
        time.sleep(0.18)

    def game_time():
        # The server echoes gameTime for `time query gametime`; parse the number
        # so sampling is pinned to a tick boundary instead of a sleep.
        command('time query gametime')
        end = time.monotonic() + 30
        while time.monotonic() < end:
            try:
                line = lines.get(timeout=1)
            except queue.Empty:
                continue
            m = re.search(r'time is (\d+)', line) or re.search(r'(\d+)\s*$', line)
            if 'gametime' in line.lower() or 'time is' in line.lower():
                if m:
                    return int(m.group(1))
        raise RuntimeError('could not read gameTime')

    def wait_game_time(target):
        end = time.monotonic() + 60
        while time.monotonic() < end:
            if game_time() >= target:
                return
        raise RuntimeError('gameTime did not reach %d' % target)

    def capture(tag):
        for cmd in snapshot_commands(tag):
            command(cmd)
        # Drain until the end marker for this tag.
        end = time.monotonic() + 60
        while time.monotonic() < end:
            try:
                line = lines.get(timeout=1)
            except queue.Empty:
                continue
            m = re.search(r'SNAP (\S+) (\d+) (\S+)', line)
            if m and m.group(1) == tag:
                recorded.append({'t': tag, 'cell': int(m.group(2)),
                                 'state': m.group(3)})
            if f'SNAP_END {tag}' in line:
                return

    try:
        wait_for('Done (')
        command('tick freeze')
        command('gamerule random_tick_speed 0')
        command('gamerule spawn_mobs false')
        command('gamerule advance_weather false')
        command('gamerule water_source_conversion true')
        command('gamerule lava_source_conversion false')
        command('forceload add -16 -16 15 15')
        for cmd in layout_commands():
            command(cmd)
        capture('T0')
        command('setblock %d %d %d minecraft:water[level=0]' % (0, Y, Z))
        for t in range(1, ticks + 1):
            # Synchronize on gameTime rather than sleeping: the golden must be
            # tied to an actual tick boundary, not to wall-clock guesswork.
            before = game_time()
            command('tick step 1')
            wait_game_time(before + 1)
            capture('T%d' % t)
    finally:
        if p.poll() is None:
            command('stop')
            try:
                p.wait(timeout=60)
            except subprocess.TimeoutExpired:
                p.kill()
        log.close()
    return recorded


if __name__ == '__main__':
    if not eula_ok():
        raise SystemExit('Accept the Minecraft EULA in this directory first.')
    data = run_vanilla(TICKS)
    # Only write the golden once the run proved complete and unique.
    expected = (TICKS + 1) * len(CELLS)
    if len(data) != expected:
        raise SystemExit('incomplete sampling: %d of %d observations'
                         % (len(data), expected))
    keys = [(r['t'], r['cell']) for r in data]
    if len(keys) != len(set(keys)):
        raise SystemExit('duplicate observations; refusing to update the golden')
    ticks = sorted({r['t'] for r in data}, key=lambda s: int(s[1:]))
    if len(ticks) != TICKS + 1:
        raise SystemExit('expected %d ticks, got %d' % (TICKS + 1, len(ticks)))
    out = root / 'fluid-vanilla-states.json'
    out.write_text(json.dumps(data, indent=1), encoding='utf-8')
    print('recorded', len(data), 'cell observations ->', out)