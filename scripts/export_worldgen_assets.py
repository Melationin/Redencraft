#!/usr/bin/env python3
"""从官方 26.1 server-26.1.jar 导出只读 worldgen JSON 资产到 assets/worldgen26_1/。"""

import argparse
from pathlib import Path
import shutil
import zipfile

CATEGORIES = (
    "biome/",
    "configured_carver/",
    "configured_feature/",
    "density_function/",
    "multi_noise_biome_source_parameter_list/",
    "noise/",
    "noise_settings/",
    "placed_feature/",
    "processor_list/",
    "structure/",
    "structure_set/",
    "template_pool/",
    "world_preset/",
)


def main() -> None:
  root = Path(__file__).resolve().parent.parent
  parser = argparse.ArgumentParser()
  parser.add_argument(
      "--jar",
      type=Path,
      default=root / "validation" / "vanilla26_1" / "versions" / "26.1" / "server-26.1.jar",
  )
  parser.add_argument(
      "--out",
      type=Path,
      default=root / "assets" / "worldgen26_1",
  )
  args = parser.parse_args()

  prefix = "data/minecraft/worldgen/"
  tag_prefix = "data/minecraft/tags/worldgen/biome/"
  nbt_prefix = "data/minecraft/structure/"
  extracted = 0
  with zipfile.ZipFile(args.jar, "r") as zf:
    for cat in CATEGORIES:
      target_cat = args.out / cat.rstrip("/")
      if target_cat.exists():
        shutil.rmtree(target_cat)
    for sub in (args.out / "tags" / "biome", args.out / "structure_nbt"):
      if sub.exists():
        shutil.rmtree(sub)
    for info in sorted(zf.infolist(), key=lambda i: i.filename):
      if info.filename.endswith(".json") and info.filename.startswith(prefix):
        rel = info.filename[len(prefix):]
        if not any(rel.startswith(cat) for cat in CATEGORIES):
          continue
        dst = args.out / rel
      elif info.filename.endswith(".json") and info.filename.startswith(tag_prefix):
        rel = info.filename[len(tag_prefix):]
        dst = args.out / "tags" / "biome" / rel
      elif info.filename.endswith(".nbt") and info.filename.startswith(nbt_prefix):
        rel = info.filename[len(nbt_prefix):]
        dst = args.out / "structure_nbt" / rel
      else:
        continue
      dst.parent.mkdir(parents=True, exist_ok=True)
      dst.write_bytes(zf.read(info.filename))
      extracted += 1
  print(f"Exported {extracted} worldgen assets to {args.out}")


if __name__ == "__main__":
  main()
