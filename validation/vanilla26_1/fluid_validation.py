"""F-7 fluid differential harness.

Runs one sealed 1-wide water channel on the official 26.1 dedicated server and
records the channel cells after each tick, so the C++ side can be compared
against the same fixture and observation points.

Every run gets a unique directory and a checked-free port. Sampling is tied to
an exact `time query gametime` boundary, not to a sleep. Run artifacts (log,
candidate states, manifest) stay in that directory; the checked-in golden is
only replaced with --promote-golden, and only after a complete, unique run and
an explicit --force-golden when a golden already exists.

RNG streams are independent on the two sides. This fixture has no random input
(water has a fixed delay, and no lava surface rise occurs), so that
independence cannot perturb the compared values.
"""
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
import sys
import threading
import time
import uuid

root = pathlib.Path(__file__).resolve().parent
DEFAULT_TICKS = 12
JAR_SHA1 = "3872a7f07a1a595e651aef8b058dfc2bb3772f46"

# Sealed channel at y=80, x in [-2, 6], z = 0.
X0, X1, Y, Z = -2, 6, 80, 0
CELLS = [(x, Y, Z) for x in range(X0, X1 + 1)]


def eula_ok():
    f = root / 'eula.txt'
    return f.exists() and 'eula=true' in f.read_text(encoding='utf-8')


def jar_sha1():
    h = hashlib.sha1()
    with (root / 'server.jar').open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def pick_port():
    """Bind an ephemeral loopback port, then use that exact port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


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


def compare_states(candidate, golden):
    key = lambda r: (r['t'], r['cell'])
    for name, rows in (('candidate', candidate), ('golden', golden)):
        keys = [key(r) for r in rows]
        if len(keys) != len(set(keys)):
            raise RuntimeError(f'{name} has duplicate observations')
    a = {key(r): r['state'] for r in candidate}
    b = {key(r): r['state'] for r in golden}
    missing = set(b) - set(a)
    extra = set(a) - set(b)
    if missing or extra:
        raise RuntimeError(
            f'observation sets differ: missing={len(missing)} extra={len(extra)}')
    diffs = [k for k in sorted(a) if a[k] != b[k]]
    return diffs


def run_vanilla(ticks):
    java = os.environ.get('JAVA_26', r'C:\Program Files\Zulu\zulu-26\bin\java.exe')
    if not pathlib.Path(java).exists():
        raise RuntimeError('Java 26 not found; set JAVA_26')
    # Each run gets its OWN unique directory and a port checked free at bind
    # time, so two runs can never share a world, log or socket, even in the same
    # second.
    # build-fluid-validation/ is ignored by git and stays inside the workspace;
    # the sandbox also permits creating fresh directories there.
    base = root.parents[1] / 'build-fluid-validation' / 'fluid-runs'
    base.mkdir(parents=True, exist_ok=True)
    run_dir = base / (
        time.strftime('%Y%m%d-%H%M%S') + f'-{os.getpid()}-{uuid.uuid4().hex[:8]}')
    run_dir.mkdir(parents=True)
    port = pick_port()
    log_path = run_dir / 'server.log'
    log = log_path.open('w', encoding='utf-8')
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
    marker_counter = 0
    last_output = [time.monotonic()]

    def next_line(timeout=60):
        # The runner is single-threaded: every command's output is consumed in
        # order. The watchdog below bounds a blocked pipe read, so a dead server
        # cannot hang the sampler forever.
        line = p.stdout.readline()
        if not line:
            raise RuntimeError(f'server exited {p.returncode}')
        last_output[0] = time.monotonic()
        log.write(line)
        log.flush()
        return line

    def watchdog():
        while p.poll() is None:
            time.sleep(5)
            if time.monotonic() - last_output[0] > 300:
                p.kill()
                return

    threading.Thread(target=watchdog, daemon=True).start()

    def wait_for(text, timeout=300):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = next_line(timeout - (end - time.monotonic()))
            if text in line:
                return line
        raise RuntimeError('Timed out waiting for ' + text)

    def send_lines(commands):
        for s in commands:
            p.stdin.write(s + '\n')
        p.stdin.flush()

    def marked(commands, timeout=60):
        nonlocal marker_counter
        marker_counter += 1
        marker = f'__F7_SYNC_{os.getpid()}_{marker_counter}_{uuid.uuid4().hex[:6]}'
        send_lines(list(commands) + [f'say {marker}'])
        collected = []
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = next_line(timeout - (end - time.monotonic()))
            collected.append(line)
            if marker in line:
                return collected
        raise RuntimeError('marker not observed: ' + marker)

    def game_time():
        output = marked(['time query gametime'])
        for line in reversed(output):
            match = re.search(r'time is (\d+)', line, re.IGNORECASE)
            if match:
                return int(match.group(1))
        raise RuntimeError('could not read gameTime from ' + repr(output[-3:]))

    def wait_game_time(target):
        end = time.monotonic() + 60
        while time.monotonic() < end:
            current = game_time()
            if current == target:
                return
            if current > target:
                raise RuntimeError(f'gameTime overshot: {current} > {target}')
        raise RuntimeError(f'gameTime did not reach {target}')

    def capture(tag):
        send_lines(snapshot_commands(tag))
        observations = []
        end = time.monotonic() + 120
        while time.monotonic() < end:
            line = next_line(timeout=max(1.0, end - time.monotonic()))
            match = re.search(r'SNAP (\S+) (\d+) (\S+)', line)
            if match and match.group(1) == tag:
                observations.append({'t': tag, 'cell': int(match.group(2)),
                                     'state': match.group(3)})
            if f'SNAP_END {tag}' in line:
                # The whole batch ran at one frozen gameTime; no sleeping and no
                # per-command sampling of a moving world.
                keys = [(r['t'], r['cell']) for r in observations]
                if len(keys) != len(set(keys)) or len(keys) != len(CELLS):
                    raise RuntimeError(
                        f'{tag}: {len(keys)} observations, {len(set(keys))} unique')
                return observations
        raise RuntimeError(f'{tag}: snapshot did not finish')

    t0 = None
    try:
        wait_for('Done (')
        marked(['tick freeze',
                'gamerule random_tick_speed 0',
                'gamerule spawn_mobs false',
                'gamerule advance_weather false',
                'gamerule water_source_conversion true',
                'gamerule lava_source_conversion false',
                'forceload add -16 -16 15 15'])
        send_lines(layout_commands())
        marker_counter += 1
        marker = f'__F7_LAYOUT_{marker_counter}_{uuid.uuid4().hex[:6]}'
        send_lines([f'say {marker}'])
        while marker not in next_line(timeout=120):
            pass
        # Loaded is not the same as tickable. Let the forceloaded chunks finish
        # their promotion before defining T0; otherwise the first fluid tick is
        # delayed by chunk eligibility and every later sample shifts.
        warm = game_time()
        send_lines(['tick step 100'])
        wait_game_time(warm + 100)
        # T0 is the gameTime after preparation but before the first action.
        t0 = game_time()
        recorded = capture('T0')
        marked(['setblock %d %d %d minecraft:water[level=0]' % (0, Y, Z)])
        for t in range(1, ticks + 1):
            before = game_time()
            send_lines(['tick step 1'])
            # The target is exact: a step that runs two ticks is a fixture error,
            # not something to paper over with a sleep.
            wait_game_time(before + 1)
            recorded.extend(capture('T%d' % t))
        return recorded, t0, run_dir, log_path
    finally:
        if p.poll() is None:
            try:
                send_lines(['stop'])
                p.wait(timeout=60)
            except (subprocess.TimeoutExpired, BrokenPipeError):
                p.kill()
        log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ticks', type=int, default=DEFAULT_TICKS)
    parser.add_argument('--promote-golden', action='store_true',
                        help='replace the checked-in golden after validation')
    parser.add_argument('--force-golden', action='store_true',
                        help='allow --promote-golden to replace an existing file')
    args = parser.parse_args()
    if not eula_ok():
        raise SystemExit('Accept the Minecraft EULA in this directory first.')
    actual = jar_sha1()
    if actual != JAR_SHA1:
        raise SystemExit(f'server.jar SHA-1 {actual} != {JAR_SHA1}')
    data, t0, run_dir, log_path = run_vanilla(args.ticks)
    expected = (args.ticks + 1) * len(CELLS)
    if len(data) != expected:
        raise SystemExit('incomplete sampling: %d of %d observations'
                         % (len(data), expected))
    keys = [(r['t'], r['cell']) for r in data]
    if len(keys) != len(set(keys)):
        raise SystemExit('duplicate observations; refusing to update the golden')
    ticks = sorted({r['t'] for r in data}, key=lambda s: int(s[1:]))
    if len(ticks) != args.ticks + 1:
        raise SystemExit('expected %d ticks, got %d' % (args.ticks + 1, len(ticks)))
    candidate = run_dir / 'fluid-vanilla-states.candidate.json'
    candidate.write_text(json.dumps(data, indent=1), encoding='utf-8')
    manifest = {
        'jar_sha1': actual,
        'java': os.environ.get('JAVA_26', r'C:\Program Files\Zulu\zulu-26\bin\java.exe'),
        'ticks': args.ticks,
        't0_game_time': t0,
        'cells': [list(c) for c in CELLS],
        'observations': len(data),
        'run_dir': str(run_dir),
        'log_sha1': hashlib.sha1(log_path.read_bytes()).hexdigest(),
    }
    (run_dir / 'run-manifest.json').write_text(
        json.dumps(manifest, indent=1), encoding='utf-8')
    golden = root / 'fluid-vanilla-states.json'
    if golden.exists():
        diffs = compare_states(data, json.loads(golden.read_text(encoding='utf-8')))
        if diffs:
            for key in diffs[:20]:
                print('DIFF', key, flush=True)
            raise SystemExit('%d mismatching observations' % len(diffs))
        print('candidate matches golden over', len(data), 'observations')
    if args.promote_golden:
        if golden.exists() and not args.force_golden:
            raise SystemExit('golden exists; pass --force-golden to replace it')
        golden.write_text(json.dumps(data, indent=1), encoding='utf-8')
        # Raw evidence is promoted together with the golden, never by default.
        shutil.copyfile(log_path, root / 'fluid-vanilla.log')
        print('promoted', len(data), 'observations and server.log')
    print('run artifacts:', run_dir)


if __name__ == '__main__':
    main()
