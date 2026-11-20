#pragma once
// 对齐 Minecraft Java Edition 26.1 的 `NoiseChunk.java`：
// 为单个区块封装噪声插值网格（`NoiseInterpolator` 三线性插值切片）、
// `FlatCache` / `Cache2D` / `CacheOnce` / `CacheAllInCell` 缓存替换节点、
// 初始地表高度缓存 `preliminarySurfaceLevel`，以及连接含水层 `Aquifer` 与巨型矿脉 `OreVeinifier` 的方块状态规则链。

#include "redstone/chunk_lifecycle.hpp"
#include "redstone/flat_hash_map.hpp"
#include "redstone/worldgen/aquifer.hpp"
#include "redstone/worldgen/noise_router.hpp"
#include "redstone/worldgen/proto_chunk.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace redstone::worldgen {

class NoiseChunk final : public DensityContextProvider {
public:
  static std::unique_ptr<NoiseChunk>
  forChunk(const BaseChunk &chunk, RandomState &randomState,
           const NoiseGeneratorSettings &settings,
           FluidPicker globalFluidPicker, DensityNodePtr beardifier = {});

  static std::unique_ptr<NoiseChunk>
  forChunkBiomes(const BaseChunk &chunk, RandomState &randomState,
                 const NoiseGeneratorSettings &settings);

  NoiseChunk(int cellCountXZ, RandomState &randomState, int firstBlockX,
             int firstBlockZ, int minY, int height, int sizeHorizontal,
             int sizeVertical, const NoiseGeneratorSettings &settings,
             FluidPicker globalFluidPicker, DensityNodePtr beardifier = {},
             bool initTerrain = true, bool isColumn = false);

  bool isTerrainInitialized() const noexcept { return terrainInitialized_; }
  void ensureTerrainInitialized(RandomState &randomState,
                                const NoiseGeneratorSettings &settings,
                                FluidPicker globalFluidPicker,
                                DensityNodePtr beardifier = {});

  ~NoiseChunk() override = default;
  NoiseChunk(const NoiseChunk &) = delete;
  NoiseChunk &operator=(const NoiseChunk &) = delete;

  // 为 `ChunkStatus::Biomes` 返回经本区块 `FlatCache` / `Cache2D` 包装的气候采样器。
  ClimateSampler cachedClimateSampler(
      const NoiseRouter &router,
      std::vector<ClimateParameterPoint> spawnTarget);

  void initializeForFirstCellX();
  void advanceCellX(int cellXIndex);
  void selectCellYZ(int cellYIndex, int cellZIndex);
  void updateForY(int posY, double factorY);
  void updateForX(int posX, double factorX);
  void updateForZ(int posZ, double factorZ);
  void swapSlices();
  void stopInterpolation();

  std::optional<BlockState> getInterpolatedState();
  double getInterpolatedDensity();

  int preliminarySurfaceLevel(int sampleX, int sampleZ);
  int maxPreliminarySurfaceLevel(int minBlockX, int minBlockZ, int maxBlockX,
                                 int maxBlockZ);

  Aquifer &aquifer() noexcept { return *aquifer_; }
  const Aquifer &aquifer() const noexcept { return *aquifer_; }
  int cellWidth() const noexcept { return cellWidth_; }
  int cellHeight() const noexcept { return cellHeight_; }
  int cellCountXZ() const noexcept { return cellCountXZ_; }
  int cellCountY() const noexcept { return cellCountY_; }
  int cellNoiseMinY() const noexcept { return cellNoiseMinY_; }
  bool hasPostDensityInterpolators() const noexcept {
    return hasPostDensityInterpolators_;
  }
  bool hasDirectFinalCellCache() const noexcept {
    return finalCellCache_ != nullptr;
  }
  std::span<const double> directFinalCellValues() const noexcept;
  void advanceVoxelCounter() noexcept { ++interpolationCounter_; }
  // 仅在 fastCell（直接读 CacheAllInCell 数组）路径下调用：按需把矿脉插值器
  // 推进到当前体素。非 fastCell 路径仍由 updateForY/X/Z 逐体素更新。
  void prepareVeinInterpolatorsFor(int x, int y, int z);
  std::optional<BlockState> computeSubstanceFast(int x, int y, int z,
                                                 double density);

  // `DensityContextProvider`（单元内全量填充，供 `CacheAllInCell` 调用）
  DensityFunctionContext forIndex(int cellIndex) override;
  void fillAllDirectly(double *output, std::size_t count,
                       const DensityNode &node) override;
  const void *noiseChunkToken() const noexcept override { return this; }

  DensityFunctionContext currentContext() const noexcept {
    return DensityFunctionContext{
        cellStartBlockX_ + inCellX_,
        cellStartBlockY_ + inCellY_,
        cellStartBlockZ_ + inCellZ_,
        this,
    };
  }

  DensityNodePtr wrap(const DensityNodePtr &function);

  struct FlatCacheState;
  struct Cache2DState;
  struct CacheOnceState;
  struct CacheAllInCellState;
  struct NoiseInterpolatorState;

private:
  class SliceContextProvider final : public DensityContextProvider {
  public:
    explicit SliceContextProvider(NoiseChunk &owner) : owner_(owner) {}
    DensityFunctionContext forIndex(int cellYIndex) override;
    void fillAllDirectly(double *output, std::size_t count,
                         const DensityNode &node) override;

  private:
    NoiseChunk &owner_;
  };

  struct NodePtrHash {
    std::uint64_t operator()(const DensityNode *p) const noexcept {
      std::uint64_t x =
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
      x ^= x >> 33;
      x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33;
      return x;
    }
  };
  struct Int64Hash {
    std::uint64_t operator()(std::int64_t v) const noexcept {
      std::uint64_t x = static_cast<std::uint64_t>(v);
      x ^= x >> 33;
      x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33;
      return x;
    }
  };

  void fillSlice(bool slice0, int cellX);
  DensityNodePtr wrapRec(const DensityNodePtr &node);

  int cellCountXZ_ = 0;
  int cellCountY_ = 0;
  int cellNoiseMinY_ = 0;
  int firstCellX_ = 0;
  int firstCellZ_ = 0;
  int firstNoiseX_ = 0;
  int firstNoiseZ_ = 0;
  int noiseSizeXZ_ = 0;
  int cellWidth_ = 4;
  int cellHeight_ = 8;
  int cellWidthShift_ = -1;
  int cellWidthMask_ = 0;
  int minY_ = 0;
  int height_ = 0;

  bool terrainInitialized_ = false;
  bool isColumn_ = false;
  bool hasPostDensityInterpolators_ = false;
  std::size_t postDensityInterpStart_ = 0; // eager 区间下界
  std::size_t eagerPostInterpEnd_ = 0;     // eager 区间上界 = 矿脉插值器起点
  std::size_t veinInterpStart_ = 0;        // 矿脉插值器 [veinInterpStart_, veinInterpEnd_)
  std::size_t veinInterpEnd_ = 0;

  bool interpolating_ = false;
  bool fillingCell_ = false;
  int cellStartBlockX_ = 0;
  int cellStartBlockY_ = 0;
  int cellStartBlockZ_ = 0;
  int inCellX_ = 0;
  int inCellY_ = 0;
  int inCellZ_ = 0;
  std::uint64_t interpolationCounter_ = 0;
  std::uint64_t arrayInterpolationCounter_ = 0;
  std::size_t arrayIndex_ = 0;

  SliceContextProvider sliceFillingContextProvider_;
  std::vector<std::shared_ptr<NoiseInterpolatorState>> interpolators_;
  std::vector<std::shared_ptr<CacheAllInCellState>> cellCaches_;
  CacheAllInCellState *finalCellCache_ = nullptr;
  FlatHashMap<const DensityNode *, DensityNodePtr, NodePtrHash> wrapped_;
  // 直读列缓存：键为量化列 (x,z)。不同调用方（含水层网格、地表四角）会查询到
  // 区块外较远的列，键数量不可控，保留哈希表。
  FlatHashMap<std::int64_t, int, Int64Hash> preliminarySurfaceLevelCache_;

  DensityNodePtr preliminarySurfaceLevel_;
  DensityNodePtr fullNoiseDensity_;
  DensityNodePtr beardifier_;
  std::unique_ptr<Aquifer> aquifer_;
  BlockStateFiller oreVeinifier_;
};

} // namespace redstone::worldgen
