#pragma once
// 对齐 Minecraft Java Edition 26.1 的结构放置、结构生成、边界盒与地形适应系统：
// 包含 `BoundingBox`、`StructurePlacement`（`RandomSpread` / `ConcentricRings` / 频率衰减 / 互斥区）、
// `StructureSet`、`StructureDefinition`、`ChunkGeneratorStructureState`、
// `StructureStart`、`StructurePiece` 与 24×24×24 高斯核地形适应节点 `Beardifier`。
// 全部结构与结构集参数均从游戏资源文件（`assets/worldgen26_1/structure*`）加载，不硬编码配置表。

#include "redstone/block.hpp"
#include "redstone/chunk_lifecycle.hpp"
#include "redstone/engine_types.hpp"
#include "redstone/worldgen/density_function.hpp"
#include "redstone/worldgen/random_source.hpp"
#include "redstone/worldgen/value_providers.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace picojson {
class value;
}

namespace redstone::worldgen {

class BiomeSource;
struct ClimateSampler;
class ChunkGenerator;
class NoiseChunk;
class ProtoChunk;
class RandomState;
class WorldGenRegion;
class WorldgenRegistries;
struct NoiseGeneratorSettings;
struct StructureTemplate;

enum class DecorationStep : std::uint8_t {
  RawGeneration = 0,
  Lakes = 1,
  LocalModifications = 2,
  UndergroundStructures = 3,
  SurfaceStructures = 4,
  Strongholds = 5,
  UndergroundOres = 6,
  UndergroundDecoration = 7,
  FluidSprings = 8,
  VegetalDecoration = 9,
  TopLayerModification = 10,
};

inline constexpr std::size_t kDecorationStepCount = 11;

DecorationStep parseDecorationStep(std::string_view name);

enum class TerrainAdjustment : std::uint8_t {
  None,
  Bury,
  BeardThin,
  BeardBox,
  Encapsulate,
};

TerrainAdjustment parseTerrainAdjustment(std::string_view name);

enum class StructureType : std::uint8_t {
  Jigsaw,
  BuriedTreasure,
  DesertPyramid,
  EndCity,
  Fortress,
  Igloo,
  JungleTemple,
  Mineshaft,
  NetherFossil,
  OceanMonument,
  OceanRuin,
  RuinedPortal,
  Shipwreck,
  Stronghold,
  SwampHut,
  WoodlandMansion,
};

enum class SpreadType : std::uint8_t {
  Linear,
  Triangular,
};

enum class FrequencyReductionMethod : std::uint8_t {
  Default,
  LegacyType1,
  LegacyType2,
  LegacyType3,
};

enum class LiquidSettings : std::uint8_t {
  IgnoreWaterlogging,
  ApplyWaterlogging,
};

enum class Mirror : std::uint8_t {
  None,
  LeftRight,
  FrontBack,
};

struct BoundingBox {
  int minX = 0;
  int minY = 0;
  int minZ = 0;
  int maxX = 0;
  int maxY = 0;
  int maxZ = 0;

  static BoundingBox fromCorners(Pos a, Pos b) noexcept;
  static BoundingBox orientBox(int footX, int footY, int footZ, int offX,
                               int offY, int offZ, int width, int height,
                               int depth, Direction dir) noexcept;

  int xSpan() const noexcept { return maxX - minX + 1; }
  int ySpan() const noexcept { return maxY - minY + 1; }
  int zSpan() const noexcept { return maxZ - minZ + 1; }
  int centerX() const noexcept { return minX + (xSpan() / 2); }
  int centerY() const noexcept { return minY + (ySpan() / 2); }
  int centerZ() const noexcept { return minZ + (zSpan() / 2); }

  bool isInside(int x, int y, int z) const noexcept {
    return x >= minX && x <= maxX && y >= minY && y <= maxY && z >= minZ &&
           z <= maxZ;
  }
  bool intersects(const BoundingBox &o) const noexcept {
    return maxX >= o.minX && minX <= o.maxX && maxZ >= o.minZ &&
           minZ <= o.maxZ && maxY >= o.minY && minY <= o.maxY;
  }
  bool intersectsXZ(int x0, int z0, int x1, int z1) const noexcept {
    return maxX >= x0 && minX <= x1 && maxZ >= z0 && minZ <= z1;
  }
  BoundingBox moved(int dx, int dy, int dz) const noexcept {
    return {minX + dx, minY + dy, minZ + dz, maxX + dx, maxY + dy, maxZ + dz};
  }
  BoundingBox inflatedBy(int amount) const noexcept {
    return {minX - amount, minY - amount, minZ - amount,
            maxX + amount, maxY + amount, maxZ + amount};
  }
  void encapsulate(const BoundingBox &o) noexcept;
  void encapsulate(Pos p) noexcept;
};

struct JigsawJunction {
  int sourceX = 0;
  int sourceGroundY = 0;
  int sourceZ = 0;
  int deltaY = 0;
  bool destProjectionRigid = true;
};

struct StructurePiece {
  enum class Kind : std::uint8_t {
    PoolElement,
    SimpleBoxFill,
    TemplatePlacement,
  };

  Kind kind = Kind::PoolElement;
  BoundingBox boundingBox;
  int groundLevelDelta = 0;
  bool rigidProjection = true;
  Rotation rotation = Rotation::None;
  Mirror mirror = Mirror::None;
  Pos position{};
  Pos pivot{};
  LiquidSettings liquidSettings = LiquidSettings::ApplyWaterlogging;
  bool overwriteAirOnly = false;
  bool ignoreEntities = false;
  std::string templateId;
  std::string processorListId;
  BlockState fillBlock; // used when kind == SimpleBoxFill
  std::vector<JigsawJunction> junctions;

  bool isCloseToChunk(ChunkTicketPos pos, int padding) const noexcept {
    const int minBlockX = pos.x * 16 - padding;
    const int minBlockZ = pos.z * 16 - padding;
    return boundingBox.intersectsXZ(minBlockX, minBlockZ,
                                    minBlockX + 15 + 2 * padding,
                                    minBlockZ + 15 + 2 * padding);
  }
};

struct StructureStart {
  std::string structureId;
  ChunkTicketPos chunkPos{};
  TerrainAdjustment terrainAdjustment = TerrainAdjustment::None;
  std::vector<StructurePiece> pieces;
  int references = 0;

  bool isValid() const noexcept { return !pieces.empty(); }
  BoundingBox calculateBoundingBox() const noexcept;
};

// 对齐 `Beardifier.java`：使用 24×24×24 预计算高斯核与快速平方根倒数 (`6910469410427058090LL`)
// 调整结构件周围地形密度。
class Beardifier final : public CustomDensityFunction {
public:
  struct Rigid {
    BoundingBox box;
    TerrainAdjustment terrainAdjustment = TerrainAdjustment::None;
    int groundLevelDelta = 0;
  };

  static std::shared_ptr<Beardifier> empty();
  static std::shared_ptr<Beardifier>
  forChunk(const std::vector<const StructureStart *> &starts,
           ChunkTicketPos chunkPos);

  Beardifier(std::vector<Rigid> pieces, std::vector<JigsawJunction> junctions,
             std::optional<BoundingBox> affectedBox);

  double compute(const DensityFunctionContext &ctx) override;
  void fillArray(double *output, std::size_t count,
                 DensityContextProvider &provider) override;

  bool isEmpty() const noexcept { return !affectedBox_.has_value(); }
  const std::vector<Rigid> &pieces() const noexcept { return pieces_; }
  const std::vector<JigsawJunction> &junctions() const noexcept {
    return junctions_;
  }

private:
  std::vector<Rigid> pieces_;
  std::vector<JigsawJunction> junctions_;
  std::optional<BoundingBox> affectedBox_;
};

// 从 `assets/worldgen26_1/structure/*.json` 解析的结构定义。
struct StructureDefinition {
  std::string id;
  StructureType type = StructureType::Jigsaw;
  std::vector<std::string> biomeSelectors; // biome IDs or "#tag"
  DecorationStep step = DecorationStep::SurfaceStructures;
  TerrainAdjustment terrainAdjustment = TerrainAdjustment::None;

  // Jigsaw fields
  std::string startPool;
  std::string startJigsawName;
  int size = 0;
  HeightProvider startHeight;
  bool useExpansionHack = false;
  std::optional<HeightmapType> projectStartToHeightmap;
  int maxDistanceHorizontal = 80;
  int maxDistanceVertical = 2048;
  int dimensionPaddingBottom = 0;
  int dimensionPaddingTop = 0;
  LiquidSettings liquidSettings = LiquidSettings::ApplyWaterlogging;

  // Non-jigsaw data-driven fields (nether_fossil / ocean_ruin / shipwreck / ruined_portal / mineshaft)
  HeightProvider fossilHeight;
  bool isBeached = false;
  float largeProbability = 0.3f;
  float clusterProbability = 0.9f;
  std::string oceanRuinTemp = "cold";
  struct RuinedPortalSetup {
    std::string placement = "on_land_surface";
    float airPocketProbability = 0.5f;
    float mossiness = 0.2f;
    bool overgrown = false;
    bool vines = false;
    bool canBeCold = true;
    bool replaceWithBlackstone = false;
    float weight = 1.0f;
  };
  std::vector<RuinedPortalSetup> portalSetups;

  // Pool aliases (`random`, `direct`, `random_group`) parsed cleanly in jigsaw.
  struct PoolAliasEntry {
    enum class Kind : std::uint8_t { Direct, Random, RandomGroup };
    Kind kind = Kind::Direct;
    std::string alias;
    std::string target;
    struct WeightedTarget {
      std::string target;
      int weight = 1;
    };
    std::vector<WeightedTarget> weighted;
    struct GroupEntry {
      std::vector<PoolAliasEntry> entries;
      int weight = 1;
    };
    std::vector<GroupEntry> groups;
  };
  std::vector<PoolAliasEntry> poolAliases;
};

struct ExclusionZoneConfig {
  std::string otherSetId;
  int chunkCount = 1;
};

enum class StructurePlacementKind : std::uint8_t {
  RandomSpread,
  ConcentricRings,
};

struct StructurePlacementConfig {
  StructurePlacementKind kind = StructurePlacementKind::RandomSpread;
  Pos locateOffset{0, 0, 0};
  FrequencyReductionMethod frequencyReductionMethod =
      FrequencyReductionMethod::Default;
  float frequency = 1.0f;
  int salt = 0;
  std::optional<ExclusionZoneConfig> exclusionZone;

  // RandomSpread
  int spacing = 1;
  int separation = 0;
  SpreadType spreadType = SpreadType::Linear;

  // ConcentricRings
  int distance = 32;
  int spread = 3;
  int count = 128;
  std::string preferredBiomesSelector;

  ChunkTicketPos getPotentialStructureChunk(std::int64_t worldSeed, int sourceX,
                                            int sourceZ) const noexcept;
  bool applyAdditionalChunkRestrictions(int sourceX, int sourceZ,
                                        std::int64_t worldSeed) const noexcept;
  Pos getLocatePos(ChunkTicketPos chunkPos) const noexcept {
    return Pos{(chunkPos.x << 4) + locateOffset.x, locateOffset.y,
               (chunkPos.z << 4) + locateOffset.z};
  }
};

struct StructureSelectionEntry {
  std::string structureId;
  int weight = 1;
};

struct StructureSetDefinition {
  std::string id;
  std::vector<StructureSelectionEntry> structures;
  StructurePlacementConfig placement;
};

// 对齐 `ChunkGeneratorStructureState.java`：负责筛选与维度 `BiomeSource` 匹配的 `StructureSet`，
// 计算 `ConcentricRings` 要塞同心圆环坐标表与结构互斥判定。
class ChunkGeneratorStructureState {
public:
  ChunkGeneratorStructureState(std::int64_t levelSeed,
                               std::int64_t concentricRingsSeed,
                               const WorldgenRegistries &registries,
                               const BiomeSource &biomeSource,
                               const ClimateSampler &sampler,
                               bool includeConcentricRings = true);

  std::int64_t levelSeed() const noexcept { return levelSeed_; }
  const std::vector<const StructureSetDefinition *> &
  possibleStructureSets() const noexcept {
    return possibleStructureSets_;
  }

  void ensureStructuresGenerated() const;
  bool isStructureChunk(const StructureSetDefinition &set, int chunkX,
                        int chunkZ) const;
  bool hasStructureChunkInRange(std::string_view structureSetId, int chunkX,
                                int chunkZ, int range) const;
  const std::vector<ChunkTicketPos> *
  getRingPositionsFor(const StructureSetDefinition &set) const;

private:
  std::vector<ChunkTicketPos>
  generateRingPositions(const StructurePlacementConfig &placement) const;

  std::int64_t levelSeed_ = 0;
  std::int64_t concentricRingsSeed_ = 0;
  bool includeConcentricRings_ = true;
  const WorldgenRegistries &registries_;
  const BiomeSource &biomeSource_;
  const ClimateSampler &sampler_;
  std::vector<const StructureSetDefinition *> possibleStructureSets_;
  mutable std::mutex mutex_;
  mutable bool generated_ = false;
  mutable std::map<std::string, std::vector<ChunkTicketPos>, std::less<>>
      ringPositions_;
};

StructureDefinition parseStructureDefinitionJson(const picojson::value &v,
                                                 std::string_view id);
StructureSetDefinition parseStructureSetJson(const picojson::value &v,
                                             std::string_view id);

} // namespace redstone::worldgen
