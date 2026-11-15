#include "redstone/terrain_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace redstone;
using namespace redstone::terrain;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
void add(nbt::Tag &compound, std::string key, nbt::Tag value) {
  compound.compound.emplace(key, value);
  compound.compoundOrder.push_back(std::move(key));
}
nbt::Tag number(uint8_t type, std::int64_t value) {
  nbt::Tag tag;
  tag.type = type;
  tag.number = value;
  return tag;
}
nbt::Tag string(std::string value) {
  nbt::Tag tag;
  tag.type = 8;
  tag.text = std::move(value);
  return tag;
}
nbt::Tag list(uint8_t type, std::vector<nbt::Tag> values) {
  nbt::Tag tag;
  tag.type = 9;
  tag.listElementType = type;
  tag.list = std::move(values);
  return tag;
}
std::vector<BlockState> supportedStates() {
  static const auto *known = new std::vector<const Block *>{
      &Blocks::AIR,          &Blocks::GRASS_BLOCK, &Blocks::BEDROCK,
      &Blocks::SLIME_BLOCK,  &Blocks::HONEY_BLOCK, &Blocks::SMOOTH_STONE,
      &Blocks::STONE,        &Blocks::COBBLESTONE, &Blocks::STONE_BRICKS,
      &Blocks::SAND,         &Blocks::REDSTONE_LAMP, &Blocks::OBSIDIAN,
      &Blocks::GLASS,        &Blocks::PISTON,      &Blocks::STICKY_PISTON,
      &Blocks::OBSERVER,     &Blocks::REDSTONE_BLOCK, &Blocks::WATER};
  std::vector<BlockState> result;
  for (const Block *block : *known)
    result.push_back(block->defaultState());
  if (result.size() >= 18)
    return result;
  require(result.size() >= 18, std::string("need enough registered block states, got ") +
                                  std::to_string(result.size()));
  return result;
}

nbt::Tag paletteEntry(const BlockState &state) {
  nbt::Tag result;
  result.type = 10;
  add(result, "Name", string(state.name()));
  const auto properties = state.properties();
  if (!properties.empty()) {
    nbt::Tag values;
    values.type = 10;
    for (const auto &[key, value] : properties)
      add(values, key, string(value));
    add(result, "Properties", std::move(values));
  }
  return result;
}
std::vector<std::int64_t> packedData(const std::vector<std::uint16_t> &cells,
                                     unsigned bits) {
  const std::size_t perLong = 64 / bits;
  std::vector<std::int64_t> result((cells.size() + perLong - 1) / perLong, 0);
  const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    auto &word = result[i / perLong];
    const std::size_t offset = (i % perLong) * bits;
    word = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(word) & ~(mask << offset)) |
        (static_cast<std::uint64_t>(cells[i]) << offset));
  }
  return result;
}
nbt::RootTag chunkTemplate() {
  const auto supported = supportedStates();
  nbt::RootTag root;
  root.name = "Level";
  root.value.type = 10;
  add(root.value, "DataVersion", number(3, 4786));
  add(root.value, "xPos", number(3, -1));
  add(root.value, "zPos", number(3, -1));
  add(root.value, "Status", string("minecraft:full"));
  add(root.value, "Y", number(1, -7));
  add(root.value, "short_width", number(2, 1234));
  add(root.value, "long_width", number(4, 0x123456789LL));

  nbt::Tag section;
  section.type = 10;
  add(section, "Y", number(1, 0));
  nbt::Tag states;
  states.type = 10;
  std::vector<nbt::Tag> palette;
  for (std::size_t i = 0; i < 16; ++i)
    palette.push_back(paletteEntry(supported[i]));
  add(states, "palette", list(10, std::move(palette)));
  std::vector<std::uint16_t> cells(4096, 0);
  cells[4094] = 15;
  nbt::Tag data;
  data.type = 12;
  data.longs = packedData(cells, 4);
  add(states, "data", std::move(data));
  add(section, "block_states", std::move(states));
  nbt::Tag biomes;
  biomes.type = 10;
  add(biomes, "palette_empty_to_preserve", list(8, {}));
  add(section, "biomes", std::move(biomes));
  nbt::Tag blockLight;
  blockLight.type = 7;
  blockLight.bytes.assign(2048, -1);
  add(section, "BlockLight", std::move(blockLight));

  nbt::Tag untouchedSection;
  untouchedSection.type = 10;
  add(untouchedSection, "Y", number(1, 1));
  nbt::Tag untouchedStates;
  untouchedStates.type = 10;
  add(untouchedStates, "palette",
      list(10, {paletteEntry(supported[17])}));
  add(untouchedSection, "block_states", std::move(untouchedStates));
  nbt::Tag untouchedUnknown;
  untouchedUnknown.type = 10;
  add(untouchedUnknown, "empty_unknown_list", list(0, {}));
  add(untouchedSection, "untouched_extension", std::move(untouchedUnknown));
  nbt::Tag untouchedSkyLight;
  untouchedSkyLight.type = 7;
  untouchedSkyLight.bytes.assign(2048, 63);
  untouchedSkyLight.bytes[123] = 17;
  add(untouchedSection, "SkyLight", std::move(untouchedSkyLight));

  nbt::Tag sections = list(10, {section, untouchedSection});
  add(root.value, "sections", std::move(sections));

  nbt::Tag heightmaps;
  heightmaps.type = 10;
  nbt::Tag surface;
  surface.type = 12;
  surface.longs = {7, 8, 9};
  add(heightmaps, "WORLD_SURFACE", std::move(surface));
  add(root.value, "Heightmaps", std::move(heightmaps));
  add(root.value, "isLightOn", number(1, 1));

  nbt::Tag tick;
  tick.type = 10;
  add(tick, "i", string("minecraft:stone"));
  add(tick, "x", number(3, -1));
  add(tick, "y", number(3, 64));
  add(tick, "z", number(3, -1));
  add(tick, "t", number(3, 12));
  add(tick, "p", number(3, 7));
  add(root.value, "block_ticks", list(10, {tick}));
  nbt::Tag fluid = tick;
  fluid.compound["i"].text = "minecraft:water";
  add(root.value, "fluid_ticks", list(10, {fluid}));

  nbt::Tag blockEntity;
  blockEntity.type = 10;
  add(blockEntity, "id", string("minecraft:chest"));
  add(blockEntity, "x", number(3, -1));
  add(root.value, "block_entities", list(10, {blockEntity}));

  nbt::Tag unknown;
  unknown.type = 10;
  add(unknown, "nested_empty", list(2, {}));
  add(root.value, "unknown_compound", std::move(unknown));
  add(root.value, "unknown_empty", list(8, {}));
  add(root.value, "structures", [&] {
    nbt::Tag value;
    value.type = 10;
    add(value, "starts", [] {
      nbt::Tag starts;
      starts.type = 10;
      return starts;
    }());
    return value;
  }());
  return root;
}
void testNbtBoundaryRoundTrip() {
  nbt::RootTag root;
  root.name = "NamedRoot";
  root.value.type = 10;
  add(root.value, "byte", number(1, -3));
  add(root.value, "short", number(2, -1234));
  add(root.value, "int", number(3, -123456));
  add(root.value, "long", number(4, -1234567890123LL));
  add(root.value, "empty_strings", list(8, {}));
  add(root.value, "empty_compounds", list(10, {}));
  add(root.value, "empty_unknown", list(0, {}));
  const auto bytes = nbt::writeRoot(root);
  const auto reread = nbt::parseRoot(bytes);
  require(reread.name == root.name &&
              reread.value.compoundOrder == root.value.compoundOrder,
          "NBT root name and compound order round-trip");
  require(reread.value.get("byte")->type == 1 &&
              reread.value.get("short")->type == 2 &&
              reread.value.get("int")->type == 3 &&
              reread.value.get("long")->type == 4,
          "NBT integer widths round-trip");
  require(nbt::writeRoot(reread) == bytes,
          "covered normal-value NBT document round-trips byte-identically");
  require(reread.value.get("empty_strings")->listElementType == 8 &&
              reread.value.get("empty_strings")->list.empty() &&
              reread.value.get("empty_compounds")->listElementType == 10 &&
              reread.value.get("empty_compounds")->list.empty() &&
              reread.value.get("empty_unknown")->listElementType == 0 &&
              reread.value.get("empty_unknown")->list.empty(),
          "empty typed lists round-trip");

  nbt::RootTag duplicateOrder;
  duplicateOrder.name = "DuplicateOrder";
  duplicateOrder.value.type = 10;
  add(duplicateOrder.value, "value", number(1, 7));
  duplicateOrder.value.compoundOrder.push_back("value");
  const auto duplicateBytes = nbt::writeRoot(duplicateOrder);
  require(nbt::parseRoot(duplicateBytes).value.get("value")->number == 7,
          "duplicate compoundOrder entries are emitted once");

  nbt::RootTag longName;
  longName.name = std::string(65536, 'n');
  longName.value.type = 10;
  bool longNameRejected = false;
  try {
    (void)nbt::writeRoot(longName);
  } catch (const nbt::Error &) {
    longNameRejected = true;
  }
  require(longNameRejected, "NBT strings longer than uint16 are rejected");

  nbt::Tag deep = number(1, 0);
  for (int i = 0; i < 66; ++i) {
    nbt::Tag parent;
    parent.type = 10;
    add(parent, "child", std::move(deep));
    deep = std::move(parent);
  }
  nbt::RootTag deepRoot;
  deepRoot.name = "TooDeep";
  deepRoot.value = std::move(deep);
  bool depthRejected = false;
  try {
    (void)nbt::writeRoot(deepRoot);
  } catch (const nbt::Error &) {
    depthRejected = true;
  }
  require(depthRejected, "writer enforces its depth/complexity limit");
}
void testSavedTickPatch() {
  nbt::RootTag root = chunkTemplate();
  TickListPatch patch;
  patch.blockTicks.emplace();
  patch.blockTicks->push_back(
      {"minecraft:stone", -1, 0, -1, 5, 2});
  patch.blockTicks->push_back(
      {"minecraft:stone", 0, 0, 0, -7, -4});
  patch.fluidTicks.emplace();
  std::string error;
  require(patchChunkTicks(root, patch, error), error);
  const auto bytes = nbt::writeRoot(root);
  DecodedChunk decoded = decodeChunkNbt(bytes, {.retainRawBlockEntities = true});
  require(decoded.status == DecodeStatus::Ok &&
              decoded.blockTicks.size() == 2 &&
              decoded.fluidTicks.empty(),
          "tick patch replaces block and clears fluid lists explicitly");
  require(decoded.blockTicks[0].delay == 5 &&
              decoded.blockTicks[1].delay == -7 &&
              decoded.blockTicks[1].priority == -3,
          "tick patch preserves signed delay and clamps priority");
  const nbt::Tag *blockList = decoded.root.value.get("block_ticks");
  require(blockList && !blockList->list.empty() &&
              !blockList->list[0].get("SubTick") &&
              !blockList->list[0].get("subTick"),
          "chunk persistence emits no invented SubTick field");

  TickListPatch blockOnly;
  blockOnly.blockTicks.emplace();
  require(patchChunkTicks(root, blockOnly, error), error);
  DecodedChunk recleared = decodeChunkNbt(nbt::writeRoot(root));
  require(recleared.blockTicks.empty() && recleared.fluidTicks.empty(),
          "engaged empty list clears while omitted list stays cleared");
}

void testInPlaceBatchEdits() {
  const auto supported = supportedStates();
  const auto bytes = nbt::writeRoot(chunkTemplate());

  DecodedChunk same = decodeChunkNbt(bytes);
  require(same.status == DecodeStatus::Ok, same.detail.c_str());
  const std::int8_t *untouchedSky =
      same.root.value.get("sections")->list[1].get("SkyLight")->bytes.data();
  const nbt::Tag *unknownNode =
      &same.root.value.compound.at("unknown_compound");
  const std::array<BlockEdit, 3> sameEdits{{
      {0, 0, supported[16]},
      {0, 1, supported[17]},
      {0, 4094, supported[16]},
  }};
  PatchResult samePatch = patchBlockStates(same.root, sameEdits);
  require(samePatch.edited && samePatch.error.empty() &&
              samePatch.changedCells == 3 &&
              samePatch.repackedSections == 1,
          "three edits in one section share one final repack");
  require(same.root.value.get("sections")->list[1].get("SkyLight")->bytes.data() ==
              untouchedSky &&
              &same.root.value.compound.at("unknown_compound") == unknownNode,
          "in-place section edit leaves untouched payload and root nodes owned in place");
  DecodedChunk sameChanged = decodeChunkNbt(encodeChunkNbt(same.root));
  require(sameChanged.status == DecodeStatus::Ok &&
              sameChanged.sections[0].blockStates->cell(0) == 16 &&
              sameChanged.sections[0].blockStates->cell(1) == 17 &&
              sameChanged.sections[0].blockStates->cell(4094) == 16 &&
              sameChanged.sections[0].blockStates->bitsPerEntry == 5,
          "one-section batch applies every final cell value");

  DecodedChunk different = decodeChunkNbt(bytes);
  const nbt::Tag *differentUnknown =
      &different.root.value.compound.at("unknown_compound");
  const std::array<BlockEdit, 2> differentEdits{{
      {0, 2, supported[16]},
      {1, 5, supported[6]},
  }};
  PatchResult differentPatch =
      patchBlockStates(different.root, differentEdits);
  require(differentPatch.edited &&
              differentPatch.changedCells == 2 &&
              differentPatch.repackedSections == 2,
          "edits in two sections repack each affected section once");
  require(&different.root.value.compound.at("unknown_compound") ==
              differentUnknown,
          "multi-section in-place edit does not replace the root DOM");
  DecodedChunk differentChanged =
      decodeChunkNbt(encodeChunkNbt(different.root));
  require(differentChanged.status == DecodeStatus::Ok &&
              differentChanged.sections[0].blockStates->cell(2) == 16 &&
              differentChanged.sections[1].blockStates->cell(5) == 1,
          "different-section batch reaches both final packed arrays");

  DecodedChunk noOp = decodeChunkNbt(bytes);
  const std::int8_t *noOpSky =
      noOp.root.value.get("sections")->list[1].get("SkyLight")->bytes.data();
  const std::int64_t *noOpData =
      noOp.root.value.get("sections")->list[0].get("block_states")
          ->get("data")->longs.data();
  const std::array<BlockEdit, 4> noOpEdits{{
      {0, 0, supported[16]},
      {0, 0, supported[0]},
      {0, 4094, supported[15]},
      {1, 7, supported[17]},
  }};
  PatchResult noOpPatch = patchBlockStates(noOp.root, noOpEdits);
  require(!noOpPatch.edited && noOpPatch.changedCells == 0 &&
              noOpPatch.repackedSections == 0,
          "batch no-op reports no changed or repacked sections");
  require(encodeChunkNbt(noOp.root) == bytes &&
              noOp.root.value.get("sections")->list[1].get("SkyLight")->bytes.data() ==
                  noOpSky &&
              noOp.root.value.get("sections")->list[0].get("block_states")
                      ->get("data")->longs.data() == noOpData,
          "batch no-op preserves packed payload addresses and bytes");

  DecodedChunk moved = decodeChunkNbt(bytes);
  EditResult movedEdit =
      setBlockState(std::move(moved), 0, 0, supported[16]);
  require(movedEdit.edited,
          "move-owned compatibility wrapper edits without cloning its root");
}

void testCompactBatchEdits() {
  static_assert(sizeof(CompactBlockEdit) == 8,
                "compact edit transfer must stay at eight bytes");
  const auto supported = supportedStates();
  const auto bytes = nbt::writeRoot(chunkTemplate());

  DecodedChunk compact = decodeChunkNbt(bytes);
  const std::array<CompactBlockEdit, 4> edits{{
      {0, 0, supported[0].id()},
      {0, 1, supported[17].id()},
      {0, 0, supported[15].id()}, // last write for the cell wins
      {1, 7, supported[0].id()},
  }};
  PatchResult patched = patchBlockStatesCompact(compact.root, edits);
  if (!(patched.edited && patched.error.empty() &&
        patched.changedCells == 3 && patched.repackedSections == 2))
    throw std::runtime_error("compact edits patch effective cells: error=" +
                             patched.error + " changed=" +
                             std::to_string(patched.changedCells) +
                             " repacked=" +
                             std::to_string(patched.repackedSections) +
                             " air=" + std::to_string(supported[0].id()) +
                             " glass=" + std::to_string(supported[16].id()) +
                             " stone=" + std::to_string(supported[15].id()));
  DecodedChunk decoded = decodeChunkNbt(encodeChunkNbt(compact.root));
  if (!(decoded.status == DecodeStatus::Ok &&
        decoded.sections[0].blockStates->cell(0) == 15 &&
        decoded.sections[0].blockStates->cell(1) == 16 &&
        decoded.sections[1].blockStates->cell(7) == 1))
    throw std::runtime_error("compact patch resolves stable state ids: " +
                             std::to_string(decoded.sections[0].blockStates->cell(0)) +
                             "," +
                             std::to_string(decoded.sections[0].blockStates->cell(1)) +
                             "," +
                             std::to_string(decoded.sections[1].blockStates->cell(7)));

  DecodedChunk rejected = decodeChunkNbt(bytes);
  const CompactBlockEdit unknown{0, 2, 0xffffffffu};
  PatchResult unknownPatch =
      patchBlockStatesCompact(rejected.root,
                              std::span<const CompactBlockEdit>(&unknown, 1));
  require(!unknownPatch.edited &&
              unknownPatch.error == "unknown block state id",
          "unknown compact state id is rejected instead of decaying to air");
  const CompactBlockEdit missing{2, 0, supported[16].id()};
  PatchResult missingPatch =
      patchBlockStatesCompact(rejected.root,
                              std::span<const CompactBlockEdit>(&missing, 1));
  require(!missingPatch.edited &&
              missingPatch.error == "section Y is missing",
          "compact edit rejects a section absent from the template");
}

void testMovingPistonCodec() {
  nbt::RootTag root = chunkTemplate();
  const BlockState moving =
      Blocks::MOVING_PISTON.defaultState().with("facing", "east");
  const BlockEdit terrainEdit{0, 255, moving};
  require(patchBlockStates(root, {&terrainEdit, 1}).edited,
          "moving piston codec terrain cell patches");
  const MovingPistonEntity piston{
      -1, 0, -1, Blocks::STONE.defaultState(), 5, 0.25f, true, false};
  std::string error;
  require(patchMovingPistonEntities(root, {&piston, 1}, error), error);
  const auto bytes = encodeChunkNbt(root);
  const DecodedChunk decoded = decodeChunkNbt(bytes);
  require(decoded.status == DecodeStatus::Ok &&
              !decoded.hasUnsupportedBlockEntities &&
              decoded.blockEntities.empty(),
          "online decode keeps only compact moving piston data");
  require(decoded.movingPistons.size() == 1 &&
              decoded.movingPistons[0].x == -1 &&
              decoded.movingPistons[0].y == 0 &&
              decoded.movingPistons[0].z == -1 &&
              decoded.movingPistons[0].movedState.name() ==
                  "minecraft:stone" &&
              decoded.movingPistons[0].facing == 5 &&
              decoded.movingPistons[0].progressO == 0.25f &&
              decoded.movingPistons[0].extending &&
              !decoded.movingPistons[0].source,
          "moving piston fields round-trip through progressO");

  nbt::RootTag missingPair = chunkTemplate();
  require(!patchMovingPistonEntities(missingPair, {&piston, 1}, error) &&
              error.find("terrain cells") != std::string::npos,
          "moving piston patch rejects a missing terrain counterpart");
  nbt::RootTag duplicateRoot = root;
  const std::array<MovingPistonEntity, 2> duplicate{piston, piston};
  require(!patchMovingPistonEntities(duplicateRoot, duplicate, error) &&
              error.find("duplicate") != std::string::npos,
          "moving piston patch rejects duplicate positions");
  MovingPistonEntity crossChunk = piston;
  crossChunk.x = 0;
  nbt::RootTag crossRoot = root;
  require(!patchMovingPistonEntities(
              crossRoot, std::span<const MovingPistonEntity>(&crossChunk, 1),
              error) &&
              error.find("outside its chunk") != std::string::npos,
          "moving piston patch rejects cross-chunk records");

  nbt::RootTag unknown = root;
  nbt::Tag &saved = unknown.value.compound["block_entities"].list[0];
  add(saved, "unexpected", number(1, 1));
  const DecodedChunk rejected = decodeChunkNbt(encodeChunkNbt(unknown));
  require(rejected.status == DecodeStatus::Ok &&
              rejected.hasUnsupportedBlockEntities &&
              rejected.blockEntityError.find("unknown field") !=
                  std::string::npos,
          "unknown moving piston fields are rejected explicitly");

  nbt::RootTag packed = root;
  packed.value.compound["block_entities"].list[0].compound["keepPacked"].number = 1;
  const DecodedChunk rejectedPacked = decodeChunkNbt(encodeChunkNbt(packed));
  require(rejectedPacked.hasUnsupportedBlockEntities &&
              rejectedPacked.blockEntityError.find("keepPacked") !=
                  std::string::npos,
          "keepPacked moving pistons are rejected explicitly");

  nbt::RootTag components = root;
  add(components.value.compound["block_entities"].list[0].compound["components"],
      "minecraft:custom", string("value"));
  const DecodedChunk rejectedComponents =
      decodeChunkNbt(encodeChunkNbt(components));
  require(rejectedComponents.hasUnsupportedBlockEntities &&
              rejectedComponents.blockEntityError.find("components") !=
                  std::string::npos,
          "nonempty moving piston components are rejected explicitly");

  nbt::RootTag extreme = chunkTemplate();
  extreme.value.compound["xPos"].number = std::numeric_limits<int>::max();
  const DecodedChunk edgeDecode = decodeChunkNbt(encodeChunkNbt(extreme));
  std::string edgeSaveError;
  require(edgeDecode.status == DecodeStatus::Unsupported &&
              !validateChunkRootForSave(extreme, edgeSaveError) &&
              edgeDecode.detail.find("world bounds") != std::string::npos,
          "extreme chunk coordinates are rejected before coordinate scaling");
  nbt::RootTag extremePatch = chunkTemplate();
  extremePatch.value.compound["zPos"].number =
      std::numeric_limits<int>::min();
  require(!patchMovingPistonEntities(extremePatch, {}, error) &&
              error.find("in-range") != std::string::npos,
          "moving piston patch rejects extreme chunk coordinates");
}

void testChunkCodecAndEdit(const std::filesystem::path &scratch) {
  const nbt::RootTag original = chunkTemplate();
  const auto bytes = nbt::writeRoot(original);
  DecodedChunk decoded = decodeChunkNbt(bytes, {.retainRawBlockEntities = true});
  require(decoded.status == DecodeStatus::Ok, decoded.detail.c_str());
  require(encodeChunkNbt(decoded) == bytes,
          "covered full chunk fixture round-trips byte-identically");
  require(decoded.chunkX == -1 && decoded.chunkZ == -1 &&
              decoded.sections.size() == 2 &&
              decoded.blockTicks.size() == 1 &&
              decoded.fluidTicks.size() == 1 &&
              decoded.blockEntities.size() == 1,
          "FULL chunk core fields decode");
  require(decoded.blockTicks[0].priority == 3 &&
              decoded.fluidTicks[0].id == "minecraft:water",
          "tick codec parses and clamps priority");
  require(decoded.sections[0].blockStates &&
              decoded.sections[0].blockStates->cell(4094) == 15,
          "non-crossing palette words decode");

  const auto supported = supportedStates();
  EditResult noOp = setBlockState(decoded, 0, 0, supported[0]);
  require(!noOp.edited && noOp.error.empty(),
          "writing the current palette state is a semantic no-op");
  require(encodeChunkNbt(noOp.root) == bytes,
          "no-op edit leaves the NBT document byte-identical");
  require(noOp.root.value.get("isLightOn")->number == 1 &&
              noOp.root.value.get("Heightmaps")->compound.size() == 1,
          "no-op edit leaves derived light/heightmap state untouched");

  const BlockState editedState = supported[16];
  EditResult edit = setBlockState(decoded, 0, 0, editedState);
  require(edit.edited, edit.error.c_str());
  const auto editedBytes = encodeChunkNbt(edit.root);
  DecodedChunk changed = decodeChunkNbt(editedBytes, {.retainRawBlockEntities = true});
  require(changed.status == DecodeStatus::Ok &&
              changed.sections[0].blockStates->cell(0) == 16 &&
              changed.sections[0].blockStates->cell(4094) == 15 &&
              changed.sections[0].blockStates->bitsPerEntry == 5,
          "palette growth repacks 4-bit contents into 5-bit storage");
  require(changed.root.value.get("isLightOn")->number == 0 &&
              changed.root.value.get("Heightmaps")->compound.empty(),
          "derived light/heightmap state is invalidated");
  const nbt::Tag &changedSection =
      changed.root.value.compound["sections"].list[0];
  require(changed.root.value.get("unknown_compound") &&
              changed.root.value.get("unknown_empty")->listElementType == 8 &&
              changed.root.value.get("structures") &&
              changedSection.get("biomes") &&
              changedSection.get("biomes")
                      ->get("palette_empty_to_preserve")
                      ->listElementType == 8 &&
              changedSection.get("BlockLight") &&
              changedSection.get("BlockLight")->bytes.size() == 2048,
          "unrecognized and unedited chunk fields remain present");
  require(!changed.blockEntities.empty() &&
              changed.blockEntities[0].get("id")->text == "minecraft:chest",
          "raw block entity tags are retained");
  const nbt::Tag &originalUntouched =
      original.value.get("sections")->list[1];
  const nbt::Tag &changedUntouched =
      changed.root.value.get("sections")->list[1];
  auto encodeSection = [](const nbt::Tag &tag) {
    nbt::RootTag wrapper;
    wrapper.name = "section";
    wrapper.value.type = 10;
    add(wrapper.value, "value", tag);
    return nbt::writeRoot(wrapper);
  };
  require(encodeSection(originalUntouched) ==
              encodeSection(changedUntouched) &&
              changedUntouched.get("Y")->number == 1 &&
              changedUntouched.get("SkyLight")->bytes[123] == 17,
          "unedited section, including unknown fields and raw sky light, "
          "is byte-identical after an edit elsewhere");
  const auto cppEdited = scratch / "cpp-edited";
  (void)region::writeChunkNbt(cppEdited, -1, -1, editedBytes, 600);


  nbt::RootTag bad = original;
  bad.value.compound["xPos"].number = 2;
  const auto badRegion = scratch / "bad-coordinates";
  std::filesystem::create_directories(badRegion);
  (void)region::writeChunkNbt(badRegion, -1, -1,
                              nbt::writeRoot(bad), 500);
  require(decodeChunkAt(badRegion, -1, -1).status == DecodeStatus::Corrupt,
          "xPos mismatch against the requested region slot is rejected");

  nbt::RootTag wrongVersion = original;
  wrongVersion.value.compound["DataVersion"].number = 4787;
  require(decodeChunkNbt(nbt::writeRoot(wrongVersion)).status ==
              DecodeStatus::Unsupported,
          "non-4786 chunk is explicitly unsupported");
  nbt::RootTag nonFull = original;
  nonFull.value.compound["Status"].text = "minecraft:features";
  require(decodeChunkNbt(nbt::writeRoot(nonFull)).status ==
              DecodeStatus::Unsupported,
          "non-FULL chunk is explicitly unsupported");

  nbt::RootTag unsupported = original;
  nbt::Tag &palette = unsupported.value.compound["sections"].list[0]
                          .compound["block_states"]
                          .compound["palette"];
  palette.list[0].compound["Name"].text = "example:unknown_block";
  require(decodeChunkNbt(nbt::writeRoot(unsupported)).status ==
              DecodeStatus::Unsupported,
          "unknown palette block is explicitly unsupported");
  nbt::RootTag unimplemented = original;
  auto &pending = unimplemented.value.compound["sections"].list[0]
                      .compound["block_states"].compound["palette"];
  pending.list[0] = paletteEntry(Blocks::WHITE_BED.defaultState());
  require(decodeChunkNbt(nbt::writeRoot(unimplemented)).status ==
              DecodeStatus::Unsupported,
          "registered state with unsupported behavior is not accepted for writeback");

  nbt::RootTag badProperties = original;
  nbt::Tag &badPalette = badProperties.value.compound["sections"].list[0]
                             .compound["block_states"]
                             .compound["palette"];
  nbt::Tag invalidProperties;
  invalidProperties.type = 10;
  add(invalidProperties, "not_a_stone_property", string("true"));
  badPalette.list[0].compound["Properties"] =
      std::move(invalidProperties);
  require(decodeChunkNbt(nbt::writeRoot(badProperties)).status ==
              DecodeStatus::Unsupported,
          "known block with a non-schema palette state is unsupported");

  nbt::RootTag oversized = original;
  nbt::Tag &oversizedPalette = oversized.value.compound["sections"].list[0]
                                   .compound["block_states"]
                                   .compound["palette"];
  oversizedPalette.list.resize(4097, paletteEntry(supported[0]));
  DecodedChunk oversizedDecoded =
      decodeChunkNbt(nbt::writeRoot(oversized));
  require(oversizedDecoded.status == DecodeStatus::Corrupt &&
              oversizedDecoded.detail.find("exceeds 4096") !=
                  std::string::npos,
          "palette with more than 4096 entries is rejected");

  const BlockState nonSchema("minecraft:stone",
                             {{"not_a_stone_property", "true"}});
  EditResult invalidTarget = setBlockState(decoded, 0, 0, nonSchema);
  require(!invalidTarget.edited && !invalidTarget.error.empty(),
          "edit target must be a registered schema state");

  // Validate patchChunkLight round-trip, padding sections, all-zero omission,
  // and clearing on isLightOn=false.
  nbt::RootTag lightRoot = original;
  ChunkLightPatch patch;
  patch.isLightOn = true;
  SectionLightPatch padBelow;
  padBelow.y = -5;
  padBelow.blockLight.emplace().fill(0);
  padBelow.blockLight->at(17) = 0x5A;
  SectionLightPatch sec0;
  sec0.y = 0;
  sec0.blockLight.emplace().fill(0); // All zero: must be omitted from NBT
  sec0.skyLight.emplace().fill(0xFF);
  SectionLightPatch padAbove;
  padAbove.y = 20;
  padAbove.skyLight.emplace().fill(0xEE);
  patch.sections = {padBelow, sec0, padAbove};

  std::string lightError;
  require(patchChunkLight(lightRoot, patch, lightError),
          "patchChunkLight succeeds: " + lightError);
  DecodedChunk lightDecoded = decodeChunkNbt(encodeChunkNbt(lightRoot));
  require(lightDecoded.status == DecodeStatus::Ok && lightDecoded.isLightOn,
          "decoded chunk reports isLightOn=true");
  auto findSection = [&](std::int8_t y) -> const Section * {
    for (const Section &s : lightDecoded.sections) {
      if (s.y == y)
        return &s;
    }
    return nullptr;
  };
  const Section *decBelow = findSection(-5);
  const Section *dec0 = findSection(0);
  const Section *decAbove = findSection(20);
  require(decBelow && decBelow->blockLight &&
              decBelow->blockLight->at(17) == 0x5A && !decBelow->skyLight,
          "padding light section Y=-5 preserves non-zero BlockLight");
  require(dec0 && !dec0->blockLight && dec0->skyLight &&
              dec0->skyLight->at(2047) == 0xFF,
          "section 0 omits all-zero BlockLight and preserves SkyLight");
  require(decAbove && !decAbove->blockLight && decAbove->skyLight &&
              decAbove->skyLight->at(0) == 0xEE,
          "padding light section Y=20 preserves SkyLight");

  // Duplicate Y must fail without mutating.
  ChunkLightPatch dupPatch = patch;
  dupPatch.sections.push_back(padBelow);
  require(!patchChunkLight(lightRoot, dupPatch, lightError),
          "duplicate light section Y is rejected");

  // isLightOn=false clears stale light layers and prunes light-only sections.
  ChunkLightPatch offPatch;
  offPatch.isLightOn = false;
  require(patchChunkLight(lightRoot, offPatch, lightError),
          "patchChunkLight(isLightOn=false) succeeds: " + lightError);
  DecodedChunk offDecoded = decodeChunkNbt(encodeChunkNbt(lightRoot));
  require(offDecoded.status == DecodeStatus::Ok && !offDecoded.isLightOn,
          "decoded chunk reports isLightOn=false");
  for (const Section &s : offDecoded.sections) {
    require(!s.blockLight && !s.skyLight,
            "isLightOn=false clears stale BlockLight and SkyLight");
    require(s.y != -5 && s.y != 20,
            "isLightOn=false prunes empty light-only padding sections");
  }

  // Bad nibble array length must be rejected as Corrupt.
  nbt::RootTag badLight = original;
  badLight.value.compound["sections"]
      .list[0]
      .compound["BlockLight"]
      .bytes.resize(2047);
  require(decodeChunkNbt(nbt::writeRoot(badLight)).status ==
              DecodeStatus::Corrupt,
          "BlockLight length != 2048 is rejected as Corrupt");
}
DecodedChunk testRealFixture(const std::filesystem::path &regionDirectory) {
  std::size_t missing = 0, nonFull = 0, unsupported = 0, corrupt = 0;
  std::string lastDetail;
  for (int chunkX = -32; chunkX < 32; ++chunkX) {
    for (int chunkZ = -32; chunkZ < 32; ++chunkZ) {
      const region::ReadResult raw =
          region::readChunkNbt(regionDirectory, chunkX, chunkZ);
      if (!raw) {
        if (raw.status == region::ReadStatus::Missing)
          ++missing;
        else
          ++corrupt;
        continue;
      }
      DecodedChunk candidate = decodeChunkNbt(raw.nbt);
      if (candidate.status == DecodeStatus::Ok) {
        if (candidate.chunkX != chunkX || candidate.chunkZ != chunkZ)
          throw std::runtime_error("real chunk coordinates disagree with region slot");
        require(encodeChunkNbt(candidate) == raw.nbt,
                "real 26.1 FULL raw NBT round-trips byte-identically");
        std::cout << "real_26_1_chunk=" << chunkX << "," << chunkZ
                  << " sections=" << candidate.sections.size()
                  << " block_ticks=" << candidate.blockTicks.size()
                  << " fluid_ticks=" << candidate.fluidTicks.size() << "\n";
        return candidate;
      }
      if (candidate.status == DecodeStatus::Unsupported) {
        if (candidate.detail.find("not full") != std::string::npos)
          ++nonFull;
        else
          ++unsupported;
      } else {
        ++corrupt;
      }
      lastDetail = candidate.detail;
    }
  }
  throw std::runtime_error(
      "official 26.1 fixture has no supported FULL chunk; missing=" +
      std::to_string(missing) + " nonfull=" + std::to_string(nonFull) +
      " unsupported=" + std::to_string(unsupported) +
      " corrupt=" + std::to_string(corrupt) + " last=" + lastDetail);
}

} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2 || argc == 3,
            "terrain_codec_tests needs scratch [official region dir]");
    const std::filesystem::path scratch(argv[1]);
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    testNbtBoundaryRoundTrip();
    testInPlaceBatchEdits();
    testCompactBatchEdits();
    testSavedTickPatch();
    testMovingPistonCodec();
    testChunkCodecAndEdit(scratch);
    if (argc == 3)
      (void)testRealFixture(argv[2]);
    std::cout << "terrain codec tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "terrain codec test failure: " << error.what() << '\n';
    return 1;
  }
}
