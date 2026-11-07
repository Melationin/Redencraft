#include "redstone/litematic.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>

namespace redstone {
namespace {
int chunkCoord(int coordinate) {
  int result = coordinate / 16;
  return coordinate % 16 < 0 ? result - 1 : result;
}
const nbt::Tag &required(const nbt::Tag &tag, const char *key, int type) {
  const auto *value = tag.get(key);
  if (!value || value->type != type)
    throw nbt::Error(std::string("invalid litematic tag: ") + key);
  return *value;
}
std::int64_t numeric(const nbt::Tag &tag, const char *key) {
  const auto *value = tag.get(key);
  if (!value || value->type < 1 || value->type > 4)
    throw nbt::Error(std::string("invalid litematic integer: ") + key);
  return value->number;
}
int bounded(std::int64_t value, int lo, int hi) {
  if (value < lo || value > hi)
    throw nbt::Error("litematic coordinate or size outside supported bounds");
  return static_cast<int>(value);
}
Pos vector(const nbt::Tag &tag) {
  return {bounded(numeric(tag, "x"), -30000000, 30000000),
          bounded(numeric(tag, "y"), -30000000, 30000000),
          bounded(numeric(tag, "z"), -30000000, 30000000)};
}
Pos add(Pos a, Pos b, const LitematicOptions &options) {
  return {bounded(std::int64_t(a.x) + b.x, -29999984, 29999983),
          bounded(std::int64_t(a.y) + b.y, options.minY, options.maxY - 1),
          bounded(std::int64_t(a.z) + b.z, -29999984, 29999983)};
}
nbt::Tag integerTag(int value) {
  nbt::Tag tag;
  tag.type = 3;
  tag.number = value;
  return tag;
}
bool sign(const std::string &id) {
  return id.ends_with("_sign") || id.ends_with("_hanging_sign");
}
bool anvil(const std::string &id) {
  return id == "minecraft:anvil" || id == "minecraft:chipped_anvil" ||
         id == "minecraft:damaged_anvil";
}
const std::vector<nbt::Tag> &list(const nbt::Tag &tag, const char *key) {
  static const std::vector<nbt::Tag> empty;
  const auto *value = tag.get(key);
  if (!value)
    return empty;
  if (value->type != 9)
    throw nbt::Error("invalid litematic list");
  for (const auto &entry : value->list)
    if (entry.type != 10)
      throw nbt::Error("litematic list needs compounds");
  return value->list;
}
BlockState state(const nbt::Tag &tag) {
  const auto &id = required(tag, "Name", 8).text;
  std::map<std::string, std::string> properties;
  if (const auto *props = tag.get("Properties")) {
    if (props->type != 10)
      throw nbt::Error("invalid state Properties");
    for (const auto &[key, value] : props->compound) {
      if (value.type != 8)
        throw nbt::Error("invalid state property value");
      properties[key] = value.text;
    }
  }
  return properties.empty() ? defaultState(id) : BlockState(id, std::move(properties));
}
} // namespace

LitematicImport loadLitematic(const nbt::Tag &root,
                              const LitematicOptions &options) {
  if (root.type != 10)
    throw nbt::Error("litematic root must be a compound");
  const auto version = numeric(root, "Version");
  // LitematicaSchematic.readSubRegionsFromNBT uses the same region coordinates,
  // palette and compact cross-long storage for v6/v7. Entity/block-entity data
  // still follows the explicit supported-field checks below, without DataFixer.
  if ((version != 6 && version != 7) || numeric(root, "SubVersion") != 1)
    throw nbt::Error("only litematic version 6/7, subversion 1 is supported");
  if (options.minY >= options.maxY)
    throw nbt::Error("invalid litematic world height");
  LitematicImport result;
  result.world.minY = options.minY;
  result.world.maxY = options.maxY;
  result.sourceDataVersion = bounded(numeric(root, "MinecraftDataVersion"), 0,
                                     std::numeric_limits<int>::max());
  // Do not interpret an arbitrary generator DataVersion as 26.1 save data.
  // This loader handles the explicit schematic schema and validates used
  // states;
  // TODO: general vanilla DataFixer conversion for older schematic/entity data.
  const auto &regions = required(root, "Regions", 10).compound;
  if (regions.empty() || regions.size() > 8192)
    throw nbt::Error("invalid litematic region count");
  struct Box {
    Pos lo, hi;
  };
  std::vector<Box> boxes;
  boxes.reserve(regions.size());
  std::uint64_t totalVolume = 0;
  for (const auto &[name, region] : regions) {
    (void)name;
    if (region.type != 10)
      throw nbt::Error("invalid litematic region");
    const auto origin = vector(required(region, "Position", 10));
    auto signedSize = vector(required(region, "Size", 10));
    Pos size{std::abs(signedSize.x), std::abs(signedSize.y),
             std::abs(signedSize.z)};
    if (!size.x || !size.y || !size.z)
      throw nbt::Error("empty litematic region");
    if (size.x > 65536 || size.y > 65536 || size.z > 65536)
      throw nbt::Error("litematic region axis limit exceeded");
    const std::uint64_t volume = std::uint64_t(size.x) * size.y * size.z;
    totalVolume += volume;
    if (volume > 64000000 || totalVolume > 64000000)
      throw nbt::Error("litematic decoded volume limit exceeded");
    Pos relativeMin{origin.x + std::min(0, signedSize.x + 1),
                    origin.y + std::min(0, signedSize.y + 1),
                    origin.z + std::min(0, signedSize.z + 1)};
    const Pos lo = add(relativeMin, options.offset, options);
    const Pos last = add(lo, {size.x - 1, size.y - 1, size.z - 1}, options);
    const Pos hi{last.x + 1, last.y + 1, last.z + 1};
    for (const auto &box : boxes)
      if (lo.x < box.hi.x && hi.x > box.lo.x && lo.y < box.hi.y &&
          hi.y > box.lo.y && lo.z < box.hi.z && hi.z > box.lo.z)
        throw nbt::Error(
            "overlapping litematic regions need explicit paste order");
    boxes.push_back({lo, hi});
    if (result.regionCount++ == 0) {
      result.min = lo;
      result.maxExclusive = hi;
    } else {
      result.min = {std::min(result.min.x, lo.x), std::min(result.min.y, lo.y),
                    std::min(result.min.z, lo.z)};
      result.maxExclusive = {std::max(result.maxExclusive.x, hi.x),
                             std::max(result.maxExclusive.y, hi.y),
                             std::max(result.maxExclusive.z, hi.z)};
    }
    const auto &paletteTag = required(region, "BlockStatePalette", 9).list;
    if (paletteTag.empty() || paletteTag.size() > 65536)
      throw nbt::Error("invalid litematic palette");
    std::vector<BlockState> palette;
    for (const auto &entry : paletteTag)
      palette.push_back(state(entry));
    const auto bits =
        std::max(2, int(std::bit_width(unsigned(palette.size() - 1))));
    const auto &words = required(region, "BlockStates", 12).longs;
    if (words.size() != (volume * bits + 63) / 64)
      throw nbt::Error("invalid litematic packed array length");
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    std::vector<bool> checked(palette.size(), false);
    for (std::uint64_t index = 0; index < volume; ++index) {
      const auto bit = index * bits, word = bit / 64, shift = bit % 64;
      auto id = std::uint64_t(words[word]) >> shift;
      if (shift + bits > 64)
        id |= std::uint64_t(words[word + 1]) << (64 - shift);
      id &= mask;
      if (id >= palette.size())
        throw nbt::Error("invalid litematic palette index");
      auto block = palette[id];
      if (sign(block.name())) {
        ++result.skippedSigns;
        continue;
      }
      if (anvil(block.name())) {
        ++result.replacedAnvils;
        block = defaultState("obsidian");
      }
      if (!checked[id]) {
        if (!isSupportedBlock(block.name()) || isLiquidBlock(block))
          throw nbt::Error("unsupported schematic block: " + block.name());
        if (options.validateState)
          options.validateState(block);
        checked[id] = true;
      }
      if (block.isAir())
        continue;
      const Pos pos{lo.x + int(index % size.x),
                    lo.y + int(index / (std::uint64_t(size.x) * size.z)),
                    lo.z + int((index / size.x) % size.z)};
      result.world.blocks.emplace(pos, std::move(block));
    }
    for (auto entity : list(region, "Entities")) {
      const auto &id = required(entity, "id", 8).text;
      if (!entityType(id))
        throw nbt::Error("unsupported schematic entity: " + id);
      if (entity.get("Passengers"))
        throw nbt::Error(
            "schematic passengers are not implemented"); // TODO: passengers.
      auto &position = entity.compound.at("Pos");
      if (position.type != 9 || position.list.size() != 3)
        throw nbt::Error("invalid schematic entity Pos");
      const double offsets[]{double(origin.x) + options.offset.x,
                             double(origin.y) + options.offset.y,
                             double(origin.z) + options.offset.z};
      for (int axis = 0; axis < 3; ++axis) {
        auto &value = position.list[axis];
        if (value.type != 6 || !std::isfinite(value.real))
          throw nbt::Error("invalid schematic entity coordinate");
        value.real += offsets[axis];
      }
      const auto &v = position.list;
      if (std::abs(v[0].real) > 29999984 || std::abs(v[2].real) > 29999984 ||
          v[1].real < options.minY || v[1].real >= options.maxY)
        throw nbt::Error("schematic entity outside world bounds");
      // Structure instances must not inherit repeated template UUIDs. Identity
      // is unique within this fresh world; simple entities do not expose UUIDs.
      const auto serial = ++result.regeneratedEntityUuids;
      nbt::Tag uuid;
      uuid.type = 11;
      uuid.ints = {0x4c495445, 0x4d415449, 0x43505000,
                   static_cast<int>(serial)};
      entity.compound["UUID"] = std::move(uuid);
      result.world.entities.push_back(std::move(entity));
    }
    for (auto entity : list(region, "TileEntities")) {
      const auto &id = required(entity, "id", 8).text;
      if (id == "minecraft:sign" || id == "minecraft:hanging_sign") {
        ++result.skippedSignBlockEntities;
        continue;
      }
      if (id != "minecraft:piston")
        throw nbt::Error("unsupported schematic block entity: " + id);
      const Pos pos = add(lo, vector(entity), options);
      entity.compound["x"] = integerTag(pos.x);
      entity.compound["y"] = integerTag(pos.y);
      entity.compound["z"] = integerTag(pos.z);
      const auto moved = state(required(entity, "blockState", 10));
      if (!isSupportedBlock(moved.name()))
        throw nbt::Error("unsupported moving schematic block");
      if (options.validateState)
        options.validateState(moved);
      result.world.blockEntities.push_back(std::move(entity));
    }
    for (const auto &tick : list(region, "PendingBlockTicks")) {
      const auto pos = add(lo, vector(tick), options);
      const auto &type = required(tick, "Block", 8).text;
      if (!isSupportedBlock(type))
        throw nbt::Error("unsupported schematic tick");
      result.world.ticks.push_back(
          {pos, type,
           bounded(numeric(tick, "Time"), std::numeric_limits<int>::min(),
                   std::numeric_limits<int>::max()),
           bounded(numeric(tick, "Priority"), -3, 3), false});
    }
    if (!list(region, "PendingFluidTicks").empty())
      throw nbt::Error(
          "schematic fluid ticks are excluded"); // TODO: fluid ticks.
  }
  const auto minX = chunkCoord(result.min.x),
             maxX = chunkCoord(result.maxExclusive.x - 1);
  const auto minZ = chunkCoord(result.min.z),
             maxZ = chunkCoord(result.maxExclusive.z - 1);
  if (std::int64_t(maxX - minX + 1) * (maxZ - minZ + 1) > 16384)
    throw nbt::Error("schematic chunk bounding box limit exceeded");
  for (int x = minX; x <= maxX; ++x)
    for (int z = minZ; z <= maxZ; ++z)
      result.world.chunks.emplace_back(x, z);
  return result;
}

LitematicImport loadLitematic(const std::filesystem::path &path,
                              const LitematicOptions &options) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || file.tellg() > 128 * 1024 * 1024)
    throw nbt::Error("cannot read schematic or compressed file too large");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
    throw nbt::Error("failed reading schematic");
  return loadLitematic(nbt::parse(nbt::decompress(bytes, 31)), options);
}
} // namespace redstone
