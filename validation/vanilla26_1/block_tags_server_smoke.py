"""Exercise explicit pack startup and console block-tag reload, without clients."""
import subprocess
import sys
import tempfile
import gzip
import struct
from pathlib import Path

with tempfile.TemporaryDirectory(prefix='redstone-tag-reload-') as tmp:
    path = Path(tmp) / 'data/minecraft/tags/block'
    path.mkdir(parents=True)
    (path / 'snow.json').write_text('{"replace":true,"values":["minecraft:stone"]}', encoding='utf-8')
    result = subprocess.run([sys.argv[1], '--port', '0', '--data-pack', tmp, '--ticks', '20'],
                            input='reload\nstop\n', capture_output=True, text=True, timeout=30)
    if result.returncode != 0 or 'BLOCK_TAGS reload complete' not in result.stdout:
        raise SystemExit(f'block tag reload failed: {result.returncode}\n{result.stdout}\n{result.stderr}')
    print('explicit data pack and console tag reload passed')

with tempfile.TemporaryDirectory(prefix='redstone-world-tags-') as tmp:
    world = Path(tmp)
    (world / 'region').mkdir()
    tags = world / 'datapacks/example/data/minecraft/tags/block'
    tags.mkdir(parents=True)
    (tags / 'snow.json').write_text('{"replace":true,"values":["minecraft:stone"]}', encoding='utf-8')
    def name(value):
        data = value.encode('utf-8')
        return struct.pack('>H', len(data)) + data
    def tag(kind, key, value):
        return bytes([kind]) + name(key) + value
    enabled = bytes([8]) + struct.pack('>i', 2) + name('vanilla') + name('file/example')
    data = (tag(3, 'DataVersion', struct.pack('>i', 4786)) +
            tag(4, 'Time', struct.pack('>q', 0)) +
            tag(10, 'DataPacks', tag(9, 'Enabled', enabled) + b'\0') + b'\0')
    (world / 'level.dat').write_bytes(gzip.compress(b'\x0a\0\0' + tag(10, 'Data', data) + b'\0'))
    result = subprocess.run([sys.argv[1], '--port', '0', '--world', str(world), '--ticks', '20'],
                            input='reload\nstop\n', capture_output=True, text=True, timeout=30)
    if result.returncode != 0 or 'BLOCK_TAGS loaded packs=1' not in result.stdout or 'BLOCK_TAGS reload complete' not in result.stdout:
        raise SystemExit(f'world data pack selection failed: {result.returncode}\n{result.stdout}\n{result.stderr}')
    print('level.dat data pack selection and reload passed')
