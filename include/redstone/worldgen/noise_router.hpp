#pragma once
// 对齐 Minecraft Java Edition 26.1 的 15 通道噪声路由（`NoiseRouter`）、
// 噪声生成设定（`NoiseGeneratorSettings`）、官方 JSON 资产注册表（`WorldgenRegistries`）
// 以及按世界种子完成全部噪声与随机工厂实例化的 `RandomState`。
// 之所以将资产加载与 `RandomState` 绑定分离，是因为数据包定义的噪声路由图在服务端启动后只读共享，
// 而不同世界种子（及不同维度）各自独立派生 `NormalNoise`、`BlendedNoise` 与 `EndIslands` 实例。

#include "redstone/block.hpp"
#include "redstone/flat_hash_map.hpp"
#include "redstone/worldgen/carver.hpp"
#include "redstone/worldgen/climate.hpp"
#include "redstone/worldgen/density_function.hpp"
#include "redstone/worldgen/jigsaw.hpp"
#include "redstone/worldgen/noise.hpp"
#include "redstone/worldgen/random_source.hpp"
#include "redstone/worldgen/structure.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace picojson {
class value;
}

namespace redstone::worldgen {

BlockState parseBlockStateJson(const picojson::value &v, std::string_view ctx);

struct SurfaceRuleNode;
using SurfaceRuleSourcePtr = std::shared_ptr<const SurfaceRuleNode>;
class SurfaceSystem;
struct PlacedFeature;
struct ConfiguredFeature;
using PlacedFeaturePtr = std::shared_ptr<const PlacedFeature>;
using ConfiguredFeaturePtr = std::shared_ptr<const ConfiguredFeature>;

// 对应 26.1 `NoiseRouter.java` 的 15 条核心密度通道（G-2 气候采样、G-3 地形/含水层/巨型矿脉共同依赖）。
struct NoiseRouter {
  DensityNodePtr barrierNoise;
  DensityNodePtr fluidLevelFloodednessNoise;
  DensityNodePtr fluidLevelSpreadNoise;
  DensityNodePtr lavaNoise;
  DensityNodePtr temperature;
  DensityNodePtr vegetation;
  DensityNodePtr continents;
  DensityNodePtr erosion;
  DensityNodePtr depth;
  DensityNodePtr ridges;
  DensityNodePtr preliminarySurfaceLevel;
  DensityNodePtr finalDensity;
  DensityNodePtr veinToggle;
  DensityNodePtr veinRidged;
  DensityNodePtr veinGap;

  NoiseRouter mapAll(
      const density_functions::NodeTransform &nodeVisitor,
      const std::function<NoiseHolder(const NoiseHolder &)> &noiseVisitor = {})
      const;
};

// 对应 `Climate.Sampler`：由已剥离 `Marker` 包装器的 6 条气候通道构成，用于 G-2 生物群系查找与出生点搜索。
struct ClimateSampler {
  DensityNodePtr temperature;
  DensityNodePtr humidity;
  DensityNodePtr continentalness;
  DensityNodePtr erosion;
  DensityNodePtr depth;
  DensityNodePtr weirdness;
  std::vector<ClimateParameterPoint> spawnTarget;
  std::uint8_t yDependentMask = 0x3F;
  // 6 条气候通道共享的扁平求值器（纯函数子图）；为空时回退到逐通道递归求值。
  std::shared_ptr<const FlatDensityFunction> flat;

  void computeYDependentMask() noexcept;
  ClimateTargetPoint sample(int quartX, int quartY, int quartZ) const;
  Pos findSpawnPosition() const;
};

// 对应 26.1 `NoiseGeneratorSettings`（从 `assets/worldgen26_1/noise_settings/*.json` 解析）。
struct NoiseGeneratorSettings {
  std::string id;
  int minY = -64;
  int height = 384;
  int sizeHorizontal = 1;
  int sizeVertical = 2;
  int seaLevel = 63;
  std::string defaultBlockName = "minecraft:stone";
  std::string defaultFluidName = "minecraft:water";
  BlockState defaultBlockState;
  BlockState defaultFluidState;
  bool aquifersEnabled = true;
  bool oreVeinsEnabled = true;
  bool disableMobGeneration = false;
  bool legacyRandomSource = false;
  std::vector<ClimateParameterPoint> spawnTarget;
  NoiseRouter noiseRouter;
  SurfaceRuleSourcePtr surfaceRule;
};

struct BiomeGenerationSettings {
  std::vector<std::string> carvers;
  std::array<std::vector<std::string>, 11> features;
  std::vector<const ConfiguredCarver *> resolvedCarvers;
};

// 从官方导出的 `assets/worldgen26_1/` 目录加载全部只读 `biome`、`noise`、`density_function`、
// `configured_carver`、`tags/biome` 与 `noise_settings` 等世界生成注册表。
class WorldgenRegistries {
public:
  static constexpr std::uint16_t kInvalidBiomeId = 0xFFFFu;

  WorldgenRegistries() = default;
  WorldgenRegistries(const WorldgenRegistries &other);
  WorldgenRegistries &operator=(const WorldgenRegistries &other);
  WorldgenRegistries(WorldgenRegistries &&) noexcept = default;
  WorldgenRegistries &operator=(WorldgenRegistries &&) noexcept = default;

  static WorldgenRegistries loadFromDirectory(
      const std::filesystem::path &worldgenDir);

  const NoiseParameters *findNoise(std::string_view id) const;
  const DensityNodePtr *findDensityFunction(std::string_view id) const;
  const NoiseGeneratorSettings *findNoiseSettings(std::string_view id) const;
  const BiomeClimateProperties *findBiomeClimate(std::string_view id) const;
  const BiomeGenerationSettings *findBiomeGeneration(std::string_view id) const;
  const ConfiguredCarver *findCarver(std::string_view id) const;
  const std::vector<std::string> *findBiomeTag(std::string_view tagId) const;
  const StructureDefinition *findStructure(std::string_view id) const;
  const StructureSetDefinition *findStructureSet(std::string_view id) const;
  const StructureProcessorList *findProcessorList(std::string_view id) const;
  const StructureTemplatePool *findTemplatePool(std::string_view id) const;
  const StructureTemplateCache *templateCache() const noexcept {
    return templateCache_.get();
  }
  const PlacedFeature *findPlacedFeature(std::string_view id) const;
  const ConfiguredFeature *findConfiguredFeature(std::string_view id) const;
  bool biomeMatches(std::string_view biomeId,
                    std::string_view biomeOrTagSelector) const;

  std::uint16_t findBiomeId(std::string_view id) const noexcept;
  std::size_t biomeCount() const noexcept { return biomeNames_.size(); }
  std::string_view biomeName(std::uint16_t id) const noexcept {
    return id < biomeNames_.size() ? biomeNames_[id] : std::string_view{};
  }
  const BiomeClimateProperties *
  biomeClimateById(std::uint16_t id) const noexcept {
    return id < biomeClimatesById_.size() ? biomeClimatesById_[id] : nullptr;
  }
  const BiomeGenerationSettings *
  biomeGenerationById(std::uint16_t id) const noexcept {
    return id < biomeGenerationsById_.size() ? biomeGenerationsById_[id]
                                             : nullptr;
  }
  bool buildBiomeSelectorMask(std::span<const std::string> selectors,
                              std::vector<std::uint64_t> &outWords) const;
  const std::vector<std::uint64_t> *findBiomeTagMask(std::string_view tagId) const;
  static bool biomeMatchesMask(std::uint16_t biomeId,
                               std::span<const std::uint64_t> maskWords) noexcept {
    if (biomeId == kInvalidBiomeId)
      return false;
    const std::size_t word = static_cast<std::size_t>(biomeId) >> 6u;
    return word < maskWords.size() &&
           ((maskWords[word] >> (biomeId & 63u)) & 1ULL) != 0ULL;
  }

  const std::map<std::string, NoiseParameters, std::less<>> &noises() const {
    return noises_;
  }
  const std::map<std::string, DensityNodePtr, std::less<>> &
  densityFunctions() const {
    return densityFunctions_;
  }
  const std::map<std::string, NoiseGeneratorSettings, std::less<>> &
  noiseSettings() const {
    return noiseSettings_;
  }
  const std::map<std::string, BiomeClimateProperties, std::less<>> &
  biomeClimates() const {
    return biomeClimates_;
  }
  const std::map<std::string, BiomeGenerationSettings, std::less<>> &
  biomeGenerations() const {
    return biomeGenerations_;
  }
  const std::map<std::string, ConfiguredCarver, std::less<>> &carvers() const {
    return carvers_;
  }
  const std::map<std::string, std::vector<std::string>, std::less<>> &
  biomeTags() const {
    return biomeTags_;
  }
  const std::map<std::string, StructureDefinition, std::less<>> &
  structures() const {
    return structures_;
  }
  const std::map<std::string, StructureSetDefinition, std::less<>> &
  structureSets() const {
    return structureSets_;
  }
  const std::map<std::string, StructureProcessorList, std::less<>> &
  processorLists() const {
    return processorLists_;
  }
  const std::map<std::string, StructureTemplatePool, std::less<>> &
  templatePools() const {
    return templatePools_;
  }
  const std::map<std::string, PlacedFeaturePtr, std::less<>> &
  placedFeatures() const {
    return placedFeatures_;
  }
  const std::map<std::string, ConfiguredFeaturePtr, std::less<>> &
  configuredFeatures() const {
    return configuredFeatures_;
  }

private:
  template <class T>
  using StringHashMap =
      FlatHashMap<std::string_view, T, TransparentStringHash, std::equal_to<>>;

  void rebuildLookupIndices();

  std::map<std::string, NoiseParameters, std::less<>> noises_;
  std::map<std::string, DensityNodePtr, std::less<>> densityFunctions_;
  std::map<std::string, NoiseGeneratorSettings, std::less<>> noiseSettings_;
  std::map<std::string, BiomeClimateProperties, std::less<>> biomeClimates_;
  std::map<std::string, BiomeGenerationSettings, std::less<>> biomeGenerations_;
  std::map<std::string, ConfiguredCarver, std::less<>> carvers_;
  std::map<std::string, std::vector<std::string>, std::less<>> biomeTags_;
  std::map<std::string, StructureDefinition, std::less<>> structures_;
  std::map<std::string, StructureSetDefinition, std::less<>> structureSets_;
  std::map<std::string, StructureProcessorList, std::less<>> processorLists_;
  std::map<std::string, StructureTemplatePool, std::less<>> templatePools_;
  std::shared_ptr<const StructureTemplateCache> templateCache_;
  std::map<std::string, PlacedFeaturePtr, std::less<>> placedFeatures_;
  std::map<std::string, ConfiguredFeaturePtr, std::less<>> configuredFeatures_;

  StringHashMap<const NoiseParameters *> noiseIdx_;
  StringHashMap<const DensityNodePtr *> densityFunctionIdx_;
  StringHashMap<const NoiseGeneratorSettings *> noiseSettingsIdx_;
  StringHashMap<const BiomeClimateProperties *> biomeClimateIdx_;
  StringHashMap<const BiomeGenerationSettings *> biomeGenerationIdx_;
  StringHashMap<const ConfiguredCarver *> carverIdx_;
  StringHashMap<const std::vector<std::string> *> biomeTagIdx_;
  StringHashMap<const StructureDefinition *> structureIdx_;
  StringHashMap<const StructureSetDefinition *> structureSetIdx_;
  StringHashMap<const StructureProcessorList *> processorListIdx_;
  StringHashMap<const StructureTemplatePool *> templatePoolIdx_;
  StringHashMap<const PlacedFeature *> placedFeatureIdx_;
  StringHashMap<const ConfiguredFeature *> configuredFeatureIdx_;

  std::vector<std::string_view> biomeNames_;
  StringHashMap<std::uint16_t> biomeIdByName_;
  std::vector<const BiomeClimateProperties *> biomeClimatesById_;
  std::vector<const BiomeGenerationSettings *> biomeGenerationsById_;
  std::size_t biomeTagStrideWords_ = 0;
  StringHashMap<std::vector<std::uint64_t>> biomeTagMasks_;
};

// 按世界种子绑定全部噪声与派生随机工厂（对齐 26.1 `RandomState.java`）。
class RandomState {
public:
  static RandomState create(const NoiseGeneratorSettings &settings,
                            const WorldgenRegistries &registries,
                            std::int64_t seed);
  ~RandomState();
  RandomState(RandomState &&) noexcept;
  RandomState &operator=(RandomState &&) noexcept;
  RandomState(const RandomState &) = delete;
  RandomState &operator=(const RandomState &) = delete;

  std::shared_ptr<const NormalNoise> getOrCreateNoise(std::string_view noiseId);
  PositionalRandomFactory getOrCreateRandomFactory(std::string_view name);
  SurfaceSystem &surfaceSystem(const NoiseGeneratorSettings &settings);

  const NoiseRouter &router() const noexcept { return router_; }
  const ClimateSampler &sampler() const noexcept { return sampler_; }
  const PositionalRandomFactory &aquiferRandom() const noexcept {
    return aquiferRandom_;
  }
  const PositionalRandomFactory &oreRandom() const noexcept {
    return oreRandom_;
  }
  const PositionalRandomFactory &positionalRandom() const noexcept {
    return random_;
  }
  std::int64_t seed() const noexcept { return seed_; }

private:
  RandomState(const NoiseGeneratorSettings &settings,
              const WorldgenRegistries &registries, std::int64_t seed);

  std::int64_t seed_ = 0;
  const WorldgenRegistries *registries_ = nullptr;
  PositionalRandomFactory random_;
  PositionalRandomFactory aquiferRandom_;
  PositionalRandomFactory oreRandom_;
  FlatHashMap<std::string, std::shared_ptr<const NormalNoise>,
              TransparentStringHash, std::equal_to<>>
      noiseInstances_;
  FlatHashMap<std::string, PositionalRandomFactory, TransparentStringHash,
              std::equal_to<>>
      positionalRandoms_;
  NoiseRouter router_;
  ClimateSampler sampler_;
  std::unique_ptr<SurfaceSystem> surfaceSystem_;
  struct CacheMutexes;
  mutable std::unique_ptr<CacheMutexes> cacheMutexes_;
};

} // namespace redstone::worldgen
