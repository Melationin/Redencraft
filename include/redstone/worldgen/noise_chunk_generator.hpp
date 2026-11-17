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

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace redstone::worldgen {

class NoiseBasedChunkGenerator final : public ChunkGenerator {
public:
  NoiseBasedChunkGenerator(const WorldgenRegistries &registries,
                           NoiseGeneratorSettings settings,
                           std::shared_ptr<const BiomeSource> biomeSource);

  ~NoiseBasedChunkGenerator() override;

  int minY() const noexcept override { return settings_.minY; }
  int height() const noexcept override { return settings_.height; }

  const NoiseGeneratorSettings &settings() const noexcept { return settings_; }
  const BiomeSource &biomeSource() const noexcept { return *biomeSource_; }
  const WorldgenRegistries &registries() const noexcept { return registries_; }

  static FluidPicker createFluidPicker(const NoiseGeneratorSettings &settings);

  RandomState &randomStateForSeed(std::int64_t worldSeed) const;
  ChunkGeneratorStructureState &
  structureStateForSeed(std::int64_t worldSeed) const;

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
  std::vector<StepFeatureData> featuresPerStep_;
};

} // namespace redstone::worldgen
