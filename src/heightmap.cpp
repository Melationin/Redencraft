#include "redstone/heightmap.hpp"

#include "redstone/fluid.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>

namespace redstone {
namespace {

bool leavesBlock(const BlockState &) {
  // TODO(tree-blocks): LeavesBlock is not in this registry subset. Imported
  // unsupported states cannot be classified without official block-class
  // metadata; do not infer that identity from a block name.
  return false;
}

bool matches(HeightmapType type, const BlockState &state) {
  switch (type) {
  case HeightmapType::WorldSurfaceWG:
  case HeightmapType::WorldSurface:
    return !state.isAir();
  case HeightmapType::OceanFloorWG:
  case HeightmapType::OceanFloor:
    return blocksMotion(state);
  case HeightmapType::MotionBlocking:
    return blocksMotion(state) || !fluidStateOf(state).isEmpty();
  case HeightmapType::MotionBlockingNoLeaves:
    return (blocksMotion(state) || !fluidStateOf(state).isEmpty()) &&
           !leavesBlock(state);
  }
  return false;
}

// ChunkHeightmaps evaluates all four final maps for one block state. Compute
// the expensive motion/fluid predicates once while preserving each map's
// independent result.
std::array<bool, 4> finalMapMatches(const BlockState &state) {
  const bool nonAir = !state.isAir();
  const bool motion = blocksMotion(state);
  const bool motionOrFluid = motion || !fluidStateOf(state).isEmpty();
  return {nonAir, motion, motionOrFluid, motionOrFluid};
}

int ceilLog2(int value) {
  if (value <= 1)
    return 0;
  return std::bit_width(static_cast<unsigned>(value - 1));
}

} // namespace

std::string_view heightmapSerializationKey(HeightmapType type) {
  switch (type) {
  case HeightmapType::WorldSurfaceWG: return "WORLD_SURFACE_WG";
  case HeightmapType::WorldSurface: return "WORLD_SURFACE";
  case HeightmapType::OceanFloorWG: return "OCEAN_FLOOR_WG";
  case HeightmapType::OceanFloor: return "OCEAN_FLOOR";
  case HeightmapType::MotionBlocking: return "MOTION_BLOCKING";
  case HeightmapType::MotionBlockingNoLeaves:
    return "MOTION_BLOCKING_NO_LEAVES";
  }
  return {};
}

HeightmapUsage heightmapUsage(HeightmapType type) {
  if (type == HeightmapType::WorldSurfaceWG || type == HeightmapType::OceanFloorWG)
    return HeightmapUsage::Worldgen;
  if (type == HeightmapType::OceanFloor) return HeightmapUsage::LiveWorld;
  return HeightmapUsage::Client;
}

bool heightmapSendToClient(HeightmapType type) {
  return heightmapUsage(type) == HeightmapUsage::Client;
}

bool heightmapKeepAfterWorldgen(HeightmapType type) {
  return heightmapUsage(type) != HeightmapUsage::Worldgen;
}

bool heightmapIsLive(HeightmapType type) {
  return heightmapKeepAfterWorldgen(type);
}

Heightmap::Heightmap(HeightmapType type, int minY, int height)
    : type_(type), minY_(minY), height_(height),
      bits_(ceilLog2(height + 1)), valuesPerLong_(bits_ ? 64 / bits_ : 64),
      mask_(bits_ == 64 ? ~0ull : ((1ull << bits_) - 1ull)) {
  if (bits_ < 1 || bits_ > 32)
    throw std::invalid_argument("heightmap bit width must be 1..32");
  const std::size_t count =
      (256u + static_cast<unsigned>(valuesPerLong_) - 1u) /
      static_cast<unsigned>(valuesPerLong_);
  data_.assign(count, 0);
}

int Heightmap::get(int valueIndex) const {
  const int cell = valueIndex / valuesPerLong_;
  const int bit = (valueIndex % valuesPerLong_) * bits_;
  return static_cast<int>((data_[static_cast<std::size_t>(cell)] >> bit) & mask_);
}

void Heightmap::set(int valueIndex, int value) {
  if (value < 0 || static_cast<unsigned>(value) > mask_)
    throw std::out_of_range("heightmap value exceeds storage width");
  const int cell = valueIndex / valuesPerLong_;
  const int bit = (valueIndex % valuesPerLong_) * bits_;
  auto &word = data_[static_cast<std::size_t>(cell)];
  word = (word & ~(mask_ << bit)) |
         ((static_cast<std::uint64_t>(value) & mask_) << bit);
}

int Heightmap::firstAvailable(int x, int z) const {
  return get(index(x & 15, z & 15)) + minY_;
}

int Heightmap::highestTaken(int x, int z) const {
  return firstAvailable(x, z) - 1;
}

void Heightmap::setHeight(int x, int z, int height) {
  set(index(x & 15, z & 15), height - minY_);
}

void Heightmap::reset() { std::fill(data_.begin(), data_.end(), 0); }

bool Heightmap::matches(const BlockState &state) const {
  return redstone::matches(type_, state);
}

bool Heightmap::update(int localX, int localY, int localZ,
                       const BlockState &state,
                       const HeightmapBlockGetter &getBlock) {
  const int first = firstAvailable(localX, localZ);
  if (localY <= first - 2)
    return false;
  if (matches(state)) {
    if (localY >= first) {
      setHeight(localX, localZ, localY + 1);
      return true;
    }
  } else if (first - 1 == localY) {
    for (int y = localY - 1; y >= minY_; --y) {
      if (matches(getBlock(localX, y, localZ))) {
        setHeight(localX, localZ, y + 1);
        return true;
      }
    }
    setHeight(localX, localZ, minY_);
    return true;
  }
  return false;
}

bool Heightmap::setRawData(const std::vector<std::uint64_t> &data, int maxY,
                           const HeightmapBlockGetter &getBlock) {
  if (data.size() == data_.size()) {
    data_ = data;
    return true;
  }
  reset();
  for (int x = 0; x < 16; ++x) {
    for (int z = 0; z < 16; ++z) {
      for (int y = maxY - 1; y >= minY_; --y) {
        if (matches(getBlock(x, y, z))) {
          setHeight(x, z, y + 1);
          break;
        }
      }
    }
  }
  return false;
}

ChunkHeightmaps::ChunkHeightmaps(int minY, int height)
    : maps_{Heightmap(HeightmapType::WorldSurface, minY, height),
            Heightmap(HeightmapType::OceanFloor, minY, height),
            Heightmap(HeightmapType::MotionBlocking, minY, height),
            Heightmap(HeightmapType::MotionBlockingNoLeaves, minY, height)} {
  minFirstAvailable_.fill(minY);
}

Heightmap *ChunkHeightmaps::find(HeightmapType type) {
  for (Heightmap &map : maps_)
    if (map.type() == type)
      return &map;
  return nullptr;
}

const Heightmap *ChunkHeightmaps::find(HeightmapType type) const {
  for (const Heightmap &map : maps_)
    if (map.type() == type)
      return &map;
  return nullptr;
}

Heightmap &ChunkHeightmaps::get(HeightmapType type) {
  Heightmap *map = find(type);
  if (!map)
    throw std::out_of_range("heightmap is not present on a final chunk");
  return *map;
}

const Heightmap &ChunkHeightmaps::get(HeightmapType type) const {
  const Heightmap *map = find(type);
  if (!map)
    throw std::out_of_range("heightmap is not present on a final chunk");
  return *map;
}

void ChunkHeightmaps::prime(int maxY, const HeightmapBlockGetter &getBlock) {
  for (Heightmap &map : maps_)
    map.reset();
  minFirstAvailable_.fill(maps_.front().minY());
  for (int x = 0; x < 16; ++x) {
    for (int z = 0; z < 16; ++z) {
      std::array<bool, 4> pending{true, true, true, true};
      pending.fill(true);
      for (int y = maxY - 1; y >= maps_.front().minY(); --y) {
        const BlockState state = getBlock(x, y, z);
        if (state.is(Blocks::AIR))
          continue;
        const auto stateMatches = finalMapMatches(state);
        for (std::size_t i = 0; i < maps_.size(); ++i) {
          if (pending[i] && stateMatches[i]) {
            maps_[i].setHeight(x, z, y + 1);
            pending[i] = false;
          }
        }
        if (std::none_of(pending.begin(), pending.end(),
                         [](bool value) { return value; }))
          break;
      }
      int minFirst = std::numeric_limits<int>::max();
      for (const Heightmap &map : maps_)
        minFirst = std::min(minFirst, map.firstAvailable(x, z));
      minFirstAvailable_[Heightmap::index(x, z)] = minFirst;
    }
  }
}

bool ChunkHeightmaps::update(int localX, int localY, int localZ,
                             const BlockState &state,
                             const HeightmapBlockGetter &getBlock) {
  const int column = Heightmap::index(localX, localZ);
  // This filter is the union of the four independent Heightmap.update
  // short-circuits. Most deep piston/redstone writes stop here after one array
  // load instead of probing four packed heights and block predicates.
  if (localY <= minFirstAvailable_[column] - 2)
    return false;

  // Match LevelChunk.setBlockState's four calls while sharing the new state's
  // predicate evaluation. The maps remain independent; only the downward read
  // for columns in the same write is merged.
  static constexpr std::array<std::size_t, 4> updateOrder{2, 3, 1, 0};
  const auto stateMatches = finalMapMatches(state);
  std::array<bool, 4> rescan{};
  bool changed = false;
  for (std::size_t mapIndex : updateOrder) {
    Heightmap &map = maps_[mapIndex];
    const int first = map.firstAvailable(localX, localZ);
    if (localY <= first - 2)
      continue;
    if (stateMatches[mapIndex]) {
      if (localY >= first) {
        map.setHeight(localX, localZ, localY + 1);
        changed = true;
      }
    } else if (first - 1 == localY) {
      rescan[mapIndex] = true;
    }
  }

  if (std::any_of(rescan.begin(), rescan.end(),
                  [](bool value) { return value; })) {
    const int minY = maps_.front().minY();
    for (int y = localY - 1; y >= minY; --y) {
      const BlockState below = getBlock(localX, y, localZ);
      const auto belowMatches = finalMapMatches(below);
      for (std::size_t i = 0; i < maps_.size(); ++i) {
        if (rescan[i] && belowMatches[i]) {
          maps_[i].setHeight(localX, localZ, y + 1);
          rescan[i] = false;
          changed = true;
        }
      }
      if (std::none_of(rescan.begin(), rescan.end(),
                       [](bool value) { return value; }))
        break;
    }
    for (std::size_t i = 0; i < maps_.size(); ++i) {
      if (rescan[i]) {
        maps_[i].setHeight(localX, localZ, minY);
        changed = true;
      }
    }
  }

  if (changed) {
    int minFirst = std::numeric_limits<int>::max();
    for (const Heightmap &map : maps_)
      minFirst = std::min(minFirst, map.firstAvailable(localX, localZ));
    minFirstAvailable_[column] = minFirst;
  }
  return changed;
}

} // namespace redstone
