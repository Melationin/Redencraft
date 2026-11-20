#pragma once
// 对齐 Minecraft Java Edition 26.1 的 `VerticalAnchor`、`SurfaceRules` 状态机与 `SurfaceSystem`。
// 用途：在 `ChunkStatus::Noise` 填好基岩/石头/流体骨架后，于 `ChunkStatus::Surface` 阶段执行：
// 1. 侵蚀恶地陶瓦柱延伸（`erodedBadlandsExtension`）；
// 2. 自顶向下扫描每列 `stoneAboveDepth`/`stoneBelowDepth`/`waterHeight` 并求值 `surface_rule`（基岩渐变、深板岩过渡、草方块/泥土/沙子/恶地色带等）；
// 3. 冰冻海洋冰山延伸（`frozenOceanExtension`）。

#include "redstone/block.hpp"
#include "redstone/worldgen/biome_source.hpp"
#include "redstone/worldgen/noise.hpp"
#include "redstone/worldgen/noise_chunk.hpp"
#include "redstone/worldgen/proto_chunk.hpp"
#include "redstone/worldgen/value_providers.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace redstone::worldgen {

class WorldgenRegistries;
class RandomState;

struct SurfaceConditionNode;
struct SurfaceRuleNode;
using SurfaceConditionSourcePtr = std::shared_ptr<const SurfaceConditionNode>;
using SurfaceRuleSourcePtr = std::shared_ptr<const SurfaceRuleNode>;

enum class SurfaceConditionKind : std::uint8_t {
  Biome = 0,
  NoiseThreshold,
  VerticalGradient,
  YAbove,
  Water,
  Temperature,
  Steep,
  Not,
  Hole,
  AbovePreliminarySurface,
  StoneDepth,
};

struct SurfaceConditionNode {
  SurfaceConditionKind kind = SurfaceConditionKind::Hole;
  std::vector<std::string> biomes;
  std::string noiseId;
  double minThreshold = 0.0;
  double maxThreshold = 0.0;
  std::string randomName;
  VerticalAnchor anchor;
  VerticalAnchor trueAtAndBelow;
  VerticalAnchor falseAtAndAbove;
  int offset = 0;
  int surfaceDepthMultiplier = 0;
  bool addStoneDepth = false;
  bool addSurfaceDepth = false;
  int secondaryDepthRange = 0;
  bool ceiling = false;
  SurfaceConditionSourcePtr target;
};

enum class SurfaceRuleKind : std::uint8_t {
  Bandlands = 0,
  Block,
  Sequence,
  Condition,
};

struct SurfaceRuleNode {
  SurfaceRuleKind kind = SurfaceRuleKind::Block;
  BlockState blockState;
  std::vector<SurfaceRuleSourcePtr> sequence;
  SurfaceConditionSourcePtr ifTrue;
  SurfaceRuleSourcePtr thenRun;
};

namespace surface_rules {

SurfaceConditionSourcePtr biome(std::vector<std::string> biomes);
SurfaceConditionSourcePtr noiseThreshold(std::string noiseId, double minThresh,
                                         double maxThresh);
SurfaceConditionSourcePtr verticalGradient(std::string randomName,
                                           VerticalAnchor trueAtAndBelow,
                                           VerticalAnchor falseAtAndAbove);
SurfaceConditionSourcePtr yAbove(VerticalAnchor anchor,
                                 int surfaceDepthMultiplier, bool addStoneDepth);
SurfaceConditionSourcePtr water(int offset, int surfaceDepthMultiplier,
                                bool addStoneDepth);
SurfaceConditionSourcePtr temperature();
SurfaceConditionSourcePtr steep();
SurfaceConditionSourcePtr hole();
SurfaceConditionSourcePtr abovePreliminarySurface();
SurfaceConditionSourcePtr stoneDepth(int offset, bool addSurfaceDepth,
                                     int secondaryDepthRange, bool ceiling);
SurfaceConditionSourcePtr invert(SurfaceConditionSourcePtr target);

SurfaceRuleSourcePtr bandlands();
SurfaceRuleSourcePtr block(BlockState state);
SurfaceRuleSourcePtr sequence(std::vector<SurfaceRuleSourcePtr> rules);
SurfaceRuleSourcePtr condition(SurfaceConditionSourcePtr ifTrue,
                               SurfaceRuleSourcePtr thenRun);

} // namespace surface_rules

// 对齐 26.1 `SurfaceSystem.java`（含恶地 192 层陶瓦色带、冰山/恶地延伸与列状态机）。
class SurfaceSystem {
public:
  SurfaceSystem(RandomState &randomState, BlockState defaultBlock, int seaLevel,
                const PositionalRandomFactory &noiseRandom);

  void buildSurface(RandomState &randomState, const BiomeManager &biomeManager,
                    const BiomeManager::NoiseBiomeReader &noiseBiomeReader,
                    const WorldgenRegistries &registries, bool useLegacyRandom,
                    int genMinY, int genDepth, ProtoChunk &chunk,
                    NoiseChunk &noiseChunk,
                    const SurfaceRuleSourcePtr &ruleSource) const;

  std::optional<BlockState>
  topMaterial(RandomState &randomState, const BiomeManager &biomeManager,
              const BiomeManager::NoiseBiomeReader &noiseBiomeReader,
              const WorldgenRegistries &registries, int genMinY, int genDepth,
              ProtoChunk &chunk, NoiseChunk &noiseChunk,
              const SurfaceRuleSourcePtr &ruleSource, int blockX, int blockY,
              int blockZ, bool underFluid) const;

  int seaLevel() const noexcept { return seaLevel_; }
  const BlockState &defaultBlock() const noexcept { return defaultBlock_; }
  int getSurfaceDepth(int blockX, int blockZ) const;
  double getSurfaceSecondary(int blockX, int blockZ) const;
  BlockState getBand(int worldX, int y, int worldZ) const;

private:
  struct PreparedProgram;
  struct ProgramCacheKey {
    const SurfaceRuleNode *rule = nullptr;
    const WorldgenRegistries *registries = nullptr;
    int genMinY = 0;
    int genDepth = 0;
    bool operator==(const ProgramCacheKey &) const = default;
  };
  struct CachedProgramEntry {
    ProgramCacheKey key;
    std::shared_ptr<const PreparedProgram> program;
  };

  std::shared_ptr<const PreparedProgram>
  getOrCompileProgram(RandomState &randomState,
                      const WorldgenRegistries &registries, int genMinY,
                      int genDepth,
                      const SurfaceRuleSourcePtr &ruleSource) const;

  void erodedBadlandsExtension(ProtoChunk &chunk, int blockX, int blockZ,
                               int height) const;
  void frozenOceanExtension(int minSurfaceLevel,
                            const BiomeClimateProperties &biomeClimate,
                            ProtoChunk &chunk, int blockX, int blockZ,
                            int height) const;

  BlockState defaultBlock_;
  int seaLevel_ = 63;
  std::array<BlockState, 192> clayBands_{};
  std::shared_ptr<const NormalNoise> clayBandsOffsetNoise_;
  std::shared_ptr<const NormalNoise> badlandsPillarNoise_;
  std::shared_ptr<const NormalNoise> badlandsPillarRoofNoise_;
  std::shared_ptr<const NormalNoise> badlandsSurfaceNoise_;
  std::shared_ptr<const NormalNoise> icebergPillarNoise_;
  std::shared_ptr<const NormalNoise> icebergPillarRoofNoise_;
  std::shared_ptr<const NormalNoise> icebergSurfaceNoise_;
  PositionalRandomFactory noiseRandom_;
  std::shared_ptr<const NormalNoise> surfaceNoise_;
  std::shared_ptr<const NormalNoise> surfaceSecondaryNoise_;
  mutable std::unique_ptr<std::mutex> programMutex_;
  mutable std::vector<CachedProgramEntry> cachedPrograms_;
};

} // namespace redstone::worldgen
