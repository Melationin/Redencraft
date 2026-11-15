"""Generate C++ block declarations, definitions, and state shape tables from 26.1 official exports."""
from __future__ import annotations

import itertools
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASICS_TSV = ROOT / "assets/block26_1/block_basics.tsv"
STATES_TSV = ROOT / "assets/block26_1/block_state_basics.tsv"
PROTOCOL_STATES_TSV = ROOT / "assets/protocol775/block_states.tsv"
BLOCKS_CPP = ROOT / "src/blocks.cpp"

DECL_INC = ROOT / "include/redstone/blocks_decl.inc"
REG_INC = ROOT / "src/blocks_registry.inc"
SHAPES_INC = ROOT / "src/block_shapes.inc"

FLAG_MAP = {
    "air_like": 0,
    "rail": 1,
    "liquid": 2,
    "carpet": 3,
    "wall": 4,
    "fence": 5,
    "fence_gate": 6,
    "stairs": 7,
    "slab": 8,
    "door": 9,
    "trapdoor": 10,
    "button": 11,
    "pressure_plate": 12,
    "sign": 13,
    "leaves": 14,
    "sapling": 15,
    "falling": 16,
    "coral_fan": 17,
    "coral_wall_fan": 18,
    "snowy": 19,
    "flower": 20,
    "vegetation": 21,
    "dry_vegetation": 22,
    "rotated_pillar": 23,
    "bed": 24,
    "chest": 25,
    "shulker_box": 26,
    "piston": 27,
    "moving_piston": 28,
    "copper_bulb": 29,
    "candle": 30,
    "cauldron": 31,
    "banner": 32,
    "skull": 33,
    "waterlogged_host": 34,
}

INSTRUMENT_MAP = {
    "harp": "Instrument::Harp",
    "basedrum": "Instrument::Basedrum",
    "snare": "Instrument::Snare",
    "hat": "Instrument::Hat",
    "bass": "Instrument::Bass",
    "flute": "Instrument::Flute",
    "bell": "Instrument::Bell",
    "guitar": "Instrument::Guitar",
    "chime": "Instrument::Chime",
    "xylophone": "Instrument::Xylophone",
    "iron_xylophone": "Instrument::IronXylophone",
    "cow_bell": "Instrument::CowBell",
    "didgeridoo": "Instrument::Didgeridoo",
    "bit": "Instrument::Bit",
    "banjo": "Instrument::Banjo",
    "pling": "Instrument::Pling",
    "trumpet": "Instrument::Trumpet",
    "trumpet_exposed": "Instrument::TrumpetExposed",
    "trumpet_oxidized": "Instrument::TrumpetOxidized",
    "trumpet_weathered": "Instrument::TrumpetWeathered",
    "zombie": "Instrument::Zombie",
    "skeleton": "Instrument::Skeleton",
    "creeper": "Instrument::Creeper",
    "dragon": "Instrument::Dragon",
    "wither_skeleton": "Instrument::WitherSkeleton",
    "piglin": "Instrument::Piglin",
    "custom_head": "Instrument::CustomHead",
}


SUPPORTED_BLOCKS = {
    "minecraft:air",
    "minecraft:stone",
    "minecraft:granite",
    "minecraft:polished_granite",
    "minecraft:diorite",
    "minecraft:polished_diorite",
    "minecraft:andesite",
    "minecraft:polished_andesite",
    "minecraft:grass_block",
    "minecraft:dirt",
    "minecraft:coarse_dirt",
    "minecraft:podzol",
    "minecraft:cobblestone",
    "minecraft:oak_planks",
    "minecraft:spruce_planks",
    "minecraft:birch_planks",
    "minecraft:jungle_planks",
    "minecraft:acacia_planks",
    "minecraft:cherry_planks",
    "minecraft:dark_oak_planks",
    "minecraft:pale_oak_wood",
    "minecraft:pale_oak_planks",
    "minecraft:mangrove_planks",
    "minecraft:bamboo_planks",
    "minecraft:bamboo_mosaic",
    "minecraft:bedrock",
    "minecraft:water",
    "minecraft:lava",
    "minecraft:sand",
    "minecraft:red_sand",
    "minecraft:gravel",
    "minecraft:gold_ore",
    "minecraft:deepslate_gold_ore",
    "minecraft:iron_ore",
    "minecraft:deepslate_iron_ore",
    "minecraft:coal_ore",
    "minecraft:deepslate_coal_ore",
    "minecraft:nether_gold_ore",
    "minecraft:oak_log",
    "minecraft:spruce_log",
    "minecraft:birch_log",
    "minecraft:jungle_log",
    "minecraft:acacia_log",
    "minecraft:cherry_log",
    "minecraft:dark_oak_log",
    "minecraft:pale_oak_log",
    "minecraft:mangrove_log",
    "minecraft:mangrove_roots",
    "minecraft:muddy_mangrove_roots",
    "minecraft:bamboo_block",
    "minecraft:stripped_spruce_log",
    "minecraft:stripped_birch_log",
    "minecraft:stripped_jungle_log",
    "minecraft:stripped_acacia_log",
    "minecraft:stripped_cherry_log",
    "minecraft:stripped_dark_oak_log",
    "minecraft:stripped_pale_oak_log",
    "minecraft:stripped_oak_log",
    "minecraft:stripped_mangrove_log",
    "minecraft:stripped_bamboo_block",
    "minecraft:oak_wood",
    "minecraft:spruce_wood",
    "minecraft:birch_wood",
    "minecraft:jungle_wood",
    "minecraft:acacia_wood",
    "minecraft:cherry_wood",
    "minecraft:dark_oak_wood",
    "minecraft:mangrove_wood",
    "minecraft:stripped_oak_wood",
    "minecraft:stripped_spruce_wood",
    "minecraft:stripped_birch_wood",
    "minecraft:stripped_jungle_wood",
    "minecraft:stripped_acacia_wood",
    "minecraft:stripped_cherry_wood",
    "minecraft:stripped_dark_oak_wood",
    "minecraft:stripped_pale_oak_wood",
    "minecraft:stripped_mangrove_wood",
    "minecraft:glass",
    "minecraft:lapis_ore",
    "minecraft:deepslate_lapis_ore",
    "minecraft:lapis_block",
    "minecraft:sandstone",
    "minecraft:chiseled_sandstone",
    "minecraft:cut_sandstone",
    "minecraft:note_block",
    "minecraft:powered_rail",
    "minecraft:detector_rail",
    "minecraft:sticky_piston",
    "minecraft:piston",
    "minecraft:piston_head",
    "minecraft:white_wool",
    "minecraft:orange_wool",
    "minecraft:magenta_wool",
    "minecraft:light_blue_wool",
    "minecraft:yellow_wool",
    "minecraft:lime_wool",
    "minecraft:pink_wool",
    "minecraft:gray_wool",
    "minecraft:light_gray_wool",
    "minecraft:cyan_wool",
    "minecraft:purple_wool",
    "minecraft:blue_wool",
    "minecraft:brown_wool",
    "minecraft:green_wool",
    "minecraft:red_wool",
    "minecraft:black_wool",
    "minecraft:moving_piston",
    "minecraft:tnt",
    "minecraft:obsidian",
    "minecraft:rail",
    "minecraft:soul_soil",
    "minecraft:basalt",
    "minecraft:stone_bricks",
    "minecraft:oak_fence_gate",
    "minecraft:redstone_lamp",
    "minecraft:cobblestone_wall",
    "minecraft:redstone_block",
    "minecraft:activator_rail",
    "minecraft:white_terracotta",
    "minecraft:slime_block",
    "minecraft:white_carpet",
    "minecraft:orange_carpet",
    "minecraft:magenta_carpet",
    "minecraft:light_blue_carpet",
    "minecraft:yellow_carpet",
    "minecraft:lime_carpet",
    "minecraft:pink_carpet",
    "minecraft:gray_carpet",
    "minecraft:light_gray_carpet",
    "minecraft:cyan_carpet",
    "minecraft:purple_carpet",
    "minecraft:blue_carpet",
    "minecraft:brown_carpet",
    "minecraft:green_carpet",
    "minecraft:red_carpet",
    "minecraft:black_carpet",
    "minecraft:smooth_stone",
    "minecraft:birch_fence_gate",
    "minecraft:observer",
    "minecraft:dead_tube_coral_fan",
    "minecraft:dead_brain_coral_fan",
    "minecraft:dead_bubble_coral_fan",
    "minecraft:dead_fire_coral_fan",
    "minecraft:dead_horn_coral_fan",
    "minecraft:dead_tube_coral_wall_fan",
    "minecraft:dead_brain_coral_wall_fan",
    "minecraft:dead_bubble_coral_wall_fan",
    "minecraft:dead_fire_coral_wall_fan",
    "minecraft:dead_horn_coral_wall_fan",
    "minecraft:blue_ice",
    "minecraft:stone_brick_wall",
    "minecraft:honey_block",
    "minecraft:moss_block",
    # Audited and completed beyond the original 153:
    "minecraft:mycelium",
    "minecraft:budding_amethyst",
    "minecraft:copper_block",
    "minecraft:exposed_copper",
    "minecraft:weathered_copper",
    "minecraft:oxidized_copper",
    "minecraft:cut_copper",
    "minecraft:exposed_cut_copper",
    "minecraft:weathered_cut_copper",
    "minecraft:oxidized_cut_copper",
    "minecraft:chiseled_copper",
    "minecraft:exposed_chiseled_copper",
    "minecraft:weathered_chiseled_copper",
    "minecraft:oxidized_chiseled_copper",
    # Building-block families audited with placement / shape / fluid rules:
    "minecraft:crimson_planks",
    "minecraft:warped_planks",
    "minecraft:moss_carpet",
    "minecraft:mossy_cobblestone",
    "minecraft:mossy_stone_bricks",
    "minecraft:cracked_stone_bricks",
    "minecraft:chiseled_stone_bricks",
    "minecraft:packed_mud",
    "minecraft:mud_bricks",
    "minecraft:mud",
    "minecraft:rooted_dirt",
    "minecraft:resin_block",
    "minecraft:resin_bricks",
    "minecraft:chiseled_resin_bricks",
    "minecraft:nether_bricks",
    "minecraft:red_nether_bricks",
    "minecraft:chiseled_nether_bricks",
    "minecraft:cracked_nether_bricks",
    "minecraft:quartz_block",
    "minecraft:chiseled_quartz_block",
    "minecraft:quartz_bricks",
    "minecraft:smooth_quartz",
    "minecraft:prismarine",
    "minecraft:prismarine_bricks",
    "minecraft:dark_prismarine",
    "minecraft:red_sandstone",
    "minecraft:chiseled_red_sandstone",
    "minecraft:cut_red_sandstone",
    "minecraft:smooth_sandstone",
    "minecraft:smooth_red_sandstone",
    "minecraft:purpur_block",
    "minecraft:end_stone",
    "minecraft:end_stone_bricks",
    "minecraft:blackstone",
    "minecraft:polished_blackstone",
    "minecraft:polished_blackstone_bricks",
    "minecraft:cracked_polished_blackstone_bricks",
    "minecraft:chiseled_polished_blackstone",
    "minecraft:tuff",
    "minecraft:polished_tuff",
    "minecraft:chiseled_tuff",
    "minecraft:tuff_bricks",
    "minecraft:chiseled_tuff_bricks",
    "minecraft:calcite",
    "minecraft:cobbled_deepslate",
    "minecraft:polished_deepslate",
    "minecraft:deepslate_tiles",
    "minecraft:deepslate_bricks",
    "minecraft:chiseled_deepslate",
    "minecraft:cracked_deepslate_bricks",
    "minecraft:cracked_deepslate_tiles",
    "minecraft:smooth_basalt",
    "minecraft:dripstone_block",
    "minecraft:clay",
    "minecraft:netherrack",
    "minecraft:nether_wart_block",
    "minecraft:warped_wart_block",
    "minecraft:dried_kelp_block",
    "minecraft:honeycomb_block",
    "minecraft:amethyst_block",
    "minecraft:pale_moss_block",
    "minecraft:glowstone",
    "minecraft:sea_lantern",
    "minecraft:shroomlight",
    "minecraft:coal_block",
    "minecraft:diamond_block",
    "minecraft:emerald_block",
    "minecraft:netherite_block",
    "minecraft:raw_iron_block",
    "minecraft:raw_copper_block",
    "minecraft:raw_gold_block",
    "minecraft:dead_tube_coral_block",
    "minecraft:dead_brain_coral_block",
    "minecraft:dead_bubble_coral_block",
    "minecraft:dead_fire_coral_block",
    "minecraft:dead_horn_coral_block",
    "minecraft:waxed_copper_block",
    "minecraft:waxed_exposed_copper",
    "minecraft:waxed_weathered_copper",
    "minecraft:waxed_oxidized_copper",
    "minecraft:waxed_cut_copper",
    "minecraft:waxed_exposed_cut_copper",
    "minecraft:waxed_weathered_cut_copper",
    "minecraft:waxed_oxidized_cut_copper",
    "minecraft:waxed_chiseled_copper",
    "minecraft:waxed_exposed_chiseled_copper",
    "minecraft:waxed_weathered_chiseled_copper",
    "minecraft:waxed_oxidized_chiseled_copper",
    "minecraft:copper_grate",
    "minecraft:exposed_copper_grate",
    "minecraft:weathered_copper_grate",
    "minecraft:oxidized_copper_grate",
    "minecraft:waxed_copper_grate",
    "minecraft:waxed_exposed_copper_grate",
    "minecraft:waxed_weathered_copper_grate",
    "minecraft:waxed_oxidized_copper_grate",
    "minecraft:iron_bars",
    "minecraft:glass_pane",
    "minecraft:copper_bars",
    "minecraft:exposed_copper_bars",
    "minecraft:weathered_copper_bars",
    "minecraft:oxidized_copper_bars",
    "minecraft:waxed_copper_bars",
    "minecraft:waxed_exposed_copper_bars",
    "minecraft:waxed_weathered_copper_bars",
    "minecraft:waxed_oxidized_copper_bars",
    "minecraft:tinted_glass",
    "minecraft:terracotta",
}

SUPPORTED_CLASS_FLAGS = {
    "slab",
    "stairs",
    "wall",
    "fence",
    "fence_gate",
    "rotated_pillar",
}

DYED_COLORS = (
    "white",
    "orange",
    "magenta",
    "light_blue",
    "yellow",
    "lime",
    "pink",
    "gray",
    "light_gray",
    "cyan",
    "purple",
    "blue",
    "brown",
    "green",
    "red",
    "black",
)


def extract_currently_supported(basics_lines: list[str]) -> set[str]:
    supported = set(SUPPORTED_BLOCKS)
    for color in DYED_COLORS:
        supported.add(f"minecraft:{color}_stained_glass")
        supported.add(f"minecraft:{color}_stained_glass_pane")
        supported.add(f"minecraft:{color}_terracotta")
        supported.add(f"minecraft:{color}_glazed_terracotta")
        supported.add(f"minecraft:{color}_concrete")
    for row in basics_lines:
        cols = row.split("\t")
        mc_name = cols[1]
        flags = set(cols[14].split(",")) if cols[14] else set()
        if flags & SUPPORTED_CLASS_FLAGS:
            supported.add(mc_name)
    return supported


def cpp_ident(mc_name: str) -> str:
    assert mc_name.startswith("minecraft:")
    return mc_name[len("minecraft:") :].upper()


def fmt_float(text: str) -> str:
    if "." not in text and "e" not in text and "E" not in text:
        return text + ".0f"
    return text + "f"


def main() -> None:
    basics_lines = BASICS_TSV.read_text(encoding="utf-8").splitlines()
    supported_set = extract_currently_supported(basics_lines)
    state_basics_lines = STATES_TSV.read_text(encoding="utf-8").splitlines()
    proto_state_lines = PROTOCOL_STATES_TSV.read_text(encoding="utf-8").splitlines()

    assert len(state_basics_lines) == len(proto_state_lines) == 29873
    assert len(basics_lines) == 1168

    # Map (block_name, sorted_prop_tuple) -> (rand_tick, blocks_motion, conductor, col_shape, out_shape)
    state_lookup: dict[tuple[str, tuple[tuple[str, str], ...]], tuple[int, int, int, str, str]] = {}
    for p_line, b_line in zip(proto_state_lines, state_basics_lines):
        _, b_name, p_str = p_line.split("\t")
        _, rt, bm, cond, col_s, out_s = b_line.split("\t")
        if p_str:
            kv = tuple(sorted(tuple(item.split("=", 1)) for item in p_str.split(",")))
        else:
            kv = ()
        state_lookup[(b_name, kv)] = (int(rt), int(bm), int(cond), col_s, out_s)

    decl_out: list[str] = ["// Auto-generated by scripts/generate_blocks_data.py. Do not edit manually."]
    reg_out: list[str] = ["// Auto-generated by scripts/generate_blocks_data.py. Do not edit manually."]

    shape_pool: dict[str, int] = {"": 0}
    shape_boxes: list[str] = []
    shape_spans: list[tuple[int, int]] = [(0, 0)]

    def intern_shape(shape_str: str) -> int:
        if shape_str in shape_pool:
            return shape_pool[shape_str]
        boxes = shape_str.split(";")
        offset = len(shape_boxes)
        for b in boxes:
            shape_boxes.append(b)
        idx = len(shape_spans)
        shape_spans.append((offset, len(boxes)))
        shape_pool[shape_str] = idx
        return idx

    cpp_state_rows: list[tuple[int, int, int]] = []

    for row in basics_lines:
        cols = row.split("\t")
        (
            _bid,
            mc_name,
            hardness,
            res,
            friction,
            speed,
            push,
            conductor,
            liquid,
            liq_container,
            force_solid,
            replaceable,
            ignited,
            instrument,
            flags_str,
            props_str,
        ) = cols
        ident = cpp_ident(mc_name)
        decl_out.append(f"extern const Block {ident};")

        is_sup = mc_name in supported_set
        # Preserve existing redstone conductor behavior for pistons
        if mc_name in ("minecraft:piston", "minecraft:sticky_piston"):
            conductor = "1"
        # Blocks.COBWEB has .forceSolidOn() in 26.1 (Blocks.java:626)
        if mc_name == "minecraft:cobweb":
            force_solid = "1"

        flag_mask = 0
        if flags_str:
            for fl in flags_str.split(","):
                flag_mask |= 1 << FLAG_MAP[fl]

        inst_cpp = INSTRUMENT_MAP[instrument]
        meta_cpp = (
            f"BlockMetadata{{{fmt_float(hardness)}, {'true' if conductor == '1' else 'false'}, "
            f'"{push}", {"true" if is_sup else "false"}, {fmt_float(friction)}, {fmt_float(speed)}, '
            f"{fmt_float(res)}, {'true' if liquid == '1' else 'false'}, "
            f"{'true' if force_solid == '1' else 'false'}, "
            f"{'true' if liq_container == '1' else 'false'}, "
            f"{'true' if replaceable == '1' else 'false'}, "
            f"{'true' if ignited == '1' else 'false'}, "
            f"{inst_cpp}, 0x{flag_mask:x}ULL}}"
        )

        parsed_props: list[tuple[str, list[str], str]] = []
        if props_str:
            for p in props_str.split(";"):
                pname, pvals, pdef = p.split("=")
                parsed_props.append((pname, pvals.split("|"), pdef))

        # Sort by property name to match C++ StateDefinition::StateDefinition
        sorted_props = sorted(parsed_props, key=lambda x: x[0])

        if sorted_props:
            prop_decls = []
            for pname, vals, pdef in sorted_props:
                vals_cpp = ", ".join(f'"{v}"' for v in vals)
                prop_decls.append(f'Property{{"{pname}", {{{vals_cpp}}}, "{pdef}"}}')
            reg_out.append(
                f'const Block {ident}{{"{mc_name}", {meta_cpp},\n'
                f'    StateDefinition{{{{{", ".join(prop_decls)}}}}}}};'
            )
        else:
            reg_out.append(f'const Block {ident}{{"{mc_name}", {meta_cpp}}};')

        # Emit state rows in exact C++ StateDefinition variant order
        if sorted_props:
            value_lists = [[(pname, v) for v in vals] for pname, vals, _ in sorted_props]
            for combo in itertools.product(*value_lists):
                kv = tuple(combo)  # already sorted by pname
                rt, bm, cond, col_s, out_s = state_lookup[(mc_name, kv)]
                col_id = intern_shape(col_s)
                out_id = intern_shape(out_s)
                sflags = (rt & 1) | ((bm & 1) << 1) | ((cond & 1) << 2)
                cpp_state_rows.append((col_id, out_id, sflags))
        else:
            rt, bm, cond, col_s, out_s = state_lookup[(mc_name, ())]
            col_id = intern_shape(col_s)
            out_id = intern_shape(out_s)
            sflags = (rt & 1) | ((bm & 1) << 1) | ((cond & 1) << 2)
            cpp_state_rows.append((col_id, out_id, sflags))

    assert len(cpp_state_rows) == 29873

    shapes_out: list[str] = [
        "// Auto-generated by scripts/generate_blocks_data.py. Do not edit manually.",
        "struct ShapeSpan { std::uint16_t offset; std::uint16_t count; };",
        "struct StateBasicInfo { std::uint16_t collisionShape; std::uint16_t outlineShape; std::uint8_t flags; };",
        "inline constexpr ShapeBox kBlockShapeBoxes[] = {",
    ]
    for b in shape_boxes:
        shapes_out.append(f"  {{{b}}},")
    shapes_out.append("};")
    shapes_out.append("inline constexpr ShapeSpan kBlockShapeSpans[] = {")
    for off, cnt in shape_spans:
        shapes_out.append(f"  {{{off}u, {cnt}u}},")
    shapes_out.append("};")
    shapes_out.append("inline constexpr StateBasicInfo kStateBasicInfo[] = {")
    for col_id, out_id, sflags in cpp_state_rows:
        shapes_out.append(f"  {{{col_id}u, {out_id}u, {sflags}u}},")
    shapes_out.append("};")

    DECL_INC.write_text("\n".join(decl_out) + "\n", encoding="utf-8")
    REG_INC.write_text("\n".join(reg_out) + "\n", encoding="utf-8")
    SHAPES_INC.write_text("\n".join(shapes_out) + "\n", encoding="utf-8")
    print(f"Generated {len(basics_lines)} blocks, {len(cpp_state_rows)} states, {len(shape_spans)} unique shapes ({len(shape_boxes)} boxes)")


if __name__ == "__main__":
    main()
