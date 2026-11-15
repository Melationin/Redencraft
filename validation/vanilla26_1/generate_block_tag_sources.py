"""Compile original vanilla tag definition graph and vanilla block names."""
import argparse
import hashlib
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = args.root / 'assets/block26_1/block_tag_sources.tsv'
if hashlib.sha256(source.read_bytes()).hexdigest() != '813798eec9b64e94584a876200080599e873a4806c9cfeb888b1a1b12e9fcf8a':
    raise SystemExit('vanilla tag sources differ from pinned official export')
rows = source.read_text().splitlines()
out = ['// Generated from original vanilla tag sources; do not edit.',
       'static constexpr SourceDefinition kVanillaTagSources[] = {']
for line in rows:
    name, entries = line.split('\t')
    out.append(f'    {{"{name}", "{entries}"}},')
out += ['};', 'static constexpr std::string_view kVanillaBlockNames[] = {']
names = sorted(line.split('\t')[1] for line in
               (args.root / 'assets/protocol775/blocks.tsv').read_text().splitlines())
out += [f'    "{name}",' for name in names]
out += ['};']
args.output.write_text('\n'.join(out) + '\n', encoding='utf-8')
