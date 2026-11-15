#include "redstone/poi.hpp"

#include "redstone/engine.hpp"
#include "redstone/nbt.hpp"
#include "redstone/region.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace redstone {
namespace detail {
const std::uint8_t *gPoiStateData = nullptr;
std::uint32_t gPoiStateCount = 0;
} // namespace detail

namespace {

// Exact vanilla order from net.minecraft.world.entity.ai.village.poi.PoiTypes.
constexpr std::array<PoiTypeSpec, kPoiTypeCount + 1> kPoiTypeSpecs{{
    {PoiTypeId::None, "", 0, 0},
    {PoiTypeId::Armorer, "minecraft:armorer", 1, 1},
    {PoiTypeId::Butcher, "minecraft:butcher", 1, 1},
    {PoiTypeId::Cartographer, "minecraft:cartographer", 1, 1},
    {PoiTypeId::Cleric, "minecraft:cleric", 1, 1},
    {PoiTypeId::Farmer, "minecraft:farmer", 1, 1},
    {PoiTypeId::Fisherman, "minecraft:fisherman", 1, 1},
    {PoiTypeId::Fletcher, "minecraft:fletcher", 1, 1},
    {PoiTypeId::Leatherworker, "minecraft:leatherworker", 1, 1},
    {PoiTypeId::Librarian, "minecraft:librarian", 1, 1},
    {PoiTypeId::Mason, "minecraft:mason", 1, 1},
    {PoiTypeId::Shepherd, "minecraft:shepherd", 1, 1},
    {PoiTypeId::Toolsmith, "minecraft:toolsmith", 1, 1},
    {PoiTypeId::Weaponsmith, "minecraft:weaponsmith", 1, 1},
    {PoiTypeId::Home, "minecraft:home", 1, 1},
    {PoiTypeId::Meeting, "minecraft:meeting", 32, 6},
    {PoiTypeId::Beehive, "minecraft:beehive", 0, 1},
    {PoiTypeId::BeeNest, "minecraft:bee_nest", 0, 1},
    {PoiTypeId::NetherPortal, "minecraft:nether_portal", 0, 1},
    {PoiTypeId::Lodestone, "minecraft:lodestone", 0, 1},
    {PoiTypeId::LightningRod, "minecraft:lightning_rod", 0, 1},
    {PoiTypeId::TestInstance, "minecraft:test_instance", 0, 1},
}};

std::vector<std::uint8_t> gOwnedPoiStateTable;
std::once_flag gPoiStateInitOnce;

void registerAllStatesOfBlock(std::vector<std::uint8_t> &table,
                              const Block &block, PoiTypeId type) {
  const BlockState::Id first = block.firstState();
  const BlockState::Id end = first + block.stateCount();
  if (end > table.size())
    table.resize(end, 0);
  const auto val = static_cast<std::uint8_t>(type);
  for (BlockState::Id id = first; id < end; ++id)
    table[id] = val;
}

void registerBedHeadStates(std::vector<std::uint8_t> &table,
                           const Block &block) {
  const BlockState::Id first = block.firstState();
  const BlockState::Id end = first + block.stateCount();
  if (end > table.size())
    table.resize(end, 0);
  const auto val = static_cast<std::uint8_t>(PoiTypeId::Home);
  for (BlockState::Id id = first; id < end; ++id) {
    if (BlockState::fromId(id).getView("part") == "head")
      table[id] = val;
  }
}

void buildPoiStateTableImpl() {
  std::vector<std::uint8_t> table;
  table.reserve(30000);

  registerAllStatesOfBlock(table, Blocks::BLAST_FURNACE, PoiTypeId::Armorer);
  registerAllStatesOfBlock(table, Blocks::SMOKER, PoiTypeId::Butcher);
  registerAllStatesOfBlock(table, Blocks::CARTOGRAPHY_TABLE,
                           PoiTypeId::Cartographer);
  registerAllStatesOfBlock(table, Blocks::BREWING_STAND, PoiTypeId::Cleric);
  registerAllStatesOfBlock(table, Blocks::COMPOSTER, PoiTypeId::Farmer);
  registerAllStatesOfBlock(table, Blocks::BARREL, PoiTypeId::Fisherman);
  registerAllStatesOfBlock(table, Blocks::FLETCHING_TABLE, PoiTypeId::Fletcher);
  registerAllStatesOfBlock(table, Blocks::CAULDRON, PoiTypeId::Leatherworker);
  registerAllStatesOfBlock(table, Blocks::WATER_CAULDRON,
                           PoiTypeId::Leatherworker);
  registerAllStatesOfBlock(table, Blocks::LAVA_CAULDRON,
                           PoiTypeId::Leatherworker);
  registerAllStatesOfBlock(table, Blocks::POWDER_SNOW_CAULDRON,
                           PoiTypeId::Leatherworker);
  registerAllStatesOfBlock(table, Blocks::LECTERN, PoiTypeId::Librarian);
  registerAllStatesOfBlock(table, Blocks::STONECUTTER, PoiTypeId::Mason);
  registerAllStatesOfBlock(table, Blocks::LOOM, PoiTypeId::Shepherd);
  registerAllStatesOfBlock(table, Blocks::SMITHING_TABLE, PoiTypeId::Toolsmith);
  registerAllStatesOfBlock(table, Blocks::GRINDSTONE, PoiTypeId::Weaponsmith);

  const std::array<const Block *, 16> beds{{
      &Blocks::WHITE_BED,      &Blocks::ORANGE_BED,     &Blocks::MAGENTA_BED,
      &Blocks::LIGHT_BLUE_BED, &Blocks::YELLOW_BED,     &Blocks::LIME_BED,
      &Blocks::PINK_BED,       &Blocks::GRAY_BED,       &Blocks::LIGHT_GRAY_BED,
      &Blocks::CYAN_BED,       &Blocks::PURPLE_BED,     &Blocks::BLUE_BED,
      &Blocks::BROWN_BED,      &Blocks::GREEN_BED,      &Blocks::RED_BED,
      &Blocks::BLACK_BED,
  }};
  for (const Block *bed : beds)
    registerBedHeadStates(table, *bed);

  registerAllStatesOfBlock(table, Blocks::BELL, PoiTypeId::Meeting);
  registerAllStatesOfBlock(table, Blocks::BEEHIVE, PoiTypeId::Beehive);
  registerAllStatesOfBlock(table, Blocks::BEE_NEST, PoiTypeId::BeeNest);
  registerAllStatesOfBlock(table, Blocks::NETHER_PORTAL,
                           PoiTypeId::NetherPortal);
  registerAllStatesOfBlock(table, Blocks::LODESTONE, PoiTypeId::Lodestone);
  registerAllStatesOfBlock(table, Blocks::TEST_INSTANCE_BLOCK,
                           PoiTypeId::TestInstance);

  const std::array<const Block *, 8> lightningRods{{
      &Blocks::LIGHTNING_ROD,
      &Blocks::EXPOSED_LIGHTNING_ROD,
      &Blocks::WEATHERED_LIGHTNING_ROD,
      &Blocks::OXIDIZED_LIGHTNING_ROD,
      &Blocks::WAXED_LIGHTNING_ROD,
      &Blocks::WAXED_EXPOSED_LIGHTNING_ROD,
      &Blocks::WAXED_WEATHERED_LIGHTNING_ROD,
      &Blocks::WAXED_OXIDIZED_LIGHTNING_ROD,
  }};
  for (const Block *rod : lightningRods)
    registerAllStatesOfBlock(table, *rod, PoiTypeId::LightningRod);

  gOwnedPoiStateTable = std::move(table);
  detail::gPoiStateData = gOwnedPoiStateTable.data();
  detail::gPoiStateCount = static_cast<std::uint32_t>(gOwnedPoiStateTable.size());
}

constexpr std::int64_t distSq64(PoiPos a, PoiPos b) noexcept {
  const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
  const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
  const std::int64_t dz = static_cast<std::int64_t>(a.z) - b.z;
  return dx * dx + dy * dy + dz * dz;
}

constexpr std::int64_t minChunkHorizDistSq(PoiPos center, int chunkX,
                                           int chunkZ) noexcept {
  const int minX = chunkX * 16;
  const int minZ = chunkZ * 16;
  const int cx = std::clamp(center.x, minX, minX + 15);
  const int cz = std::clamp(center.z, minZ, minZ + 15);
  const std::int64_t dx = static_cast<std::int64_t>(center.x) - cx;
  const std::int64_t dz = static_cast<std::int64_t>(center.z) - cz;
  return dx * dx + dz * dz;
}

constexpr std::int64_t minSectionDistSq(PoiPos center, int chunkX,
                                        int secBaseY, int chunkZ) noexcept {
  const int minX = chunkX * 16;
  const int minZ = chunkZ * 16;
  const int cx = std::clamp(center.x, minX, minX + 15);
  const int cy = std::clamp(center.y, secBaseY, secBaseY + 15);
  const int cz = std::clamp(center.z, minZ, minZ + 15);
  const std::int64_t dx = static_cast<std::int64_t>(center.x) - cx;
  const std::int64_t dy = static_cast<std::int64_t>(center.y) - cy;
  const std::int64_t dz = static_cast<std::int64_t>(center.z) - cz;
  return dx * dx + dy * dy + dz * dz;
}

constexpr std::int64_t ringLowerBoundDistSq(PoiPos center, int originCx,
                                            int originCz, int ring,
                                            std::int64_t minWorldYDistSq) noexcept {
  if (ring <= 0)
    return minWorldYDistSq;
  const int ox = center.x - originCx * 16;
  const int oz = center.z - originCz * 16;
  const int edge = std::min({ox + 1, 16 - ox, oz + 1, 16 - oz});
  const std::int64_t d = static_cast<std::int64_t>(ring - 1) * 16 + edge;
  return d * d + minWorldYDistSq;
}

struct PortalCandidate {
  std::int64_t distSq = 0;
  int chunkZ = 0;
  int chunkX = 0;
  int sectionIndex = 0;
  std::uint16_t localPos = 0;
  PoiMatch match;
};

struct SphereCandidate {
  std::int64_t distSq = 0;
  int chunkZ = 0;
  int chunkX = 0;
  int sectionIndex = 0;
  std::uint16_t localPos = 0;
  PoiMatch match;
};

// Portal tie-breaking matches PortalForcer.java:49 (distSqr, then lowest Y),
// with deterministic chunk/section/cell order on exact ties.
bool portalLess(const PortalCandidate &a, const PortalCandidate &b) noexcept {
  return std::tie(a.distSq, a.match.pos.y, a.chunkZ, a.chunkX, a.sectionIndex,
                  a.localPos) < std::tie(b.distSq, b.match.pos.y, b.chunkZ,
                                         b.chunkX, b.sectionIndex, b.localPos);
}

bool sphereLess(const SphereCandidate &a, const SphereCandidate &b) noexcept {
  return std::tie(a.distSq, a.chunkZ, a.chunkX, a.sectionIndex, a.localPos) <
         std::tie(b.distSq, b.chunkZ, b.chunkX, b.sectionIndex, b.localPos);
}

// Visit all chunks on the Chebyshev ring max(|dx|, |dz|) == ring clamped to bounds.
template <class Fn>
void forEachChunkInRing(int originCx, int originCz, int ring, int minCx,
                        int maxCx, int minCz, int maxCz, Fn &&fn) {
  if (ring == 0) {
    if (originCx >= minCx && originCx <= maxCx && originCz >= minCz &&
        originCz <= maxCz)
      fn(originCx, originCz);
    return;
  }
  const int topZ = originCz - ring;
  const int bottomZ = originCz + ring;
  const int leftX = originCx - ring;
  const int rightX = originCx + ring;
  const int xLo = std::max(minCx, leftX);
  const int xHi = std::min(maxCx, rightX);
  if (topZ >= minCz && topZ <= maxCz) {
    for (int cx = xLo; cx <= xHi; ++cx)
      fn(cx, topZ);
  }
  if (bottomZ >= minCz && bottomZ <= maxCz) {
    for (int cx = xLo; cx <= xHi; ++cx)
      fn(cx, bottomZ);
  }
  const int zLo = std::max(minCz, topZ + 1);
  const int zHi = std::min(maxCz, bottomZ - 1);
  if (leftX >= minCx && leftX <= maxCx) {
    for (int cz = zLo; cz <= zHi; ++cz)
      fn(leftX, cz);
  }
  if (rightX >= minCx && rightX <= maxCx) {
    for (int cz = zLo; cz <= zHi; ++cz)
      fn(rightX, cz);
  }
}

bool parseSignedIntStrict(std::string_view text, int &out) noexcept {
  if (text.empty())
    return false;
  std::size_t idx = 0;
  bool neg = false;
  if (text[0] == '-') {
    neg = true;
    idx = 1;
  }
  if (idx >= text.size())
    return false;
  std::int64_t val = 0;
  for (; idx < text.size(); ++idx) {
    const char c = text[idx];
    if (c < '0' || c > '9')
      return false;
    val = val * 10 + (c - '0');
    if (val > std::numeric_limits<int>::max() + (neg ? 1LL : 0LL))
      return false;
  }
  out = static_cast<int>(neg ? -val : val);
  return true;
}

} // namespace

namespace detail {
void initPoiStateTable() { std::call_once(gPoiStateInitOnce, buildPoiStateTableImpl); }
} // namespace detail

void ensurePoiStateTable() {
  if (detail::gPoiStateCount == 0)
    detail::initPoiStateTable();
}

const PoiTypeSpec &poiTypeSpec(PoiTypeId id) {
  const auto index = static_cast<std::size_t>(id);
  if (index >= kPoiTypeSpecs.size())
    return kPoiTypeSpecs[0];
  return kPoiTypeSpecs[index];
}

PoiTypeId poiTypeFromName(std::string_view name) {
  if (name.empty())
    return PoiTypeId::None;
  const bool hasNs = name.find(':') != std::string_view::npos;
  for (std::size_t i = 1; i < kPoiTypeSpecs.size(); ++i) {
    const std::string_view specName = kPoiTypeSpecs[i].name;
    if (hasNs) {
      if (specName == name)
        return kPoiTypeSpecs[i].id;
    } else {
      if (specName.substr(10) == name) // strip "minecraft:"
        return kPoiTypeSpecs[i].id;
    }
  }
  return PoiTypeId::None;
}

bool paletteHasAnyPoi(std::span<const BlockState::Id> palette) noexcept {
  ensurePoiStateTable();
  for (BlockState::Id sid : palette) {
    if (fastPoiTypeForState(sid) != PoiTypeId::None)
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// PoiSection
// ---------------------------------------------------------------------------

std::optional<std::size_t>
PoiSection::findIndex(std::uint16_t localPos) const noexcept {
  if (!indexByLocal.empty()) {
    if (auto it = indexByLocal.find(localPos); it != indexByLocal.end())
      return static_cast<std::size_t>(it->second);
    return std::nullopt;
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].localPos() == localPos)
      return i;
  }
  return std::nullopt;
}

bool PoiSection::add(std::uint16_t localPos, PoiTypeId type,
                     std::uint8_t freeTickets, bool *replacedMismatch) {
  if (replacedMismatch)
    *replacedMismatch = false;
  if (type == PoiTypeId::None)
    return false;
  const auto typeIndex = static_cast<std::size_t>(type) - 1u;
  if (typeIndex >= kPoiTypeCount)
    return false;

  if (auto existingIdx = findIndex(localPos)) {
    PackedPoiEntry &existing = entries[*existingIdx];
    if (existing.type() == type)
      return false;
    if (replacedMismatch)
      *replacedMismatch = true;
    const auto oldIndex = static_cast<std::size_t>(existing.type()) - 1u;
    if (--typeCounts[oldIndex] == 0)
      typeMask &= ~poiTypeMask(existing.type());
    existing = PackedPoiEntry::make(localPos, type, freeTickets);
    ++typeCounts[typeIndex];
    typeMask |= poiTypeMask(type);
    return true;
  }

  const auto newIdx = static_cast<std::uint16_t>(entries.size());
  entries.push_back(PackedPoiEntry::make(localPos, type, freeTickets));
  if (!indexByLocal.empty()) {
    indexByLocal[localPos] = newIdx;
  } else if (entries.size() > kLinearScanLimit) {
    for (std::size_t i = 0; i < entries.size(); ++i)
      indexByLocal[entries[i].localPos()] = static_cast<std::uint16_t>(i);
  }
  ++typeCounts[typeIndex];
  typeMask |= poiTypeMask(type);
  return true;
}

bool PoiSection::remove(std::uint16_t localPos, PoiTypeId *removedType) {
  auto idxOpt = findIndex(localPos);
  if (!idxOpt)
    return false;
  const std::size_t idx = *idxOpt;
  const PoiTypeId oldType = entries[idx].type();
  if (removedType)
    *removedType = oldType;

  const auto oldTypeIndex = static_cast<std::size_t>(oldType) - 1u;
  if (oldTypeIndex < kPoiTypeCount && --typeCounts[oldTypeIndex] == 0)
    typeMask &= ~poiTypeMask(oldType);

  const std::size_t lastIdx = entries.size() - 1u;
  if (!indexByLocal.empty())
    indexByLocal.erase(localPos);
  if (idx != lastIdx) {
    const PackedPoiEntry moved = entries[lastIdx];
    entries[idx] = moved;
    if (!indexByLocal.empty())
      indexByLocal[moved.localPos()] = static_cast<std::uint16_t>(idx);
  }
  entries.pop_back();
  if (!indexByLocal.empty() && entries.size() <= kLinearScanLimit / 2)
    indexByLocal.clear();
  return true;
}

void PoiSection::clear() noexcept {
  typeMask = 0;
  typeCounts.fill(0);
  entries.clear();
  indexByLocal.clear();
}

bool PoiSection::matchesOccupancy(PackedPoiEntry entry,
                                  PoiOccupancy occupancy) noexcept {
  switch (occupancy) {
  case PoiOccupancy::Any:
    return true;
  case PoiOccupancy::HasSpace:
    return entry.freeTickets() > 0;
  case PoiOccupancy::IsOccupied: {
    const std::uint8_t maxTickets = poiTypeSpec(entry.type()).maxTickets;
    return entry.freeTickets() != maxTickets;
  }
  }
  return false;
}

std::uint32_t
PoiChunkColumn::candidateSections(std::uint32_t queryTypeMask) const noexcept {
  std::uint32_t activeTypes = typeMask & queryTypeMask;
  if (activeTypes == 0)
    return 0;
  // Fast single-bit path (nether_portal, home, lightning_rod, etc.)
  if (std::has_single_bit(activeTypes))
    return sectionMasksByType[static_cast<std::size_t>(std::countr_zero(activeTypes))];
  std::uint32_t result = 0;
  while (activeTypes != 0) {
    const unsigned bit = static_cast<unsigned>(std::countr_zero(activeTypes));
    result |= sectionMasksByType[bit];
    activeTypes &= activeTypes - 1u;
  }
  return result;
}

// ---------------------------------------------------------------------------
// PoiChunkCodec (Vanilla 26.1 poi/r.x.z.mca)
// ---------------------------------------------------------------------------

DecodedPoiChunk decodePoiChunk(std::span<const std::uint8_t> nbtBytes,
                               int minSectionY, int maxSectionYInclusive) {
  DecodedPoiChunk out;
  try {
    const nbt::RootTag root = nbt::parseRoot(nbtBytes);
    if (root.value.type != 10) {
      out.status = DecodedPoiChunk::Status::Corrupt;
      out.detail = "poi chunk root is not a compound";
      return out;
    }
    const nbt::Tag *dv = root.value.get("DataVersion");
    if (!dv || dv->type != 3) {
      out.status = DecodedPoiChunk::Status::Corrupt;
      out.detail = "poi chunk missing integer DataVersion";
      return out;
    }
    out.dataVersion = static_cast<int>(dv->number);
    if (out.dataVersion != 4786) {
      out.status = DecodedPoiChunk::Status::Unsupported;
      out.detail =
          "unsupported poi chunk DataVersion: " + std::to_string(out.dataVersion);
      return out;
    }

    const nbt::Tag *sectionsTag = root.value.get("Sections");
    if (!sectionsTag) {
      out.status = DecodedPoiChunk::Status::Ok;
      return out;
    }
    if (sectionsTag->type != 10) {
      out.status = DecodedPoiChunk::Status::Corrupt;
      out.detail = "poi chunk Sections is not a compound";
      return out;
    }

    for (const auto &[secName, secTag] : sectionsTag->compound) {
      int secY = 0;
      if (!parseSignedIntStrict(secName, secY))
        continue;
      if (secY < minSectionY || secY > maxSectionYInclusive)
        continue;
      if (secTag.type != 10) {
        out.status = DecodedPoiChunk::Status::Corrupt;
        out.detail = "poi section entry is not a compound";
        return out;
      }

      DecodedPoiSection decodedSec;
      decodedSec.sectionY = secY;
      if (const nbt::Tag *validTag = secTag.get("Valid")) {
        if (validTag->type != 1) {
          out.status = DecodedPoiChunk::Status::Corrupt;
          out.detail = "poi section Valid tag is not a byte";
          return out;
        }
        decodedSec.valid = validTag->number != 0;
      }

      const nbt::Tag *recordsTag = secTag.get("Records");
      if (!recordsTag || recordsTag->type != 9) {
        out.status = DecodedPoiChunk::Status::Corrupt;
        out.detail = "poi section missing Records list";
        return out;
      }
      decodedSec.entries.reserve(recordsTag->list.size());
      for (const nbt::Tag &recTag : recordsTag->list) {
        if (recTag.type != 10) {
          out.status = DecodedPoiChunk::Status::Corrupt;
          out.detail = "poi record entry is not a compound";
          return out;
        }
        const nbt::Tag *posTag = recTag.get("pos");
        const nbt::Tag *typeTag = recTag.get("type");
        if (!posTag || posTag->type != 11 || posTag->ints.size() != 3 ||
            !typeTag || typeTag->type != 8) {
          out.status = DecodedPoiChunk::Status::Corrupt;
          out.detail = "poi record malformed pos or type";
          return out;
        }
        const PoiTypeId poiType = poiTypeFromName(typeTag->text);
        if (poiType == PoiTypeId::None) {
          out.status = DecodedPoiChunk::Status::Unsupported;
          out.detail = "unsupported poi type: " + typeTag->text;
          return out;
        }
        int freeTickets = 0;
        if (const nbt::Tag *ftTag = recTag.get("free_tickets")) {
          if (ftTag->type != 3) {
            out.status = DecodedPoiChunk::Status::Corrupt;
            out.detail = "poi record free_tickets is not an integer";
            return out;
          }
          freeTickets = static_cast<int>(ftTag->number);
        }
        freeTickets = std::clamp(freeTickets, 0, 63);
        const int x = posTag->ints[0];
        const int y = posTag->ints[1];
        const int z = posTag->ints[2];
        const std::uint16_t localPos = static_cast<std::uint16_t>(
            ((y & 15) << 8) | ((z & 15) << 4) | (x & 15));
        decodedSec.entries.push_back(PackedPoiEntry::make(
            localPos, poiType, static_cast<std::uint8_t>(freeTickets)));
      }
      out.sections.push_back(std::move(decodedSec));
    }
    out.status = DecodedPoiChunk::Status::Ok;
    return out;
  } catch (const nbt::Error &err) {
    out.status = DecodedPoiChunk::Status::Corrupt;
    out.detail = err.what();
    return out;
  }
}

std::vector<std::uint8_t> encodePoiChunk(int chunkX, int chunkZ, int minSectionY,
                                         int sectionCount,
                                         const PoiChunkColumn &column) {
  nbt::RootTag root;
  root.name = "";
  root.value.type = 10;

  nbt::Tag sectionsTag;
  sectionsTag.type = 10;
  for (int si = 0; si < sectionCount && si < static_cast<int>(kPoiSectionCapacity);
       ++si) {
    const PoiSection *section = column.sections[si].get();
    if (!section || (section->empty() && !section->valid))
      continue;
    if (section->empty())
      continue;
    const int secY = minSectionY + si;
    const int baseY = secY * 16;

    nbt::Tag secTag;
    secTag.type = 10;

    nbt::Tag validTag;
    validTag.type = 1;
    validTag.number = section->valid ? 1 : 0;
    secTag.compound["Valid"] = std::move(validTag);
    secTag.compoundOrder.push_back("Valid");

    nbt::Tag recordsTag;
    recordsTag.type = 9;
    recordsTag.listElementType = 10;
    recordsTag.list.reserve(section->entries.size());
    for (PackedPoiEntry entry : section->entries) {
      const std::uint16_t lp = entry.localPos();
      const int wx = chunkX * 16 + static_cast<int>(lp & 15u);
      const int wy = baseY + static_cast<int>((lp >> 8) & 15u);
      const int wz = chunkZ * 16 + static_cast<int>((lp >> 4) & 15u);

      nbt::Tag rec;
      rec.type = 10;

      nbt::Tag pos;
      pos.type = 11;
      pos.ints = {wx, wy, wz};
      rec.compound["pos"] = std::move(pos);
      rec.compoundOrder.push_back("pos");

      nbt::Tag type;
      type.type = 8;
      type.text = std::string(poiTypeSpec(entry.type()).name);
      rec.compound["type"] = std::move(type);
      rec.compoundOrder.push_back("type");

      nbt::Tag ft;
      ft.type = 3;
      ft.number = entry.freeTickets();
      rec.compound["free_tickets"] = std::move(ft);
      rec.compoundOrder.push_back("free_tickets");

      recordsTag.list.push_back(std::move(rec));
    }
    secTag.compound["Records"] = std::move(recordsTag);
    secTag.compoundOrder.push_back("Records");

    const std::string key = std::to_string(secY);
    sectionsTag.compound[key] = std::move(secTag);
    sectionsTag.compoundOrder.push_back(key);
  }

  nbt::Tag dv;
  dv.type = 3;
  dv.number = 4786;

  root.value.compound["Sections"] = std::move(sectionsTag);
  root.value.compoundOrder.push_back("Sections");
  root.value.compound["DataVersion"] = std::move(dv);
  root.value.compoundOrder.push_back("DataVersion");
  return nbt::writeRoot(root);
}

DecodedPoiChunk loadPoiChunkFromRegion(const std::filesystem::path &poiDir,
                                       int chunkX, int chunkZ, int minSectionY,
                                       int maxSectionYInclusive) {
  const region::ReadResult raw = region::readChunkNbt(poiDir, chunkX, chunkZ);
  if (raw.status == region::ReadStatus::Missing) {
    DecodedPoiChunk res;
    res.status = DecodedPoiChunk::Status::Missing;
    return res;
  }
  if (raw.status == region::ReadStatus::Unsupported) {
    DecodedPoiChunk res;
    res.status = DecodedPoiChunk::Status::Unsupported;
    res.detail = raw.detail;
    return res;
  }
  if (raw.status != region::ReadStatus::Found) {
    DecodedPoiChunk res;
    res.status = DecodedPoiChunk::Status::Corrupt;
    res.detail = raw.detail;
    return res;
  }
  return decodePoiChunk(raw.nbt, minSectionY, maxSectionYInclusive);
}

void savePoiChunkToRegion(const std::filesystem::path &poiDir, int chunkX,
                          int chunkZ, int minSectionY, int sectionCount,
                          const PoiChunkColumn &column) {
  const std::vector<std::uint8_t> bytes =
      encodePoiChunk(chunkX, chunkZ, minSectionY, sectionCount, column);
  region::writeChunkNbt(poiDir, chunkX, chunkZ, bytes);
}

// ---------------------------------------------------------------------------
// PoiStorage
// ---------------------------------------------------------------------------

PoiStorage::PoiStorage(int minY, int maxY)
    : minY_(minY), maxY_(maxY),
      sectionCount_(std::clamp(((maxY - minY) + 15) / 16, 1,
                               static_cast<int>(kPoiSectionCapacity))) {
  ensurePoiStateTable();
}

PoiChunkColumn &PoiStorage::getOrCreateColumn(int chunkX, int chunkZ) {
  return columns_[chunkKey(chunkX, chunkZ)];
}

void PoiStorage::refreshColumnSectionMasks(PoiChunkColumn &col,
                                           int sectionIndex) {
  const std::uint32_t secBit = std::uint32_t{1} << sectionIndex;
  const PoiSection *sec = col.sections[sectionIndex].get();
  if (!sec || sec->empty()) {
    col.nonEmptyMask &= ~secBit;
    for (std::size_t t = 0; t < kPoiTypeCount; ++t) {
      if (col.sectionMasksByType[t] & secBit) {
        col.sectionMasksByType[t] &= ~secBit;
        if (col.sectionMasksByType[t] == 0)
          col.typeMask &= ~(std::uint32_t{1} << t);
      }
    }
    if (sec && sec->valid)
      col.validMask |= secBit;
    else
      col.validMask &= ~secBit;
    return;
  }

  col.nonEmptyMask |= secBit;
  if (sec->valid)
    col.validMask |= secBit;
  else
    col.validMask &= ~secBit;

  for (std::size_t t = 0; t < kPoiTypeCount; ++t) {
    const std::uint32_t tBit = std::uint32_t{1} << t;
    if (sec->typeMask & tBit) {
      col.sectionMasksByType[t] |= secBit;
      col.typeMask |= tBit;
    } else if (col.sectionMasksByType[t] & secBit) {
      col.sectionMasksByType[t] &= ~secBit;
      if (col.sectionMasksByType[t] == 0)
        col.typeMask &= ~tBit;
    }
  }
}

PoiSection &PoiStorage::getOrCreateSection(PoiChunkColumn &col, int sectionIndex,
                                           bool markValid) {
  auto &ptr = col.sections[sectionIndex];
  if (!ptr)
    ptr = std::make_unique<PoiSection>();
  if (markValid && !ptr->valid) {
    ptr->valid = true;
    col.validMask |= (std::uint32_t{1} << sectionIndex);
  }
  return *ptr;
}

bool PoiStorage::add(PoiPos pos, PoiTypeId type) {
  return addWithTickets(pos, type, poiTypeSpec(type).maxTickets);
}

bool PoiStorage::addWithTickets(PoiPos pos, PoiTypeId type,
                                std::uint8_t freeTickets) {
  if (type == PoiTypeId::None || pos.y < minY_ || pos.y >= maxY_)
    return false;
  const int cx = pos.x >> 4;
  const int cz = pos.z >> 4;
  const int si = sectionIndexForY(pos.y);
  if (si < 0 || si >= sectionCount_)
    return false;
  PoiChunkColumn &col = getOrCreateColumn(cx, cz);
  PoiSection &sec = getOrCreateSection(col, si, true);
  const std::uint16_t lp = localPosFor(pos, si);
  if (!sec.add(lp, type, freeTickets))
    return false;
  refreshColumnSectionMasks(col, si);
  col.dirty = true;
  return true;
}

bool PoiStorage::remove(PoiPos pos) {
  if (pos.y < minY_ || pos.y >= maxY_)
    return false;
  const int cx = pos.x >> 4;
  const int cz = pos.z >> 4;
  const int si = sectionIndexForY(pos.y);
  if (si < 0 || si >= sectionCount_)
    return false;
  auto colIt = columns_.find(chunkKey(cx, cz));
  if (colIt == columns_.end())
    return false;
  PoiChunkColumn *col = &colIt->second;
  PoiSection *sec = col->sections[si].get();
  if (!sec || !sec->remove(localPosFor(pos, si)))
    return false;
  refreshColumnSectionMasks(*col, si);
  col->dirty = true;
  return true;
}

void PoiStorage::onBlockStateChange(PoiPos pos, BlockState::Id oldStateId,
                                    BlockState::Id newStateId) {
  const PoiTypeId oldPoi = fastPoiTypeForState(oldStateId);
  const PoiTypeId newPoi = fastPoiTypeForState(newStateId);
  if (oldPoi == newPoi)
    return;
  if (oldPoi != PoiTypeId::None)
    remove(pos);
  if (newPoi != PoiTypeId::None)
    add(pos, newPoi);
}

bool PoiStorage::isSectionValid(int chunkX, int chunkZ,
                                int sectionIndex) const noexcept {
  if (sectionIndex < 0 || sectionIndex >= sectionCount_)
    return false;
  const PoiChunkColumn *col = findColumn(chunkX, chunkZ);
  return col != nullptr &&
         (col->validMask & (std::uint32_t{1} << sectionIndex)) != 0;
}

void PoiStorage::markSectionValidEmpty(int chunkX, int chunkZ,
                                       int sectionIndex) {
  if (sectionIndex < 0 || sectionIndex >= sectionCount_)
    return;
  auto colIt = columns_.find(chunkKey(chunkX, chunkZ));
  if (colIt == columns_.end())
    return;
  PoiChunkColumn *col = &colIt->second;
  const std::uint32_t secBit = std::uint32_t{1} << sectionIndex;
  if (PoiSection *sec = col->sections[sectionIndex].get()) {
    if (!sec->empty()) {
      sec->clear();
      col->dirty = true;
    }
    sec->valid = true;
    refreshColumnSectionMasks(*col, sectionIndex);
  } else {
    col->validMask |= secBit;
  }
}

void PoiStorage::reconcileSectionFromScan(
    int chunkX, int chunkZ, int sectionIndex,
    std::span<const std::pair<std::uint16_t, PoiTypeId>> found,
    bool markDirtyOnChange) {
  if (sectionIndex < 0 || sectionIndex >= sectionCount_)
    return;
  if (found.empty()) {
    markSectionValidEmpty(chunkX, chunkZ, sectionIndex);
    return;
  }

  PoiChunkColumn &col = getOrCreateColumn(chunkX, chunkZ);
  PoiSection &sec = getOrCreateSection(col, sectionIndex, false);

  // Retain freeTickets for unchanged positions/types, matching PoiSection.refresh.
  std::vector<PackedPoiEntry> previousEntries = std::move(sec.entries);
  sec.clear();
  for (const auto &[lp, type] : found) {
    if (type == PoiTypeId::None)
      continue;
    std::uint8_t tickets = poiTypeSpec(type).maxTickets;
    for (PackedPoiEntry old : previousEntries) {
      if (old.localPos() == lp && old.type() == type) {
        tickets = old.freeTickets();
        break;
      }
    }
    sec.add(lp, type, tickets);
  }
  sec.valid = true;
  refreshColumnSectionMasks(col, sectionIndex);
  if (markDirtyOnChange)
    col.dirty = true;
}

void PoiStorage::applyDecodedChunk(int chunkX, int chunkZ,
                                   const DecodedPoiChunk &decoded) {
  if (decoded.status != DecodedPoiChunk::Status::Ok || decoded.sections.empty())
    return;
  PoiChunkColumn &col = getOrCreateColumn(chunkX, chunkZ);
  const int minSecY = minSectionY();
  for (const DecodedPoiSection &dsec : decoded.sections) {
    const int si = dsec.sectionY - minSecY;
    if (si < 0 || si >= sectionCount_)
      continue;
    auto sec = std::make_unique<PoiSection>();
    sec->valid = dsec.valid;
    for (PackedPoiEntry entry : dsec.entries) {
      sec->add(entry.localPos(), entry.type(), entry.freeTickets());
    }
    col.sections[si] = std::move(sec);
    refreshColumnSectionMasks(col, si);
  }
  col.dirty = false;
}

void PoiStorage::removeChunk(int chunkX, int chunkZ) {
  columns_.erase(chunkKey(chunkX, chunkZ));
}

void PoiStorage::clear() { columns_.clear(); }

const PoiChunkColumn *PoiStorage::findColumn(int chunkX,
                                             int chunkZ) const noexcept {
  const auto it = columns_.find(chunkKey(chunkX, chunkZ));
  return it != columns_.end() ? &it->second : nullptr;
}

bool PoiStorage::exists(PoiPos pos, std::uint32_t typeMask) const noexcept {
  const PoiTypeId type = getType(pos);
  return type != PoiTypeId::None && (poiTypeMask(type) & typeMask) != 0;
}

bool PoiStorage::existsAtPosition(PoiTypeId type, PoiPos pos) const noexcept {
  return getType(pos) == type;
}

PoiTypeId PoiStorage::getType(PoiPos pos) const noexcept {
  if (pos.y < minY_ || pos.y >= maxY_)
    return PoiTypeId::None;
  const PoiChunkColumn *col = findColumn(pos.x >> 4, pos.z >> 4);
  if (!col)
    return PoiTypeId::None;
  const int si = sectionIndexForY(pos.y);
  if (si < 0 || si >= sectionCount_ ||
      (col->nonEmptyMask & (std::uint32_t{1} << si)) == 0)
    return PoiTypeId::None;
  const PoiSection *sec = col->sections[si].get();
  if (!sec)
    return PoiTypeId::None;
  if (auto idx = sec->findIndex(localPosFor(pos, si)))
    return sec->entries[*idx].type();
  return PoiTypeId::None;
}

std::uint8_t PoiStorage::freeTickets(PoiPos pos) const noexcept {
  if (pos.y < minY_ || pos.y >= maxY_)
    return 0;
  const PoiChunkColumn *col = findColumn(pos.x >> 4, pos.z >> 4);
  if (!col)
    return 0;
  const int si = sectionIndexForY(pos.y);
  if (si < 0 || si >= sectionCount_)
    return 0;
  const PoiSection *sec = col->sections[si].get();
  if (!sec)
    return 0;
  if (auto idx = sec->findIndex(localPosFor(pos, si)))
    return sec->entries[*idx].freeTickets();
  return 0;
}

bool PoiStorage::acquireTicket(PoiPos pos) {
  if (pos.y < minY_ || pos.y >= maxY_)
    return false;
  auto colIt = columns_.find(chunkKey(pos.x >> 4, pos.z >> 4));
  if (colIt == columns_.end())
    return false;
  PoiChunkColumn *col = &colIt->second;
  const int si = sectionIndexForY(pos.y);
  if (si < 0 || si >= sectionCount_)
    return false;
  PoiSection *sec = col->sections[si].get();
  if (!sec)
    return false;
  auto idx = sec->findIndex(localPosFor(pos, si));
  if (!idx)
    return false;
  PackedPoiEntry &entry = sec->entries[*idx];
  const std::uint8_t ft = entry.freeTickets();
  if (ft == 0)
    return false;
  entry.setFreeTickets(static_cast<std::uint8_t>(ft - 1u));
  col->dirty = true;
  return true;
}

bool PoiStorage::releaseTicket(PoiPos pos) {
  if (pos.y < minY_ || pos.y >= maxY_)
    return false;
  auto colIt = columns_.find(chunkKey(pos.x >> 4, pos.z >> 4));
  if (colIt == columns_.end())
    return false;
  PoiChunkColumn *col = &colIt->second;
  const int si = sectionIndexForY(pos.y);
  if (si < 0 || si >= sectionCount_)
    return false;
  PoiSection *sec = col->sections[si].get();
  if (!sec)
    return false;
  auto idx = sec->findIndex(localPosFor(pos, si));
  if (!idx)
    return false;
  PackedPoiEntry &entry = sec->entries[*idx];
  const std::uint8_t ft = entry.freeTickets();
  const std::uint8_t maxT = poiTypeSpec(entry.type()).maxTickets;
  if (ft >= maxT)
    return false;
  entry.setFreeTickets(static_cast<std::uint8_t>(ft + 1u));
  col->dirty = true;
  return true;
}

std::optional<PoiPos>
PoiStorage::take(PoiPos center, int radius, std::uint32_t typeMask,
                 const std::function<bool(PoiTypeId, PoiPos)> &filter) {
  collectInRange(center, radius, typeMask, PoiOccupancy::HasSpace,
                 scratchMatches_);
  for (const PoiMatch &m : scratchMatches_) {
    if (!filter || filter(m.type, m.pos)) {
      if (acquireTicket(m.pos))
        return m.pos;
    }
  }
  return std::nullopt;
}

bool PoiStorage::hasDirtyChunks() const noexcept {
  for (const auto &[key, col] : columns_) {
    (void)key;
    if (col.dirty)
      return true;
  }
  return false;
}

std::vector<std::pair<int, int>> PoiStorage::dirtyChunks() const {
  std::vector<std::pair<int, int>> out;
  for (const auto &[key, col] : columns_) {
    if (col.dirty) {
      out.emplace_back(static_cast<int>(static_cast<std::int32_t>(key >> 32)),
                       static_cast<int>(static_cast<std::int32_t>(key)));
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

void PoiStorage::clearChunkDirty(int chunkX, int chunkZ) noexcept {
  if (auto colIt = columns_.find(chunkKey(chunkX, chunkZ));
      colIt != columns_.end())
    colIt->second.dirty = false;
}

void PoiStorage::collectInChunk(int chunkX, int chunkZ, std::uint32_t typeMask,
                                PoiOccupancy occupancy,
                                std::vector<PoiMatch> &out) const {
  const PoiChunkColumn *col = findColumn(chunkX, chunkZ);
  if (!col)
    return;
  std::uint32_t secMask = col->candidateSections(typeMask);
  while (secMask != 0) {
    const int si = std::countr_zero(secMask);
    secMask &= secMask - 1u;
    const PoiSection *sec = col->sections[si].get();
    if (!sec)
      continue;
    for (PackedPoiEntry entry : sec->entries) {
      if ((poiTypeMask(entry.type()) & typeMask) == 0)
        continue;
      if (!PoiSection::matchesOccupancy(entry, occupancy))
        continue;
      out.push_back(PoiMatch{worldPosFor(chunkX, chunkZ, si, entry.localPos()),
                             entry.type(), entry.freeTickets()});
    }
  }
}

void PoiStorage::enqueueColumnCandidateSections(
    int chunkX, int chunkZ, const PoiChunkColumn &col, std::uint32_t typeMask,
    PoiPos center, std::vector<QueuedSection> &queue) const {
  std::uint32_t secMask = col.candidateSections(typeMask);
  while (secMask != 0) {
    const int si = std::countr_zero(secMask);
    secMask &= secMask - 1u;
    const PoiSection *sec = col.sections[si].get();
    if (!sec)
      continue;
    const std::int64_t d2 =
        minSectionDistSq(center, chunkX, sectionBaseY(si), chunkZ);
    queue.push_back(QueuedSection{d2, chunkX, chunkZ,
                                  static_cast<std::uint8_t>(si), sec});
    std::push_heap(queue.begin(), queue.end(),
                   [](const QueuedSection &a, const QueuedSection &b) {
                     return a.minDistSq > b.minDistSq;
                   });
  }
}

void PoiStorage::collectInSquare(PoiPos center, int radius,
                                 std::uint32_t typeMask,
                                 PoiOccupancy occupancy,
                                 std::vector<PoiMatch> &out) const {
  out.clear();
  if (radius < 0 || columns_.empty() || typeMask == 0)
    return;
  const int minCx = (center.x - radius) >> 4;
  const int maxCx = (center.x + radius) >> 4;
  const int minCz = (center.z - radius) >> 4;
  const int maxCz = (center.z + radius) >> 4;
  for (int cz = minCz; cz <= maxCz; ++cz) {
    for (int cx = minCx; cx <= maxCx; ++cx) {
      const PoiChunkColumn *col = findColumn(cx, cz);
      if (!col)
        continue;
      std::uint32_t secMask = col->candidateSections(typeMask);
      while (secMask != 0) {
        const int si = std::countr_zero(secMask);
        secMask &= secMask - 1u;
        const PoiSection *sec = col->sections[si].get();
        if (!sec)
          continue;
        for (PackedPoiEntry entry : sec->entries) {
          if ((poiTypeMask(entry.type()) & typeMask) == 0 ||
              !PoiSection::matchesOccupancy(entry, occupancy))
            continue;
          const PoiPos p = worldPosFor(cx, cz, si, entry.localPos());
          if (std::abs(p.x - center.x) <= radius &&
              std::abs(p.z - center.z) <= radius) {
            out.push_back(PoiMatch{p, entry.type(), entry.freeTickets()});
          }
        }
      }
    }
  }
}

void PoiStorage::collectInRange(PoiPos center, int radius,
                                std::uint32_t typeMask,
                                PoiOccupancy occupancy,
                                std::vector<PoiMatch> &out) const {
  out.clear();
  if (radius < 0 || columns_.empty() || typeMask == 0)
    return;
  const std::int64_t rSq =
      static_cast<std::int64_t>(radius) * static_cast<std::int64_t>(radius);
  const int minCx = (center.x - radius) >> 4;
  const int maxCx = (center.x + radius) >> 4;
  const int minCz = (center.z - radius) >> 4;
  const int maxCz = (center.z + radius) >> 4;
  for (int cz = minCz; cz <= maxCz; ++cz) {
    for (int cx = minCx; cx <= maxCx; ++cx) {
      if (minChunkHorizDistSq(center, cx, cz) > rSq)
        continue;
      const PoiChunkColumn *col = findColumn(cx, cz);
      if (!col)
        continue;
      std::uint32_t secMask = col->candidateSections(typeMask);
      while (secMask != 0) {
        const int si = std::countr_zero(secMask);
        secMask &= secMask - 1u;
        if (minSectionDistSq(center, cx, sectionBaseY(si), cz) > rSq)
          continue;
        const PoiSection *sec = col->sections[si].get();
        if (!sec)
          continue;
        for (PackedPoiEntry entry : sec->entries) {
          if ((poiTypeMask(entry.type()) & typeMask) == 0 ||
              !PoiSection::matchesOccupancy(entry, occupancy))
            continue;
          const PoiPos p = worldPosFor(cx, cz, si, entry.localPos());
          if (distSq64(center, p) <= rSq)
            out.push_back(PoiMatch{p, entry.type(), entry.freeTickets()});
        }
      }
    }
  }
}

std::size_t PoiStorage::countInRange(PoiPos center, int radius,
                                     std::uint32_t typeMask,
                                     PoiOccupancy occupancy) const {
  if (radius < 0 || columns_.empty() || typeMask == 0)
    return 0;
  std::size_t count = 0;
  const std::int64_t rSq =
      static_cast<std::int64_t>(radius) * static_cast<std::int64_t>(radius);
  const int minCx = (center.x - radius) >> 4;
  const int maxCx = (center.x + radius) >> 4;
  const int minCz = (center.z - radius) >> 4;
  const int maxCz = (center.z + radius) >> 4;
  for (int cz = minCz; cz <= maxCz; ++cz) {
    for (int cx = minCx; cx <= maxCx; ++cx) {
      if (minChunkHorizDistSq(center, cx, cz) > rSq)
        continue;
      const PoiChunkColumn *col = findColumn(cx, cz);
      if (!col)
        continue;
      std::uint32_t secMask = col->candidateSections(typeMask);
      while (secMask != 0) {
        const int si = std::countr_zero(secMask);
        secMask &= secMask - 1u;
        if (minSectionDistSq(center, cx, sectionBaseY(si), cz) > rSq)
          continue;
        const PoiSection *sec = col->sections[si].get();
        if (!sec)
          continue;
        for (PackedPoiEntry entry : sec->entries) {
          if ((poiTypeMask(entry.type()) & typeMask) == 0 ||
              !PoiSection::matchesOccupancy(entry, occupancy))
            continue;
          const PoiPos p = worldPosFor(cx, cz, si, entry.localPos());
          if (distSq64(center, p) <= rSq)
            ++count;
        }
      }
    }
  }
  return count;
}

std::optional<PoiPos> PoiStorage::findFirst(PoiPos center, int radius,
                                            std::uint32_t typeMask,
                                            PoiOccupancy occupancy,
                                            const PosFilter &filter) const {
  if (radius < 0 || columns_.empty() || typeMask == 0)
    return std::nullopt;
  const std::int64_t rSq =
      static_cast<std::int64_t>(radius) * static_cast<std::int64_t>(radius);
  const int minCx = (center.x - radius) >> 4;
  const int maxCx = (center.x + radius) >> 4;
  const int minCz = (center.z - radius) >> 4;
  const int maxCz = (center.z + radius) >> 4;
  for (int cz = minCz; cz <= maxCz; ++cz) {
    for (int cx = minCx; cx <= maxCx; ++cx) {
      if (minChunkHorizDistSq(center, cx, cz) > rSq)
        continue;
      const PoiChunkColumn *col = findColumn(cx, cz);
      if (!col)
        continue;
      std::uint32_t secMask = col->candidateSections(typeMask);
      while (secMask != 0) {
        const int si = std::countr_zero(secMask);
        secMask &= secMask - 1u;
        if (minSectionDistSq(center, cx, sectionBaseY(si), cz) > rSq)
          continue;
        const PoiSection *sec = col->sections[si].get();
        if (!sec)
          continue;
        for (PackedPoiEntry entry : sec->entries) {
          if ((poiTypeMask(entry.type()) & typeMask) == 0 ||
              !PoiSection::matchesOccupancy(entry, occupancy))
            continue;
          const PoiPos p = worldPosFor(cx, cz, si, entry.localPos());
          if (distSq64(center, p) <= rSq && (!filter || filter(p)))
            return p;
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<PoiMatch>
PoiStorage::findClosestWithType(PoiPos center, int radius,
                                std::uint32_t typeMask, PoiOccupancy occupancy,
                                const PosFilter &filter) const {
  if (radius < 0 || columns_.empty() || typeMask == 0)
    return std::nullopt;
  const std::int64_t rSq =
      static_cast<std::int64_t>(radius) * static_cast<std::int64_t>(radius);
  const int minCx = (center.x - radius) >> 4;
  const int maxCx = (center.x + radius) >> 4;
  const int minCz = (center.z - radius) >> 4;
  const int maxCz = (center.z + radius) >> 4;
  const int originCx = center.x >> 4;
  const int originCz = center.z >> 4;
  const int ringMax = std::max(
      {originCx - minCx, maxCx - originCx, originCz - minCz, maxCz - originCz});
  const int clampedY = std::clamp(center.y, minY_, maxY_ - 1);
  const std::int64_t minWorldYDistSq =
      static_cast<std::int64_t>(center.y - clampedY) * (center.y - clampedY);

  auto &sectionHeap = scratchSections_;
  sectionHeap.clear();
  auto secGreater = [](const QueuedSection &a, const QueuedSection &b) {
    return a.minDistSq > b.minDistSq;
  };
  bool hasBest = false;
  SphereCandidate best{};

  auto drainCloserThan = [&](std::int64_t nextRingDistSq) {
    while (!sectionHeap.empty() &&
           sectionHeap.front().minDistSq <= nextRingDistSq) {
      if (hasBest && sectionHeap.front().minDistSq > best.distSq)
        return true;
      std::pop_heap(sectionHeap.begin(), sectionHeap.end(), secGreater);
      const QueuedSection q = sectionHeap.back();
      sectionHeap.pop_back();
      for (PackedPoiEntry entry : q.section->entries) {
        if ((poiTypeMask(entry.type()) & typeMask) == 0 ||
            !PoiSection::matchesOccupancy(entry, occupancy))
          continue;
        const PoiPos p =
            worldPosFor(q.chunkX, q.chunkZ, q.sectionIndex, entry.localPos());
        const std::int64_t d2 = distSq64(center, p);
        if (d2 > rSq)
          continue;
        const SphereCandidate cand{
            d2,
            q.chunkZ,
            q.chunkX,
            q.sectionIndex,
            entry.localPos(),
            PoiMatch{p, entry.type(), entry.freeTickets()}};
        if (hasBest && !sphereLess(cand, best))
          continue;
        if (!filter || filter(p)) {
          best = cand;
          hasBest = true;
        }
      }
    }
    return hasBest &&
           (sectionHeap.empty() ? nextRingDistSq : sectionHeap.front().minDistSq) >
               best.distSq;
  };

  for (int ring = 0; ring <= ringMax; ++ring) {
    const std::int64_t ringMin =
        ringLowerBoundDistSq(center, originCx, originCz, ring, minWorldYDistSq);
    if (ringMin > rSq)
      break;
    if (drainCloserThan(ringMin))
      return best.match;

    forEachChunkInRing(
        originCx, originCz, ring, minCx, maxCx, minCz, maxCz,
        [&](int cx, int cz) {
          const std::int64_t cMin =
              minChunkHorizDistSq(center, cx, cz) + minWorldYDistSq;
          if (cMin > rSq || (hasBest && cMin > best.distSq))
            return;
          const PoiChunkColumn *col = findColumn(cx, cz);
          if (!col)
            return;
          std::uint32_t secMask = col->candidateSections(typeMask);
          while (secMask != 0) {
            const int si = std::countr_zero(secMask);
            secMask &= secMask - 1u;
            const std::int64_t sMin =
                minSectionDistSq(center, cx, sectionBaseY(si), cz);
            if (sMin > rSq || (hasBest && sMin > best.distSq))
              continue;
            const PoiSection *sec = col->sections[si].get();
            if (!sec)
              continue;
            sectionHeap.push_back(QueuedSection{
                sMin, cx, cz, static_cast<std::uint8_t>(si), sec});
            std::push_heap(sectionHeap.begin(), sectionHeap.end(), secGreater);
          }
        });
  }

  drainCloserThan(std::numeric_limits<std::int64_t>::max());
  if (hasBest)
    return best.match;
  return std::nullopt;
}

std::optional<PoiPos> PoiStorage::findClosest(PoiPos center, int radius,
                                              std::uint32_t typeMask,
                                              PoiOccupancy occupancy,
                                              const PosFilter &filter) const {
  auto res =
      findClosestWithType(center, radius, typeMask, occupancy, filter);
  return res ? std::optional<PoiPos>(res->pos) : std::nullopt;
}

std::vector<PoiMatch> PoiStorage::findClosestN(PoiPos center, int radius,
                                               std::uint32_t typeMask,
                                               PoiOccupancy occupancy,
                                               std::size_t limit,
                                               const PosFilter &filter) const {
  if (limit == 0 || radius < 0 || columns_.empty() || typeMask == 0)
    return {};
  collectInRange(center, radius, typeMask, occupancy, scratchMatches_);
  std::vector<SphereCandidate> candidates;
  candidates.reserve(scratchMatches_.size());
  for (const PoiMatch &m : scratchMatches_) {
    if (filter && !filter(m.pos))
      continue;
    const int cx = m.pos.x >> 4;
    const int cz = m.pos.z >> 4;
    const int si = sectionIndexForY(m.pos.y);
    candidates.push_back(SphereCandidate{
        distSq64(center, m.pos), cz, cx, si, localPosFor(m.pos, si), m});
  }
  const std::size_t count = std::min(limit, candidates.size());
  std::partial_sort(candidates.begin(), candidates.begin() + count,
                    candidates.end(), sphereLess);
  std::vector<PoiMatch> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    out.push_back(candidates[i].match);
  return out;
}

std::optional<PoiPos>
PoiStorage::findClosestPortal(PoiPos center, int radius,
                              const PortalFilter &filter) const {
  constexpr std::uint32_t kPortalMask = poiTypeMask(PoiTypeId::NetherPortal);
  if (radius < 0 || columns_.empty())
    return std::nullopt;
  const int minCx = (center.x - radius) >> 4;
  const int maxCx = (center.x + radius) >> 4;
  const int minCz = (center.z - radius) >> 4;
  const int maxCz = (center.z + radius) >> 4;
  const int originCx = center.x >> 4;
  const int originCz = center.z >> 4;
  const int ringMax = std::max(
      {originCx - minCx, maxCx - originCx, originCz - minCz, maxCz - originCz});
  const int clampedY = std::clamp(center.y, minY_, maxY_ - 1);
  const std::int64_t minWorldYDistSq =
      static_cast<std::int64_t>(center.y - clampedY) * (center.y - clampedY);

  auto &sectionHeap = scratchSections_;
  sectionHeap.clear();
  auto secGreater = [](const QueuedSection &a, const QueuedSection &b) {
    return a.minDistSq > b.minDistSq;
  };

  if (!filter) {
    bool hasBest = false;
    PortalCandidate best{};
    auto processSectionsUpTo = [&](std::int64_t limitDistSq) -> bool {
      while (!sectionHeap.empty() &&
             sectionHeap.front().minDistSq <= limitDistSq) {
        if (hasBest && sectionHeap.front().minDistSq > best.distSq)
          return true;
        std::pop_heap(sectionHeap.begin(), sectionHeap.end(), secGreater);
        const QueuedSection q = sectionHeap.back();
        sectionHeap.pop_back();
        for (PackedPoiEntry entry : q.section->entries) {
          if (entry.type() != PoiTypeId::NetherPortal)
            continue;
          const PoiPos p =
              worldPosFor(q.chunkX, q.chunkZ, q.sectionIndex, entry.localPos());
          if (std::abs(p.x - center.x) > radius ||
              std::abs(p.z - center.z) > radius)
            continue;
          const PortalCandidate cand{
              distSq64(center, p), q.chunkZ, q.chunkX, q.sectionIndex,
              entry.localPos(), PoiMatch{p, entry.type(), entry.freeTickets()}};
          if (!hasBest || portalLess(cand, best)) {
            best = cand;
            hasBest = true;
          }
        }
      }
      return hasBest &&
             (sectionHeap.empty() ? limitDistSq
                                  : sectionHeap.front().minDistSq) >
                 best.distSq;
    };

    for (int ring = 0; ring <= ringMax; ++ring) {
      const std::int64_t ringMin = ringLowerBoundDistSq(
          center, originCx, originCz, ring, minWorldYDistSq);
      if (processSectionsUpTo(ringMin))
        return best.match.pos;
      forEachChunkInRing(
          originCx, originCz, ring, minCx, maxCx, minCz, maxCz,
          [&](int cx, int cz) {
            if (hasBest &&
                minChunkHorizDistSq(center, cx, cz) + minWorldYDistSq >
                    best.distSq)
              return;
            const PoiChunkColumn *col = findColumn(cx, cz);
            if (!col)
              return;
            std::uint32_t secMask = col->candidateSections(kPortalMask);
            while (secMask != 0) {
              const int si = std::countr_zero(secMask);
              secMask &= secMask - 1u;
              const std::int64_t sMin =
                  minSectionDistSq(center, cx, sectionBaseY(si), cz);
              if (hasBest && sMin > best.distSq)
                continue;
              const PoiSection *sec = col->sections[si].get();
              if (!sec)
                continue;
              sectionHeap.push_back(QueuedSection{
                  sMin, cx, cz, static_cast<std::uint8_t>(si), sec});
              std::push_heap(sectionHeap.begin(), sectionHeap.end(), secGreater);
            }
          });
    }
    processSectionsUpTo(std::numeric_limits<std::int64_t>::max());
    return hasBest ? std::optional<PoiPos>(best.match.pos) : std::nullopt;
  }

  // Filtered path: double min-heap guarantees lazy post-filter runs in exact
  // (distSq ASC, y ASC, ...) global order without false early termination.
  std::vector<PortalCandidate> candHeap;
  auto candGreater = [](const PortalCandidate &a, const PortalCandidate &b) {
    return portalLess(b, a);
  };

  auto flushGuaranteedPrefix =
      [&](std::int64_t nextRingDistSq) -> std::optional<PoiPos> {
    while (true) {
      const std::int64_t unexploredMin = std::min(
          nextRingDistSq,
          sectionHeap.empty() ? std::numeric_limits<std::int64_t>::max()
                              : sectionHeap.front().minDistSq);
      while (!candHeap.empty() && candHeap.front().distSq < unexploredMin) {
        std::pop_heap(candHeap.begin(), candHeap.end(), candGreater);
        const PortalCandidate c = candHeap.back();
        candHeap.pop_back();
        if (filter(c.match))
          return c.match.pos;
      }
      if (sectionHeap.empty() || sectionHeap.front().minDistSq > nextRingDistSq)
        break;
      std::pop_heap(sectionHeap.begin(), sectionHeap.end(), secGreater);
      const QueuedSection q = sectionHeap.back();
      sectionHeap.pop_back();
      for (PackedPoiEntry entry : q.section->entries) {
        if (entry.type() != PoiTypeId::NetherPortal)
          continue;
        const PoiPos p =
            worldPosFor(q.chunkX, q.chunkZ, q.sectionIndex, entry.localPos());
        if (std::abs(p.x - center.x) > radius ||
            std::abs(p.z - center.z) > radius)
          continue;
        candHeap.push_back(PortalCandidate{
            distSq64(center, p), q.chunkZ, q.chunkX, q.sectionIndex,
            entry.localPos(), PoiMatch{p, entry.type(), entry.freeTickets()}});
        std::push_heap(candHeap.begin(), candHeap.end(), candGreater);
      }
    }
    return std::nullopt;
  };

  for (int ring = 0; ring <= ringMax; ++ring) {
    const std::int64_t ringMin =
        ringLowerBoundDistSq(center, originCx, originCz, ring, minWorldYDistSq);
    if (auto hit = flushGuaranteedPrefix(ringMin))
      return hit;
    forEachChunkInRing(
        originCx, originCz, ring, minCx, maxCx, minCz, maxCz,
        [&](int cx, int cz) {
          const PoiChunkColumn *col = findColumn(cx, cz);
          if (!col)
            return;
          std::uint32_t secMask = col->candidateSections(kPortalMask);
          while (secMask != 0) {
            const int si = std::countr_zero(secMask);
            secMask &= secMask - 1u;
            const PoiSection *sec = col->sections[si].get();
            if (!sec)
              continue;
            const std::int64_t sMin =
                minSectionDistSq(center, cx, sectionBaseY(si), cz);
            sectionHeap.push_back(QueuedSection{
                sMin, cx, cz, static_cast<std::uint8_t>(si), sec});
            std::push_heap(sectionHeap.begin(), sectionHeap.end(), secGreater);
          }
        });
  }
  return flushGuaranteedPrefix(std::numeric_limits<std::int64_t>::max());
}

} // namespace redstone
