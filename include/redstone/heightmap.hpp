#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "redstone/block.hpp"

namespace redstone {

enum class HeightmapType : std::uint8_t {
  WorldSurfaceWG = 0,
  WorldSurface = 1,
  OceanFloorWG = 2,
  OceanFloor = 3,
  MotionBlocking = 4,
  MotionBlockingNoLeaves = 5,
};

enum class HeightmapUsage : std::uint8_t { Worldgen, LiveWorld, Client };

std::string_view heightmapSerializationKey(HeightmapType type);
HeightmapUsage heightmapUsage(HeightmapType type);
bool heightmapSendToClient(HeightmapType type);
bool heightmapKeepAfterWorldgen(HeightmapType type);
bool heightmapIsLive(HeightmapType type);

using HeightmapBlockGetter = std::function<BlockState(int, int, int)>;
// One downward column scan shared by all requested FINAL_HEIGHTMAPS. The
// returned array contains each map's first-available height, or minY when no
// matching block exists. Section layout and direct storage reads remain in the
// world; the four predicates remain in ChunkHeightmaps.
using HeightmapColumnScanner = std::function<std::array<int, 4>(
    int localX, int startY, int localZ, const std::array<bool, 4> &needed)>;

// Heightmap.java's final-heightmap BitStorage: 256 column heights packed with
// ceil(log2(height + 1)) bits in little-endian fields. Heights are relative to
// minY in storage and exposed as absolute first-available coordinates.
class Heightmap {
public:
  Heightmap(HeightmapType type, int minY, int height);

  HeightmapType type() const { return type_; }
  int minY() const { return minY_; }
  int bits() const { return bits_; }
  int firstAvailable(int x, int z) const;
  int highestTaken(int x, int z) const;
  int height(int x, int z) const { return highestTaken(x, z); }
  const std::vector<std::uint64_t> &rawData() const { return data_; }

  // Heightmap.update: localY is the absolute block Y. The getter returns the
  // current world state for the same chunk column.
  bool update(int localX, int localY, int localZ, const BlockState &state,
              const HeightmapBlockGetter &getBlock);

  // ChunkAccess.setHeightmap: length mismatch discards data and primes.
  bool setRawData(const std::vector<std::uint64_t> &data, int maxY,
                  const HeightmapBlockGetter &getBlock);

  bool matches(const BlockState &state) const;

private:
  friend class ChunkHeightmaps;
  int get(int index) const;
  void set(int index, int value);
  void setHeight(int x, int z, int height);
  void reset();
  static int index(int x, int z) { return x + z * 16; }

  HeightmapType type_;
  int minY_;
  int height_;
  int bits_;
  int valuesPerLong_;
  std::uint64_t mask_;
  std::vector<std::uint64_t> data_;
};

// LevelChunk creates the four ChunkStatus.FINAL_HEIGHTMAPS in enum order.
class ChunkHeightmaps {
public:
  ChunkHeightmaps(int minY, int height);

  Heightmap *find(HeightmapType type);
  const Heightmap *find(HeightmapType type) const;
  Heightmap &get(HeightmapType type);
  const Heightmap &get(HeightmapType type) const;

  void prime(int maxY, const HeightmapBlockGetter &getBlock);
  bool update(int localX, int localY, int localZ, const BlockState &state,
              const HeightmapBlockGetter &getBlock);
  bool updateFromColumn(int localX, int localY, int localZ,
                        const BlockState &state,
                        const HeightmapColumnScanner &scanColumn);
  static std::array<bool, 4> matchesFinalMaps(const BlockState &state);

private:
  bool applyUpdate(int localX, int localY, int localZ,
                   const BlockState &state, std::array<bool, 4> &rescan);
  void refreshMinimum(int localX, int localZ);
  std::array<Heightmap, 4> maps_;
  // Fast negative filter for LevelChunk.setBlockState. The minimum across all
  // four maps is conservative: a table with a lower height may still rise even
  // when another table's surface is higher.
  std::array<int, 256> minFirstAvailable_{};
};

} // namespace redstone
