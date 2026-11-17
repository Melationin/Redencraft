"""Generate C++ item declarations, registry tables, and 26.1 vanilla item tags.

Sources (read-only 26.1 references):
- assets/protocol775/items.tsv          (1,506 official vanilla items in protocol ID order)
- assets/protocol775/blocks.tsv         (1,168 official vanilla blocks in protocol ID order)
- assets/protocol775/configuration.bin  (207 official resolved vanilla minecraft:item tags)
- horizon-test/resources/items.json     (default item components filtered strictly to vanilla IDs)
- ../net/minecraft/world/item/Items.java
- ../net/minecraft/world/level/block/Blocks.java
- ../net/minecraft/world/entity/EntityType.java
- ../net/minecraft/world/level/block/ComposterBlock.java
- ../net/minecraft/world/level/block/entity/FuelValues.java
"""
from __future__ import annotations

import json
import pathlib
import re
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent
JAVA_ROOT = ROOT.parent / "net/minecraft"

ITEMS_TSV = ROOT / "assets/protocol775/items.tsv"
BLOCKS_TSV = ROOT / "assets/protocol775/blocks.tsv"
CONFIG_BIN = ROOT / "assets/protocol775/configuration.bin"
ITEMS_JSON = ROOT / "horizon-test/resources/items.json"

ITEMS_JAVA = JAVA_ROOT / "world/item/Items.java"
BLOCKS_JAVA = JAVA_ROOT / "world/level/block/Blocks.java"
ENTITY_TYPE_JAVA = JAVA_ROOT / "world/entity/EntityType.java"
COMPOSTER_JAVA = JAVA_ROOT / "world/level/block/ComposterBlock.java"
FUEL_JAVA = JAVA_ROOT / "world/level/block/entity/FuelValues.java"

DECL_INC = ROOT / "include/redstone/items_decl.inc"
REG_INC = ROOT / "src/items_registry.inc"
TAGS_INC = ROOT / "src/item_tags.inc"


def c_str(s: str) -> str:
    escaped = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def c_float(v: float) -> str:
    s = f"{float(v):.9g}"
    if "." not in s and "e" not in s and "E" not in s:
        s += ".0"
    return s + "f"


def read_varint(buf: bytes, pos: int) -> tuple[int, int]:
    val = 0
    shift = 0
    while True:
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            return val, pos
        shift += 7


def read_str(buf: bytes, pos: int) -> tuple[str, int]:
    length, pos = read_varint(buf, pos)
    return buf[pos : pos + length].decode("utf-8"), pos + length


def load_tsv(path: pathlib.Path) -> list[tuple[int, str]]:
    rows: list[tuple[int, str]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        idx_str, name = line.split("\t", 1)
        rows.append((int(idx_str), name))
    return rows


def extract_vanilla_item_tags(item_names: list[str]) -> dict[str, list[str]]:
    data = CONFIG_BIN.read_bytes()
    pos = 0
    tags: dict[str, list[str]] = {}
    while pos < len(data):
        pkt_len = int.from_bytes(data[pos : pos + 4], "big")
        pos += 4
        end = pos + pkt_len
        chunk = data[pos:end]
        pid, q = read_varint(chunk, 0)
        if pid == 13:  # configuration clientbound minecraft:update_tags
            reg_count, q = read_varint(chunk, q)
            for _ in range(reg_count):
                reg_name, q = read_str(chunk, q)
                tag_count, q = read_varint(chunk, q)
                for _ in range(tag_count):
                    tag_name, q = read_str(chunk, q)
                    entry_count, q = read_varint(chunk, q)
                    ids: list[int] = []
                    for _ in range(entry_count):
                        eid, q = read_varint(chunk, q)
                        ids.append(eid)
                    if reg_name == "minecraft:item":
                        tags[tag_name] = [item_names[eid] for eid in ids]
        pos = end
    if not tags:
        raise RuntimeError("No minecraft:item tags found in configuration.bin")
    return dict(sorted(tags.items()))


def build_block_field_map() -> dict[str, str]:
    b_src = BLOCKS_JAVA.read_text(encoding="utf-8")
    result: dict[str, str] = {}
    for field, reg_name in re.findall(
        r'public static final Block ([A-Z0-9_]+)\s*=\s*register\(\s*"([a-z0-9_]+)"',
        b_src,
    ):
        result[field] = "minecraft:" + reg_name
    # Also handle WeatheringCopperBlocks.create(prefix)
    # COPPER_BARS -> minecraft:copper_bars / exposed_copper_bars / ...
    for field in re.findall(
        r"public static final WeatheringCopperBlocks ([A-Z0-9_]+)", b_src
    ):
        base = field.lower()
        result[field] = "minecraft:" + base
        for prefix in (
            "exposed_",
            "weathered_",
            "oxidized_",
            "waxed_",
            "waxed_exposed_",
            "waxed_weathered_",
            "waxed_oxidized_",
        ):
            result[f"{prefix.upper()}{field}"] = f"minecraft:{prefix}{base}"
    return result


def build_entity_field_map() -> dict[str, str]:
    e_src = ENTITY_TYPE_JAVA.read_text(encoding="utf-8")
    result: dict[str, str] = {}
    for field, reg_name in re.findall(
        r'public static final EntityType<[^>]+>\s*([A-Z0-9_]+)\s*=\s*register\(\s*"([a-z0-9_]+)"',
        e_src,
    ):
        result[field] = "minecraft:" + reg_name
    return result


CLASS_TO_BEHAVIOR = {
    "AirItem": "Air",
    "BlockItem": "Block",
    "StandingAndWallBlockItem": "StandingAndWallBlock",
    "DoubleHighBlockItem": "DoubleHighBlock",
    "PlaceOnWaterBlockItem": "PlaceOnWaterBlock",
    "ScaffoldingBlockItem": "ScaffoldingBlock",
    "GameMasterBlockItem": "GameMasterBlock",
    "SignItem": "Sign",
    "HangingSignItem": "HangingSign",
    "BedItem": "Bed",
    "BannerItem": "Banner",
    "PlayerHeadItem": "PlayerHead",
    "AxeItem": "Axe",
    "ShovelItem": "Shovel",
    "HoeItem": "Hoe",
    "ShearsItem": "Shears",
    "BrushItem": "Brush",
    "FlintAndSteelItem": "FlintAndSteel",
    "FireChargeItem": "FireCharge",
    "BoneMealItem": "BoneMeal",
    "HoneycombItem": "Honeycomb",
    "BucketItem": "Bucket",
    "SolidBucketItem": "SolidBucket",
    "MobBucketItem": "MobBucket",
    "BottleItem": "Bottle",
    "MinecartItem": "Minecart",
    "BoatItem": "Boat",
    "SnowballItem": "Snowball",
    "EggItem": "Egg",
    "EnderpearlItem": "EnderPearl",
    "ExperienceBottleItem": "ExperienceBottle",
    "WindChargeItem": "WindCharge",
    "FireworkRocketItem": "FireworkRocket",
    "BowItem": "Bow",
    "CrossbowItem": "Crossbow",
    "TridentItem": "Trident",
    "MaceItem": "Mace",
    "ArrowItem": "Arrow",
    "SpectralArrowItem": "SpectralArrow",
    "TippedArrowItem": "TippedArrow",
    "ArmorStandItem": "ArmorStand",
    "ItemFrameItem": "ItemFrame",
    "EndCrystalItem": "EndCrystal",
    "HangingEntityItem": "HangingEntity",
    "SpawnEggItem": "SpawnEgg",
    "PotionItem": "Potion",
    "SplashPotionItem": "SplashPotion",
    "LingeringPotionItem": "LingeringPotion",
    "DyeItem": "Dye",
    "InkSacItem": "InkSac",
    "GlowInkSacItem": "GlowInkSac",
    "BundleItem": "Bundle",
    "ShieldItem": "Shield",
    "SpyglassItem": "Spyglass",
    "CompassItem": "Compass",
    "MapItem": "Map",
    "EmptyMapItem": "EmptyMap",
    "WritableBookItem": "WritableBook",
    "WrittenBookItem": "WrittenBook",
    "EnderEyeItem": "EnderEye",
    "FishingRodItem": "FishingRod",
    "LeadItem": "Lead",
    "NameTagItem": "NameTag",
    "DebugStickItem": "DebugStick",
    "KnowledgeBookItem": "KnowledgeBook",
    "InstrumentItem": "Instrument",
    "SmithingTemplateItem": "SmithingTemplate",
    "DiscFragmentItem": "DiscFragment",
}


def parse_items_java(
    block_fields: dict[str, str],
    entity_fields: dict[str, str],
    item_names_set: set[str],
    block_names_set: set[str],
) -> tuple[dict[str, dict[str, Any]], dict[str, str]]:
    src = ITEMS_JAVA.read_text(encoding="utf-8")
    decls = re.findall(r"public static final Item ([A-Z0-9_]+)\s*=\s*(.*?);", src, re.S)

    item_info: dict[str, dict[str, Any]] = {}
    by_block: dict[str, str] = {}

    # Expand WeatheringCopperItems
    for field in ("COPPER_BARS", "COPPER_CHAIN", "COPPER_LANTERN"):
        base = field.lower()
        for prefix in (
            "",
            "exposed_",
            "weathered_",
            "oxidized_",
            "waxed_",
            "waxed_exposed_",
            "waxed_weathered_",
            "waxed_oxidized_",
        ):
            name = f"minecraft:{prefix}{base}"
            item_info[name] = {
                "behavior": "Block",
                "primaryBlock": name,
                "wallBlock": "",
                "craftRemainder": "",
                "spawnEntity": "",
                "javaClass": "BlockItem",
            }
            by_block[name] = name

    for field_name, expr in decls:
        expr_clean = " ".join(expr.split())
        name: str | None = None
        behavior = "Item"
        java_class = "Item"
        primary_block = ""
        wall_block = ""
        craft_rem = ""
        spawn_entity = ""

        # craftRemainder
        cm = re.search(r"craftRemainder\((?:Items\.)?([A-Z0-9_]+)\)", expr_clean)
        if cm:
            rem_field = cm.group(1).lower()
            rem_name = "minecraft:" + rem_field
            if rem_name in item_names_set:
                craft_rem = rem_name

        # Spawn eggs
        se = re.match(r"registerSpawnEgg\(EntityType\.([A-Z0-9_]+)\)", expr_clean)
        if se:
            ent = entity_fields.get(se.group(1), "minecraft:" + se.group(1).lower())
            name = ent + "_spawn_egg"
            behavior = "SpawnEgg"
            java_class = "SpawnEggItem"
            spawn_entity = ent

        # registerBlock(...)
        elif expr_clean.startswith("registerBlock("):
            bm = re.search(r"registerBlock\(\s*Blocks\.([A-Z0-9_]+)", expr_clean)
            if bm:
                bf = bm.group(1)
                primary_block = block_fields.get(bf, "minecraft:" + bf.lower())
                name = primary_block

                # Check alternatives: registerBlock(Blocks.A, Blocks.B, ...)
                alts = re.match(
                    r"registerBlock\(\s*(Blocks\.[A-Z0-9_]+(?:\s*,\s*Blocks\.[A-Z0-9_]+)+)\s*\)",
                    expr_clean,
                )
                by_block[primary_block] = name
                if alts:
                    for ab in re.findall(r"Blocks\.([A-Z0-9_]+)", alts.group(1)):
                        ab_name = block_fields.get(ab, "minecraft:" + ab.lower())
                        by_block[ab_name] = name

                # Determine subclass
                found_cls = None
                for cls in CLASS_TO_BEHAVIOR:
                    if f"new {cls}(" in expr_clean or f"{cls}::new" in expr_clean:
                        found_cls = cls
                        break
                if found_cls:
                    behavior = CLASS_TO_BEHAVIOR[found_cls]
                    java_class = found_cls
                else:
                    behavior = "Block"
                    java_class = "BlockItem"

                # Check wall block in StandingAndWall / Sign / Banner / PlayerHead
                wm = re.search(
                    r"new (?:StandingAndWallBlockItem|SignItem|HangingSignItem|BannerItem|PlayerHeadItem)\([^,]+,\s*Blocks\.([A-Z0-9_]+)",
                    expr_clean,
                )
                if wm:
                    wb_name = block_fields.get(
                        wm.group(1), "minecraft:" + wm.group(1).lower()
                    )
                    wall_block = wb_name
                    by_block[wb_name] = name

        # registerItem(...)
        elif expr_clean.startswith("registerItem("):
            nm = re.match(r'registerItem\(\s*"([a-z0-9_]+)"', expr_clean)
            if nm:
                name = "minecraft:" + nm.group(1)
            else:
                name = "minecraft:" + field_name.lower()

            found_cls = None
            for cls in CLASS_TO_BEHAVIOR:
                if f"new {cls}(" in expr_clean or f"{cls}::new" in expr_clean:
                    found_cls = cls
                    break
            if found_cls:
                behavior = CLASS_TO_BEHAVIOR[found_cls]
                java_class = found_cls

            # SolidBucketItem / createBlockItemWithCustomItemName / registerItem(..., Blocks.X)
            bm = re.search(
                r"(?:BlockItem|SolidBucketItem|createBlockItemWithCustomItemName)\(\s*Blocks\.([A-Z0-9_]+)",
                expr_clean,
            )
            if not bm:
                bm = re.search(r"new BlockItem\(\s*Blocks\.([A-Z0-9_]+)", expr_clean)
            if bm:
                bf = bm.group(1)
                primary_block = block_fields.get(bf, "minecraft:" + bf.lower())
                by_block[primary_block] = name
                if not found_cls:
                    behavior = "Block"
                    java_class = "BlockItem"

            # EntityType attached to BoatItem, MinecartItem, ItemFrameItem, etc.
            em = re.search(r"EntityType\.([A-Z0-9_]+)", expr_clean)
            if em:
                spawn_entity = entity_fields.get(
                    em.group(1), "minecraft:" + em.group(1).lower()
                )

        if name and name in item_names_set:
            item_info[name] = {
                "behavior": behavior,
                "primaryBlock": primary_block if primary_block in block_names_set else "",
                "wallBlock": wall_block if wall_block in block_names_set else "",
                "craftRemainder": craft_rem,
                "spawnEntity": spawn_entity,
                "javaClass": java_class,
            }

    # Explicit Projectile / entity mappings not passed as EntityType args in Items.java
    EXPLICIT_SPAWN = {
        "minecraft:snowball": "minecraft:snowball",
        "minecraft:egg": "minecraft:egg",
        "minecraft:brown_egg": "minecraft:egg",
        "minecraft:blue_egg": "minecraft:egg",
        "minecraft:ender_pearl": "minecraft:ender_pearl",
        "minecraft:experience_bottle": "minecraft:experience_bottle",
        "minecraft:wind_charge": "minecraft:wind_charge",
        "minecraft:armor_stand": "minecraft:armor_stand",
        "minecraft:end_crystal": "minecraft:end_crystal",
        "minecraft:firework_rocket": "minecraft:firework_rocket",
        "minecraft:fire_charge": "minecraft:small_fireball",
        "minecraft:arrow": "minecraft:arrow",
        "minecraft:spectral_arrow": "minecraft:spectral_arrow",
        "minecraft:tipped_arrow": "minecraft:arrow",
        "minecraft:trident": "minecraft:trident",
    }
    for iname, ename in EXPLICIT_SPAWN.items():
        if iname in item_info and not item_info[iname]["spawnEntity"]:
            item_info[iname]["spawnEntity"] = ename

    # Tool subclass fallbacks by name/behavior if direct lambda wasn't named PickaxeItem
    for iname, info in item_info.items():
        if iname == "minecraft:air":
            info["behavior"] = "Air"
            info["javaClass"] = "AirItem"
        elif iname.endswith("_pickaxe") and info["behavior"] == "Item":
            info["behavior"] = "Pickaxe"
        elif iname.endswith("_sword") and info["behavior"] == "Item":
            info["behavior"] = "Sword"
        elif iname.endswith("_spear") and info["behavior"] == "Item":
            info["behavior"] = "Spear"
        elif iname == "minecraft:wooden_shovel" or iname.endswith("_shovel"):
            info["behavior"] = "Shovel"

    return item_info, by_block


def parse_compostables(item_names_set: set[str]) -> dict[str, float]:
    c_src = COMPOSTER_JAVA.read_text(encoding="utf-8")
    out: dict[str, float] = {}
    for prob_s, field in re.findall(r"add\(([0-9.]+)F,\s*Items\.([A-Z0-9_]+)\)", c_src):
        # Java field DRY_SHORT_GRASS is item minecraft:short_dry_grass; DRY_TALL_GRASS -> tall_dry_grass
        remap = {
            "DRY_SHORT_GRASS": "minecraft:short_dry_grass",
            "DRY_TALL_GRASS": "minecraft:tall_dry_grass",
        }
        iname = remap.get(field, "minecraft:" + field.lower())
        if iname in item_names_set:
            out[iname] = float(prob_s)
    return out


def parse_fuel_times(
    tags: dict[str, list[str]],
    by_block: dict[str, str],
    item_names_set: set[str],
    block_fields: dict[str, str],
) -> dict[str, int]:
    src = FUEL_JAVA.read_text(encoding="utf-8")
    body = re.search(
        r"vanillaBurnTimes\([^)]*int baseUnit\)\s*\{(.*?)\}", src, re.S
    )
    if not body:
        raise RuntimeError("Could not locate FuelValues.vanillaBurnTimes")
    text = body.group(1)
    base_unit = 200
    fuels: dict[str, int] = {}

    def eval_ticks(expr: str) -> int:
        # Safe eval on baseUnit arithmetic
        return int(eval(expr, {"__builtins__": {}}, {"baseUnit": base_unit}))

    for call, arg, ticks_expr in re.findall(
        r"\.(add|remove)\(([^,]+?)(?:,\s*([^)]+))?\)", text
    ):
        arg = arg.strip()
        if call == "add":
            ticks = eval_ticks(ticks_expr.strip())
            if arg.startswith("Items."):
                iname = "minecraft:" + arg.split(".", 1)[1].lower()
                if iname in item_names_set:
                    fuels[iname] = ticks
            elif arg.startswith("Blocks."):
                bf = arg.split(".", 1)[1]
                bname = block_fields.get(bf, "minecraft:" + bf.lower())
                iname = by_block.get(bname, bname)
                if iname in item_names_set:
                    fuels[iname] = ticks
            elif arg.startswith("ItemTags."):
                tname = "minecraft:" + arg.split(".", 1)[1].lower()
                for iname in tags.get(tname, []):
                    fuels[iname] = ticks
        elif call == "remove":
            if arg.startswith("ItemTags."):
                tname = "minecraft:" + arg.split(".", 1)[1].lower()
                for iname in tags.get(tname, []):
                    fuels.pop(iname, None)
    return fuels


RARITY_ENUM = {
    "common": "ItemRarity::Common",
    "uncommon": "ItemRarity::Uncommon",
    "rare": "ItemRarity::Rare",
    "epic": "ItemRarity::Epic",
}

SLOT_ENUM = {
    "feet": "EquipSlotKind::Feet",
    "legs": "EquipSlotKind::Legs",
    "chest": "EquipSlotKind::Chest",
    "head": "EquipSlotKind::Head",
    "offhand": "EquipSlotKind::OffHand",
    "body": "EquipSlotKind::Body",
    "saddle": "EquipSlotKind::Saddle",
}

USE_ANIM_ENUM = {
    None: "ItemUseAnim::None",
    "eat": "ItemUseAnim::Eat",
    "drink": "ItemUseAnim::Drink",
    "block": "ItemUseAnim::Block",
    "bow": "ItemUseAnim::Bow",
    "trident": "ItemUseAnim::Trident",
    "crossbow": "ItemUseAnim::Crossbow",
    "spyglass": "ItemUseAnim::Spyglass",
    "toot_horn": "ItemUseAnim::TootHorn",
    "brush": "ItemUseAnim::Brush",
    "bundle": "ItemUseAnim::Bundle",
    "spear": "ItemUseAnim::Spear",
}


def main() -> None:
    items_tsv = load_tsv(ITEMS_TSV)
    blocks_tsv = load_tsv(BLOCKS_TSV)
    item_names = [name for _, name in items_tsv]
    item_names_set = set(item_names)
    block_names = [name for _, name in blocks_tsv]
    block_names_set = set(block_names)

    tags = extract_vanilla_item_tags(item_names)
    raw_items_json: dict[str, Any] = json.loads(
        ITEMS_JSON.read_text(encoding="utf-8")
    )
    block_fields = build_block_field_map()
    entity_fields = build_entity_field_map()
    java_info, by_block = parse_items_java(
        block_fields, entity_fields, item_names_set, block_names_set
    )
    compostables = parse_compostables(item_names_set)
    fuels = parse_fuel_times(tags, by_block, item_names_set, block_fields)

    missing_java = [n for n in item_names if n not in java_info]
    missing_json = [n for n in item_names if n not in raw_items_json]
    if missing_java or missing_json:
        raise RuntimeError(
            f"Missing items: java={missing_java[:5]} json={missing_json[:5]}"
        )

    # Deduplicate tool rule slices
    all_tool_rules: list[tuple[str, bool, float, bool, int]] = []
    item_rule_ranges: dict[str, tuple[int, int]] = {}
    for iname in item_names:
        comp = raw_items_json[iname]
        tool = comp.get("minecraft:tool")
        if tool and tool.get("rules"):
            start = len(all_tool_rules)
            for rule in tool["rules"]:
                blocks_spec = rule["blocks"]
                is_tag = blocks_spec.startswith("#")
                target = blocks_spec[1:] if is_tag else blocks_spec
                has_speed = "speed" in rule
                speed = float(rule.get("speed", 1.0))
                if "correct_for_drops" in rule:
                    cfd = 1 if rule["correct_for_drops"] else 0
                else:
                    cfd = -1
                all_tool_rules.append((target, is_tag, speed, has_speed, cfd))
            item_rule_ranges[iname] = (start, len(all_tool_rules) - start)
        else:
            item_rule_ranges[iname] = (0, 0)

    # 1. Write include/redstone/items_decl.inc
    decl_lines = ["// Auto-generated by scripts/generate_items_data.py. Do not edit manually."]
    for _, name in items_tsv:
        ident = name.split(":", 1)[1].upper()
        decl_lines.append(f"extern const Item {ident};")
    DECL_INC.write_text("\n".join(decl_lines) + "\n", encoding="utf-8")

    # 2. Write src/item_tags.inc
    tag_lines = [
        "// Auto-generated by scripts/generate_items_data.py. Do not edit manually.",
        "struct VanillaItemTagSource { std::string_view tag; std::span<const std::string_view> items; };",
    ]
    for idx, (tag_name, members) in enumerate(tags.items()):
        elems = ", ".join(c_str(m) for m in members)
        if elems:
            tag_lines.append(
                f"static constexpr std::string_view kItemTagMembers_{idx}[] = {{{elems}}};"
            )
    tag_lines.append("static constexpr VanillaItemTagSource kVanillaItemTagSources[] = {")
    for idx, (tag_name, members) in enumerate(tags.items()):
        if members:
            tag_lines.append(
                f"    {{{c_str(tag_name)}, kItemTagMembers_{idx}}},"
            )
        else:
            tag_lines.append(f"    {{{c_str(tag_name)}, {{}}}},")
    tag_lines.append("};")
    TAGS_INC.write_text("\n".join(tag_lines) + "\n", encoding="utf-8")

    # 3. Write src/items_registry.inc
    reg_lines = [
        "// Auto-generated by scripts/generate_items_data.py. Do not edit manually.",
        "static constexpr ToolRuleDef kToolRules[] = {",
    ]
    if not all_tool_rules:
        reg_lines.append('    {"", false, 1.0f, false, -1},')
    else:
        for target, is_tag, speed, has_speed, cfd in all_tool_rules:
            speed_lit = c_float(speed)
            reg_lines.append(
                f"    {{{c_str(target)}, {'true' if is_tag else 'false'}, {speed_lit}, {'true' if has_speed else 'false'}, {cfd}}},"
            )
    reg_lines.append("};")

    # Block -> Item table
    reg_lines.append("struct BlockToItemEntry { std::string_view block; const Item *item; };")
    reg_lines.append("namespace Items {")

    # Item definitions
    for proto_id, iname in items_tsv:
        ident = iname.split(":", 1)[1].upper()
        comp = raw_items_json[iname]
        ji = java_info[iname]

        max_stack = int(comp.get("minecraft:max_stack_size", 64))
        max_dmg = int(comp.get("minecraft:max_damage", 0))
        rarity = RARITY_ENUM[comp.get("minecraft:rarity", "common")]
        behavior = f"ItemBehaviorKind::{ji['behavior']}"
        enchant_val = int(comp.get("minecraft:enchantable", {}).get("value", 0))
        dmg_res = comp.get("minecraft:damage_resistant", {}).get("types", "")
        fire_res = "true" if dmg_res == "#minecraft:is_fire" else "false"
        glider = "true" if "minecraft:glider" in comp else "false"
        blocks_atk = "true" if "minecraft:blocks_attacks" in comp else "false"

        tool = comp.get("minecraft:tool", {})
        can_destroy_creative = (
            "false" if tool.get("can_destroy_blocks_in_creative", True) is False else "true"
        )
        tool_dmg = int(tool.get("damage_per_block", 1 if tool else 0))
        weapon_dmg = int(comp.get("minecraft:weapon", {}).get("item_damage_per_attack", 0))

        food = comp.get("minecraft:food")
        is_food = "true" if food else "false"
        nutrition = int(food.get("nutrition", 0)) if food else 0
        saturation = c_float(food.get("saturation", 0.0)) if food else "0.0f"
        always_eat = "true" if (food and food.get("can_always_eat", False)) else "false"

        cons = comp.get("minecraft:consumable")
        if cons is not None:
            anim_key = cons.get("animation", "eat")
            use_anim = USE_ANIM_ENUM[anim_key]
            consume_sec = c_float(cons.get("consume_seconds", 1.6))
        else:
            use_anim = "ItemUseAnim::None"
            consume_sec = "0.0f"

        cd = comp.get("minecraft:use_cooldown")
        cooldown_sec = c_float(cd.get("seconds", 0.0)) if cd else "0.0f"

        atk_dmg = 0.0
        atk_spd = 0.0
        armor_val = 0.0
        armor_tough = 0.0
        kb_res = 0.0
        for mod in comp.get("minecraft:attribute_modifiers", []):
            mt = mod.get("type")
            amt = float(mod.get("amount", 0.0))
            if mt == "minecraft:attack_damage":
                atk_dmg += amt
            elif mt == "minecraft:attack_speed":
                atk_spd += amt
            elif mt == "minecraft:armor":
                armor_val += amt
            elif mt == "minecraft:armor_toughness":
                armor_tough += amt
            elif mt == "minecraft:knockback_resistance":
                kb_res += amt

        eq = comp.get("minecraft:equippable", {})
        eq_slot = SLOT_ENUM.get(eq.get("slot", ""), "EquipSlotKind::None")
        eq_swap = "true" if (eq and eq.get("swappable", True)) else "false"
        eq_dmg_hurt = "true" if (eq and eq.get("damage_on_hurt", True)) else "false"

        comp_chance = c_float(compostables.get(iname, 0.0))
        fuel_ticks = fuels.get(iname, 0)

        p_block = ji["primaryBlock"]
        w_block = ji["wallBlock"]
        spawn_ent = ji["spawnEntity"]
        use_rem = comp.get("minecraft:use_remainder", {}).get("id", "")
        craft_rem = ji["craftRemainder"]
        repair_mat = comp.get("minecraft:repairable", {}).get("items", "")
        r_start, r_count = item_rule_ranges[iname]

        meta = (
            f"ItemMetadata{{{proto_id}, {max_stack}, {max_dmg}, {rarity}, {behavior}, "
            f"{enchant_val}, {fire_res}, {glider}, {blocks_atk}, {can_destroy_creative}, "
            f"{tool_dmg}, {weapon_dmg}, {is_food}, {nutrition}, {saturation}, {always_eat}, "
            f"{use_anim}, {consume_sec}, {cooldown_sec}, "
            f"{c_float(atk_dmg)}, {c_float(atk_spd)}, {c_float(armor_val)}, {c_float(armor_tough)}, {c_float(kb_res)}, "
            f"{eq_slot}, {eq_swap}, {eq_dmg_hurt}, {comp_chance}, {fuel_ticks}, "
            f"{c_str(p_block)}, {c_str(w_block)}, {c_str(spawn_ent)}, "
            f"{c_str(use_rem)}, {c_str(craft_rem)}, {c_str(repair_mat)}, {r_start}, {r_count}}}"
        )
        reg_lines.append(f"const Item {ident}({c_str(iname)}, {meta});")

    reg_lines.append("} // namespace Items")
    reg_lines.append("static const BlockToItemEntry kBlockToItemEntries[] = {")
    for bname in block_names:
        mapped = by_block.get(bname)
        if mapped:
            ident = mapped.split(":", 1)[1].upper()
            reg_lines.append(f"    {{{c_str(bname)}, &Items::{ident}}},")
    reg_lines.append("};")
    REG_INC.write_text("\n".join(reg_lines) + "\n", encoding="utf-8")

    print(
        f"Generated {len(items_tsv)} items, {len(tags)} item tags, "
        f"{len(by_block)} block->item mappings, {len(compostables)} compostables, "
        f"{len(fuels)} fuels, {len(all_tool_rules)} tool rules."
    )


if __name__ == "__main__":
    main()
