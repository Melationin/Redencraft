"""Extract original 26.1 vanilla tag definitions, retaining nested references."""
import argparse
import hashlib
import json
import zipfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--jar', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if hashlib.sha256(args.jar.read_bytes()).hexdigest() != 'a7fed6f7d88379349e35ae0c6e9881d4484605132f6f620376a3868eea6cce52':
    raise SystemExit('unexpected 26.1 server classes hash')
prefix = 'data/minecraft/tags/block/'
rows = []
with zipfile.ZipFile(args.jar) as jar:
    for path in sorted(jar.namelist()):
        if not path.startswith(prefix) or not path.endswith('.json'):
            continue
        data = json.loads(jar.read(path))
        if data.get('replace', False):
            raise SystemExit(f'unexpected replace in vanilla tag {path}')
        entries = []
        for item in data['values']:
            if isinstance(item, str):
                entries.append(item)
            else:
                entries.append(item['id'] + ('' if item.get('required', True) else '?'))
        rows.append('minecraft:' + path[len(prefix):-5] + '\t' + ','.join(entries))
args.output.write_text('\n'.join(rows) + '\n', encoding='utf-8')
print(f'extracted {len(rows)} vanilla tag definitions')
