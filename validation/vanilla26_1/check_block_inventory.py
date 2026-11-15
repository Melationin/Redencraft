"""Check official inventory provenance and completeness against 775 states."""
import hashlib
import json
from collections import defaultdict
from pathlib import Path
import sys

root = Path(sys.argv[1])
inventory = root / "assets/block26_1/block_inventory.tsv"
manifest = json.loads((inventory.parent / "manifest.json").read_text(encoding="utf-8"))
assert manifest["jar_sha1"] == "3872a7f07a1a595e651aef8b058dfc2bb3772f46"
assert hashlib.sha256(inventory.read_bytes()).hexdigest() == manifest["inventory_sha256"]
blocks = [line.split("\t") for line in
          (root / "assets/protocol775/blocks.tsv").read_text().splitlines()]
states = defaultdict(set)
for line in (root / "assets/protocol775/block_states.tsv").read_text().splitlines():
    _, name, properties = line.split("\t")
    states[name].add(properties)
rows = inventory.read_text(encoding="utf-8").splitlines()
assert len(rows) == len(blocks) == manifest["rows"]
for row, block in zip(rows, blocks):
    fields = row.split("\t")
    assert len(fields) == 7 and fields[:2] == block
    _, name, java_class, default, count, random, entities = fields
    assert java_class.startswith("net.minecraft.world.level.block.")
    assert int(count) == len(states[name]) and default in states[name]
    assert 0 <= int(random) <= int(count)
    assert 0 <= int(entities) <= int(count)
print(f"verified {len(rows)} official blocks and their default/state counts")
