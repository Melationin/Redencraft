"""Check the pinned vanilla block tags and generated C++ lookup."""
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(sys.argv[1])
with tempfile.TemporaryDirectory() as tmp:
    output = Path(tmp) / 'block_tags.inc'
    subprocess.run([sys.executable, str(root / 'validation/vanilla26_1/generate_block_tags.py'),
                    '--root', str(root), '--output', str(output)], check=True)
    if output.read_text(encoding='utf-8') != (root / 'src/block_tags.inc').read_text(encoding='utf-8'):
        raise SystemExit('generated block tag table is stale')
    sources = Path(tmp) / 'block_tag_sources.inc'
    subprocess.run([sys.executable, str(root / 'validation/vanilla26_1/generate_block_tag_sources.py'),
                    '--root', str(root), '--output', str(sources)], check=True)
    if sources.read_text(encoding='utf-8') != (root / 'src/block_tag_sources.inc').read_text(encoding='utf-8'):
        raise SystemExit('generated vanilla tag source graph is stale')
