"""M-01 real-server differential: water next to lava.

Two sealed one-cell cases at y=80 and y=90: a lava source and a flowing lava
level=2, each with a water source on the north side. The shell is built and the
water placed first; T0 is captured, then the lava is written at T0. Every tick
samples the two liquids and the five surrounding cells with strict block-state
commands. Run artifacts and the run log stay under build-fluid-validation/.
"""
import hashlib
import json
import os
import pathlib
import re
import socket
import subprocess
import time
import uuid

root = pathlib.Path(__file__).resolve().parent
JAR_SHA1 = "3872a7f07a1a595e651aef8b058dfc2bb3772f46"
JAVA = os.environ.get('JAVA_26', r'C:\Program Files\Zulu\zulu-26\bin\java.exe')
TICKS = 10


def pick_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def shell_commands(y):
    cmds = []
    for z in range(-2, 3):
        cmds.append(f'fill 0 {y-1} {z} 0 {y-1} {z} minecraft:stone')
        cmds.append(f'fill 0 {y+1} {z} 0 {y+1} {z} minecraft:stone')
        cmds.append(f'fill -1 {y} {z} -1 {y} {z} minecraft:stone')
        cmds.append(f'fill 1 {y} {z} 1 {y} {z} minecraft:stone')
    cmds.append(f'fill 0 {y} -2 0 {y} 2 minecraft:air')
    cmds.append(f'fill 0 {y} -2 0 {y} -2 minecraft:stone')
    cmds.append(f'fill 0 {y} 2 0 {y} 2 minecraft:stone')
    return cmds


CELLS = [(0, 0, -1), (0, 0, 0), (0, 0, 1), (-1, 0, 0), (1, 0, 0),
         (0, -1, 0), (0, 1, 0)]


def sample_commands(y, tag):
    cmds = []
    for i, (dx, dy, dz) in enumerate(CELLS):
        x, yy, z = dx, y + dy, dz
        for state in ['minecraft:air', 'minecraft:stone', 'minecraft:obsidian',
                      'minecraft:cobblestone']:
            cmds.append(f'execute if block {x} {yy} {z} {state} '
                        f'run say MIX {tag} {i} {state.split(":")[1]}')
        for level in range(16):
            cmds.append(f'execute if block {x} {yy} {z} '
                        f'minecraft:water[level={level}] '
                        f'run say MIX {tag} {i} water{level}')
            cmds.append(f'execute if block {x} {yy} {z} '
                        f'minecraft:lava[level={level}] '
                        f'run say MIX {tag} {i} lava{level}')
    cmds.append(f'say MIX_END {tag}')
    return cmds


def main():
    if not (root / 'eula.txt').exists() or 'eula=true' not in (
            root / 'eula.txt').read_text(encoding='utf-8'):
        raise SystemExit('Accept the Minecraft EULA first.')
    sha = hashlib.sha1((root / 'server.jar').read_bytes()).hexdigest()
    if sha != JAR_SHA1:
        raise SystemExit(f'server.jar SHA-1 {sha} != {JAR_SHA1}')
    base = root.parents[1] / 'build-fluid-validation' / 'mixing-runs'
    base.mkdir(parents=True, exist_ok=True)
    run_dir = base / (time.strftime('%Y%m%d-%H%M%S') +
                      f'-{os.getpid()}-{uuid.uuid4().hex[:8]}')
    run_dir.mkdir(parents=True)
    log_path = run_dir / 'server.log'
    port = pick_port()
    (run_dir / 'eula.txt').write_text('eula=true\n', encoding='ascii')
    (run_dir / 'server.properties').write_text(
        'server-ip=127.0.0.1\nserver-port=%d\nonline-mode=true\n'
        'spawn-protection=0\nview-distance=3\nsimulation-distance=3\n'
        'max-players=1\nlevel-name=mixing-validation-world\n'
        'pause-when-empty-seconds=-1\n' % port,
        encoding='ascii')
    log = log_path.open('w', encoding='utf-8')
    p = subprocess.Popen(
        [JAVA, '-Xms512M', '-Xmx2G', '-jar', str(root / 'server.jar'), 'nogui'],
        cwd=run_dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1, encoding='utf-8',
        errors='replace')
    counter = 0

    def line():
        value = p.stdout.readline()
        if not value:
            raise RuntimeError(f'server exited {p.returncode}')
        log.write(value)
        log.flush()
        return value

    def send(lines):
        for s in lines:
            p.stdin.write(s + '\n')
        p.stdin.flush()

    def sync(commands):
        nonlocal counter
        counter += 1
        marker = f'__MIX_SYNC_{counter}_{uuid.uuid4().hex[:6]}'
        send(list(commands) + [f'say {marker}'])
        while marker not in line():
            pass

    def game_time():
        nonlocal counter
        counter += 1
        marker = f'__MIX_GT_{counter}_{uuid.uuid4().hex[:6]}'
        send(['time query gametime', f'say {marker}'])
        value = None
        while True:
            text = line()
            match = re.search(r'time is (\d+)', text, re.IGNORECASE)
            if match:
                value = int(match.group(1))
            if marker in text:
                if value is None:
                    raise RuntimeError('no gameTime')
                return value

    def step(n):
        before = game_time()
        send([f'tick step {n}'])
        while game_time() != before + n:
            pass

    def capture(y, tag):
        send(sample_commands(y, tag))
        values = {}
        while True:
            text = line()
            match = re.search(r'MIX (%s) (\d+) (\S+)' % tag, text)
            if match:
                cell = int(match.group(2))
                if cell in values:
                    raise RuntimeError('duplicate cell')
                values[cell] = match.group(3)
            if f'MIX_END {tag}' in text:
                if len(values) != len(CELLS):
                    raise RuntimeError(f'{tag}: {len(values)} cells')
                return [{'case': case, 't': tag, 'cell': cell,
                         'state': values[cell]} for cell in range(len(CELLS))]

    records = []
    try:
        while 'Done (' not in line():
            pass
        sync(['tick freeze', 'gamerule random_tick_speed 0',
              'gamerule spawn_mobs false', 'gamerule advance_weather false',
              'gamerule water_source_conversion true',
              'gamerule lava_source_conversion false',
              'forceload add -16 -16 15 15'])
        step(100)
        for case, y, lava in [('m01-source', 80, 'minecraft:lava[level=0]'),
                              ('m01-flowing', 90, 'minecraft:lava[level=2]')]:
            sync(shell_commands(y))
            sync([f'setblock 0 {y} -1 minecraft:water[level=0]'])
            t0 = game_time()
            records.extend(capture(y, 'T0'))
            sync([f'setblock 0 {y} 0 {lava}'])
            for t in range(1, TICKS + 1):
                step(1)
                records.extend(capture(y, f'T{t}'))
            records.append({'case': case, 't': 'T0_game_time', 'cell': -1,
                            'state': str(t0)})
        print(json.dumps({'jar_sha1': sha, 'records': records,
                          'run_dir': str(run_dir)}, sort_keys=True))
        (run_dir / 'fluid-vanilla-mixing.json').write_text(
            json.dumps(records, indent=1), encoding='utf-8')
    finally:
        if p.poll() is None:
            try:
                send(['stop'])
                p.wait(timeout=60)
            except Exception:
                p.kill()
        log.close()
    print('run artifacts:', run_dir)


if __name__ == '__main__':
    main()
