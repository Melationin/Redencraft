"""Read-only block coverage audit. Requires nbtlib and numpy; no world import.

BlockStates uses continuous bit packing, including entries spanning two longs:
https://github.com/maruohon/litematica/discussions/463
"""

import argparse
import collections
import hashlib
import json
import math
import pathlib
import re

import nbtlib
import numpy as np


def decode(region):
    palette = [entry.unpack() for entry in region['BlockStatePalette']]
    size = tuple(abs(int(region['Size'][axis])) for axis in 'xyz')
    volume = math.prod(size)
    bits = max(2, (len(palette) - 1).bit_length())
    words = np.asarray(region['BlockStates'], dtype=np.int64).astype(np.uint64)
    if len(words) != (volume * bits + 63) // 64:
        raise ValueError('Unexpected packed array length')
    positions = np.arange(volume, dtype=np.uint64) * np.uint64(bits)
    indices, shifts = positions >> np.uint64(6), positions & np.uint64(63)
    values = words[indices] >> shifts
    crossing = shifts + np.uint64(bits) > np.uint64(64)
    values[crossing] |= words[indices[crossing] + np.uint64(1)] << (
        np.uint64(64) - shifts[crossing])
    values &= np.uint64((1 << bits) - 1)
    if volume and int(values.max()) >= len(palette):
        raise ValueError('Packed palette index outside palette')
    # Independent scalar checks include both sides of every word boundary.
    checks = set(range(min(128, volume)))
    checks.update(range(max(0, volume - 128), volume))
    for boundary in range(64, min(volume * bits, 4096), 64):
        checks.update(i for i in (boundary // bits - 1, boundary // bits,
                                 boundary // bits + 1) if 0 <= i < volume)
    for i in checks:
        start, offset = divmod(i * bits, 64)
        scalar = int(words[start]) >> offset
        if offset + bits > 64:
            scalar |= int(words[start + 1]) << (64 - offset)
        if scalar & ((1 << bits) - 1) != int(values[i]):
            raise ValueError('Scalar/vector decode disagreement')
    return palette, size, np.bincount(values.astype(np.int64), minlength=len(palette))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('schematic', type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    root_dir = pathlib.Path(__file__).resolve().parents[1]
    engine = (root_dir / 'src/engine.cpp').read_text(encoding='utf-8')
    registry = engine.split('blockRegistry() {', 1)[1].split('return entries;', 1)[0]
    supported = set(re.findall(r'\{"(minecraft:[a-z0-9_]+)"', registry))
    entity_registry = engine.split('entityRegistry() {', 1)[1].split('return entries;', 1)[0]
    supported_entities = set(re.findall(r'\{"(minecraft:[a-z0-9_]+)"', entity_registry))
    document = nbtlib.load(args.schematic)
    block_counts, state_counts = collections.Counter(), collections.Counter()
    entities, tile_entities, ticks = collections.Counter(), collections.Counter(), collections.Counter()
    uuids = collections.Counter()
    boxes, overlaps = [], []
    unused_palette_names = set()
    volume = 0
    for name, region in document['Regions'].items():
        palette, size, counts = decode(region)
        volume += math.prod(size)
        position = tuple(int(region['Position'][axis]) for axis in 'xyz')
        signed_size = tuple(int(region['Size'][axis]) for axis in 'xyz')
        low = tuple(p + min(0, s + 1) for p, s in zip(position, signed_size))
        high = tuple(lo + n for lo, n in zip(low, size))
        for other, other_low, other_high in boxes:
            if all(max(a, b) < min(c, d) for a, b, c, d in
                   zip(low, other_low, high, other_high)):
                overlaps.append([other, str(name)])
        boxes.append((str(name), low, high))
        for state, count in zip(palette, counts):
            if not count:
                unused_palette_names.add(state['Name'])
                continue
            block_counts[state['Name']] += int(count)
            state_counts[json.dumps(state, sort_keys=True)] += int(count)
        for entity in region.get('Entities', []):
            entities[str(entity.get('id', '<missing>'))] += 1
            if 'UUID' in entity:
                uuids[tuple(int(x) for x in entity['UUID'])] += 1
        for entity in region.get('TileEntities', []):
            tile_entities[str(entity.get('id', '<missing>'))] += 1
        for key in ('PendingBlockTicks', 'PendingFluidTicks'):
            ticks[key] += len(region.get(key, []))
    replacements = {'minecraft:anvil': 'minecraft:obsidian',
                    'minecraft:chipped_anvil': 'minecraft:obsidian',
                    'minecraft:damaged_anvil': 'minecraft:obsidian'}
    rows = []
    for name, count in sorted(block_counts.items()):
        effective = replacements.get(name, name)
        status = ('replaced' if effective != name else
                  'fluid_placeholder' if name in ('minecraft:water', 'minecraft:lava') else
                  'registered' if name in supported else 'missing')
        rows.append(dict(name=name, count=count, effective_name=effective, status=status))
    report = dict(
        source=str(args.schematic.resolve()),
        sha256=hashlib.sha256(args.schematic.read_bytes()).hexdigest(),
        version=int(document['Version']),
        data_version=int(document.get('MinecraftDataVersion', 0)),
        enclosing_size={k: int(v) for k, v in document['Metadata']['EnclosingSize'].items()},
        region_count=len(boxes),
        declared_total_blocks=int(document['Metadata'].get('TotalBlocks', -1)),
        declared_total_volume=int(document['Metadata'].get('TotalVolume', -1)),
        decoded_region_volume=volume,
        decoded_non_air=sum(v for k, v in block_counts.items() if k not in
                            ('minecraft:air', 'minecraft:cave_air', 'minecraft:void_air')),
        overlapping_region_pairs=overlaps,
        blocks=rows,
        unused_palette_names=sorted(unused_palette_names - block_counts.keys()),
        block_states=[dict(state=json.loads(k), count=v) for k, v in sorted(state_counts.items())],
        entities=dict(entities),
        unsupported_entities=sorted(set(entities) - supported_entities),
        entity_uuid_count=len(uuids),
        duplicate_entity_uuid_groups=sum(n > 1 for n in uuids.values()),
        duplicate_entity_uuid_entries=sum(n - 1 for n in uuids.values()),
        tile_entities=dict(tile_entities),
        pending_ticks=dict(ticks),
        count_semantics='Sum of decoded region cells; exact unique cells when regions do not overlap.',
        coverage_semantics='Registry coverage only; registration is not proof of full vanilla parity.',
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    summary = {k: v for k, v in report.items() if k not in ('block_states',)}
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
