#pragma once
// 对齐 Minecraft Java Edition 26.1 的 `NoiseBasedChunkGenerator.java`：
// 将 `NoiseChunk` 缓存气候采样器、`Aquifer` 含水层、`OreVeinifier` 巨型矿脉、
// 三线性噪声网格插值与 `SurfaceSystem` 地表规则串联为可按世界种子驱动的 `ChunkGenerator` 实现。

#include "redstone/worldgen/biome_source.hpp"
#include "redstone/worldgen/chunk_generator.hpp"
#include "redstone/worldgen/feature.hpp"
#include "redstone/worldgen/noise_chunk.hpp"
#include "redstone/worldgen/structure.hpp"
#include "redstone/worldgen/surface_rules.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace redstone::worldgen {

class NoiseBasedChunkGenerator final : public ChunkGenerator {
public:
  NoiseBasedChunkGenerator(const WorldgenRegistries &registries,
                           NoiseGeneratorSettings settings,
                           std::shared_ptr<const BiomeSource> biomeSource,
                           int levelMinY = std::numeric_limits<int>::min(),
                           int levelHeight = 0);

  ~NoiseBasedChunkGenerator() override;

  int minY() const noexcept override { return settings_.minY; }
  int height() const noexcept override { return settings_.height; }
  int chunkMinY() const noexcept override { return levelMinY_; }
  int chunkHeight() const noexcept override { return levelHeight_; }

  const NoiseGeneratorSettings &settings() const noexcept { return settings_; }
  const BiomeSource &biomeSource() const noexcept { return *biomeSource_; }
  const WorldgenRegistries &registries() const noexcept { return registries_; }

  static FluidPicker createFluidPicker(const NoiseGeneratorSettings &settings);

  RandomState &randomStateForSeed(std::int64_t worldSeed) const;
  ChunkGeneratorStructureState &
  structureStateForSeed(std::int64_t worldSeed) const;

  // 基准/测试用：关闭同心圆结构（当前仅要塞）。默认开启，正常生成路径不受影响。
  void setConcentricRingsEnabled(bool enabled);

  void createStructures(ProtoChunk &chunk, std::int64_t worldSeed) override;
  void createReferences(WorldGenRegion &region, ProtoChunk &chunk) override;
  void createBiomes(WorldGenRegion &region, ProtoChunk &chunk,
                    std::int64_t worldSeed) override;
  void fillFromNoise(WorldGenRegion &region, ProtoChunk &chunk,
                     std::int64_t worldSeed) override;
  void buildSurface(WorldGenRegion &region, ProtoChunk &chunk,
                    std::int64_t worldSeed) override;
  void applyCarvers(WorldGenRegion &region, ProtoChunk &chunk,
                    std::int64_t worldSeed) override;
  void applyBiomeDecoration(WorldGenRegion &region, ProtoChunk &chunk,
                            std::int64_t worldSeed) override;

  // 按 26.1 `getBaseHeight` / `iterateNoiseColumn`（单个 1×1 cell 列的 `NoiseChunk::forColumn`）
  // 查询指定 `(x, z)` 处满足高度图谓词的最大 `firstAvailable` Y 值（即最高命中方块 y+1，未命中返回 `minY`）。
  int getBaseHeight(int x, int z, HeightmapType type,
                    std::int64_t worldSeed) const;

private:
  std::shared_ptr<NoiseChunk>
  getOrCreateNoiseChunk(ChunkAccess &chunk, RandomState &rs,
                        const WorldGenRegion *region = nullptr,
                        bool requireTerrain = true) const;

  // applyCarvers 的源区块群系只依赖 (worldSeed, chunkX, chunkZ)，同一源区块会被
  // 最多 289 个目标区块重复采样；按 32 分片做有界 memoization。
  struct CarverBiomeKey {
    std::int64_t seed = 0;
    std::int32_t x = 0;
    std::int32_t z = 0;
    bool operator==(const CarverBiomeKey &) const = default;
  };
  struct CarverBiomeKeyHash {
    std::size_t operator()(const CarverBiomeKey &key) const noexcept;
  };
  struct CarverBiomeShard {
    mutable std::mutex mutex;
    std::unordered_map<CarverBiomeKey, std::uint16_t, CarverBiomeKeyHash> map;
  };
  std::string_view carverBiomeAt(ChunkTicketPos pos, std::int64_t worldSeed,
                                 RandomState &rs) const;

  const WorldgenRegistries &registries_;
  NoiseGeneratorSettings settings_;
  std::shared_ptr<const BiomeSource> biomeSource_;
  FluidPicker globalFluidPicker_;
  mutable std::mutex mutex_;
  mutable std::unordered_map<std::int64_t, std::unique_ptr<RandomState>>
      randomStates_;
  mutable std::unordered_map<std::int64_t,
                             std::unique_ptr<ChunkGeneratorStructureState>>
      structureStates_;
  bool concentricRingsEnabled_ = true;
  int levelMinY_ = 0;
  int levelHeight_ = 0;
  mutable std::array<CarverBiomeShard, 32> carverBiomeCache_;
  std::vector<StepFeatureData> featuresPerStep_;
};

} // namespace redstone::worldgen
