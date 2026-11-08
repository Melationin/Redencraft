"""Whole-server 26.1 timing for the W-01 water channel.

This is deliberately labelled whole-server: it includes vanilla's full
`ServerLevel.tick` and the dedicated-server loop, not just the fluid stage. The
C++ probe's BENCH line is `World::tick` only, so the two numbers are recorded
side by side but must not be turned into a percentage without matching stages.
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
TICKS = 1000


def pick_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def main():
    if not (root / 'eula.txt').exists() or 'eula=true' not in (
            root / 'eula.txt').read_text(encoding='utf-8'):
        raise SystemExit('Accept the Minecraft EULA first.')
    sha = hashlib.sha1((root / 'server.jar').read_bytes()).hexdigest()
    if sha != JAR_SHA1:
        raise SystemExit(f'server.jar SHA-1 {sha} != {JAR_SHA1}')
    base = root.parents[1] / 'build-fluid-validation' / 'sprint-runs'
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
        'max-players=1\nlevel-name=sprint-validation-world\n'
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
        marker = f'__SPRINT_SYNC_{counter}_{uuid.uuid4().hex[:6]}'
        send(list(commands) + [f'say {marker}'])
        out = []
        while True:
            value = line()
            out.append(value)
            if marker in value:
                return out

    def game_time():
        out = sync(['time query gametime'])
        for value in reversed(out):
            match = re.search(r'time is (\d+)', value, re.IGNORECASE)
            if match:
                return int(match.group(1))
        raise RuntimeError('no gameTime')

    def step(n):
        before = game_time()
        send([f'tick step {n}'])
        while game_time() != before + n:
            pass

    try:
        while 'Done (' not in line():
            pass
        sync(['tick freeze', 'gamerule random_tick_speed 0',
              'gamerule spawn_mobs false', 'gamerule advance_weather false',
              'gamerule water_source_conversion true',
              'gamerule lava_source_conversion false',
              'forceload add -16 -16 15 15'])
        step(100)
        cmds = []
        for x in range(-2, 7):
            cmds.append(f'fill {x} 79 0 {x} 79 0 minecraft:stone')
            cmds.append(f'fill {x} 81 0 {x} 81 0 minecraft:stone')
            cmds.append(f'fill {x} 80 -1 {x} 80 -1 minecraft:stone')
            cmds.append(f'fill {x} 80 1 {x} 80 1 minecraft:stone')
            cmds.append(f'fill {x} 80 0 {x} 80 0 minecraft:air')
        cmds += ['fill -2 80 0 -2 80 0 minecraft:stone',
                 'fill 6 80 0 6 80 0 minecraft:stone']
        sync(cmds)
        t0 = game_time()
        send(['setblock 0 80 0 minecraft:water[level=0]'])
        sync(['say SPRINT_SOURCE_PLACED'])
        # sprint unfreezes by itself and restores the frozen state afterwards.
        send([f'tick sprint {TICKS}'])
        report = None
        end = time.monotonic() + 120
        while time.monotonic() < end:
            value = line()
            match = re.search(
                r'Sprint completed with (\d+) ticks per second, or ([\d.]+) ms per tick',
                value)
            if match:
                report = {'ticks_per_second': int(match.group(1)),
                          'ms_per_tick': float(match.group(2))}
                break
        if report is None:
            raise RuntimeError('sprint report missing')
        query = sync(['tick query'])
        query_text = ''.join(query)
        p50 = re.search(r'P50[:：]\s*([\d.]+)', query_text)
        p95 = re.search(r'P95[:：]\s*([\d.]+)', query_text)
        p99 = re.search(r'P99[:：]\s*([\d.]+)', query_text)
        result = {
            'jar_sha1': sha,
            'stage': 'vanilla whole dedicated-server tick (includes lighting/random/save loop)',
            'ticks': TICKS,
            't0_game_time': t0,
            'sprint': report,
            'query_p50_ms': float(p50.group(1)) if p50 else None,
            'query_p95_ms': float(p95.group(1)) if p95 else None,
            'query_p99_ms': float(p99.group(1)) if p99 else None,
            'run_dir': str(run_dir),
        }
        print(json.dumps(result, sort_keys=True))
        (run_dir / 'fluid-vanilla-sprint.json').write_text(
            json.dumps(result, indent=1), encoding='utf-8')
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
