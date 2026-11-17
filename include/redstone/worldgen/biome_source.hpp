#pragma once
// 对齐 Minecraft Java Edition 26.1 的生物群系源体系（`BiomeSource`、`MultiNoiseBiomeSource`、
// `OverworldBiomeBuilder`、`TheEndBiomeSource`、`FixedBiomeSource` 与 `BiomeManager`）。
// 之所以将其独立分层于 `NoiseRouter` 之上，是为了让区块生成 `BIOMES` 阶段按 4×4×4 Quart 填充
// `ProtoChunk` 调色板，同时也让运行期世界通过 `BiomeManager` 的 3D 抖动 LCG 插值
// 将块坐标平滑映射回原版生物群系，而不破坏两者各自的缓存语义。

#include "redstone/worldgen/climate.hpp"
#include "redstone/worldgen/noise_router.hpp"
#include "redstone/worldgen/proto_chunk.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace redstone::worldgen {

struct BiomeSearchResult {
  Pos pos;
  std::string biomeId;
};

// 所有维度共享的生物群系源只读接口：输入 Quart 坐标与 `ClimateSampler`，输出生物群系资源 ID。
class BiomeSource {
public:
  virtual ~BiomeSource() = default;

  virtual std::vector<std::string> possibleBiomes() const = 0;
  virtual std::string_view getNoiseBiome(int quartX, int quartY, int quartZ,
                                         const ClimateSampler &sampler) const = 0;

  std::optional<BiomeSearchResult> findBiomeHorizontal(
      int originX, int originY, int originZ, int searchRadius,
      const std::function<bool(std::string_view)> &allowed, RandomSource &rng,
      const ClimateSampler &sampler) const;

  std::optional<BiomeSearchResult> findBiomeHorizontal(
      int originX, int originY, int originZ, int searchRadius, int skipSteps,
      const std::function<bool(std::string_view)> &allowed, RandomSource &rng,
      bool findClosest, const ClimateSampler &sampler) const;
};

// 单一固定生物群系源（`FixedBiomeSource.java`），常用于超平坦或特定隔离夹具。
class FixedBiomeSource final : public BiomeSource {
public:
  explicit FixedBiomeSource(std::string biomeId);

  std::vector<std::string> possibleBiomes() const override;
  std::string_view getNoiseBiome(int quartX, int quartY, int quartZ,
                                 const ClimateSampler &sampler) const override;

private:
  std::string biomeId_;
};

// 26.1 主世界完整 6D 生物群系参数表构建器（`OverworldBiomeBuilder.java`），生成共 7593 条超矩形条目。
class OverworldBiomeBuilder {
public:
  static std::vector<std::pair<ClimateParameterPoint, std::string>>
  buildEntries();
  static std::vector<ClimateParameterPoint> spawnTarget();
};

// 基于 7D R-Tree 的多层气候噪声生物群系源（`MultiNoiseBiomeSource.java`），服务主世界与下界。
class MultiNoiseBiomeSource final : public BiomeSource {
public:
  explicit MultiNoiseBiomeSource(ClimateParameterList<std::string> parameters);

  static MultiNoiseBiomeSource createOverworld();
  static MultiNoiseBiomeSource createNether();
  static MultiNoiseBiomeSource
  createFromList(std::vector<std::pair<ClimateParameterPoint, std::string>> list);

  std::vector<std::string> possibleBiomes() const override;
  std::string_view getNoiseBiome(int quartX, int quartY, int quartZ,
                                 const ClimateSampler &sampler) const override;
  const std::string &getNoiseBiome(const ClimateTargetPoint &target) const;

  const ClimateParameterList<std::string> &parameters() const noexcept {
    return parameters_;
  }

private:
  ClimateParameterList<std::string> parameters_;
};

// 末地生物群系源（`TheEndBiomeSource.java`）：中心圆盘 <= 4096 区块平方内为 `the_end`，
// 外圈按区块中心 `(chunkX*2+1)*8, y, (chunkZ*2+1)*8` 的 `erosion` 通道划分为高地/内陆/荒原/小岛。
class TheEndBiomeSource final : public BiomeSource {
public:
  std::vector<std::string> possibleBiomes() const override;
  std::string_view getNoiseBiome(int quartX, int quartY, int quartZ,
                                 const ClimateSampler &sampler) const override;
};

// 对齐 `BiomeManager.java`：对块坐标进行基于世界种子 SHA-256 前 8 字节与 LCG 的
// 8 角点抖动放大采样（从 4×4×4 Quart 网格平滑还原至 1×1×1 方块坐标）。
class BiomeManager {
public:
  using NoiseBiomeReader = std::function<std::string_view(int, int, int)>;

  static std::int64_t obfuscateSeed(std::int64_t worldSeed) noexcept;

  explicit BiomeManager(std::int64_t biomeZoomSeed) noexcept
      : biomeZoomSeed_(biomeZoomSeed) {}

  std::int64_t biomeZoomSeed() const noexcept { return biomeZoomSeed_; }

  std::string_view getBiome(int blockX, int blockY, int blockZ,
                            const NoiseBiomeReader &source) const;

  static int pickCornerIndex(std::int64_t biomeZoomSeed, int parentX,
                             int parentY, int parentZ, int absX, int absY,
                             int absZ) noexcept;

  static std::string_view getNoiseBiomeAtQuart(int quartX, int quartY, int quartZ,
                                               const NoiseBiomeReader &source) {
    return source(quartX, quartY, quartZ);
  }

private:
  static double getFiddledDistance(std::int64_t seed, int xRandom, int yRandom,
                                   int zRandom, double distanceX,
                                   double distanceY, double distanceZ) noexcept;

  std::int64_t biomeZoomSeed_ = 0;
};

// 执行 G-2 的区块生物群系填充步骤（对应 `NoiseBasedChunkGenerator.doBiomes` / `ChunkStatus.BIOMES`）：
// 遍历原型区块内全部区段的 4×4×4 Quart 格并写入 `ProtoChunk::setNoiseBiome`。
void fillBiomesFromNoise(ProtoChunk &chunk, const BiomeSource &biomeSource,
                         const ClimateSampler &sampler);

} // namespace redstone::worldgen
