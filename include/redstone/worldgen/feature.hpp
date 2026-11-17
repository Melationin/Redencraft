#pragma once
// 对齐 Minecraft Java Edition 26.1 的地物与放置系统（`PlacedFeature`、`PlacementModifier`、
// `BlockPredicate`、`BlockStateProvider`、`ConfiguredFeature` 与 `FeatureSorter`）：
// 所有放置修饰器与地物配置均从 `assets/worldgen26_1/{placed_feature,configured_feature}`
// 实时解析加载，驱动 11 个装饰阶段（`applyBiomeDecoration`）逐区块生成矿物、泉水、植物、树木、黑曜石柱与冻顶。

#include "redstone/block.hpp"
#include "redstone/engine_types.hpp"
#include "redstone/worldgen/biome_source.hpp"
#include "redstone/worldgen/jigsaw.hpp"
#include "redstone/worldgen/noise.hpp"
#include "redstone/worldgen/random_source.hpp"
#include "redstone/worldgen/structure.hpp"
#include "redstone/worldgen/value_providers.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace picojson {
class value;
}

namespace redstone::worldgen {

class NoiseBasedChunkGenerator;
class WorldGenRegion;
class WorldgenRegistries;
struct ConfiguredFeature;
struct PlacedFeature;
using ConfiguredFeaturePtr = std::shared_ptr<const ConfiguredFeature>;
using PlacedFeaturePtr = std::shared_ptr<const PlacedFeature>;

struct BlockPredicate {
  enum class Kind : std::uint8_t {
    AlwaysTrue,
    AllOf,
    AnyOf,
    Not,
    MatchingBlocks,
    MatchingBlockTag,
    MatchingFluids,
    HasSturdyFace,
    Solid,
    Replaceable,
    WouldSurvive,
    InsideWorldBounds,
    Unobstructed,
  };

  Kind kind = Kind::AlwaysTrue;
  Pos offset{0, 0, 0};
  Direction direction = Direction::Up;
  BlockState matchState;
  std::vector<std::string> ids;
  std::string tag;
  std::vector<BlockPredicate> children;

  bool test(const WorldGenRegion &region, Pos pos) const;
};

struct BlockStateProvider {
  enum class Kind : std::uint8_t {
    Simple,
    Weighted,
    RuleBased,
    RotatedBlock,
    Noise,
    NoiseThreshold,
    DualNoise,
    RandomizedInt,
  };

  struct WeightedEntry {
    BlockState state;
    int weight = 1;
  };
  struct RuleEntry {
    BlockPredicate ifTrue;
    std::shared_ptr<BlockStateProvider> thenProvider;
  };

  Kind kind = Kind::Simple;
  BlockState simpleState;
  std::vector<WeightedEntry> weighted;
  std::vector<RuleEntry> rules;
  std::shared_ptr<BlockStateProvider> fallback;
  std::int64_t noiseSeed = 0;
  double scale = 1.0;
  double threshold = 0.0;
  double highChance = 0.0;
  NoiseParameters noiseParams{0, {1.0}};
  NoiseParameters slowNoiseParams{-10, {1.0}};
  double slowScale = 1.0;
  int varietyMin = 1;
  int varietyMax = 1;
  std::shared_ptr<const NormalNoise> normalNoise;
  std::shared_ptr<const NormalNoise> slowNormalNoise;
  BlockState defaultStateVal;
  std::vector<BlockState> states;
  std::vector<BlockState> highStates;
  std::vector<BlockState> lowStates;
  std::string randomizedProperty;
  IntProvider randomizedValues;

  BlockState getState(RandomSource &rng, Pos pos) const;
  std::optional<BlockState> getOptionalState(const WorldGenRegion &region,
                                             RandomSource &rng, Pos pos) const;
};

struct PlacementModifier {
  enum class Kind : std::uint8_t {
    Biome,
    InSquare,
    Count,
    CountOnEveryLayer,
    HeightRange,
    Heightmap,
    RandomOffset,
    RarityFilter,
    SurfaceWaterDepthFilter,
    SurfaceRelativeThresholdFilter,
    BlockPredicateFilter,
    EnvironmentScan,
    NoiseBasedCount,
    NoiseThresholdCount,
    FixedPlacement,
  };

  Kind kind = Kind::Biome;
  IntProvider countProvider;
  IntProvider xzSpread;
  IntProvider ySpread;
  HeightProvider heightProvider;
  HeightmapType heightmap = HeightmapType::WorldSurfaceWG;
  int rarityChance = 1;
  int maxWaterDepth = 0;
  int minInclusive = std::numeric_limits<int>::min();
  int maxInclusive = std::numeric_limits<int>::max();
  BlockPredicate predicate;
  Direction searchDir = Direction::Down;
  BlockPredicate targetCondition;
  BlockPredicate allowedSearchCondition;
  int maxSteps = 1;
  int noiseToCountRatio = 1;
  double noiseFactor = 1.0;
  double noiseOffset = 0.0;
  double noiseLevel = 0.0;
  int belowNoise = 0;
  int aboveNoise = 0;
  std::vector<Pos> fixedPositions;
};

struct PlacementContext {
  WorldGenRegion &region;
  const NoiseBasedChunkGenerator &generator;
  std::int64_t worldSeed = 0;
  std::string topFeatureId;
  std::unique_ptr<BiomeManager> biomeManager;
};

struct ConfiguredFeature {
  enum class Kind : std::uint8_t {
    Ore,
    ScatteredOre,
    SpringFeature,
    Disk,
    SimpleBlock,
    RandomSelector,
    SimpleRandomSelector,
    RandomBooleanSelector,
    BlockColumn,
    BlockPile,
    VegetationPatch,
    WaterloggedVegetationPatch,
    Lake,
    NetherForestVegetation,
    GlowstoneBlob,
    NetherrackReplaceBlobs,
    FreezeTopLayer,
    EndSpike,
    EndIsland,
    DesertWell,
    BlockBlob,
    BlueIce,
    Vines,
    Seagrass,
    Kelp,
    SeaPickle,
    Bamboo,
    BasaltColumns,
    BasaltPillar,
    BonusChest,
    ChorusPlant,
    DeltaFeature,
    DripstoneCluster,
    EndGateway,
    EndPlatform,
    Fossil,
    Geode,
    Iceberg,
    LargeDripstone,
    MonsterRoom,
    MultifaceGrowth,
    RootSystem,
    SculkPatch,
    Spike,
    TwistingVines,
    UnderwaterMagma,
    VoidStartPlatform,
    WeepingVines,
    HugeRedMushroom,
    HugeBrownMushroom,
    HugeFungus,
    FallenTree,
    Tree,
    Unsupported,
  };

  struct TreeDecoratorConfig {
    enum class Kind : std::uint8_t {
      Beehive,
      PlaceOnGround,
      LeaveVine,
      TrunkVine,
      AlterGround,
      AttachedToLeaves,
      AttachedToLogs,
      PaleMoss,
      CreakingHeart,
      Cocoa,
    };
    Kind kind = Kind::Beehive;
    float probability = 0.0f;
    int tries = 128;
    int radius = 2;
    int height = 1;
    BlockStateProvider blockProvider;
    int exclusionRadiusXZ = 0;
    int exclusionRadiusY = 0;
    int requiredEmptyBlocks = 1;
    std::vector<Direction> directions;
    float leavesProbability = 0.0f;
    float trunkProbability = 0.0f;
    float groundProbability = 0.0f;
  };

  struct RootPlacerConfig {
    IntProvider trunkOffsetY;
    BlockStateProvider rootProvider;
    std::optional<BlockStateProvider> aboveRootProvider;
    float aboveRootPlacementChance = 0.0f;
    std::vector<std::string> canGrowThroughIds;
    std::string canGrowThroughTag;
    std::vector<std::string> muddyRootsInIds;
    std::string muddyRootsInTag;
    BlockStateProvider muddyRootsProvider;
    int maxRootLength = 15;
    int maxRootWidth = 8;
    float randomSkewChance = 0.2f;
  };

  struct OreTarget {
    RulePredicate target;
    BlockState state;
  };
  struct WeightedFeatureEntry {
    PlacedFeaturePtr feature;
    float chance = 0.5f;
  };
  struct ColumnLayer {
    IntProvider height;
    BlockStateProvider provider;
  };
  struct TreeConfig {
    BlockStateProvider trunkProvider;
    BlockStateProvider foliageProvider;
    BlockStateProvider dirtProvider;
    std::shared_ptr<BlockStateProvider> belowTrunkProvider;
    std::string trunkPlacerType = "minecraft:straight_trunk_placer";
    int baseHeight = 4;
    int heightRandA = 2;
    int heightRandB = 0;
    int minHeightForLeaves = 1;
    IntProvider bendLength;
    IntProvider branchCount;
    IntProvider branchHorizontalLength;
    int branchStartMin = -4;
    int branchStartMax = -3;
    IntProvider branchEndOffset;
    IntProvider extraBranchSteps;
    float placeBranchPerLogProbability = 0.5f;
    IntProvider extraBranchLength;
    std::string trunkCanGrowThroughTag;
    std::vector<std::string> trunkCanGrowThroughIds;

    std::string foliagePlacerType = "minecraft:blob_foliage_placer";
    IntProvider foliageRadius;
    IntProvider foliageOffset;
    int foliageHeight = 3;
    IntProvider trunkHeightProvider;
    IntProvider cherryHeight;
    IntProvider crownHeight;
    int leafPlacementAttempts = 0;
    float wideBottomLayerHoleChance = 0.25f;
    float cornerHoleChance = 0.25f;
    float hangingLeavesChance = 0.16666667f;
    float hangingLeavesExtensionChance = 0.33333334f;

    bool isThreeLayersSize = false;
    int minSizeLimit = 1;
    int upperSizeLimit = 1;
    int lowerSize = 0;
    int middleSize = 0;
    int upperSize = 1;
    std::optional<int> minClippedHeight;
    bool ignoreVines = false;

    std::vector<TreeDecoratorConfig> decorators;
    std::optional<RootPlacerConfig> rootPlacer;
  };

  struct FallenTreeConfig {
    BlockStateProvider trunkProvider;
    IntProvider logLength;
    std::vector<TreeDecoratorConfig> stumpDecorators;
    std::vector<TreeDecoratorConfig> logDecorators;
  };

  struct HugeMushroomConfig {
    BlockStateProvider capProvider;
    BlockStateProvider stemProvider;
    int foliageRadius = 2;
    BlockPredicate canPlaceOn;
  };

  struct HugeFungusConfig {
    BlockState validBaseState;
    BlockState stemState;
    BlockState hatState;
    BlockState decorState;
    BlockPredicate replaceableBlocks;
    bool planted = false;
  };

  struct BasaltColumnsConfig {
    IntProvider reach;
    IntProvider height;
  };

  struct EndGatewayConfig {
    bool exact = false;
    std::optional<Pos> exit;
  };

  struct FossilConfig {
    std::vector<std::string> fossilStructures;
    std::vector<std::string> overlayStructures;
    std::string fossilProcessors;
    std::string overlayProcessors;
    int maxEmptyCornersAllowed = 4;
  };

  struct MultifaceGrowthConfig {
    std::string blockId = "minecraft:glow_lichen";
    int searchRange = 10;
    bool canPlaceOnFloor = false;
    bool canPlaceOnCeiling = false;
    bool canPlaceOnWall = false;
    float chanceOfSpreading = 0.5f;
    std::string canBePlacedOnTag;
    std::vector<std::string> canBePlacedOnIds;
  };

  struct SculkPatchConfig {
    int chargeCount = 10;
    int amountPerCharge = 32;
    int spreadAttempts = 64;
    int growthRounds = 0;
    int spreadRounds = 1;
    IntProvider extraRareGrowths;
    float catalystChance = 0.5f;
  };

  struct TwistingVinesConfig {
    int spreadWidth = 8;
    int spreadHeight = 4;
    int maxHeight = 8;
  };

  struct DeltaConfig {
    BlockState contents;
    BlockState rim;
    IntProvider size;
    IntProvider rimSize;
  };

  struct DripstoneClusterConfig {
    int floorToCeilingSearchRange = 12;
    IntProvider height;
    IntProvider radius;
    int maxStalagmiteStalactiteHeightDiff = 1;
    int heightDeviation = 3;
    IntProvider dripstoneBlockLayerThickness;
    FloatProvider density;
    FloatProvider wetness;
    float chanceOfDripstoneColumnAtMaxDistanceFromCenter = 0.1f;
    int maxDistanceFromEdgeAffectingChanceOfDripstoneColumn = 3;
    int maxDistanceFromCenterAffectingHeightBias = 8;
  };

  struct LargeDripstoneConfig {
    int floorToCeilingSearchRange = 30;
    IntProvider columnRadius;
    FloatProvider heightScale;
    float maxColumnRadiusToCaveHeightRatio = 0.33f;
    FloatProvider stalactiteBluntness;
    FloatProvider stalagmiteBluntness;
    FloatProvider windSpeed;
    int minRadiusForWind = 4;
    float minBluntnessForWind = 0.6f;
  };

  struct GeodeConfig {
    BlockStateProvider fillingProvider;
    BlockStateProvider innerLayerProvider;
    BlockStateProvider alternateInnerLayerProvider;
    BlockStateProvider middleLayerProvider;
    BlockStateProvider outerLayerProvider;
    std::vector<BlockState> innerPlacements;
    std::string cannotReplaceTag = "minecraft:features_cannot_replace";
    std::string invalidBlocksTag = "minecraft:geode_invalid_blocks";
    double filling = 1.7;
    double innerLayer = 2.2;
    double middleLayer = 3.2;
    double outerLayer = 4.2;
    double generateCrackChance = 1.0;
    double baseCrackSize = 2.0;
    int crackPointOffset = 2;
    double usePotentialPlacementsChance = 0.35;
    double useAlternateLayer0Chance = 0.0;
    bool placementsRequireLayer0Alternate = true;
    IntProvider outerWallDistance;
    IntProvider distributionPoints;
    IntProvider pointOffset;
    int minGenOffset = -16;
    int maxGenOffset = 16;
    double noiseMultiplier = 0.05;
    int invalidBlocksThreshold = 1;
  };

  struct RootSystemConfig {
    PlacedFeaturePtr treeFeature;
    int requiredVerticalSpaceForTree = 3;
    int rootRadius = 3;
    std::string rootReplaceableTag = "minecraft:azalea_root_replaceable";
    BlockStateProvider rootStateProvider;
    int rootPlacementAttempts = 20;
    int rootColumnMaxHeight = 100;
    int hangingRootRadius = 3;
    int hangingRootsVerticalSpan = 2;
    BlockStateProvider hangingRootStateProvider;
    int hangingRootPlacementAttempts = 20;
    int allowedVerticalWaterForTree = 2;
    BlockPredicate allowedTreePosition;
  };

  struct UnderwaterMagmaConfig {
    int floorSearchRange = 5;
    int placementRadiusAroundFloor = 1;
    float placementProbabilityPerValidPosition = 0.5f;
  };

  struct BlockBlobConfig {
    BlockState state;
    BlockPredicate canPlaceOn;
  };

  struct SpikeConfig {
    BlockState state;
    BlockPredicate canPlaceOn;
    BlockPredicate canReplace;
  };

  std::string id;
  std::string rawType;
  Kind kind = Kind::Unsupported;

  // Ore / ScatteredOre
  int oreSize = 0;
  float discardChanceOnAirExposure = 0.0f;
  std::vector<OreTarget> oreTargets;

  // SpringFeature
  BlockState springState;
  bool requiresBlockBelow = true;
  int rockCount = 4;
  int holeCount = 1;
  std::vector<std::string> validBlocks;

  // Disk
  int diskHalfHeight = 1;
  IntProvider diskRadius;
  BlockStateProvider stateProvider;
  BlockPredicate targetPredicate;

  // SimpleBlock
  bool scheduleTick = false;

  // RandomSelector / SimpleRandom / RandomBoolean
  std::vector<WeightedFeatureEntry> weightedFeatures;
  PlacedFeaturePtr defaultFeature;
  std::vector<PlacedFeaturePtr> randomFeatures;
  PlacedFeaturePtr featureTrue;
  PlacedFeaturePtr featureFalse;

  // BlockColumn
  Direction columnDir = Direction::Up;
  bool prioritizeTip = false;
  BlockPredicate allowedPlacement;
  std::vector<ColumnLayer> layers;

  // VegetationPatch
  std::string replaceableTag;
  BlockStateProvider groundState;
  PlacedFeaturePtr vegetationFeature;
  int verticalRange = 5;
  IntProvider patchDepth;
  IntProvider xzRadius;
  float extraBottomBlockChance = 0.0f;
  float extraEdgeColumnChance = 0.3f;
  float vegetationChance = 0.8f;
  bool surfaceCeiling = false;

  // Lake
  BlockStateProvider lakeFluid;
  BlockStateProvider lakeBarrier;

  // NetherForestVegetation / HugeMushroom
  int spreadWidth = 8;
  int spreadHeight = 4;
  int foliageRadiusInt = 2;

  // ReplaceBlobs
  BlockState replaceTarget;
  BlockState replaceState;
  IntProvider replaceRadius;

  // Seagrass / Kelp / SeaPickle
  float probability = 0.5f;
  IntProvider countProvider;

  // EndSpike
  bool crystalInvulnerable = false;
  struct EndSpikeDef {
    int centerX = 0;
    int centerZ = 0;
    int radius = 0;
    int height = 0;
    bool guarded = false;
  };
  std::vector<EndSpikeDef> endSpikes;

  // Tree / FallenTree / HugeMushroom / HugeFungus
  TreeConfig tree;
  FallenTreeConfig fallenTree;
  HugeMushroomConfig hugeMushroom;
  HugeFungusConfig hugeFungus;

  // Extra features (29 types)
  BlockState simpleBlockState;
  BasaltColumnsConfig basaltColumns;
  EndGatewayConfig endGateway;
  FossilConfig fossil;
  MultifaceGrowthConfig multifaceGrowth;
  SculkPatchConfig sculkPatch;
  TwistingVinesConfig twistingVines;
  DeltaConfig delta;
  DripstoneClusterConfig dripstoneCluster;
  LargeDripstoneConfig largeDripstone;
  GeodeConfig geode;
  RootSystemConfig rootSystem;
  UnderwaterMagmaConfig underwaterMagma;
  BlockBlobConfig blockBlob;
  SpikeConfig spike;

  bool place(WorldGenRegion &region, const NoiseBasedChunkGenerator &generator,
             std::int64_t worldSeed, RandomSource &rng, Pos origin) const;
};

struct PlacedFeature {
  std::string id;
  std::string featureRefId;
  ConfiguredFeaturePtr inlineFeature;
  std::vector<PlacementModifier> placement;

  bool place(WorldGenRegion &region, const NoiseBasedChunkGenerator &generator,
             std::int64_t worldSeed, RandomSource &rng, Pos origin,
             bool checkBiome = false) const;
};

struct StepFeatureData {
  std::vector<std::string> featureIds;
  std::map<std::string, int, std::less<>> indexById;
};

// 对齐 `FeatureSorter.buildFeaturesPerStep`：按生物群系源的群系表拓扑排序 11 个阶段地物，
// 生成与 26.1 一致的 `globalIndexOfFeature`（用于 `setFeatureSeed(decorationSeed, index, step)`）。
std::vector<StepFeatureData>
buildFeaturesPerStep(const WorldgenRegistries &registries,
                     const std::vector<std::string> &possibleBiomes);

PlacedFeaturePtr parsePlacedFeatureJson(const picojson::value &v,
                                        std::string_view id);
ConfiguredFeaturePtr parseConfiguredFeatureJson(const picojson::value &v,
                                                std::string_view id);

namespace feature_detail {
BlockState regionGetBlockState(const WorldGenRegion &region, int x, int y,
                               int z) noexcept;
FluidState regionGetFluidState(const WorldGenRegion &region, int x, int y,
                               int z) noexcept;
int regionGetHeight(const WorldGenRegion &region, HeightmapType type, int x,
                    int z) noexcept;
bool regionSetBlockState(WorldGenRegion &region, int x, int y, int z,
                         const BlockState &state, int flags = 2);
void regionMarkPosForPostprocessing(WorldGenRegion &region, int x, int y,
                                    int z);
void regionScheduleFluidTick(WorldGenRegion &region, int x, int y, int z,
                             const Fluid &fluid, int delay = 0);
bool blockMatchesFaceSturdyUp(const BlockState &state) noexcept;
} // namespace feature_detail

bool placeExtraConfiguredFeature(const ConfiguredFeature &cf,
                                 WorldGenRegion &region,
                                 const NoiseBasedChunkGenerator &generator,
                                 std::int64_t worldSeed, RandomSource &rng,
                                 Pos origin);

bool placeTreeFeature(const ConfiguredFeature::TreeConfig &cfg,
                      WorldGenRegion &region,
                      const NoiseBasedChunkGenerator &generator,
                      std::int64_t worldSeed, RandomSource &rng, Pos origin);
bool placeFallenTreeFeature(const ConfiguredFeature::FallenTreeConfig &cfg,
                            WorldGenRegion &region, RandomSource &rng,
                            Pos origin);
bool placeHugeMushroomFeature(bool isRed,
                              const ConfiguredFeature::HugeMushroomConfig &cfg,
                              WorldGenRegion &region, RandomSource &rng,
                              Pos origin);
bool placeHugeFungusFeature(const ConfiguredFeature::HugeFungusConfig &cfg,
                            WorldGenRegion &region,
                            const NoiseBasedChunkGenerator &generator,
                            RandomSource &rng, Pos origin);

} // namespace redstone::worldgen
