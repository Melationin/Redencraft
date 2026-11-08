#include "redstone/engine.hpp"

// net/minecraft/world/level/block/Blocks: one static singleton per block type.
// Declaration order fixes block registration order, so minecraft:air is registry
// state id 0 exactly like Block.BLOCK_STATE_REGISTRY.
namespace redstone {
namespace {
BlockMetadata meta(float hardness, bool conductor, std::string pushReaction,
                   bool supported = true) {
  return BlockMetadata{hardness, conductor, std::move(pushReaction), supported};
}
std::vector<std::string> values(std::initializer_list<const char *> list) {
  std::vector<std::string> out;
  out.reserve(list.size());
  for (const char *value : list)
    out.emplace_back(value);
  return out;
}
std::vector<std::string> numbered(int count) {
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i)
    out.emplace_back(std::to_string(i));
  return out;
}
Property flag(const char *name, const char *defaultValue = "false") {
  return Property{name, values({"false", "true"}), defaultValue};
}
Property facing(const char *defaultValue) {
  return Property{"facing",
                  values({"down", "up", "north", "south", "west", "east"}),
                  defaultValue};
}
Property railShape() {
  return Property{"shape",
                  values({"north_south", "east_west", "ascending_east",
                          "ascending_west", "ascending_north",
                          "ascending_south", "south_east", "south_west",
                          "north_west", "north_east"}),
                  "north_south"};
}
Property wallSide(const char *name) {
  return Property{name, values({"none", "low", "tall"}), "none"};
}
} // namespace

namespace Blocks {
const Block AIR{"minecraft:air", meta(0, false, "destroy")};
const Block GRASS_BLOCK{"minecraft:grass_block", meta(0.6f, true, "normal"),
                        {flag("snowy")}};
const Block BEDROCK{"minecraft:bedrock", meta(-1, true, "block")};
const Block SLIME_BLOCK{"minecraft:slime_block", meta(0, true, "normal")};
const Block HONEY_BLOCK{"minecraft:honey_block", meta(0, false, "normal")};
const Block SMOOTH_STONE{"minecraft:smooth_stone", meta(2, true, "normal")};
const Block WHITE_TERRACOTTA{"minecraft:white_terracotta",
                             meta(1.25f, true, "normal")};
const Block DEAD_TUBE_CORAL_WALL_FAN{
    "minecraft:dead_tube_coral_wall_fan", meta(0, false, "normal"),
    {Property{"facing", values({"north", "east", "south", "west"}), "north"},
     flag("waterlogged", "true")}};
const Block DEAD_BRAIN_CORAL_WALL_FAN{
    "minecraft:dead_brain_coral_wall_fan", meta(0, false, "normal"),
    {Property{"facing", values({"north", "east", "south", "west"}), "north"},
     flag("waterlogged", "true")}};
const Block DEAD_BUBBLE_CORAL_WALL_FAN{
    "minecraft:dead_bubble_coral_wall_fan", meta(0, false, "normal"),
    {Property{"facing", values({"north", "east", "south", "west"}), "north"},
     flag("waterlogged", "true")}};
const Block DEAD_FIRE_CORAL_WALL_FAN{
    "minecraft:dead_fire_coral_wall_fan", meta(0, false, "normal"),
    {Property{"facing", values({"north", "east", "south", "west"}), "north"},
     flag("waterlogged", "true")}};
const Block DEAD_HORN_CORAL_WALL_FAN{
    "minecraft:dead_horn_coral_wall_fan", meta(0, false, "normal"),
    {Property{"facing", values({"north", "east", "south", "west"}), "north"},
     flag("waterlogged", "true")}};
const Block DEAD_TUBE_CORAL_FAN{"minecraft:dead_tube_coral_fan",
                                meta(0, false, "normal"),
                                {flag("waterlogged", "true")}};
const Block DEAD_BRAIN_CORAL_FAN{"minecraft:dead_brain_coral_fan",
                                 meta(0, false, "normal"),
                                 {flag("waterlogged", "true")}};
const Block DEAD_BUBBLE_CORAL_FAN{"minecraft:dead_bubble_coral_fan",
                                  meta(0, false, "normal"),
                                  {flag("waterlogged", "true")}};
const Block DEAD_FIRE_CORAL_FAN{"minecraft:dead_fire_coral_fan",
                                meta(0, false, "normal"),
                                {flag("waterlogged", "true")}};
const Block DEAD_HORN_CORAL_FAN{"minecraft:dead_horn_coral_fan",
                                meta(0, false, "normal"),
                                {flag("waterlogged", "true")}};
const Block COBBLESTONE_WALL{
    "minecraft:cobblestone_wall", meta(2, false, "normal"),
    {wallSide("east"), wallSide("north"), wallSide("south"),
     flag("up", "true"), flag("waterlogged"), wallSide("west")}};
const Block STONE_BRICK_WALL{
    "minecraft:stone_brick_wall", meta(1.5f, false, "normal"),
    {wallSide("east"), wallSide("north"), wallSide("south"),
     flag("up", "true"), flag("waterlogged"), wallSide("west")}};
const Block SAND{"minecraft:sand", meta(0.5f, true, "normal")};
const Block REDSTONE_LAMP{"minecraft:redstone_lamp", meta(0.3f, true, "normal"),
                          {flag("lit")}};
const Block OBSIDIAN{"minecraft:obsidian", meta(50, true, "block")};
const Block GLASS{"minecraft:glass", meta(0.3f, false, "normal")};
const Block PISTON{"minecraft:piston", meta(1.5f, true, "block"),
                   {facing("north"), flag("extended")}};
const Block STICKY_PISTON{"minecraft:sticky_piston", meta(1.5f, true, "block"),
                          {facing("north"), flag("extended")}};
const Block PISTON_HEAD{"minecraft:piston_head", meta(1.5f, false, "block"),
                        {facing("north"), flag("short"),
                         Property{"type", values({"normal", "sticky"}),
                                  "normal"}}};
const Block MOVING_PISTON{"minecraft:moving_piston", meta(-1, false, "block"),
                          {facing("north"),
                           Property{"type", values({"normal", "sticky"}),
                                    "normal"}}};
const Block OBSERVER{"minecraft:observer", meta(3, false, "normal"),
                     {facing("south"), flag("powered")}};
const Block NOTE_BLOCK{
    "minecraft:note_block", meta(0.8f, true, "normal"),
    {Property{"instrument",
              values({"harp", "basedrum", "snare", "hat", "bass", "flute",
                      "bell", "guitar", "chime", "xylophone", "iron_xylophone",
                      "cow_bell", "didgeridoo", "bit", "banjo", "pling"}),
              "harp"},
     Property{"note", numbered(25), "0"}, flag("powered")}};
const Block REDSTONE_BLOCK{"minecraft:redstone_block",
                           meta(5, false, "normal")};
const Block WHITE_CARPET{"minecraft:white_carpet",
                         meta(0.1f, false, "normal")};
const Block ORANGE_CARPET{"minecraft:orange_carpet",
                          meta(0.1f, false, "normal")};
const Block MAGENTA_CARPET{"minecraft:magenta_carpet",
                           meta(0.1f, false, "normal")};
const Block LIGHT_BLUE_CARPET{"minecraft:light_blue_carpet",
                              meta(0.1f, false, "normal")};
const Block YELLOW_CARPET{"minecraft:yellow_carpet",
                          meta(0.1f, false, "normal")};
const Block LIME_CARPET{"minecraft:lime_carpet", meta(0.1f, false, "normal")};
const Block PINK_CARPET{"minecraft:pink_carpet", meta(0.1f, false, "normal")};
const Block GRAY_CARPET{"minecraft:gray_carpet", meta(0.1f, false, "normal")};
const Block LIGHT_GRAY_CARPET{"minecraft:light_gray_carpet",
                              meta(0.1f, false, "normal")};
const Block CYAN_CARPET{"minecraft:cyan_carpet", meta(0.1f, false, "normal")};
const Block PURPLE_CARPET{"minecraft:purple_carpet",
                          meta(0.1f, false, "normal")};
const Block BLUE_CARPET{"minecraft:blue_carpet", meta(0.1f, false, "normal")};
const Block BROWN_CARPET{"minecraft:brown_carpet", meta(0.1f, false, "normal")};
const Block GREEN_CARPET{"minecraft:green_carpet", meta(0.1f, false, "normal")};
const Block RED_CARPET{"minecraft:red_carpet", meta(0.1f, false, "normal")};
const Block BLACK_CARPET{"minecraft:black_carpet", meta(0.1f, false, "normal")};
const Block TNT{"minecraft:tnt", meta(0, false, "normal"), {flag("unstable")}};
const Block RAIL{"minecraft:rail", meta(0.7f, false, "normal"),
                 {railShape(), flag("waterlogged")}};
const Block POWERED_RAIL{"minecraft:powered_rail", meta(0.7f, false, "normal"),
                         {railShape(), flag("powered"), flag("waterlogged")}};
const Block DETECTOR_RAIL{"minecraft:detector_rail", meta(0.7f, false, "normal"),
                          {railShape(), flag("powered"), flag("waterlogged")}};
const Block ACTIVATOR_RAIL{"minecraft:activator_rail",
                           meta(0.7f, false, "normal"),
                           {railShape(), flag("powered"), flag("waterlogged")}};
const Block WATER{"minecraft:water", meta(100, false, "destroy"),
                  {Property{"level", numbered(16), "0"}}};
const Block LAVA{"minecraft:lava", meta(100, false, "destroy"),
                 {Property{"level", numbered(16), "0"}}};
const Block OAK_FENCE_GATE{
    "minecraft:oak_fence_gate", BlockMetadata{2.0f, false, "normal", true, 0.6f, 1.0f, 3.0f},
    {Property{"facing", values({"north", "east", "south", "west"}), "north"},
     flag("open"), flag("powered"), flag("in_wall")}};

// Vanilla blocks outside the simulated/editable registry subset. They exist so
// that state.is(Blocks.SNOW) stays a reference comparison for imported worlds.
const Block SNOW{"minecraft:snow", meta(0, false, "unsupported", false)};
const Block SNOW_BLOCK{"minecraft:snow_block", meta(0, false, "unsupported", false)};
const Block POWDER_SNOW{"minecraft:powder_snow",
                        meta(0, false, "unsupported", false)};
const Block CRYING_OBSIDIAN{"minecraft:crying_obsidian",
                            meta(0, false, "unsupported", false)};
const Block RESPAWN_ANCHOR{"minecraft:respawn_anchor",
                           meta(0, false, "unsupported", false)};
const Block REINFORCED_DEEPSLATE{"minecraft:reinforced_deepslate",
                                 meta(0, false, "unsupported", false)};

const Block BIRCH_FENCE_GATE{
    "minecraft:birch_fence_gate", OAK_FENCE_GATE.metadata(),
    {Property{"facing", values({"north", "east", "south", "west"}), "north"},
     flag("open"), flag("powered"), flag("in_wall")}};
// TODO: bonemeal-driven moss patches require the excluded world features.
const Block MOSS_BLOCK{"minecraft:moss_block",
                       BlockMetadata{0.1f, true, "destroy", true, 0.6f, 1.0f, 0.1f}};
const Block STONE_BRICKS{"minecraft:stone_bricks",
                         BlockMetadata{1.5f, true, "normal", true, 0.6f, 1.0f, 6.0f}};
// F-4: blocks the water/lava reaction rules produce or depend on.
const Block STONE{"minecraft:stone",
                  BlockMetadata{1.5f, true, "normal", true, 0.6f, 1.0f, 6.0f}};
const Block COBBLESTONE{
    "minecraft:cobblestone",
    BlockMetadata{2.0f, true, "normal", true, 0.6f, 1.0f, 6.0f}};
// RotatedPillarBlock: declares AXIS and defaults to y (Blocks.java:2009-2018).
const Block BASALT{
    "minecraft:basalt",
    BlockMetadata{1.25f, true, "normal", true, 0.6f, 1.0f, 4.2f},
    {Property{"axis", values({"x", "y", "z"}), "y"}}};
const Block SOUL_SOIL{"minecraft:soul_soil",
                      BlockMetadata{0.5f, true, "normal", true, 0.6f, 1.0f, 0.5f}};
const Block BLUE_ICE{"minecraft:blue_ice",
                     BlockMetadata{2.8f, true, "normal", true, 0.6f, 1.0f, 2.8f}};
} // namespace Blocks
} // namespace redstone
