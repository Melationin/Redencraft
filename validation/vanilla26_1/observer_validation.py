"""Observe two facing 26.1 observers on the official dedicated server.

This is the D-class fixture behind the C++ regression
`facingObserversLatchInsteadOfOscillating`. It uses a unique run directory and a
checked-free loopback port, freezes the world, starts from A off / B on, and
records both observers once per stepped tick. No client is started.
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
    base = root.parents[1] / 'build-fluid-validation' / 'observer-runs'
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
        'max-players=1\nlevel-name=observer-validation-world\n'
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
        marker = f'__OBS_SYNC_{counter}_{uuid.uuid4().hex[:6]}'
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
        end = time.monotonic() + 60
        while time.monotonic() < end:
            if game_time() == before + n:
                return
        raise RuntimeError('tick step did not reach target')

    def sample(tag):
        cmds = [
            'execute if block 0 80 0 minecraft:observer[facing=east,powered=false] run say OBS %s A 0' % tag,
            'execute if block 0 80 0 minecraft:observer[facing=east,powered=true] run say OBS %s A 1' % tag,
            'execute if block 1 80 0 minecraft:observer[facing=west,powered=false] run say OBS %s B 0' % tag,
            'execute if block 1 80 0 minecraft:observer[facing=west,powered=true] run say OBS %s B 1' % tag,
            'say OBS_END %s' % tag,
        ]
        values = []
        send(cmds)
        while True:
            value = line()
            match = re.search(r'OBS (%s) ([AB]) ([01])' % tag, value)
            if match:
                values.append(f'{match.group(2)}={match.group(3)}')
            if f'OBS_END {tag}' in value:
                if len(values) != 2:
                    raise RuntimeError(f'{tag}: {values}')
                return sorted(values)

    try:
        while 'Done (' not in line():
            pass
        sync(['tick freeze',
              'gamerule random_tick_speed 0',
              'gamerule spawn_mobs false',
              'gamerule advance_weather false',
              'forceload add -16 -16 15 15'])
        step(100)  # let forceloaded chunks become tickable before T0
        # Normal placement, exactly like the C++ fixture: the shape updates
        # caused by placing the second observer next to the first are part of
        # the behaviour, not something the fixture filters out.
        send([
            'setblock 0 80 0 minecraft:observer[facing=east,powered=false]',
            'setblock 1 80 0 minecraft:observer[facing=west,powered=false]',
        ])
        sync(['say OBS_LAYOUT'])
        records = []
        for t in range(1, 21):
            step(1)
            records.append({'t': t, 'states': sample('T%d' % t)})
        print(json.dumps({'jar_sha1': sha, 't0_game_time': game_time(),
                          'records': records, 'run_dir': str(run_dir)},
                         sort_keys=True))
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
