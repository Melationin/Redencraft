#pragma once
// 区块高度图（`Heightmap`）与终态高度图集合（`ChunkHeightmaps`），对齐原版 26.1 `Heightmap.java`。
// 用于维护每列最高命中方块的缓存，支撑光照、天气、生成与服务端方块放置的 O(1) 高度查询。

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "redstone/block.hpp"

namespace redstone {

namespace worldgen {
class ProtoChunk;
}

// 原版高度图类型枚举：区分世界生成期（*WG）与实时世界终态高度图。
enum class HeightmapType : std::uint8_t {
  WorldSurfaceWG = 0,
  WorldSurface = 1,
  OceanFloorWG = 2,
  OceanFloor = 3,
  MotionBlocking = 4,
  MotionBlockingNoLeaves = 5,
};

// 高度图使用阶段分类：仅世界生成期使用、实时世界保留、或同步给客户端。
enum class HeightmapUsage : std::uint8_t { Worldgen, LiveWorld, Client };

std::string_view heightmapSerializationKey(HeightmapType type);
HeightmapUsage heightmapUsage(HeightmapType type);
bool heightmapSendToClient(HeightmapType type);
bool heightmapKeepAfterWorldgen(HeightmapType type);
bool heightmapIsLive(HeightmapType type);
bool heightmapMatches(HeightmapType type, const BlockState &state);

using HeightmapBlockGetter = std::function<BlockState(int, int, int)>;
// 由所有需要重扫的 `FINAL_HEIGHTMAPS` 共享的単遍向下柱面扫描回调。
// 当多张高度图同时失效时，通过一次区段直读即可算出各表的 `firstAvailable`（无匹配方块时返回 `minY`），
// 避免每张高度图重复遍历区段；区段布局与直读逻辑留在 `World`，四组方块谓词留在 `ChunkHeightmaps`。
using HeightmapColumnScanner = std::function<std::array<int, 4>(
    int localX, int startY, int localZ, const std::array<bool, 4> &needed)>;

// 对应 `Heightmap.java` 的终态高度图 `BitStorage`：将 16x16=256 列高度按每列
// `ceil(log2(height + 1))` 位打包到 64 位字的小端位域中（每个字独立存放整数个高度，不跨字）。
// 内部存储相对于 `minY` 的非负偏移，对外接口则暴露世界绝对坐标（`firstAvailable` / `highestTaken`）。
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

  // 对应 `Heightmap.update`：`localY` 为世界绝对 Y 坐标，`getBlock` 用于移除最高方块时向下回扫当前列。
  bool update(int localX, int localY, int localZ, const BlockState &state,
              const HeightmapBlockGetter &getBlock);
  // 仅当已知新方块单调升高时直接抬升 `firstAvailable`，供 `fillFromNoise` 列扫描批量写回。
  bool raiseHeight(int x, int z, int firstAvailableY);

  // 对应 `ChunkAccess.setHeightmap`：若存档数组长度与当前位数/维度高度不匹配，则丢弃旧数据并重新全列扫描初始化。
  bool setRawData(const std::vector<std::uint64_t> &data, int maxY,
                  const HeightmapBlockGetter &getBlock);

  bool matches(const BlockState &state) const;

private:
  friend class ChunkHeightmaps;
  friend class worldgen::ProtoChunk;
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

// `LevelChunk` 持有的四张 `ChunkStatus.FINAL_HEIGHTMAPS`（按枚举顺序存储），
// 统一协调单次方块写入对四张实时高度图的增量更新与联合向下扫描。
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
  // `LevelChunk.setBlockState` 热路径上的快速负向过滤表，记录每列四张高度图的最小 `firstAvailable`。
  // 采用保守下界的原因：即使某张表表面更高，另一张高度较低的表仍可能因放置新方块而被抬高；
  // 只有当修改高度严格低于四张表的最小值时，才可确保无需更新任何高度图。
  std::array<int, 256> minFirstAvailable_{};
};

} // namespace redstone
