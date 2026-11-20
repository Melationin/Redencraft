#pragma once
// 对齐 Minecraft Java Edition 26.1 的地形雕刻器体系（`WorldCarver`、`CaveWorldCarver`、
// `NetherWorldCarver`、`CanyonWorldCarver` 与 `CarvingContext`）：
// 从 `assets/worldgen26_1/configured_carver/*.json` 与 `biome/*.json` 动态加载参数，
// 在 `ChunkStatus::CARVERS` 阶段遍历周边 17×17 个起点区块，按 `CarvingMask` 去重并在
// 非下界洞穴中经由 `Aquifer::computeSubstance(..., 0.0)` 保留水体隔水屏障。

#include "redstone/chunk_lifecycle.hpp"
#include "redstone/worldgen/random_source.hpp"
#include "redstone/worldgen/value_providers.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace picojson {
class value;
}

namespace redstone::worldgen {

class Aquifer;
class BiomeManager;
class CarvingMask;
class ProtoChunk;
class NoiseChunk;
class RandomState;
struct NoiseGeneratorSettings;
class WorldgenRegistries;

using CarverNoiseBiomeReader =
    std::function<std::string_view(int, int, int)>;

enum class CarverKind : std::uint8_t {
  Cave,
  NetherCave,
  Canyon,
};

struct CanyonShapeConfig {
  FloatProvider distanceFactor = FloatProvider::constant(1.0f);
  FloatProvider thickness = FloatProvider::constant(1.0f);
  int widthSmoothness = 3;
  FloatProvider horizontalRadiusFactor = FloatProvider::constant(1.0f);
  float verticalRadiusDefaultFactor = 1.0f;
  float verticalRadiusCenterFactor = 0.0f;
};

struct ConfiguredCarver {
  std::string id;
  CarverKind kind = CarverKind::Cave;
  float probability = 0.0f;
  HeightProvider y;
  FloatProvider yScale = FloatProvider::constant(1.0f);
  VerticalAnchor lavaLevel = VerticalAnchor::aboveBottom(8);
  std::string replaceableTag = "minecraft:overworld_carver_replaceables";

  // Cave / NetherCave
  FloatProvider horizontalRadiusMultiplier = FloatProvider::constant(1.0f);
  FloatProvider verticalRadiusMultiplier = FloatProvider::constant(1.0f);
  FloatProvider floorLevel = FloatProvider::constant(-0.7f);

  // Canyon
  FloatProvider verticalRotation = FloatProvider::constant(0.0f);
  CanyonShapeConfig canyonShape;

  bool isStartChunk(RandomSource &rng) const;
  bool carve(const WorldGenerationContext &genCtx,
             const NoiseGeneratorSettings &settings, RandomState &rs,
             const BiomeManager &biomeManager,
             const CarverNoiseBiomeReader &noiseBiomeReader,
             const WorldgenRegistries &registries, ProtoChunk &chunk,
             NoiseChunk &noiseChunk, RandomSource &rng, Aquifer &aquifer,
             ChunkTicketPos sourceChunkPos, CarvingMask &mask) const;
};

ConfiguredCarver parseConfiguredCarverJson(const picojson::value &v,
                                           std::string_view id);

} // namespace redstone::worldgen
