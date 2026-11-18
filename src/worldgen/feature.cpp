#include "redstone/worldgen/feature.hpp"

#include "picojson.h"
#include "redstone/block_tags.hpp"
#include "redstone/worldgen/chunk_generator.hpp"
#include "redstone/worldgen/noise_chunk_generator.hpp"
#include "redstone/worldgen/noise_router.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace redstone::worldgen {

namespace feature_detail {

BlockState regionGetBlockState(const WorldGenRegion &region, int x, int y,
                               int z) noexcept {
  if (y < region.minY() || y >= region.maxY())
    return airState();
  const ChunkAccess *chunk =
      region.getChunkIfAllowed(x >> 4, z >> 4, ChunkStatusId::Empty);
  if (!chunk)
    return airState();
  return chunk->getBlockState(x, y, z);
}

FluidState regionGetFluidState(const WorldGenRegion &region, int x, int y,
                               int z) noexcept {
  if (y < region.minY() || y >= region.maxY())
    return FluidState{};
  const ChunkAccess *chunk =
      region.getChunkIfAllowed(x >> 4, z >> 4, ChunkStatusId::Empty);
  if (!chunk)
    return FluidState{};
  return chunk->getFluidState(x, y, z);
}

int regionGetHeight(const WorldGenRegion &region, HeightmapType type, int x,
                    int z) noexcept {
  const ChunkAccess *chunk =
      region.getChunkIfAllowed(x >> 4, z >> 4, ChunkStatusId::Empty);
  if (!chunk)
    return region.minY();
  // `ChunkAccess::getHeight` 返回最高占用方块 Y（`firstAvailable - 1`），
  // 而原版 `LevelReader::getHeight` / 高度图修饰器要求 `firstAvailable`（+1）。
  return const_cast<ChunkAccess *>(chunk)->getHeight(type, x, z) + 1;
}

bool regionSetBlockState(WorldGenRegion &region, int x, int y, int z,
                         const BlockState &state, int flags) {
  if (!region.ensureCanWrite(x, y, z))
    return false;
  return region.setBlockState(x, y, z, state, flags);
}

void regionMarkPosForPostprocessing(WorldGenRegion &region, int x, int y,
                                    int z) {
  if (!region.ensureCanWrite(x, y, z))
    return;
  if (ChunkAccess *chunk =
          region.getChunkIfAllowed(x >> 4, z >> 4, ChunkStatusId::Empty)) {
    chunk->markPosForPostprocessing(x, y, z);
  }
}

void regionScheduleFluidTick(WorldGenRegion &region, int x, int y, int z,
                             const Fluid &fluid, int delay) {
  if (!region.ensureCanWrite(x, y, z))
    return;
  if (ChunkAccess *chunk =
          region.getChunkIfAllowed(x >> 4, z >> 4, ChunkStatusId::Empty)) {
    if (auto *proto = dynamic_cast<ProtoChunk *>(chunk)) {
      proto->fluidTicks().push_back(
          terrain::SavedTick{std::string(fluid.name()), x, y, z, delay, 0});
    }
  }
}

bool blockMatchesFaceSturdyUp(const BlockState &s) noexcept {
  if (s.isAir() || s.block().isLiquid())
    return false;
  return s.block().metadata().conductor || s.block().isLeaves() ||
         s.is(Blocks::FARMLAND) || s.is(Blocks::SOUL_SAND) ||
         s.is(Blocks::MUD);
}

} // namespace feature_detail

namespace {

constexpr double kPi = 3.14159265358979323846;

int floorInt(double v) noexcept {
  const int i = static_cast<int>(v);
  return v < static_cast<double>(i) ? i - 1 : i;
}

int ceilInt(double v) noexcept {
  const int i = static_cast<int>(v);
  return v > static_cast<double>(i) ? i + 1 : i;
}

double lerpDouble(double t, double a, double b) noexcept {
  return a + t * (b - a);
}

std::string normId(std::string_view id) {
  if (id.find(':') != std::string_view::npos)
    return std::string(id);
  return "minecraft:" + std::string(id);
}

const picojson::object &asObj(const picojson::value &v, std::string_view ctx) {
  if (!v.is<picojson::object>())
    throw std::runtime_error("Expected object in " + std::string(ctx));
  return v.get<picojson::object>();
}

const picojson::value &req(const picojson::object &obj, const char *key,
                           std::string_view ctx) {
  auto it = obj.find(key);
  if (it == obj.end())
    throw std::runtime_error("Missing '" + std::string(key) + "' in " +
                             std::string(ctx));
  return it->second;
}

std::string asStr(const picojson::value &v, std::string_view ctx) {
  if (!v.is<std::string>())
    throw std::runtime_error("Expected string in " + std::string(ctx));
  return v.get<std::string>();
}

double asNum(const picojson::value &v, std::string_view ctx) {
  if (!v.is<double>())
    throw std::runtime_error("Expected number in " + std::string(ctx));
  return v.get<double>();
}

int asInt(const picojson::value &v, std::string_view ctx) {
  return static_cast<int>(asNum(v, ctx));
}

bool asBool(const picojson::value &v, std::string_view ctx) {
  if (!v.is<bool>())
    throw std::runtime_error("Expected bool in " + std::string(ctx));
  return v.get<bool>();
}

const PerlinSimplexNoise &biomeInfoNoise() {
  static const PerlinSimplexNoise kNoise = []() {
    LegacyRandomSource rng(2345LL);
    const std::array<int, 1> octaves{0};
    return PerlinSimplexNoise(rng, octaves);
  }();
  return kNoise;
}

HeightmapType parseHeightmapTypeStr(std::string_view s) {
  if (s == "WORLD_SURFACE_WG")
    return HeightmapType::WorldSurfaceWG;
  if (s == "WORLD_SURFACE")
    return HeightmapType::WorldSurface;
  if (s == "OCEAN_FLOOR_WG")
    return HeightmapType::OceanFloorWG;
  if (s == "OCEAN_FLOOR")
    return HeightmapType::OceanFloor;
  if (s == "MOTION_BLOCKING")
    return HeightmapType::MotionBlocking;
  if (s == "MOTION_BLOCKING_NO_LEAVES")
    return HeightmapType::MotionBlockingNoLeaves;
  return HeightmapType::WorldSurfaceWG;
}

inline BlockState safeGetBlockState(const WorldGenRegion &region, int x, int y,
                                    int z) noexcept {
  return feature_detail::regionGetBlockState(region, x, y, z);
}

inline FluidState safeGetFluidState(const WorldGenRegion &region, int x, int y,
                                    int z) noexcept {
  return feature_detail::regionGetFluidState(region, x, y, z);
}

inline int safeGetHeight(const WorldGenRegion &region, HeightmapType type,
                         int x, int z) noexcept {
  return feature_detail::regionGetHeight(region, type, x, z);
}

inline bool safeSetBlockState(WorldGenRegion &region, int x, int y, int z,
                              const BlockState &state, int flags = 2) {
  return feature_detail::regionSetBlockState(region, x, y, z, state, flags);
}

inline bool isFaceSturdyUpApprox(const BlockState &s) noexcept {
  return feature_detail::blockMatchesFaceSturdyUp(s);
}

bool canSurvivePlacement(const WorldGenRegion &region, const BlockState &state,
                         Pos pos) {
  const BlockState below = safeGetBlockState(region, pos.x, pos.y - 1, pos.z);
  if (state.is(Blocks::SUGAR_CANE)) {
    if (below.is(Blocks::SUGAR_CANE))
      return true;
    if (!blockHasTag(below, "minecraft:dirt").value_or(false) &&
        !blockHasTag(below, "minecraft:sand").value_or(false)) {
      return false;
    }
    for (Direction d : {Direction::North, Direction::South, Direction::West,
                        Direction::East}) {
      const Pos adj = pos.relative(d);
      if (safeGetFluidState(region, adj.x, adj.y - 1, adj.z)
              .is(Fluids::water())) {
        return true;
      }
    }
    return false;
  }
  if (state.is(Blocks::CACTUS)) {
    return below.is(Blocks::CACTUS) ||
           blockHasTag(below, "minecraft:sand").value_or(false);
  }
  if (state.is(Blocks::SEAGRASS) || state.is(Blocks::TALL_SEAGRASS) ||
      state.is(Blocks::KELP) || state.is(Blocks::KELP_PLANT)) {
    return isFaceSturdyUpApprox(below);
  }
  if (state.is(Blocks::DEAD_BUSH) || state.is(Blocks::SHORT_DRY_GRASS) ||
      state.is(Blocks::TALL_DRY_GRASS)) {
    return blockHasTag(below, "minecraft:dead_bush_may_place_on")
        .value_or(false);
  }
  if (blockHasTag(state, "minecraft:saplings").value_or(false) ||
      blockHasTag(state, "minecraft:flowers").value_or(false) ||
      state.is(Blocks::SHORT_GRASS) || state.is(Blocks::TALL_GRASS) ||
      state.is(Blocks::FERN) || state.is(Blocks::LARGE_FERN) ||
      state.is(Blocks::SWEET_BERRY_BUSH) || state.is(Blocks::BUSH) ||
      state.is(Blocks::FIREFLY_BUSH) || state.is(Blocks::WILDFLOWERS) ||
      state.is(Blocks::PINK_PETALS) || state.is(Blocks::LEAF_LITTER)) {
    return blockHasTag(below, "minecraft:supports_vegetation").value_or(false);
  }
  if (state.is(Blocks::CRIMSON_ROOTS) || state.is(Blocks::WARPED_ROOTS) ||
      state.is(Blocks::NETHER_SPROUTS) || state.is(Blocks::CRIMSON_FUNGUS) ||
      state.is(Blocks::WARPED_FUNGUS)) {
    return blockHasTag(below, "minecraft:nylium").value_or(false) ||
           below.is(Blocks::SOUL_SOIL) ||
           blockHasTag(below, "minecraft:dirt").value_or(false);
  }
  if (state.is(Blocks::SNOW)) {
    return isFaceSturdyUpApprox(below) &&
           !blockHasTag(below, "minecraft:cannot_support_snow_layer")
                .value_or(false);
  }
  return !below.isAir();
}

bool biomeShouldFreeze(const WorldgenRegistries &reg, std::string_view biomeId,
                       const WorldGenRegion &region, Pos pos, int seaLevel) {
  const auto *bc = reg.findBiomeClimate(biomeId);
  if (bc && bc->warmEnoughToRain(pos.x, pos.y, pos.z, seaLevel))
    return false;
  if (pos.y < region.minY() || pos.y >= region.maxY())
    return false;
  const BlockState s = safeGetBlockState(region, pos.x, pos.y, pos.z);
  const FluidState fs = safeGetFluidState(region, pos.x, pos.y, pos.z);
  return fs.isSourceOfType(Fluids::water()) && s.is(Blocks::WATER);
}

bool biomeShouldSnow(const WorldgenRegistries &reg, std::string_view biomeId,
                     const WorldGenRegion &region, Pos pos, int seaLevel) {
  const auto *bc = reg.findBiomeClimate(biomeId);
  if (bc && (!bc->hasPrecipitation ||
             bc->warmEnoughToRain(pos.x, pos.y, pos.z, seaLevel))) {
    return false;
  }
  if (pos.y < region.minY() || pos.y >= region.maxY())
    return false;
  const BlockState s = safeGetBlockState(region, pos.x, pos.y, pos.z);
  return (s.isAir() || s.is(Blocks::SNOW)) &&
         canSurvivePlacement(region, Blocks::SNOW.defaultState(), pos);
}

Pos parseOffsetArray(const picojson::object &obj, std::string_view ctx) {
  if (auto it = obj.find("offset");
      it != obj.end() && it->second.is<picojson::array>()) {
    const auto &arr = it->second.get<picojson::array>();
    if (arr.size() >= 3) {
      return Pos{asInt(arr[0], ctx), asInt(arr[1], ctx), asInt(arr[2], ctx)};
    }
  }
  return Pos{0, 0, 0};
}

std::vector<std::string> parseStringOrArrayIds(const picojson::value &v,
                                               std::string_view ctx) {
  std::vector<std::string> out;
  if (v.is<std::string>()) {
    out.push_back(normId(v.get<std::string>()));
  } else if (v.is<picojson::array>()) {
    for (const auto &el : v.get<picojson::array>())
      out.push_back(normId(asStr(el, ctx)));
  }
  return out;
}

RulePredicate parseOreRulePredicate(const picojson::value &v,
                                    std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  RulePredicate pred;
  const std::string type =
      normId(asStr(req(obj, "predicate_type", ctx), ctx));
  if (type == "minecraft:tag_match") {
    pred.kind = RulePredicate::Kind::TagMatch;
    pred.blockOrTag = normId(asStr(req(obj, "tag", ctx), ctx));
  } else if (type == "minecraft:block_match") {
    pred.kind = RulePredicate::Kind::BlockMatch;
    pred.blockOrTag = normId(asStr(req(obj, "block", ctx), ctx));
  } else if (type == "minecraft:blockstate_match" ||
             type == "minecraft:block_state_match") {
    pred.kind = RulePredicate::Kind::BlockStateMatch;
    pred.matchState = parseBlockStateJson(req(obj, "block_state", ctx), ctx);
  } else if (type == "minecraft:random_block_match") {
    pred.kind = RulePredicate::Kind::RandomBlockMatch;
    pred.blockOrTag = normId(asStr(req(obj, "block", ctx), ctx));
    pred.probability =
        static_cast<float>(asNum(req(obj, "probability", ctx), ctx));
  } else if (type == "minecraft:random_blockstate_match" ||
             type == "minecraft:random_block_state_match") {
    pred.kind = RulePredicate::Kind::RandomBlockStateMatch;
    pred.matchState = parseBlockStateJson(req(obj, "block_state", ctx), ctx);
    pred.probability =
        static_cast<float>(asNum(req(obj, "probability", ctx), ctx));
  }
  return pred;
}

BlockPredicate parseBlockPredicateJson(const picojson::value &v,
                                       std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  BlockPredicate pred;
  pred.offset = parseOffsetArray(obj, ctx);
  const std::string type = normId(asStr(req(obj, "type", ctx), ctx));
  if (type == "minecraft:true") {
    pred.kind = BlockPredicate::Kind::AlwaysTrue;
  } else if (type == "minecraft:all_of") {
    pred.kind = BlockPredicate::Kind::AllOf;
    for (const auto &c : req(obj, "predicates", ctx).get<picojson::array>())
      pred.children.push_back(parseBlockPredicateJson(c, ctx));
  } else if (type == "minecraft:any_of") {
    pred.kind = BlockPredicate::Kind::AnyOf;
    for (const auto &c : req(obj, "predicates", ctx).get<picojson::array>())
      pred.children.push_back(parseBlockPredicateJson(c, ctx));
  } else if (type == "minecraft:not") {
    pred.kind = BlockPredicate::Kind::Not;
    pred.children.push_back(
        parseBlockPredicateJson(req(obj, "predicate", ctx), ctx));
  } else if (type == "minecraft:matching_blocks") {
    pred.kind = BlockPredicate::Kind::MatchingBlocks;
    pred.ids = parseStringOrArrayIds(req(obj, "blocks", ctx), ctx);
  } else if (type == "minecraft:matching_block_tag") {
    pred.kind = BlockPredicate::Kind::MatchingBlockTag;
    pred.tag = normId(asStr(req(obj, "tag", ctx), ctx));
  } else if (type == "minecraft:matching_fluids") {
    pred.kind = BlockPredicate::Kind::MatchingFluids;
    pred.ids = parseStringOrArrayIds(req(obj, "fluids", ctx), ctx);
  } else if (type == "minecraft:has_sturdy_face") {
    pred.kind = BlockPredicate::Kind::HasSturdyFace;
    if (auto it = obj.find("direction");
        it != obj.end() && it->second.is<std::string>()) {
      pred.direction = directionFromString(it->second.get<std::string>());
    }
  } else if (type == "minecraft:solid") {
    pred.kind = BlockPredicate::Kind::Solid;
  } else if (type == "minecraft:replaceable") {
    pred.kind = BlockPredicate::Kind::Replaceable;
  } else if (type == "minecraft:would_survive") {
    pred.kind = BlockPredicate::Kind::WouldSurvive;
    pred.matchState = parseBlockStateJson(req(obj, "state", ctx), ctx);
  } else if (type == "minecraft:inside_world_bounds") {
    pred.kind = BlockPredicate::Kind::InsideWorldBounds;
  } else if (type == "minecraft:unobstructed") {
    pred.kind = BlockPredicate::Kind::Unobstructed;
  }
  return pred;
}

BlockStateProvider parseBlockStateProviderJson(const picojson::value &v,
                                               std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  BlockStateProvider p;
  const std::string type = normId(asStr(req(obj, "type", ctx), ctx));
  if (type == "minecraft:simple_state_provider") {
    p.kind = BlockStateProvider::Kind::Simple;
    p.simpleState = parseBlockStateJson(req(obj, "state", ctx), ctx);
  } else if (type == "minecraft:weighted_state_provider") {
    p.kind = BlockStateProvider::Kind::Weighted;
    for (const auto &el : req(obj, "entries", ctx).get<picojson::array>()) {
      const auto &eo = asObj(el, ctx);
      BlockStateProvider::WeightedEntry we;
      we.weight = asInt(req(eo, "weight", ctx), ctx);
      we.state = parseBlockStateJson(req(eo, "data", ctx), ctx);
      p.weighted.push_back(std::move(we));
    }
  } else if (type == "minecraft:rule_based_state_provider") {
    p.kind = BlockStateProvider::Kind::RuleBased;
    if (auto it = obj.find("fallback"); it != obj.end()) {
      p.fallback = std::make_shared<BlockStateProvider>(
          parseBlockStateProviderJson(it->second, ctx));
    }
    if (auto it = obj.find("rules");
        it != obj.end() && it->second.is<picojson::array>()) {
      for (const auto &r : it->second.get<picojson::array>()) {
        const auto &ro = asObj(r, ctx);
        BlockStateProvider::RuleEntry re;
        re.ifTrue = parseBlockPredicateJson(req(ro, "if_true", ctx), ctx);
        re.thenProvider = std::make_shared<BlockStateProvider>(
            parseBlockStateProviderJson(req(ro, "then", ctx), ctx));
        p.rules.push_back(std::move(re));
      }
    }
  } else if (type == "minecraft:rotated_block_provider") {
    p.kind = BlockStateProvider::Kind::RotatedBlock;
    p.simpleState = parseBlockStateJson(req(obj, "state", ctx), ctx);
  } else if (type == "minecraft:noise_provider" ||
             type == "minecraft:noise_threshold_provider" ||
             type == "minecraft:dual_noise_provider") {
    auto parseNoiseParams = [&](const picojson::value &nv) {
      const auto &no = asObj(nv, ctx);
      NoiseParameters np;
      np.firstOctave = asInt(req(no, "firstOctave", ctx), ctx);
      for (const auto &av : req(no, "amplitudes", ctx).get<picojson::array>())
        np.amplitudes.push_back(asNum(av, ctx));
      return np;
    };
    auto buildNormalNoise = [&](std::int64_t seed, const NoiseParameters &np) {
      WorldgenRandom rng = WorldgenRandom::legacy(seed);
      return std::make_shared<NormalNoise>(NormalNoise::create(rng, np));
    };

    p.noiseSeed = static_cast<std::int64_t>(asNum(req(obj, "seed", ctx), ctx));
    p.scale = static_cast<double>(
        static_cast<float>(asNum(req(obj, "scale", ctx), ctx)));
    if (auto it = obj.find("noise"); it != obj.end())
      p.noiseParams = parseNoiseParams(it->second);
    p.normalNoise = buildNormalNoise(p.noiseSeed, p.noiseParams);

    if (type == "minecraft:noise_provider") {
      p.kind = BlockStateProvider::Kind::Noise;
      for (const auto &s : req(obj, "states", ctx).get<picojson::array>())
        p.states.push_back(parseBlockStateJson(s, ctx));
    } else if (type == "minecraft:noise_threshold_provider") {
      p.kind = BlockStateProvider::Kind::NoiseThreshold;
      p.threshold = static_cast<double>(
          static_cast<float>(asNum(req(obj, "threshold", ctx), ctx)));
      p.highChance = static_cast<double>(
          static_cast<float>(asNum(req(obj, "high_chance", ctx), ctx)));
      p.defaultStateVal =
          parseBlockStateJson(req(obj, "default_state", ctx), ctx);
      for (const auto &s : req(obj, "high_states", ctx).get<picojson::array>())
        p.highStates.push_back(parseBlockStateJson(s, ctx));
      for (const auto &s : req(obj, "low_states", ctx).get<picojson::array>())
        p.lowStates.push_back(parseBlockStateJson(s, ctx));
    } else {
      p.kind = BlockStateProvider::Kind::DualNoise;
      for (const auto &s : req(obj, "states", ctx).get<picojson::array>())
        p.states.push_back(parseBlockStateJson(s, ctx));
      if (auto it = obj.find("slow_scale"); it != obj.end()) {
        p.slowScale = static_cast<double>(static_cast<float>(asNum(it->second, ctx)));
      }
      if (auto it = obj.find("slow_noise"); it != obj.end()) {
        p.slowNoiseParams = parseNoiseParams(it->second);
      }
      p.slowNormalNoise = buildNormalNoise(p.noiseSeed, p.slowNoiseParams);
      if (auto it = obj.find("variety"); it != obj.end()) {
        if (it->second.is<picojson::array>()) {
          const auto &arr = it->second.get<picojson::array>();
          if (arr.size() >= 2) {
            p.varietyMin = asInt(arr[0], ctx);
            p.varietyMax = asInt(arr[1], ctx);
          }
        } else if (it->second.is<picojson::object>()) {
          const auto &vo = it->second.get<picojson::object>();
          p.varietyMin = asInt(req(vo, "min_inclusive", ctx), ctx);
          p.varietyMax = asInt(req(vo, "max_inclusive", ctx), ctx);
        } else if (it->second.is<double>()) {
          p.varietyMin = p.varietyMax = asInt(it->second, ctx);
        }
      }
    }
  } else if (type == "minecraft:randomized_int_state_provider") {
    p.kind = BlockStateProvider::Kind::RandomizedInt;
    p.randomizedProperty = asStr(req(obj, "property", ctx), ctx);
    p.fallback = std::make_shared<BlockStateProvider>(
        parseBlockStateProviderJson(req(obj, "source", ctx), ctx));
    p.randomizedValues = parseIntProviderJson(req(obj, "values", ctx), ctx);
  }
  return p;
}

PlacedFeaturePtr parsePlacedRefOrInline(const picojson::value &v,
                                        std::string_view ctx) {
  if (v.is<std::string>()) {
    auto pf = std::make_shared<PlacedFeature>();
    pf->id = normId(v.get<std::string>());
    return pf;
  }
  auto pf = std::const_pointer_cast<PlacedFeature>(
      parsePlacedFeatureJson(v, ctx));
  // Inline definitions must not retain the outer feature's context id; otherwise
  // registry lookup resolves back into the enclosing PlacedFeature and recurses.
  pf->id.clear();
  return pf;
}

PlacementModifier parsePlacementModifierJson(const picojson::value &v,
                                             std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  PlacementModifier m;
  const std::string type = normId(asStr(req(obj, "type", ctx), ctx));
  if (type == "minecraft:biome") {
    m.kind = PlacementModifier::Kind::Biome;
  } else if (type == "minecraft:in_square") {
    m.kind = PlacementModifier::Kind::InSquare;
  } else if (type == "minecraft:count") {
    m.kind = PlacementModifier::Kind::Count;
    m.countProvider = parseIntProviderJson(req(obj, "count", ctx), ctx);
  } else if (type == "minecraft:count_on_every_layer") {
    m.kind = PlacementModifier::Kind::CountOnEveryLayer;
    m.countProvider = parseIntProviderJson(req(obj, "count", ctx), ctx);
  } else if (type == "minecraft:height_range") {
    m.kind = PlacementModifier::Kind::HeightRange;
    m.heightProvider = parseHeightProviderJson(req(obj, "height", ctx), ctx);
  } else if (type == "minecraft:heightmap") {
    m.kind = PlacementModifier::Kind::Heightmap;
    m.heightmap =
        parseHeightmapTypeStr(asStr(req(obj, "heightmap", ctx), ctx));
  } else if (type == "minecraft:random_offset") {
    m.kind = PlacementModifier::Kind::RandomOffset;
    m.xzSpread = parseIntProviderJson(req(obj, "xz_spread", ctx), ctx);
    m.ySpread = parseIntProviderJson(req(obj, "y_spread", ctx), ctx);
  } else if (type == "minecraft:rarity_filter") {
    m.kind = PlacementModifier::Kind::RarityFilter;
    m.rarityChance = std::max(1, asInt(req(obj, "chance", ctx), ctx));
  } else if (type == "minecraft:surface_water_depth_filter") {
    m.kind = PlacementModifier::Kind::SurfaceWaterDepthFilter;
    m.maxWaterDepth = asInt(req(obj, "max_water_depth", ctx), ctx);
  } else if (type == "minecraft:surface_relative_threshold_filter") {
    m.kind = PlacementModifier::Kind::SurfaceRelativeThresholdFilter;
    m.heightmap =
        parseHeightmapTypeStr(asStr(req(obj, "heightmap", ctx), ctx));
    if (auto it = obj.find("min_inclusive"); it != obj.end())
      m.minInclusive = asInt(it->second, ctx);
    if (auto it = obj.find("max_inclusive"); it != obj.end())
      m.maxInclusive = asInt(it->second, ctx);
  } else if (type == "minecraft:block_predicate_filter") {
    m.kind = PlacementModifier::Kind::BlockPredicateFilter;
    m.predicate =
        parseBlockPredicateJson(req(obj, "predicate", ctx), ctx);
  } else if (type == "minecraft:environment_scan") {
    m.kind = PlacementModifier::Kind::EnvironmentScan;
    m.searchDir =
        directionFromString(asStr(req(obj, "direction_of_search", ctx), ctx));
    m.targetCondition =
        parseBlockPredicateJson(req(obj, "target_condition", ctx), ctx);
    if (auto it = obj.find("allowed_search_condition"); it != obj.end()) {
      m.allowedSearchCondition = parseBlockPredicateJson(it->second, ctx);
    }
    m.maxSteps = asInt(req(obj, "max_steps", ctx), ctx);
  } else if (type == "minecraft:noise_based_count") {
    m.kind = PlacementModifier::Kind::NoiseBasedCount;
    m.noiseToCountRatio = asInt(req(obj, "noise_to_count_ratio", ctx), ctx);
    m.noiseFactor = asNum(req(obj, "noise_factor", ctx), ctx);
    if (auto it = obj.find("noise_offset"); it != obj.end())
      m.noiseOffset = asNum(it->second, ctx);
  } else if (type == "minecraft:noise_threshold_count") {
    m.kind = PlacementModifier::Kind::NoiseThresholdCount;
    m.noiseLevel = asNum(req(obj, "noise_level", ctx), ctx);
    m.belowNoise = asInt(req(obj, "below_noise", ctx), ctx);
    m.aboveNoise = asInt(req(obj, "above_noise", ctx), ctx);
  } else if (type == "minecraft:fixed_placement") {
    m.kind = PlacementModifier::Kind::FixedPlacement;
    for (const auto &pv : req(obj, "positions", ctx).get<picojson::array>()) {
      const auto &arr = pv.get<picojson::array>();
      if (arr.size() >= 3) {
        m.fixedPositions.push_back(
            Pos{asInt(arr[0], ctx), asInt(arr[1], ctx), asInt(arr[2], ctx)});
      }
    }
  }
  return m;
}

bool placeOreCore(const ConfiguredFeature &feat, WorldGenRegion &region,
                  RandomSource &rng, Pos origin) {
  const float dir = rng.nextFloat() * static_cast<float>(kPi);
  const float spreadXY = static_cast<float>(feat.oreSize) / 8.0f;
  const int maxRadius =
      ceilInt((static_cast<double>(feat.oreSize) / 16.0 * 2.0 + 1.0) / 2.0);
  const double sinD = std::sin(dir);
  const double cosD = std::cos(dir);
  const double x0 = static_cast<double>(origin.x) + sinD * spreadXY;
  const double x1 = static_cast<double>(origin.x) - sinD * spreadXY;
  const double z0 = static_cast<double>(origin.z) + cosD * spreadXY;
  const double z1 = static_cast<double>(origin.z) - cosD * spreadXY;
  const double y0 = static_cast<double>(origin.y + rng.nextInt(3) - 2);
  const double y1 = static_cast<double>(origin.y + rng.nextInt(3) - 2);
  const int xStart = origin.x - ceilInt(spreadXY) - maxRadius;
  const int yStart = origin.y - 2 - maxRadius;
  const int zStart = origin.z - ceilInt(spreadXY) - maxRadius;
  const int sizeXZ = 2 * (ceilInt(spreadXY) + maxRadius);
  const int sizeY = 2 * (2 + maxRadius);

  bool reachable = false;
  for (int px = xStart; px <= xStart + sizeXZ && !reachable; ++px) {
    for (int pz = zStart; pz <= zStart + sizeXZ; ++pz) {
      if (yStart <= safeGetHeight(region, HeightmapType::OceanFloorWG, px, pz)) {
        reachable = true;
        break;
      }
    }
  }
  if (!reachable || feat.oreSize <= 0)
    return false;

  std::vector<double> data(static_cast<std::size_t>(feat.oreSize) * 4);
  for (int i = 0; i < feat.oreSize; ++i) {
    const float step =
        static_cast<float>(i) / static_cast<float>(feat.oreSize);
    const double xx = lerpDouble(step, x0, x1);
    const double yy = lerpDouble(step, y0, y1);
    const double zz = lerpDouble(step, z0, z1);
    const double ss = rng.nextDouble() * static_cast<double>(feat.oreSize) / 16.0;
    const double r =
        ((std::sin(static_cast<float>(kPi) * step) + 1.0f) * ss + 1.0) / 2.0;
    data[i * 4 + 0] = xx;
    data[i * 4 + 1] = yy;
    data[i * 4 + 2] = zz;
    data[i * 4 + 3] = r;
  }

  for (int i1 = 0; i1 < feat.oreSize - 1; ++i1) {
    if (data[i1 * 4 + 3] <= 0.0)
      continue;
    for (int i2 = i1 + 1; i2 < feat.oreSize; ++i2) {
      if (data[i2 * 4 + 3] <= 0.0)
        continue;
      const double dx = data[i1 * 4 + 0] - data[i2 * 4 + 0];
      const double dy = data[i1 * 4 + 1] - data[i2 * 4 + 1];
      const double dz = data[i1 * 4 + 2] - data[i2 * 4 + 2];
      const double dr = data[i1 * 4 + 3] - data[i2 * 4 + 3];
      if (dr * dr > dx * dx + dy * dy + dz * dz) {
        if (dr > 0.0)
          data[i2 * 4 + 3] = -1.0;
        else
          data[i1 * 4 + 3] = -1.0;
      }
    }
  }

  std::vector<bool> tested(
      static_cast<std::size_t>(std::max(1, sizeXZ) * std::max(1, sizeY) *
                               std::max(1, sizeXZ)),
      false);
  int placed = 0;
  for (int i = 0; i < feat.oreSize; ++i) {
    const double r = data[i * 4 + 3];
    if (r < 0.0)
      continue;
    const double xx = data[i * 4 + 0];
    const double yy = data[i * 4 + 1];
    const double zz = data[i * 4 + 2];
    const int xMin = std::max(floorInt(xx - r), xStart);
    const int yMin = std::max(floorInt(yy - r), yStart);
    const int zMin = std::max(floorInt(zz - r), zStart);
    const int xMax = std::max(floorInt(xx + r), xMin);
    const int yMax = std::max(floorInt(yy + r), yMin);
    const int zMax = std::max(floorInt(zz + r), zMin);

    for (int x = xMin; x <= xMax; ++x) {
      const double xd = (static_cast<double>(x) + 0.5 - xx) / r;
      if (xd * xd >= 1.0)
        continue;
      for (int y = yMin; y <= yMax; ++y) {
        const double yd = (static_cast<double>(y) + 0.5 - yy) / r;
        if (xd * xd + yd * yd >= 1.0)
          continue;
        for (int z = zMin; z <= zMax; ++z) {
          const double zd = (static_cast<double>(z) + 0.5 - zz) / r;
          if (xd * xd + yd * yd + zd * zd >= 1.0)
            continue;
          if (y < region.minY() || y >= region.maxY())
            continue;
          const std::size_t bIdx =
              static_cast<std::size_t>((x - xStart) + (y - yStart) * sizeXZ +
                                       (z - zStart) * sizeXZ * sizeY);
          if (bIdx < tested.size() && !tested[bIdx]) {
            tested[bIdx] = true;
            if (!region.ensureCanWrite(x, y, z))
              continue;
            const BlockState cur = safeGetBlockState(region, x, y, z);
            for (const auto &t : feat.oreTargets) {
              if (!t.target.test(cur, rng))
                continue;
              bool skipAir = true;
              if (feat.discardChanceOnAirExposure >= 1.0f) {
                skipAir = false;
              } else if (feat.discardChanceOnAirExposure > 0.0f) {
                skipAir = rng.nextFloat() >= feat.discardChanceOnAirExposure;
              }
              if (!skipAir) {
                bool adjAir = false;
                for (Direction d :
                     {Direction::Down, Direction::Up, Direction::North,
                      Direction::South, Direction::West, Direction::East}) {
                  const Pos np = Pos{x, y, z}.relative(d);
                  if (safeGetBlockState(region, np.x, np.y, np.z).isAir()) {
                    adjAir = true;
                    break;
                  }
                }
                if (adjAir)
                  continue;
              }
              safeSetBlockState(region, x, y, z, t.state, 2);
              ++placed;
              break;
            }
          }
        }
      }
    }
  }
  return placed > 0;
}

} // namespace

bool BlockPredicate::test(const WorldGenRegion &region, Pos pos) const {
  const Pos p{pos.x + offset.x, pos.y + offset.y, pos.z + offset.z};
  switch (kind) {
  case Kind::AlwaysTrue:
    return true;
  case Kind::AllOf:
    for (const auto &c : children)
      if (!c.test(region, pos))
        return false;
    return true;
  case Kind::AnyOf:
    for (const auto &c : children)
      if (c.test(region, pos))
        return true;
    return false;
  case Kind::Not:
    return children.empty() ? false : !children.front().test(region, pos);
  case Kind::MatchingBlocks: {
    const BlockState s = safeGetBlockState(region, p.x, p.y, p.z);
    for (const auto &id : ids)
      if (s.nameView() == id)
        return true;
    return false;
  }
  case Kind::MatchingBlockTag: {
    const BlockState s = safeGetBlockState(region, p.x, p.y, p.z);
    return blockHasTag(s, tag).value_or(false);
  }
  case Kind::MatchingFluids: {
    const FluidState fs = safeGetFluidState(region, p.x, p.y, p.z);
    const std::string_view fname = fs.getType().name();
    for (const auto &id : ids)
      if (fname == id)
        return true;
    return false;
  }
  case Kind::HasSturdyFace: {
    const BlockState s = safeGetBlockState(region, p.x, p.y, p.z);
    return isFaceSturdyUpApprox(s);
  }
  case Kind::Solid: {
    const BlockState s = safeGetBlockState(region, p.x, p.y, p.z);
    return !s.isAir() && !s.block().isLiquid() && s.block().metadata().conductor;
  }
  case Kind::Replaceable: {
    const BlockState s = safeGetBlockState(region, p.x, p.y, p.z);
    return s.isAir() || s.block().metadata().replaceable;
  }
  case Kind::WouldSurvive:
    return canSurvivePlacement(region, matchState, p);
  case Kind::InsideWorldBounds:
    return p.y >= region.minY() && p.y < region.maxY();
  case Kind::Unobstructed:
    return safeGetBlockState(region, p.x, p.y, p.z).isAir();
  }
  return false;
}

BlockState BlockStateProvider::getState(RandomSource &rng, Pos pos) const {
  switch (kind) {
  case Kind::Simple:
    return simpleState;
  case Kind::Weighted: {
    int total = 0;
    for (const auto &w : weighted)
      total += w.weight;
    if (total <= 0)
      return simpleState;
    int r = rng.nextInt(total);
    for (const auto &w : weighted) {
      r -= w.weight;
      if (r < 0)
        return w.state;
    }
    return weighted.back().state;
  }
  case Kind::RotatedBlock: {
    if (!simpleState.hasProperty("axis"))
      return simpleState;
    const int ax = rng.nextInt(3);
    const char *axName = (ax == 0) ? "x" : (ax == 1 ? "y" : "z");
    return simpleState.with("axis", axName);
  }
  case Kind::Noise: {
    if (states.empty())
      return simpleState;
    const double nv =
        normalNoise
            ? normalNoise->getValue(pos.x * scale, pos.y * scale, pos.z * scale)
            : 0.0;
    const double pv = std::clamp((1.0 + nv) * 0.5, 0.0, 0.9999);
    return states[static_cast<std::size_t>(pv * states.size())];
  }
  case Kind::DualNoise: {
    if (states.empty())
      return simpleState;
    auto slowAt = [&](int ox, int oy, int oz) {
      return slowNormalNoise
                 ? slowNormalNoise->getValue((pos.x + ox) * slowScale,
                                             (pos.y + oy) * slowScale,
                                             (pos.z + oz) * slowScale)
                 : 0.0;
    };
    const double varietyNoise = slowAt(0, 0, 0);
    const double t = std::clamp((varietyNoise - (-1.0)) / 2.0, 0.0, 1.0);
    const int localVariety = static_cast<int>(
        static_cast<double>(varietyMin) +
        t * static_cast<double>((varietyMax + 1) - varietyMin));
    std::vector<BlockState> possible;
    possible.reserve(static_cast<std::size_t>(std::max(1, localVariety)));
    for (int i = 0; i < localVariety; ++i) {
      const double sn = slowAt(i * 54545, 0, i * 34234);
      const double pv = std::clamp((1.0 + sn) * 0.5, 0.0, 0.9999);
      possible.push_back(states[static_cast<std::size_t>(pv * states.size())]);
    }
    if (possible.empty())
      return simpleState;
    const double nv =
        normalNoise
            ? normalNoise->getValue(pos.x * scale, pos.y * scale, pos.z * scale)
            : 0.0;
    const double pv = std::clamp((1.0 + nv) * 0.5, 0.0, 0.9999);
    return possible[static_cast<std::size_t>(pv * possible.size())];
  }
  case Kind::NoiseThreshold: {
    const double nv =
        normalNoise
            ? normalNoise->getValue(pos.x * scale, pos.y * scale, pos.z * scale)
            : 0.0;
    if (nv < threshold) {
      return lowStates.empty()
                 ? defaultStateVal
                 : lowStates[rng.nextInt(static_cast<int>(lowStates.size()))];
    }
    return (rng.nextFloat() < static_cast<float>(highChance) &&
            !highStates.empty())
               ? highStates[rng.nextInt(static_cast<int>(highStates.size()))]
               : defaultStateVal;
  }
  case Kind::RuleBased:
    return fallback ? fallback->getState(rng, pos) : simpleState;
  case Kind::RandomizedInt: {
    BlockState base = fallback ? fallback->getState(rng, pos) : simpleState;
    if (!randomizedProperty.empty() && base.hasProperty(randomizedProperty)) {
      const int val = randomizedValues.sample(rng);
      const std::string valStr = std::to_string(val);
      return base.with(randomizedProperty, valStr);
    }
    return base;
  }
  }
  return simpleState;
}

std::optional<BlockState>
BlockStateProvider::getOptionalState(const WorldGenRegion &region,
                                     RandomSource &rng, Pos pos) const {
  if (kind == Kind::RuleBased) {
    for (const auto &r : rules) {
      if (r.ifTrue.test(region, pos)) {
        return r.thenProvider ? r.thenProvider->getState(rng, pos)
                               : std::optional<BlockState>{};
      }
    }
    if (fallback)
      return fallback->getState(rng, pos);
    return std::nullopt;
  }
  return getState(rng, pos);
}

namespace {

const PlacedFeature *resolvePlacedRef(const PlacedFeaturePtr &ref,
                                      const WorldgenRegistries &regs) {
  if (!ref)
    return nullptr;
  if (ref->inlineFeature != nullptr || !ref->featureRefId.empty() ||
      ref->id.empty()) {
    return ref.get();
  }
  return regs.findPlacedFeature(ref->id);
}

} // namespace

bool ConfiguredFeature::place(WorldGenRegion &region,
                              const NoiseBasedChunkGenerator &generator,
                              std::int64_t worldSeed, RandomSource &rng,
                              Pos origin) const {
  const auto &regs = generator.registries();
  switch (kind) {
  case Kind::Ore:
    return placeOreCore(*this, region, rng, origin);

  case Kind::ScatteredOre: {
    const int tries = rng.nextInt(oreSize + 1);
    for (int i = 0; i < tries; ++i) {
      const int md = std::min(i, 7);
      auto randAxis = [&]() {
        const float a = rng.nextFloat();
        const float b = rng.nextFloat();
        return static_cast<int>(std::lround((a - b) * md));
      };
      const int dx = randAxis();
      const int dy = randAxis();
      const int dz = randAxis();
      const Pos tp{origin.x + dx, origin.y + dy, origin.z + dz};
      if (!region.ensureCanWrite(tp.x, tp.y, tp.z))
        continue;
      const BlockState cur = safeGetBlockState(region, tp.x, tp.y, tp.z);
      for (const auto &t : oreTargets) {
        if (!t.target.test(cur, rng))
          continue;
        safeSetBlockState(region, tp.x, tp.y, tp.z, t.state, 2);
        break;
      }
    }
    return true;
  }

  case Kind::SpringFeature: {
    auto isValid = [&](const BlockState &s) {
      for (const auto &id : validBlocks)
        if (s.nameView() == id)
          return true;
      return false;
    };
    if (!isValid(safeGetBlockState(region, origin.x, origin.y + 1, origin.z)))
      return false;
    if (requiresBlockBelow &&
        !isValid(safeGetBlockState(region, origin.x, origin.y - 1, origin.z))) {
      return false;
    }
    const BlockState cur =
        safeGetBlockState(region, origin.x, origin.y, origin.z);
    if (!cur.isAir() && !isValid(cur))
      return false;
    int rocks = 0, holes = 0;
    for (Direction d : {Direction::West, Direction::East, Direction::North,
                        Direction::South, Direction::Down}) {
      const Pos np = origin.relative(d);
      const BlockState ns = safeGetBlockState(region, np.x, np.y, np.z);
      if (isValid(ns))
        ++rocks;
      if (ns.isAir())
        ++holes;
    }
    if (rocks == rockCount && holes == holeCount) {
      safeSetBlockState(region, origin.x, origin.y, origin.z, springState, 2);
      return true;
    }
    return false;
  }

  case Kind::Disk: {
    bool placedAny = false;
    const int top = origin.y + diskHalfHeight;
    const int bottom = origin.y - diskHalfHeight - 1;
    const int r = diskRadius.sample(rng);
    for (int dx = -r; dx <= r; ++dx) {
      for (int dz = -r; dz <= r; ++dz) {
        if (dx * dx + dz * dz > r * r)
          continue;
        for (int y = top; y > bottom; --y) {
          const Pos cp{origin.x + dx, y, origin.z + dz};
          if (targetPredicate.test(region, cp)) {
            if (auto st = stateProvider.getOptionalState(region, rng, cp)) {
              safeSetBlockState(region, cp.x, cp.y, cp.z, *st, 2);
              placedAny = true;
            }
          }
        }
      }
    }
    return placedAny;
  }

  case Kind::SimpleBlock: {
    const auto stOpt = stateProvider.getOptionalState(region, rng, origin);
    if (!stOpt)
      return false;
    const BlockState st = *stOpt;
    if (!canSurvivePlacement(region, st, origin))
      return false;
    if (st.hasProperty("half")) {
      if (!safeGetBlockState(region, origin.x, origin.y + 1, origin.z).isAir())
        return false;
      safeSetBlockState(region, origin.x, origin.y, origin.z,
                        st.with("half", "lower"), 2);
      safeSetBlockState(region, origin.x, origin.y + 1, origin.z,
                        st.with("half", "upper"), 2);
    } else {
      safeSetBlockState(region, origin.x, origin.y, origin.z, st, 2);
    }
    return true;
  }

  case Kind::RandomSelector: {
    for (const auto &entry : weightedFeatures) {
      if (rng.nextFloat() < entry.chance) {
        const PlacedFeature *pf = resolvePlacedRef(entry.feature, regs);
        return pf ? pf->place(region, generator, worldSeed, rng, origin, false)
                  : false;
      }
    }
    const PlacedFeature *pf = resolvePlacedRef(defaultFeature, regs);
    return pf ? pf->place(region, generator, worldSeed, rng, origin, false)
              : false;
  }

  case Kind::SimpleRandomSelector: {
    if (randomFeatures.empty())
      return false;
    const int idx = rng.nextInt(static_cast<int>(randomFeatures.size()));
    const PlacedFeature *pf =
        resolvePlacedRef(randomFeatures[static_cast<std::size_t>(idx)], regs);
    return pf ? pf->place(region, generator, worldSeed, rng, origin, false)
              : false;
  }

  case Kind::RandomBooleanSelector: {
    const bool b = rng.nextBoolean();
    const PlacedFeature *pf =
        resolvePlacedRef(b ? featureTrue : featureFalse, regs);
    return pf ? pf->place(region, generator, worldSeed, rng, origin, false)
              : false;
  }

  case Kind::BlockColumn: {
    const int n = static_cast<int>(layers.size());
    std::vector<int> heights(static_cast<std::size_t>(n), 0);
    int totalH = 0;
    for (int i = 0; i < n; ++i) {
      heights[i] = layers[i].height.sample(rng);
      totalH += heights[i];
    }
    if (totalH == 0)
      return false;
    Pos nextPos = origin.relative(columnDir);
    for (int y = 0; y < totalH; ++y) {
      if (!allowedPlacement.test(region, nextPos)) {
        int rem = totalH - y;
        const int step = prioritizeTip ? 1 : -1;
        for (int idx = prioritizeTip ? 0 : n - 1;
             idx >= 0 && idx < n && rem > 0; idx += step) {
          const int cut = std::min(heights[idx], rem);
          heights[idx] -= cut;
          rem -= cut;
        }
        break;
      }
      nextPos = nextPos.relative(columnDir);
    }
    Pos placePos = origin;
    for (int i = 0; i < n; ++i) {
      for (int y = 0; y < heights[i]; ++y) {
        safeSetBlockState(region, placePos.x, placePos.y, placePos.z,
                          layers[i].provider.getState(rng, placePos), 2);
        placePos = placePos.relative(columnDir);
      }
    }
    return true;
  }

  case Kind::BlockPile: {
    if (origin.y < region.minY() + 5)
      return false;
    const int xr = 2 + rng.nextInt(2);
    const int zr = 2 + rng.nextInt(2);
    for (int y = 0; y <= 1; ++y) {
      for (int x = -xr; x <= xr; ++x) {
        for (int z = -zr; z <= zr; ++z) {
          const Pos bp{origin.x + x, origin.y + y, origin.z + z};
          const float r1 = rng.nextFloat();
          const float r2 = rng.nextFloat();
          bool placeHere =
              static_cast<float>(x * x + z * z) <= r1 * 10.0f - r2 * 6.0f;
          if (!placeHere)
            placeHere = (rng.nextFloat() < 0.031f);
          if (placeHere && safeGetBlockState(region, bp.x, bp.y, bp.z).isAir()) {
            const BlockState below =
                safeGetBlockState(region, bp.x, bp.y - 1, bp.z);
            const bool may = below.is(Blocks::DIRT_PATH)
                                 ? rng.nextBoolean()
                                 : isFaceSturdyUpApprox(below);
            if (may) {
              safeSetBlockState(region, bp.x, bp.y, bp.z,
                                stateProvider.getState(rng, bp), 260);
            }
          }
        }
      }
    }
    return true;
  }

  case Kind::VegetationPatch:
  case Kind::WaterloggedVegetationPatch: {
    const int xRad = xzRadius.sample(rng) + 1;
    const int zRad = xzRadius.sample(rng) + 1;
    const Direction inwards = surfaceCeiling ? Direction::Up : Direction::Down;
    const Direction outwards = opposite(inwards);
    std::vector<Pos> groundSurface;
    for (int dx = -xRad; dx <= xRad; ++dx) {
      const bool xEdge = (dx == -xRad || dx == xRad);
      for (int dz = -zRad; dz <= zRad; ++dz) {
        const bool zEdge = (dz == -zRad || dz == zRad);
        const bool edge = xEdge || zEdge;
        const bool corner = xEdge && zEdge;
        if (corner ||
            (edge && (extraEdgeColumnChance == 0.0f ||
                      rng.nextFloat() > extraEdgeColumnChance))) {
          continue;
        }
        Pos p{origin.x + dx, origin.y, origin.z + dz};
        for (int off = 0; off < verticalRange &&
                          safeGetBlockState(region, p.x, p.y, p.z).isAir();
             ++off) {
          p = p.relative(inwards);
        }
        for (int off = 0; off < verticalRange &&
                          !safeGetBlockState(region, p.x, p.y, p.z).isAir();
             ++off) {
          p = p.relative(outwards);
        }
        const Pos below = p.relative(inwards);
        const BlockState belowS =
            safeGetBlockState(region, below.x, below.y, below.z);
        if (safeGetBlockState(region, p.x, p.y, p.z).isAir() &&
            isFaceSturdyUpApprox(belowS)) {
          const int baseD = patchDepth.sample(rng);
          const int extraD =
              (extraBottomBlockChance > 0.0f &&
               rng.nextFloat() < extraBottomBlockChance)
                  ? 1
                  : 0;
          const int depth = baseD + extraD;
          bool replaced = true;
          Pos cur = below;
          for (int i = 0; i < depth; ++i) {
            const BlockState cs =
                safeGetBlockState(region, cur.x, cur.y, cur.z);
            const BlockState gs = groundState.getState(rng, cur);
            if (!gs.is(cs.block())) {
              if (!replaceableTag.empty() &&
                  !blockHasTag(cs, replaceableTag).value_or(false)) {
                replaced = (i != 0);
                break;
              }
              safeSetBlockState(region, cur.x, cur.y, cur.z, gs, 2);
              cur = cur.relative(inwards);
            }
          }
          if (replaced)
            groundSurface.push_back(below);
        }
      }
    }

    std::vector<Pos> vegSurface = groundSurface;
    if (kind == Kind::WaterloggedVegetationPatch) {
      auto isExposedDir = [&](Pos gp, Direction d) {
        const Pos np = gp.relative(d);
        return !isFaceSturdyUpApprox(
            safeGetBlockState(region, np.x, np.y, np.z));
      };
      std::vector<Pos> waterSurface;
      for (const Pos gp : groundSurface) {
        if (!isExposedDir(gp, Direction::North) &&
            !isExposedDir(gp, Direction::East) &&
            !isExposedDir(gp, Direction::South) &&
            !isExposedDir(gp, Direction::West) &&
            !isExposedDir(gp, Direction::Down)) {
          waterSurface.push_back(gp);
        }
      }
      for (const Pos wp : waterSurface) {
        safeSetBlockState(region, wp.x, wp.y, wp.z, Blocks::WATER.defaultState(),
                          2);
      }
      vegSurface = std::move(waterSurface);
    }

    if (const PlacedFeature *vf = resolvePlacedRef(vegetationFeature, regs)) {
      for (const Pos gp : vegSurface) {
        if (vegetationChance > 0.0f && rng.nextFloat() < vegetationChance) {
          const Pos placePos = (kind == Kind::WaterloggedVegetationPatch)
                                   ? gp
                                   : gp.relative(outwards);
          if (vf->place(region, generator, worldSeed, rng, placePos, false) &&
              kind == Kind::WaterloggedVegetationPatch) {
            const BlockState placed =
                safeGetBlockState(region, placePos.x, placePos.y, placePos.z);
            if (placed.hasProperty("waterlogged") &&
                placed.getView("waterlogged") == "false") {
              safeSetBlockState(region, placePos.x, placePos.y, placePos.z,
                                placed.with("waterlogged", "true"), 2);
            }
          }
        }
      }
    }
    return !vegSurface.empty();
  }

  case Kind::Lake: {
    if (origin.y <= region.minY() + 4)
      return false;
    const Pos base{origin.x - 8, origin.y - 4, origin.z - 8};
    std::array<bool, 2048> grid{};
    const int spots = rng.nextInt(4) + 4;
    for (int i = 0; i < spots; ++i) {
      const double xr = rng.nextDouble() * 6.0 + 3.0;
      const double yr = rng.nextDouble() * 4.0 + 2.0;
      const double zr = rng.nextDouble() * 6.0 + 3.0;
      const double xp = rng.nextDouble() * (16.0 - xr - 2.0) + 1.0 + xr / 2.0;
      const double yp = rng.nextDouble() * (8.0 - yr - 4.0) + 2.0 + yr / 2.0;
      const double zp = rng.nextDouble() * (16.0 - zr - 2.0) + 1.0 + zr / 2.0;
      for (int xx = 1; xx < 15; ++xx)
        for (int zz = 1; zz < 15; ++zz)
          for (int yy = 1; yy < 7; ++yy) {
            const double xd = (xx - xp) / (xr / 2.0);
            const double yd = (yy - yp) / (yr / 2.0);
            const double zd = (zz - zp) / (zr / 2.0);
            if (xd * xd + yd * yd + zd * zd < 1.0)
              grid[(xx * 16 + zz) * 8 + yy] = true;
          }
    }
    const BlockState fluid = lakeFluid.getState(rng, base);
    for (int xx = 0; xx < 16; ++xx) {
      for (int zz = 0; zz < 16; ++zz) {
        for (int yy = 0; yy < 8; ++yy) {
          const bool border =
              !grid[(xx * 16 + zz) * 8 + yy] &&
              ((xx < 15 && grid[((xx + 1) * 16 + zz) * 8 + yy]) ||
               (xx > 0 && grid[((xx - 1) * 16 + zz) * 8 + yy]) ||
               (zz < 15 && grid[(xx * 16 + zz + 1) * 8 + yy]) ||
               (zz > 0 && grid[(xx * 16 + zz - 1) * 8 + yy]) ||
               (yy < 7 && grid[(xx * 16 + zz) * 8 + yy + 1]) ||
               (yy > 0 && grid[(xx * 16 + zz) * 8 + yy - 1]));
          if (!border)
            continue;
          const BlockState bs =
              safeGetBlockState(region, base.x + xx, base.y + yy, base.z + zz);
          if (yy >= 4 && bs.block().isLiquid())
            return false;
          if (yy < 4 && !bs.block().metadata().conductor && !(bs == fluid))
            return false;
        }
      }
    }
    for (int xx = 0; xx < 16; ++xx)
      for (int zz = 0; zz < 16; ++zz)
        for (int yy = 0; yy < 8; ++yy)
          if (grid[(xx * 16 + zz) * 8 + yy]) {
            const Pos pp{base.x + xx, base.y + yy, base.z + zz};
            const BlockState cs = safeGetBlockState(region, pp.x, pp.y, pp.z);
            if (!blockHasTag(cs, "minecraft:features_cannot_replace")
                     .value_or(false)) {
              safeSetBlockState(region, pp.x, pp.y, pp.z,
                                yy >= 4 ? Blocks::CAVE_AIR.defaultState()
                                        : fluid,
                                2);
            }
          }
    const BlockState barrier = lakeBarrier.getState(rng, base);
    if (!barrier.isAir()) {
      for (int xx = 0; xx < 16; ++xx)
        for (int zz = 0; zz < 16; ++zz)
          for (int yy = 0; yy < 8; ++yy) {
            const bool border =
                !grid[(xx * 16 + zz) * 8 + yy] &&
                ((xx < 15 && grid[((xx + 1) * 16 + zz) * 8 + yy]) ||
                 (xx > 0 && grid[((xx - 1) * 16 + zz) * 8 + yy]) ||
                 (zz < 15 && grid[(xx * 16 + zz + 1) * 8 + yy]) ||
                 (zz > 0 && grid[(xx * 16 + zz - 1) * 8 + yy]) ||
                 (yy < 7 && grid[(xx * 16 + zz) * 8 + yy + 1]) ||
                 (yy > 0 && grid[(xx * 16 + zz) * 8 + yy - 1]));
            if (border && (yy < 4 || rng.nextInt(2) != 0)) {
              const Pos pp{base.x + xx, base.y + yy, base.z + zz};
              const BlockState bs = safeGetBlockState(region, pp.x, pp.y, pp.z);
              if (bs.block().metadata().conductor &&
                  !blockHasTag(bs, "minecraft:lava_pool_stone_cannot_replace")
                       .value_or(false)) {
                safeSetBlockState(region, pp.x, pp.y, pp.z, barrier, 2);
              }
            }
          }
    }
    return true;
  }

  case Kind::NetherForestVegetation: {
    const BlockState below =
        safeGetBlockState(region, origin.x, origin.y - 1, origin.z);
    if (!blockHasTag(below, "minecraft:nylium").value_or(false))
      return false;
    if (origin.y < region.minY() + 1 || origin.y + 1 > region.maxY())
      return false;
    int placed = 0;
    for (int i = 0; i < spreadWidth * spreadWidth; ++i) {
      const int dx1 = rng.nextInt(spreadWidth);
      const int dx2 = rng.nextInt(spreadWidth);
      const int dy1 = rng.nextInt(spreadHeight);
      const int dy2 = rng.nextInt(spreadHeight);
      const int dz1 = rng.nextInt(spreadWidth);
      const int dz2 = rng.nextInt(spreadWidth);
      const Pos fp{origin.x + dx1 - dx2, origin.y + dy1 - dy2,
                   origin.z + dz1 - dz2};
      const BlockState st = stateProvider.getState(rng, fp);
      if (fp.y > region.minY() &&
          safeGetBlockState(region, fp.x, fp.y, fp.z).isAir() &&
          canSurvivePlacement(region, st, fp)) {
        safeSetBlockState(region, fp.x, fp.y, fp.z, st, 2);
        ++placed;
      }
    }
    return placed > 0;
  }

  case Kind::GlowstoneBlob: {
    if (!safeGetBlockState(region, origin.x, origin.y, origin.z).isAir())
      return false;
    const BlockState above =
        safeGetBlockState(region, origin.x, origin.y + 1, origin.z);
    if (!above.is(Blocks::NETHERRACK) && !above.is(Blocks::BASALT) &&
        !above.is(Blocks::BLACKSTONE)) {
      return false;
    }
    safeSetBlockState(region, origin.x, origin.y, origin.z,
                      Blocks::GLOWSTONE.defaultState(), 2);
    for (int i = 0; i < 1500; ++i) {
      const int dx1 = rng.nextInt(8);
      const int dx2 = rng.nextInt(8);
      const int dy = -rng.nextInt(12);
      const int dz1 = rng.nextInt(8);
      const int dz2 = rng.nextInt(8);
      const Pos pp{origin.x + dx1 - dx2, origin.y + dy, origin.z + dz1 - dz2};
      if (safeGetBlockState(region, pp.x, pp.y, pp.z).isAir()) {
        int nb = 0;
        for (Direction d : {Direction::Down, Direction::Up, Direction::North,
                            Direction::South, Direction::West, Direction::East}) {
          const Pos np = pp.relative(d);
          if (safeGetBlockState(region, np.x, np.y, np.z).is(Blocks::GLOWSTONE))
            ++nb;
          if (nb > 1)
            break;
        }
        if (nb == 1) {
          safeSetBlockState(region, pp.x, pp.y, pp.z,
                            Blocks::GLOWSTONE.defaultState(), 2);
        }
      }
    }
    return true;
  }

  case Kind::NetherrackReplaceBlobs: {
    Pos cursor = origin;
    cursor.y = std::clamp(cursor.y, region.minY() + 1, region.maxY() - 1);
    bool found = false;
    while (cursor.y > region.minY() + 1) {
      if (safeGetBlockState(region, cursor.x, cursor.y, cursor.z)
              .is(replaceTarget.block())) {
        found = true;
        break;
      }
      --cursor.y;
    }
    if (!found)
      return false;
    const int rx = replaceRadius.sample(rng);
    const int ry = replaceRadius.sample(rng);
    const int rz = replaceRadius.sample(rng);
    const int maxR = std::max({rx, ry, rz});
    bool any = false;
    for (int d = 0; d <= maxR; ++d) {
      for (int dx = -std::min(rx, d); dx <= std::min(rx, d); ++dx) {
        const int rem1 = d - std::abs(dx);
        for (int dy = -std::min(ry, rem1); dy <= std::min(ry, rem1); ++dy) {
          const int adz = rem1 - std::abs(dy);
          if (adz > rz)
            continue;
          for (int sgn : (adz == 0 ? std::vector<int>{0}
                                   : std::vector<int>{-adz, adz})) {
            const Pos p{cursor.x + dx, cursor.y + dy, cursor.z + sgn};
            if (safeGetBlockState(region, p.x, p.y, p.z)
                    .is(replaceTarget.block())) {
              safeSetBlockState(region, p.x, p.y, p.z, replaceState, 2);
              any = true;
            }
          }
        }
      }
    }
    return any;
  }

  case Kind::FreezeTopLayer: {
    const int seaLevel = generator.settings().seaLevel;
    for (int dx = 0; dx < 16; ++dx) {
      for (int dz = 0; dz < 16; ++dz) {
        const int x = origin.x + dx;
        const int z = origin.z + dz;
        const int y = safeGetHeight(region, HeightmapType::MotionBlocking, x, z);
        const Pos topPos{x, y, z};
        const Pos belowPos{x, y - 1, z};
        const std::string_view biome = region.getNoiseBiome(
            topPos.x >> 2, topPos.y >> 2, topPos.z >> 2);
        if (biomeShouldFreeze(regs, biome, region, belowPos, seaLevel)) {
          safeSetBlockState(region, belowPos.x, belowPos.y, belowPos.z,
                            Blocks::ICE.defaultState(), 2);
        }
        if (biomeShouldSnow(regs, biome, region, topPos, seaLevel)) {
          safeSetBlockState(region, topPos.x, topPos.y, topPos.z,
                            Blocks::SNOW.defaultState(), 2);
          const BlockState bs =
              safeGetBlockState(region, belowPos.x, belowPos.y, belowPos.z);
          if (bs.hasProperty("snowy")) {
            safeSetBlockState(region, belowPos.x, belowPos.y, belowPos.z,
                              bs.with("snowy", "true"), 2);
          }
        }
      }
    }
    return true;
  }

  case Kind::EndSpike: {
    std::vector<EndSpikeDef> spikes = endSpikes;
    if (spikes.empty()) {
      LegacyRandomSource seedRng(worldSeed);
      const std::int64_t cacheKey = seedRng.nextLong() & 65535LL;
      LegacyRandomSource spikeRng(cacheKey);
      std::vector<int> sizes(10);
      std::iota(sizes.begin(), sizes.end(), 0);
      for (int i = 9; i > 0; --i) {
        const int j = spikeRng.nextInt(i + 1);
        std::swap(sizes[static_cast<std::size_t>(i)],
                  sizes[static_cast<std::size_t>(j)]);
      }
      for (int i = 0; i < 10; ++i) {
        const double ang = 2.0 * (-kPi + (kPi / 10.0) * i);
        const int sx = floorInt(42.0 * std::cos(ang));
        const int sz = floorInt(42.0 * std::sin(ang));
        const int szVal = sizes[static_cast<std::size_t>(i)];
        spikes.push_back(EndSpikeDef{sx, sz, 2 + szVal / 3, 76 + szVal * 3,
                                     szVal == 1 || szVal == 2});
      }
    }
    for (const auto &spike : spikes) {
      if ((spike.centerX >> 4) != (origin.x >> 4) ||
          (spike.centerZ >> 4) != (origin.z >> 4)) {
        continue;
      }
      const int r = spike.radius;
      for (int x = spike.centerX - r; x <= spike.centerX + r; ++x) {
        for (int z = spike.centerZ - r; z <= spike.centerZ + r; ++z) {
          for (int y = region.minY(); y <= spike.height + 10; ++y) {
            const double d2 = static_cast<double>((x - spike.centerX) *
                                                      (x - spike.centerX) +
                                                  (z - spike.centerZ) *
                                                      (z - spike.centerZ));
            if (d2 <= static_cast<double>(r * r + 1) && y < spike.height) {
              safeSetBlockState(region, x, y, z, Blocks::OBSIDIAN.defaultState(),
                                2);
            } else if (y > 65) {
              safeSetBlockState(region, x, y, z, airState(), 2);
            }
          }
        }
      }
      if (spike.guarded) {
        for (int dx = -2; dx <= 2; ++dx) {
          for (int dz = -2; dz <= 2; ++dz) {
            for (int dy = 0; dy <= 3; ++dy) {
              const bool isX = std::abs(dx) == 2;
              const bool isZ = std::abs(dz) == 2;
              const bool top = dy == 3;
              if (isX || isZ || top) {
                const bool xe = dx == -2 || dx == 2 || top;
                const bool ze = dz == -2 || dz == 2 || top;
                BlockState bars = Blocks::IRON_BARS.defaultState();
                if (bars.hasProperty("north")) {
                  bars = bars.with("north", (xe && dz != -2) ? "true" : "false")
                             .with("south", (xe && dz != 2) ? "true" : "false")
                             .with("west", (ze && dx != -2) ? "true" : "false")
                             .with("east", (ze && dx != 2) ? "true" : "false");
                }
                safeSetBlockState(region, spike.centerX + dx, spike.height + dy,
                                  spike.centerZ + dz, bars, 2);
              }
            }
          }
        }
      }
      (void)rng.nextFloat();
      safeSetBlockState(region, spike.centerX, spike.height, spike.centerZ,
                        Blocks::BEDROCK.defaultState(), 2);
      safeSetBlockState(region, spike.centerX, spike.height + 1, spike.centerZ,
                        Blocks::FIRE.defaultState(), 2);
    }
    return true;
  }

  case Kind::EndIsland: {
    float sz = static_cast<float>(rng.nextInt(3)) + 4.0f;
    for (int y = 0; sz > 0.5f; --y) {
      for (int x = floorInt(-sz); x <= ceilInt(sz); ++x) {
        for (int z = floorInt(-sz); z <= ceilInt(sz); ++z) {
          if (static_cast<float>(x * x + z * z) <= (sz + 1.0f) * (sz + 1.0f)) {
            safeSetBlockState(region, origin.x + x, origin.y + y, origin.z + z,
                              Blocks::END_STONE.defaultState(), 2);
          }
        }
      }
      sz -= static_cast<float>(rng.nextInt(2)) + 0.5f;
    }
    return true;
  }

  case Kind::Tree:
    return placeTreeFeature(tree, region, generator, worldSeed, rng, origin);

  case Kind::FallenTree:
    return placeFallenTreeFeature(fallenTree, region, rng, origin);

  case Kind::HugeRedMushroom:
    return placeHugeMushroomFeature(true, hugeMushroom, region, rng, origin);

  case Kind::HugeBrownMushroom:
    return placeHugeMushroomFeature(false, hugeMushroom, region, rng, origin);

  case Kind::HugeFungus:
    return placeHugeFungusFeature(hugeFungus, region, generator, rng, origin);

  case Kind::DesertWell:
  case Kind::BlockBlob:
  case Kind::BlueIce:
  case Kind::Vines:
  case Kind::Seagrass:
  case Kind::Kelp:
  case Kind::SeaPickle:
  case Kind::Bamboo:
  case Kind::BasaltColumns:
  case Kind::BasaltPillar:
  case Kind::BonusChest:
  case Kind::ChorusPlant:
  case Kind::DeltaFeature:
  case Kind::DripstoneCluster:
  case Kind::EndGateway:
  case Kind::EndPlatform:
  case Kind::Fossil:
  case Kind::Geode:
  case Kind::Iceberg:
  case Kind::LargeDripstone:
  case Kind::MonsterRoom:
  case Kind::MultifaceGrowth:
  case Kind::RootSystem:
  case Kind::SculkPatch:
  case Kind::Spike:
  case Kind::TwistingVines:
  case Kind::UnderwaterMagma:
  case Kind::VoidStartPlatform:
  case Kind::WeepingVines:
    return placeExtraConfiguredFeature(*this, region, generator, worldSeed, rng,
                                       origin);

  default:
    return false;
  }
}

bool PlacedFeature::place(WorldGenRegion &region,
                          const NoiseBasedChunkGenerator &generator,
                          std::int64_t worldSeed, RandomSource &rng,
                          Pos origin, bool checkBiome) const {
  const auto &regs = generator.registries();
  PlacementContext ctx{region, generator, worldSeed, id,
                       std::make_unique<BiomeManager>(
                           BiomeManager::obfuscateSeed(worldSeed))};
  const int seaLevel = generator.settings().seaLevel;
  (void)seaLevel;

  const ConfiguredFeature *cf =
      inlineFeature ? inlineFeature.get()
                    : regs.findConfiguredFeature(featureRefId);
  if (!cf)
    return false;

  bool placedAny = false;
  auto applyModifiers = [&](auto &self, std::size_t idx, Pos p) -> void {
    if (idx == placement.size()) {
      if (cf->place(region, generator, worldSeed, rng, p))
        placedAny = true;
      return;
    }
    const PlacementModifier &mod = placement[idx];
    const std::size_t nextIdx = idx + 1;
    switch (mod.kind) {
    case PlacementModifier::Kind::Biome: {
      if (!checkBiome || id.empty()) {
        self(self, nextIdx, p);
        break;
      }
      const std::string_view biome = ctx.biomeManager->getBiome(
          p.x, p.y, p.z, [&](int qx, int qy, int qz) {
            return region.getNoiseBiome(qx, qy, qz);
          });
      const auto *bgi = regs.findBiomeGeneration(biome);
      bool has = false;
      if (bgi) {
        for (const auto &stepVec : bgi->features) {
          if (std::find(stepVec.begin(), stepVec.end(), id) != stepVec.end()) {
            has = true;
            break;
          }
        }
      }
      if (has)
        self(self, nextIdx, p);
      break;
    }
    case PlacementModifier::Kind::InSquare: {
      const int rx = rng.nextInt(16);
      const int rz = rng.nextInt(16);
      self(self, nextIdx, Pos{p.x + rx, p.y, p.z + rz});
      break;
    }
    case PlacementModifier::Kind::Count: {
      const int cnt = mod.countProvider.sample(rng);
      for (int i = 0; i < cnt; ++i)
        self(self, nextIdx, p);
      break;
    }
    case PlacementModifier::Kind::CountOnEveryLayer: {
      std::vector<Pos> layerPositions;
      int layer = 0;
      bool foundAny = false;
      do {
        foundAny = false;
        for (int i = 0; i < mod.countProvider.sample(rng); ++i) {
          const int x = rng.nextInt(16) + p.x;
          const int z = rng.nextInt(16) + p.z;
          const int startY =
              safeGetHeight(region, HeightmapType::MotionBlocking, x, z);
          int currentLayer = 0;
          BlockState curBlock = safeGetBlockState(region, x, startY, z);
          auto isEmptyLayer = [](const BlockState &s) {
            return s.isAir() || s.is(Blocks::WATER) || s.is(Blocks::LAVA);
          };
          for (int y = startY; y >= region.minY() + 1; --y) {
            const BlockState below = safeGetBlockState(region, x, y - 1, z);
            if (!isEmptyLayer(below) && isEmptyLayer(curBlock) &&
                !below.is(Blocks::BEDROCK)) {
              if (currentLayer == layer) {
                layerPositions.push_back(Pos{x, y, z});
                foundAny = true;
                break;
              }
              ++currentLayer;
            }
            curBlock = below;
          }
        }
        ++layer;
      } while (foundAny);
      for (const Pos &lp : layerPositions)
        self(self, nextIdx, lp);
      break;
    }
    case PlacementModifier::Kind::HeightRange: {
      // HeightRangePlacement 使用 ChunkGenerator.getGenDepth()（生成深度），
      // 不是维度区块高度：下界生成深度 128，128 以上是空气。
      const WorldGenerationContext wgCtx{generator.minY(), generator.height()};
      const int y = mod.heightProvider.sample(rng, wgCtx);
      self(self, nextIdx, Pos{p.x, y, p.z});
      break;
    }
    case PlacementModifier::Kind::Heightmap: {
      const int y = safeGetHeight(region, mod.heightmap, p.x, p.z);
      self(self, nextIdx, Pos{p.x, y, p.z});
      break;
    }
    case PlacementModifier::Kind::RandomOffset: {
      const int dx = mod.xzSpread.sample(rng);
      const int dy = mod.ySpread.sample(rng);
      const int dz = mod.xzSpread.sample(rng);
      self(self, nextIdx, Pos{p.x + dx, p.y + dy, p.z + dz});
      break;
    }
    case PlacementModifier::Kind::RarityFilter: {
      if (rng.nextFloat() < 1.0f / static_cast<float>(mod.rarityChance))
        self(self, nextIdx, p);
      break;
    }
    case PlacementModifier::Kind::SurfaceWaterDepthFilter: {
      const int yOcean =
          safeGetHeight(region, HeightmapType::OceanFloor, p.x, p.z);
      const int ySurf =
          safeGetHeight(region, HeightmapType::WorldSurface, p.x, p.z);
      if (ySurf - yOcean <= mod.maxWaterDepth)
        self(self, nextIdx, p);
      break;
    }
    case PlacementModifier::Kind::SurfaceRelativeThresholdFilter: {
      const std::int64_t surfY =
          safeGetHeight(region, mod.heightmap, p.x, p.z);
      if (surfY + mod.minInclusive <= p.y && p.y <= surfY + mod.maxInclusive)
        self(self, nextIdx, p);
      break;
    }
    case PlacementModifier::Kind::BlockPredicateFilter: {
      if (mod.predicate.test(region, p))
        self(self, nextIdx, p);
      break;
    }
    case PlacementModifier::Kind::EnvironmentScan: {
      Pos cur = p;
      if (mod.allowedSearchCondition.test(region, cur)) {
        bool aborted = false;
        for (int step = 0; step < mod.maxSteps; ++step) {
          if (mod.targetCondition.test(region, cur)) {
            self(self, nextIdx, cur);
            aborted = true;
            break;
          }
          cur = cur.relative(mod.searchDir);
          if (cur.y < region.minY() || cur.y >= region.maxY()) {
            aborted = true;
            break;
          }
          if (!mod.allowedSearchCondition.test(region, cur))
            break;
        }
        if (!aborted && mod.targetCondition.test(region, cur))
          self(self, nextIdx, cur);
      }
      break;
    }
    case PlacementModifier::Kind::NoiseBasedCount: {
      const double nv = biomeInfoNoise().getValue(
          static_cast<double>(p.x) / mod.noiseFactor,
          static_cast<double>(p.z) / mod.noiseFactor, false);
      const int cnt = static_cast<int>(
          std::ceil((nv + mod.noiseOffset) * mod.noiseToCountRatio));
      for (int i = 0; i < cnt; ++i)
        self(self, nextIdx, p);
      break;
    }
    case PlacementModifier::Kind::NoiseThresholdCount: {
      const double nv = biomeInfoNoise().getValue(
          static_cast<double>(p.x) / 200.0, static_cast<double>(p.z) / 200.0,
          false);
      const int cnt = nv < mod.noiseLevel ? mod.belowNoise : mod.aboveNoise;
      for (int i = 0; i < cnt; ++i)
        self(self, nextIdx, p);
      break;
    }
    case PlacementModifier::Kind::FixedPlacement: {
      const int cx = p.x >> 4;
      const int cz = p.z >> 4;
      for (const Pos &fp : mod.fixedPositions) {
        if ((fp.x >> 4) == cx && (fp.z >> 4) == cz)
          self(self, nextIdx, fp);
      }
      break;
    }
    }
  };
  applyModifiers(applyModifiers, 0, origin);
  return placedAny;
}

std::vector<StepFeatureData>
buildFeaturesPerStep(const WorldgenRegistries &registries,
                     const std::vector<std::string> &possibleBiomes) {
  std::map<std::string, int, std::less<>> featIndexMap;
  int nextFeatIdx = 0;
  struct Node {
    int featureIdx = 0;
    int step = 0;
    std::string id;
    bool operator<(const Node &o) const noexcept {
      if (step != o.step)
        return step < o.step;
      return featureIdx < o.featureIdx;
    }
    bool operator==(const Node &o) const noexcept {
      return step == o.step && featureIdx == o.featureIdx;
    }
  };
  std::map<Node, std::set<Node>> edges;
  int maxStep = 0;

  for (const std::string &biomeId : possibleBiomes) {
    const auto *bgi = registries.findBiomeGeneration(biomeId);
    if (!bgi)
      continue;
    std::vector<Node> biomeNodes;
    maxStep = std::max(maxStep, static_cast<int>(kDecorationStepCount));
    for (int step = 0; step < static_cast<int>(kDecorationStepCount); ++step) {
      for (const std::string &fid : bgi->features[step]) {
        auto [it, ins] = featIndexMap.emplace(fid, nextFeatIdx);
        if (ins)
          ++nextFeatIdx;
        biomeNodes.push_back(Node{it->second, step, fid});
      }
    }
    for (std::size_t i = 0; i < biomeNodes.size(); ++i) {
      auto &adj = edges[biomeNodes[i]];
      if (i + 1 < biomeNodes.size())
        adj.insert(biomeNodes[i + 1]);
    }
  }

  std::set<Node> discovered;
  std::set<Node> visiting;
  std::vector<Node> postOrder;
  auto dfs = [&](auto &self, const Node &u) -> bool {
    if (discovered.count(u))
      return false;
    if (visiting.count(u))
      return true;
    visiting.insert(u);
    if (auto it = edges.find(u); it != edges.end()) {
      for (const Node &v : it->second) {
        if (self(self, v))
          return true;
      }
    }
    visiting.erase(u);
    discovered.insert(u);
    postOrder.push_back(u);
    return false;
  };
  for (const auto &[node, _] : edges) {
    if (!discovered.count(node))
      (void)dfs(dfs, node);
  }
  std::reverse(postOrder.begin(), postOrder.end());

  std::vector<StepFeatureData> steps(static_cast<std::size_t>(maxStep));
  for (const Node &n : postOrder) {
    auto &sd = steps[static_cast<std::size_t>(n.step)];
    const int idx = static_cast<int>(sd.featureIds.size());
    sd.featureIds.push_back(n.id);
    sd.indexById.emplace(n.id, idx);
  }
  return steps;
}

PlacedFeaturePtr parsePlacedFeatureJson(const picojson::value &v,
                                        std::string_view id) {
  const auto &obj = asObj(v, id);
  auto pf = std::make_shared<PlacedFeature>();
  pf->id = std::string(id);
  const auto &fv = req(obj, "feature", id);
  if (fv.is<std::string>()) {
    pf->featureRefId = normId(fv.get<std::string>());
  } else if (fv.is<picojson::object>()) {
    pf->inlineFeature = parseConfiguredFeatureJson(fv, std::string(id) + "#inline");
  }
  if (auto it = obj.find("placement");
      it != obj.end() && it->second.is<picojson::array>()) {
    for (const auto &pv : it->second.get<picojson::array>()) {
      pf->placement.push_back(parsePlacementModifierJson(pv, id));
    }
  }
  return pf;
}

static ConfiguredFeature::TreeDecoratorConfig
parseTreeDecoratorJson(const picojson::value &dv, std::string_view id) {
  const auto &doObj = asObj(dv, id);
  ConfiguredFeature::TreeDecoratorConfig dec;
  const std::string dt = normId(asStr(req(doObj, "type", id), id));
  if (dt == "minecraft:beehive") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::Beehive;
    dec.probability =
        static_cast<float>(asNum(req(doObj, "probability", id), id));
  } else if (dt == "minecraft:place_on_ground") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::PlaceOnGround;
    if (auto it = doObj.find("tries"); it != doObj.end())
      dec.tries = asInt(it->second, id);
    if (auto it = doObj.find("radius"); it != doObj.end())
      dec.radius = asInt(it->second, id);
    if (auto it = doObj.find("height"); it != doObj.end())
      dec.height = asInt(it->second, id);
    dec.blockProvider =
        parseBlockStateProviderJson(req(doObj, "block_state_provider", id), id);
  } else if (dt == "minecraft:leave_vine") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::LeaveVine;
    dec.probability =
        static_cast<float>(asNum(req(doObj, "probability", id), id));
  } else if (dt == "minecraft:trunk_vine") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::TrunkVine;
  } else if (dt == "minecraft:alter_ground") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::AlterGround;
    dec.blockProvider =
        parseBlockStateProviderJson(req(doObj, "provider", id), id);
  } else if (dt == "minecraft:attached_to_leaves") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::AttachedToLeaves;
    dec.probability =
        static_cast<float>(asNum(req(doObj, "probability", id), id));
    dec.exclusionRadiusXZ = asInt(req(doObj, "exclusion_radius_xz", id), id);
    dec.exclusionRadiusY = asInt(req(doObj, "exclusion_radius_y", id), id);
    dec.blockProvider =
        parseBlockStateProviderJson(req(doObj, "block_provider", id), id);
    dec.requiredEmptyBlocks =
        asInt(req(doObj, "required_empty_blocks", id), id);
    for (const auto &d : req(doObj, "directions", id).get<picojson::array>())
      dec.directions.push_back(directionFromString(asStr(d, id)));
  } else if (dt == "minecraft:attached_to_logs") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::AttachedToLogs;
    dec.probability =
        static_cast<float>(asNum(req(doObj, "probability", id), id));
    dec.blockProvider =
        parseBlockStateProviderJson(req(doObj, "block_provider", id), id);
    for (const auto &d : req(doObj, "directions", id).get<picojson::array>())
      dec.directions.push_back(directionFromString(asStr(d, id)));
  } else if (dt == "minecraft:pale_moss") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::PaleMoss;
    dec.leavesProbability =
        static_cast<float>(asNum(req(doObj, "leaves_probability", id), id));
    dec.trunkProbability =
        static_cast<float>(asNum(req(doObj, "trunk_probability", id), id));
    dec.groundProbability =
        static_cast<float>(asNum(req(doObj, "ground_probability", id), id));
  } else if (dt == "minecraft:creaking_heart") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::CreakingHeart;
    dec.probability =
        static_cast<float>(asNum(req(doObj, "probability", id), id));
  } else if (dt == "minecraft:cocoa") {
    dec.kind = ConfiguredFeature::TreeDecoratorConfig::Kind::Cocoa;
    dec.probability =
        static_cast<float>(asNum(req(doObj, "probability", id), id));
  }
  return dec;
}

ConfiguredFeaturePtr parseConfiguredFeatureJson(const picojson::value &v,
                                                std::string_view id) {
  const auto &obj = asObj(v, id);
  auto cf = std::make_shared<ConfiguredFeature>();
  cf->id = std::string(id);
  cf->rawType = normId(asStr(req(obj, "type", id), id));
  const auto &cfgVal = req(obj, "config", id);
  const auto &cfg = asObj(cfgVal, id);

  const std::string &t = cf->rawType;
  if (t == "minecraft:ore" || t == "minecraft:scattered_ore") {
    cf->kind = (t == "minecraft:scattered_ore")
                   ? ConfiguredFeature::Kind::ScatteredOre
                   : ConfiguredFeature::Kind::Ore;
    cf->oreSize = asInt(req(cfg, "size", id), id);
    if (auto it = cfg.find("discard_chance_on_air_exposure"); it != cfg.end()) {
      cf->discardChanceOnAirExposure =
          static_cast<float>(asNum(it->second, id));
    }
    for (const auto &tv : req(cfg, "targets", id).get<picojson::array>()) {
      const auto &to = asObj(tv, id);
      ConfiguredFeature::OreTarget ot;
      ot.target = parseOreRulePredicate(req(to, "target", id), id);
      ot.state = parseBlockStateJson(req(to, "state", id), id);
      cf->oreTargets.push_back(std::move(ot));
    }
  } else if (t == "minecraft:spring_feature") {
    cf->kind = ConfiguredFeature::Kind::SpringFeature;
    cf->springState = parseBlockStateJson(req(cfg, "state", id), id);
    if (auto it = cfg.find("requires_block_below"); it != cfg.end())
      cf->requiresBlockBelow = asBool(it->second, id);
    if (auto it = cfg.find("rock_count"); it != cfg.end())
      cf->rockCount = asInt(it->second, id);
    if (auto it = cfg.find("hole_count"); it != cfg.end())
      cf->holeCount = asInt(it->second, id);
    cf->validBlocks = parseStringOrArrayIds(req(cfg, "valid_blocks", id), id);
  } else if (t == "minecraft:disk") {
    cf->kind = ConfiguredFeature::Kind::Disk;
    cf->diskHalfHeight = asInt(req(cfg, "half_height", id), id);
    cf->diskRadius = parseIntProviderJson(req(cfg, "radius", id), id);
    if (auto it = cfg.find("state_provider"); it != cfg.end())
      cf->stateProvider = parseBlockStateProviderJson(it->second, id);
    cf->targetPredicate =
        parseBlockPredicateJson(req(cfg, "target", id), id);
  } else if (t == "minecraft:simple_block") {
    cf->kind = ConfiguredFeature::Kind::SimpleBlock;
    cf->stateProvider =
        parseBlockStateProviderJson(req(cfg, "to_place", id), id);
    if (auto it = cfg.find("schedule_tick"); it != cfg.end())
      cf->scheduleTick = asBool(it->second, id);
  } else if (t == "minecraft:random_selector") {
    cf->kind = ConfiguredFeature::Kind::RandomSelector;
    for (const auto &fv : req(cfg, "features", id).get<picojson::array>()) {
      const auto &fo = asObj(fv, id);
      ConfiguredFeature::WeightedFeatureEntry w;
      w.chance = static_cast<float>(asNum(req(fo, "chance", id), id));
      w.feature = parsePlacedRefOrInline(req(fo, "feature", id), id);
      cf->weightedFeatures.push_back(std::move(w));
    }
    cf->defaultFeature = parsePlacedRefOrInline(req(cfg, "default", id), id);
  } else if (t == "minecraft:simple_random_selector") {
    cf->kind = ConfiguredFeature::Kind::SimpleRandomSelector;
    for (const auto &fv : req(cfg, "features", id).get<picojson::array>())
      cf->randomFeatures.push_back(parsePlacedRefOrInline(fv, id));
  } else if (t == "minecraft:random_boolean_selector") {
    cf->kind = ConfiguredFeature::Kind::RandomBooleanSelector;
    cf->featureTrue = parsePlacedRefOrInline(req(cfg, "feature_true", id), id);
    cf->featureFalse =
        parsePlacedRefOrInline(req(cfg, "feature_false", id), id);
  } else if (t == "minecraft:block_column") {
    cf->kind = ConfiguredFeature::Kind::BlockColumn;
    cf->columnDir = directionFromString(asStr(req(cfg, "direction", id), id));
    if (auto it = cfg.find("prioritize_tip"); it != cfg.end())
      cf->prioritizeTip = asBool(it->second, id);
    if (auto it = cfg.find("allowed_placement"); it != cfg.end())
      cf->allowedPlacement = parseBlockPredicateJson(it->second, id);
    for (const auto &lv : req(cfg, "layers", id).get<picojson::array>()) {
      const auto &lo = asObj(lv, id);
      ConfiguredFeature::ColumnLayer cl;
      cl.height = parseIntProviderJson(req(lo, "height", id), id);
      cl.provider = parseBlockStateProviderJson(req(lo, "provider", id), id);
      cf->layers.push_back(std::move(cl));
    }
  } else if (t == "minecraft:block_pile") {
    cf->kind = ConfiguredFeature::Kind::BlockPile;
    cf->stateProvider =
        parseBlockStateProviderJson(req(cfg, "state_provider", id), id);
  } else if (t == "minecraft:vegetation_patch" ||
             t == "minecraft:waterlogged_vegetation_patch") {
    cf->kind = (t == "minecraft:waterlogged_vegetation_patch")
                   ? ConfiguredFeature::Kind::WaterloggedVegetationPatch
                   : ConfiguredFeature::Kind::VegetationPatch;
    if (auto it = cfg.find("replaceable");
        it != cfg.end() && it->second.is<std::string>()) {
      std::string r = it->second.get<std::string>();
      if (!r.empty() && r.front() == '#')
        r.erase(r.begin());
      cf->replaceableTag = normId(r);
    }
    cf->groundState =
        parseBlockStateProviderJson(req(cfg, "ground_state", id), id);
    cf->vegetationFeature =
        parsePlacedRefOrInline(req(cfg, "vegetation_feature", id), id);
    if (auto it = cfg.find("vertical_range"); it != cfg.end())
      cf->verticalRange = asInt(it->second, id);
    cf->patchDepth = parseIntProviderJson(req(cfg, "depth", id), id);
    cf->xzRadius = parseIntProviderJson(req(cfg, "xz_radius", id), id);
    if (auto it = cfg.find("extra_bottom_block_chance"); it != cfg.end())
      cf->extraBottomBlockChance = static_cast<float>(asNum(it->second, id));
    if (auto it = cfg.find("extra_edge_column_chance"); it != cfg.end())
      cf->extraEdgeColumnChance = static_cast<float>(asNum(it->second, id));
    if (auto it = cfg.find("vegetation_chance"); it != cfg.end())
      cf->vegetationChance = static_cast<float>(asNum(it->second, id));
    if (auto it = cfg.find("surface");
        it != cfg.end() && it->second.is<std::string>()) {
      cf->surfaceCeiling = (it->second.get<std::string>() == "ceiling");
    }
  } else if (t == "minecraft:lake") {
    cf->kind = ConfiguredFeature::Kind::Lake;
    cf->lakeFluid = parseBlockStateProviderJson(req(cfg, "fluid", id), id);
    cf->lakeBarrier = parseBlockStateProviderJson(req(cfg, "barrier", id), id);
  } else if (t == "minecraft:nether_forest_vegetation") {
    cf->kind = ConfiguredFeature::Kind::NetherForestVegetation;
    cf->stateProvider =
        parseBlockStateProviderJson(req(cfg, "state_provider", id), id);
    cf->spreadWidth = asInt(req(cfg, "spread_width", id), id);
    cf->spreadHeight = asInt(req(cfg, "spread_height", id), id);
  } else if (t == "minecraft:glowstone_blob") {
    cf->kind = ConfiguredFeature::Kind::GlowstoneBlob;
  } else if (t == "minecraft:netherrack_replace_blobs") {
    cf->kind = ConfiguredFeature::Kind::NetherrackReplaceBlobs;
    cf->replaceTarget = parseBlockStateJson(req(cfg, "target", id), id);
    cf->replaceState = parseBlockStateJson(req(cfg, "state", id), id);
    cf->replaceRadius = parseIntProviderJson(req(cfg, "radius", id), id);
  } else if (t == "minecraft:freeze_top_layer") {
    cf->kind = ConfiguredFeature::Kind::FreezeTopLayer;
  } else if (t == "minecraft:end_spike") {
    cf->kind = ConfiguredFeature::Kind::EndSpike;
    if (auto it = cfg.find("crystal_invulnerable"); it != cfg.end())
      cf->crystalInvulnerable = asBool(it->second, id);
    if (auto it = cfg.find("spikes");
        it != cfg.end() && it->second.is<picojson::array>()) {
      for (const auto &sv : it->second.get<picojson::array>()) {
        const auto &so = asObj(sv, id);
        ConfiguredFeature::EndSpikeDef sd;
        if (auto sit = so.find("centerX"); sit != so.end())
          sd.centerX = asInt(sit->second, id);
        if (auto sit = so.find("centerZ"); sit != so.end())
          sd.centerZ = asInt(sit->second, id);
        if (auto sit = so.find("radius"); sit != so.end())
          sd.radius = asInt(sit->second, id);
        if (auto sit = so.find("height"); sit != so.end())
          sd.height = asInt(sit->second, id);
        if (auto sit = so.find("guarded"); sit != so.end())
          sd.guarded = asBool(sit->second, id);
        cf->endSpikes.push_back(sd);
      }
    }
  } else if (t == "minecraft:end_island") {
    cf->kind = ConfiguredFeature::Kind::EndIsland;
  } else if (t == "minecraft:tree") {
    cf->kind = ConfiguredFeature::Kind::Tree;
    cf->tree.trunkProvider =
        parseBlockStateProviderJson(req(cfg, "trunk_provider", id), id);
    cf->tree.foliageProvider =
        parseBlockStateProviderJson(req(cfg, "foliage_provider", id), id);
    if (auto it = cfg.find("dirt_provider"); it != cfg.end()) {
      cf->tree.dirtProvider = parseBlockStateProviderJson(it->second, id);
    } else {
      cf->tree.dirtProvider.kind = BlockStateProvider::Kind::Simple;
      cf->tree.dirtProvider.simpleState = Blocks::DIRT.defaultState();
    }
    if (auto it = cfg.find("below_trunk_provider"); it != cfg.end()) {
      cf->tree.belowTrunkProvider = std::make_shared<BlockStateProvider>(
          parseBlockStateProviderJson(it->second, id));
    }
    if (auto it = cfg.find("ignore_vines"); it != cfg.end())
      cf->tree.ignoreVines = asBool(it->second, id);

    auto parseHolderSet = [&](const picojson::value &hv, std::string &outTag,
                              std::vector<std::string> &outIds) {
      if (hv.is<std::string>()) {
        std::string s = hv.get<std::string>();
        if (!s.empty() && s.front() == '#') {
          s.erase(s.begin());
          outTag = normId(s);
        } else {
          outIds.push_back(normId(s));
        }
      } else if (hv.is<picojson::array>()) {
        for (const auto &el : hv.get<picojson::array>()) {
          if (el.is<std::string>())
            outIds.push_back(normId(el.get<std::string>()));
        }
      }
    };

    const auto &tp = asObj(req(cfg, "trunk_placer", id), id);
    cf->tree.trunkPlacerType = normId(asStr(req(tp, "type", id), id));
    cf->tree.baseHeight = asInt(req(tp, "base_height", id), id);
    cf->tree.heightRandA = asInt(req(tp, "height_rand_a", id), id);
    cf->tree.heightRandB = asInt(req(tp, "height_rand_b", id), id);
    if (auto it = tp.find("min_height_for_leaves"); it != tp.end())
      cf->tree.minHeightForLeaves = asInt(it->second, id);
    if (auto it = tp.find("bend_length"); it != tp.end())
      cf->tree.bendLength = parseIntProviderJson(it->second, id);
    if (auto it = tp.find("branch_count"); it != tp.end())
      cf->tree.branchCount = parseIntProviderJson(it->second, id);
    if (auto it = tp.find("branch_horizontal_length"); it != tp.end())
      cf->tree.branchHorizontalLength = parseIntProviderJson(it->second, id);
    if (auto it = tp.find("branch_start_offset_from_top"); it != tp.end()) {
      const auto &bo = asObj(it->second, id);
      cf->tree.branchStartMin = asInt(req(bo, "min_inclusive", id), id);
      cf->tree.branchStartMax = asInt(req(bo, "max_inclusive", id), id);
    }
    if (auto it = tp.find("branch_end_offset_from_top"); it != tp.end())
      cf->tree.branchEndOffset = parseIntProviderJson(it->second, id);
    if (auto it = tp.find("extra_branch_steps"); it != tp.end())
      cf->tree.extraBranchSteps = parseIntProviderJson(it->second, id);
    if (auto it = tp.find("place_branch_per_log_probability"); it != tp.end())
      cf->tree.placeBranchPerLogProbability = static_cast<float>(asNum(it->second, id));
    if (auto it = tp.find("extra_branch_length"); it != tp.end())
      cf->tree.extraBranchLength = parseIntProviderJson(it->second, id);
    if (auto it = tp.find("can_grow_through"); it != tp.end()) {
      parseHolderSet(it->second, cf->tree.trunkCanGrowThroughTag,
                     cf->tree.trunkCanGrowThroughIds);
    }

    const auto &fp = asObj(req(cfg, "foliage_placer", id), id);
    cf->tree.foliagePlacerType = normId(asStr(req(fp, "type", id), id));
    cf->tree.foliageRadius = parseIntProviderJson(req(fp, "radius", id), id);
    cf->tree.foliageOffset = parseIntProviderJson(req(fp, "offset", id), id);
    if (auto it = fp.find("height"); it != fp.end()) {
      if (it->second.is<double>())
        cf->tree.foliageHeight = asInt(it->second, id);
      else {
        cf->tree.trunkHeightProvider = parseIntProviderJson(it->second, id);
        cf->tree.cherryHeight = parseIntProviderJson(it->second, id);
      }
    }
    if (auto it = fp.find("trunk_height"); it != fp.end())
      cf->tree.trunkHeightProvider = parseIntProviderJson(it->second, id);
    if (auto it = fp.find("crown_height"); it != fp.end())
      cf->tree.crownHeight = parseIntProviderJson(it->second, id);
    if (auto it = fp.find("foliage_height"); it != fp.end())
      cf->tree.trunkHeightProvider = parseIntProviderJson(it->second, id);
    if (auto it = fp.find("leaf_placement_attempts"); it != fp.end())
      cf->tree.leafPlacementAttempts = asInt(it->second, id);
    if (auto it = fp.find("wide_bottom_layer_hole_chance"); it != fp.end())
      cf->tree.wideBottomLayerHoleChance = static_cast<float>(asNum(it->second, id));
    if (auto it = fp.find("corner_hole_chance"); it != fp.end())
      cf->tree.cornerHoleChance = static_cast<float>(asNum(it->second, id));
    if (auto it = fp.find("hanging_leaves_chance"); it != fp.end())
      cf->tree.hangingLeavesChance = static_cast<float>(asNum(it->second, id));
    if (auto it = fp.find("hanging_leaves_extension_chance"); it != fp.end())
      cf->tree.hangingLeavesExtensionChance = static_cast<float>(asNum(it->second, id));

    const auto &ms = asObj(req(cfg, "minimum_size", id), id);
    if (auto it = ms.find("type");
        it != ms.end() && it->second.is<std::string>() &&
        normId(it->second.get<std::string>()) ==
            "minecraft:three_layers_feature_size") {
      cf->tree.isThreeLayersSize = true;
    }
    if (auto it = ms.find("limit"); it != ms.end())
      cf->tree.minSizeLimit = asInt(it->second, id);
    if (auto it = ms.find("upper_limit"); it != ms.end())
      cf->tree.upperSizeLimit = asInt(it->second, id);
    if (auto it = ms.find("lower_size"); it != ms.end())
      cf->tree.lowerSize = asInt(it->second, id);
    if (auto it = ms.find("middle_size"); it != ms.end())
      cf->tree.middleSize = asInt(it->second, id);
    if (auto it = ms.find("upper_size"); it != ms.end())
      cf->tree.upperSize = asInt(it->second, id);
    if (auto it = ms.find("min_clipped_height");
        it != ms.end() && it->second.is<double>()) {
      cf->tree.minClippedHeight = asInt(it->second, id);
    }

    if (auto it = cfg.find("decorators");
        it != cfg.end() && it->second.is<picojson::array>()) {
      for (const auto &dv : it->second.get<picojson::array>())
        cf->tree.decorators.push_back(parseTreeDecoratorJson(dv, id));
    }

    if (auto it = cfg.find("root_placer");
        it != cfg.end() && it->second.is<picojson::object>()) {
      const auto &ro = it->second.get<picojson::object>();
      ConfiguredFeature::RootPlacerConfig rp;
      rp.trunkOffsetY = parseIntProviderJson(req(ro, "trunk_offset_y", id), id);
      rp.rootProvider =
          parseBlockStateProviderJson(req(ro, "root_provider", id), id);
      if (auto ait = ro.find("above_root_placement");
          ait != ro.end() && ait->second.is<picojson::object>()) {
        const auto &ao = ait->second.get<picojson::object>();
        rp.aboveRootProvider = parseBlockStateProviderJson(
            req(ao, "above_root_provider", id), id);
        rp.aboveRootPlacementChance = static_cast<float>(
            asNum(req(ao, "above_root_placement_chance", id), id));
      }
      if (auto mit = ro.find("mangrove_root_placement");
          mit != ro.end() && mit->second.is<picojson::object>()) {
        const auto &mo = mit->second.get<picojson::object>();
        parseHolderSet(req(mo, "can_grow_through", id), rp.canGrowThroughTag,
                       rp.canGrowThroughIds);
        parseHolderSet(req(mo, "muddy_roots_in", id), rp.muddyRootsInTag,
                       rp.muddyRootsInIds);
        rp.muddyRootsProvider =
            parseBlockStateProviderJson(req(mo, "muddy_roots_provider", id), id);
        rp.maxRootLength = asInt(req(mo, "max_root_length", id), id);
        rp.maxRootWidth = asInt(req(mo, "max_root_width", id), id);
        rp.randomSkewChance =
            static_cast<float>(asNum(req(mo, "random_skew_chance", id), id));
      }
      cf->tree.rootPlacer = std::move(rp);
    }
  } else if (t == "minecraft:fallen_tree") {
    cf->kind = ConfiguredFeature::Kind::FallenTree;
    cf->fallenTree.trunkProvider =
        parseBlockStateProviderJson(req(cfg, "trunk_provider", id), id);
    cf->fallenTree.logLength = parseIntProviderJson(req(cfg, "log_length", id), id);
    auto parseDecs = [&](const char *key,
                         std::vector<ConfiguredFeature::TreeDecoratorConfig> &out) {
      if (auto it = cfg.find(key);
          it != cfg.end() && it->second.is<picojson::array>()) {
        for (const auto &dv : it->second.get<picojson::array>()) {
          out.push_back(parseTreeDecoratorJson(dv, id));
        }
      }
    };
    parseDecs("stump_decorators", cf->fallenTree.stumpDecorators);
    parseDecs("log_decorators", cf->fallenTree.logDecorators);
  } else if (t == "minecraft:huge_red_mushroom" ||
             t == "minecraft:huge_brown_mushroom") {
    cf->kind = (t == "minecraft:huge_red_mushroom")
                   ? ConfiguredFeature::Kind::HugeRedMushroom
                   : ConfiguredFeature::Kind::HugeBrownMushroom;
    cf->hugeMushroom.capProvider =
        parseBlockStateProviderJson(req(cfg, "cap_provider", id), id);
    cf->hugeMushroom.stemProvider =
        parseBlockStateProviderJson(req(cfg, "stem_provider", id), id);
    if (auto it = cfg.find("foliage_radius"); it != cfg.end())
      cf->hugeMushroom.foliageRadius = asInt(it->second, id);
    if (auto it = cfg.find("can_place_on"); it != cfg.end())
      cf->hugeMushroom.canPlaceOn = parseBlockPredicateJson(it->second, id);
    else {
      cf->hugeMushroom.canPlaceOn.kind = BlockPredicate::Kind::AlwaysTrue;
    }
  } else if (t == "minecraft:huge_fungus") {
    cf->kind = ConfiguredFeature::Kind::HugeFungus;
    cf->hugeFungus.validBaseState =
        parseBlockStateJson(req(cfg, "valid_base_block", id), id);
    cf->hugeFungus.stemState =
        parseBlockStateJson(req(cfg, "stem_state", id), id);
    cf->hugeFungus.hatState =
        parseBlockStateJson(req(cfg, "hat_state", id), id);
    cf->hugeFungus.decorState =
        parseBlockStateJson(req(cfg, "decor_state", id), id);
    if (auto it = cfg.find("replaceable_blocks"); it != cfg.end())
      cf->hugeFungus.replaceableBlocks = parseBlockPredicateJson(it->second, id);
    if (auto it = cfg.find("planted"); it != cfg.end())
      cf->hugeFungus.planted = asBool(it->second, id);
  } else if (t == "minecraft:seagrass") {
    cf->kind = ConfiguredFeature::Kind::Seagrass;
    cf->probability = static_cast<float>(asNum(req(cfg, "probability", id), id));
  } else if (t == "minecraft:bamboo") {
    cf->kind = ConfiguredFeature::Kind::Bamboo;
    cf->probability = static_cast<float>(asNum(req(cfg, "probability", id), id));
  } else if (t == "minecraft:basalt_columns") {
    cf->kind = ConfiguredFeature::Kind::BasaltColumns;
    cf->basaltColumns.reach = parseIntProviderJson(req(cfg, "reach", id), id);
    cf->basaltColumns.height = parseIntProviderJson(req(cfg, "height", id), id);
  } else if (t == "minecraft:basalt_pillar") {
    cf->kind = ConfiguredFeature::Kind::BasaltPillar;
  } else if (t == "minecraft:block_blob") {
    cf->kind = ConfiguredFeature::Kind::BlockBlob;
    cf->blockBlob.state = parseBlockStateJson(req(cfg, "state", id), id);
    cf->blockBlob.canPlaceOn =
        parseBlockPredicateJson(req(cfg, "can_place_on", id), id);
  } else if (t == "minecraft:blue_ice") {
    cf->kind = ConfiguredFeature::Kind::BlueIce;
  } else if (t == "minecraft:bonus_chest") {
    cf->kind = ConfiguredFeature::Kind::BonusChest;
  } else if (t == "minecraft:chorus_plant") {
    cf->kind = ConfiguredFeature::Kind::ChorusPlant;
  } else if (t == "minecraft:delta_feature") {
    cf->kind = ConfiguredFeature::Kind::DeltaFeature;
    cf->delta.contents = parseBlockStateJson(req(cfg, "contents", id), id);
    cf->delta.rim = parseBlockStateJson(req(cfg, "rim", id), id);
    cf->delta.size = parseIntProviderJson(req(cfg, "size", id), id);
    cf->delta.rimSize = parseIntProviderJson(req(cfg, "rim_size", id), id);
  } else if (t == "minecraft:desert_well") {
    cf->kind = ConfiguredFeature::Kind::DesertWell;
  } else if (t == "minecraft:dripstone_cluster") {
    cf->kind = ConfiguredFeature::Kind::DripstoneCluster;
    cf->dripstoneCluster.floorToCeilingSearchRange =
        asInt(req(cfg, "floor_to_ceiling_search_range", id), id);
    cf->dripstoneCluster.height =
        parseIntProviderJson(req(cfg, "height", id), id);
    cf->dripstoneCluster.radius =
        parseIntProviderJson(req(cfg, "radius", id), id);
    cf->dripstoneCluster.maxStalagmiteStalactiteHeightDiff =
        asInt(req(cfg, "max_stalagmite_stalactite_height_diff", id), id);
    cf->dripstoneCluster.heightDeviation =
        asInt(req(cfg, "height_deviation", id), id);
    cf->dripstoneCluster.dripstoneBlockLayerThickness =
        parseIntProviderJson(req(cfg, "dripstone_block_layer_thickness", id), id);
    cf->dripstoneCluster.density =
        parseFloatProviderJson(req(cfg, "density", id), id);
    cf->dripstoneCluster.wetness =
        parseFloatProviderJson(req(cfg, "wetness", id), id);
    cf->dripstoneCluster.chanceOfDripstoneColumnAtMaxDistanceFromCenter =
        static_cast<float>(asNum(
            req(cfg, "chance_of_dripstone_column_at_max_distance_from_center", id),
            id));
    cf->dripstoneCluster.maxDistanceFromEdgeAffectingChanceOfDripstoneColumn =
        asInt(
            req(cfg, "max_distance_from_edge_affecting_chance_of_dripstone_column",
                id),
            id);
    cf->dripstoneCluster.maxDistanceFromCenterAffectingHeightBias = asInt(
        req(cfg, "max_distance_from_center_affecting_height_bias", id), id);
  } else if (t == "minecraft:end_gateway") {
    cf->kind = ConfiguredFeature::Kind::EndGateway;
    cf->endGateway.exact = asBool(req(cfg, "exact", id), id);
    if (auto it = cfg.find("exit");
        it != cfg.end() && it->second.is<picojson::array>()) {
      const auto &arr = it->second.get<picojson::array>();
      if (arr.size() >= 3) {
        cf->endGateway.exit =
            Pos{asInt(arr[0], id), asInt(arr[1], id), asInt(arr[2], id)};
      }
    }
  } else if (t == "minecraft:end_platform") {
    cf->kind = ConfiguredFeature::Kind::EndPlatform;
  } else if (t == "minecraft:fossil") {
    cf->kind = ConfiguredFeature::Kind::Fossil;
    for (const auto &s :
         req(cfg, "fossil_structures", id).get<picojson::array>()) {
      cf->fossil.fossilStructures.push_back(normId(asStr(s, id)));
    }
    for (const auto &s :
         req(cfg, "overlay_structures", id).get<picojson::array>()) {
      cf->fossil.overlayStructures.push_back(normId(asStr(s, id)));
    }
    cf->fossil.fossilProcessors =
        normId(asStr(req(cfg, "fossil_processors", id), id));
    cf->fossil.overlayProcessors =
        normId(asStr(req(cfg, "overlay_processors", id), id));
    cf->fossil.maxEmptyCornersAllowed =
        asInt(req(cfg, "max_empty_corners_allowed", id), id);
  } else if (t == "minecraft:geode") {
    cf->kind = ConfiguredFeature::Kind::Geode;
    auto parseTagNoHash = [&](const picojson::value &tv) {
      std::string s = asStr(tv, id);
      if (!s.empty() && s.front() == '#')
        s.erase(s.begin());
      return normId(s);
    };
    const auto &blocks = asObj(req(cfg, "blocks", id), id);
    cf->geode.fillingProvider =
        parseBlockStateProviderJson(req(blocks, "filling_provider", id), id);
    cf->geode.innerLayerProvider =
        parseBlockStateProviderJson(req(blocks, "inner_layer_provider", id), id);
    cf->geode.alternateInnerLayerProvider = parseBlockStateProviderJson(
        req(blocks, "alternate_inner_layer_provider", id), id);
    cf->geode.middleLayerProvider = parseBlockStateProviderJson(
        req(blocks, "middle_layer_provider", id), id);
    cf->geode.outerLayerProvider = parseBlockStateProviderJson(
        req(blocks, "outer_layer_provider", id), id);
    for (const auto &pv :
         req(blocks, "inner_placements", id).get<picojson::array>()) {
      cf->geode.innerPlacements.push_back(parseBlockStateJson(pv, id));
    }
    cf->geode.cannotReplaceTag =
        parseTagNoHash(req(blocks, "cannot_replace", id));
    cf->geode.invalidBlocksTag =
        parseTagNoHash(req(blocks, "invalid_blocks", id));

    if (auto it = cfg.find("layers");
        it != cfg.end() && it->second.is<picojson::object>()) {
      const auto &lo = it->second.get<picojson::object>();
      if (auto lit = lo.find("filling"); lit != lo.end())
        cf->geode.filling = asNum(lit->second, id);
      if (auto lit = lo.find("inner_layer"); lit != lo.end())
        cf->geode.innerLayer = asNum(lit->second, id);
      if (auto lit = lo.find("middle_layer"); lit != lo.end())
        cf->geode.middleLayer = asNum(lit->second, id);
      if (auto lit = lo.find("outer_layer"); lit != lo.end())
        cf->geode.outerLayer = asNum(lit->second, id);
    }
    if (auto it = cfg.find("crack");
        it != cfg.end() && it->second.is<picojson::object>()) {
      const auto &co = it->second.get<picojson::object>();
      if (auto cit = co.find("generate_crack_chance"); cit != co.end())
        cf->geode.generateCrackChance = asNum(cit->second, id);
      if (auto cit = co.find("base_crack_size"); cit != co.end())
        cf->geode.baseCrackSize = asNum(cit->second, id);
      if (auto cit = co.find("crack_point_offset"); cit != co.end())
        cf->geode.crackPointOffset = asInt(cit->second, id);
    }
    if (auto it = cfg.find("use_potential_placements_chance"); it != cfg.end())
      cf->geode.usePotentialPlacementsChance = asNum(it->second, id);
    if (auto it = cfg.find("use_alternate_layer0_chance"); it != cfg.end())
      cf->geode.useAlternateLayer0Chance = asNum(it->second, id);
    if (auto it = cfg.find("placements_require_layer0_alternate");
        it != cfg.end())
      cf->geode.placementsRequireLayer0Alternate = asBool(it->second, id);
    if (auto it = cfg.find("outer_wall_distance"); it != cfg.end()) {
      cf->geode.outerWallDistance = parseIntProviderJson(it->second, id);
    } else {
      cf->geode.outerWallDistance.kind = IntProviderKind::Uniform;
      cf->geode.outerWallDistance.minInclusive = 4;
      cf->geode.outerWallDistance.maxInclusive = 5;
    }
    if (auto it = cfg.find("distribution_points"); it != cfg.end()) {
      cf->geode.distributionPoints = parseIntProviderJson(it->second, id);
    } else {
      cf->geode.distributionPoints.kind = IntProviderKind::Uniform;
      cf->geode.distributionPoints.minInclusive = 3;
      cf->geode.distributionPoints.maxInclusive = 4;
    }
    if (auto it = cfg.find("point_offset"); it != cfg.end()) {
      cf->geode.pointOffset = parseIntProviderJson(it->second, id);
    } else {
      cf->geode.pointOffset.kind = IntProviderKind::Uniform;
      cf->geode.pointOffset.minInclusive = 1;
      cf->geode.pointOffset.maxInclusive = 2;
    }
    if (auto it = cfg.find("min_gen_offset"); it != cfg.end())
      cf->geode.minGenOffset = asInt(it->second, id);
    if (auto it = cfg.find("max_gen_offset"); it != cfg.end())
      cf->geode.maxGenOffset = asInt(it->second, id);
    if (auto it = cfg.find("noise_multiplier"); it != cfg.end())
      cf->geode.noiseMultiplier = asNum(it->second, id);
    cf->geode.invalidBlocksThreshold =
        asInt(req(cfg, "invalid_blocks_threshold", id), id);
  } else if (t == "minecraft:iceberg") {
    cf->kind = ConfiguredFeature::Kind::Iceberg;
    cf->simpleBlockState = parseBlockStateJson(req(cfg, "state", id), id);
  } else if (t == "minecraft:kelp") {
    cf->kind = ConfiguredFeature::Kind::Kelp;
  } else if (t == "minecraft:large_dripstone") {
    cf->kind = ConfiguredFeature::Kind::LargeDripstone;
    if (auto it = cfg.find("floor_to_ceiling_search_range"); it != cfg.end())
      cf->largeDripstone.floorToCeilingSearchRange = asInt(it->second, id);
    cf->largeDripstone.columnRadius =
        parseIntProviderJson(req(cfg, "column_radius", id), id);
    cf->largeDripstone.heightScale =
        parseFloatProviderJson(req(cfg, "height_scale", id), id);
    cf->largeDripstone.maxColumnRadiusToCaveHeightRatio = static_cast<float>(
        asNum(req(cfg, "max_column_radius_to_cave_height_ratio", id), id));
    cf->largeDripstone.stalactiteBluntness =
        parseFloatProviderJson(req(cfg, "stalactite_bluntness", id), id);
    cf->largeDripstone.stalagmiteBluntness =
        parseFloatProviderJson(req(cfg, "stalagmite_bluntness", id), id);
    cf->largeDripstone.windSpeed =
        parseFloatProviderJson(req(cfg, "wind_speed", id), id);
    cf->largeDripstone.minRadiusForWind =
        asInt(req(cfg, "min_radius_for_wind", id), id);
    cf->largeDripstone.minBluntnessForWind =
        static_cast<float>(asNum(req(cfg, "min_bluntness_for_wind", id), id));
  } else if (t == "minecraft:monster_room") {
    cf->kind = ConfiguredFeature::Kind::MonsterRoom;
  } else if (t == "minecraft:multiface_growth") {
    cf->kind = ConfiguredFeature::Kind::MultifaceGrowth;
    if (auto it = cfg.find("block"); it != cfg.end())
      cf->multifaceGrowth.blockId = normId(asStr(it->second, id));
    if (auto it = cfg.find("search_range"); it != cfg.end())
      cf->multifaceGrowth.searchRange = asInt(it->second, id);
    if (auto it = cfg.find("can_place_on_floor"); it != cfg.end())
      cf->multifaceGrowth.canPlaceOnFloor = asBool(it->second, id);
    if (auto it = cfg.find("can_place_on_ceiling"); it != cfg.end())
      cf->multifaceGrowth.canPlaceOnCeiling = asBool(it->second, id);
    if (auto it = cfg.find("can_place_on_wall"); it != cfg.end())
      cf->multifaceGrowth.canPlaceOnWall = asBool(it->second, id);
    if (auto it = cfg.find("chance_of_spreading"); it != cfg.end()) {
      cf->multifaceGrowth.chanceOfSpreading =
          static_cast<float>(asNum(it->second, id));
    }
    const auto &placedOn = req(cfg, "can_be_placed_on", id);
    if (placedOn.is<std::string>()) {
      std::string s = placedOn.get<std::string>();
      if (!s.empty() && s.front() == '#') {
        s.erase(s.begin());
        cf->multifaceGrowth.canBePlacedOnTag = normId(s);
      } else {
        cf->multifaceGrowth.canBePlacedOnIds.push_back(normId(s));
      }
    } else if (placedOn.is<picojson::array>()) {
      for (const auto &el : placedOn.get<picojson::array>()) {
        if (el.is<std::string>())
          cf->multifaceGrowth.canBePlacedOnIds.push_back(
              normId(el.get<std::string>()));
      }
    }
  } else if (t == "minecraft:root_system") {
    cf->kind = ConfiguredFeature::Kind::RootSystem;
    cf->rootSystem.treeFeature =
        parsePlacedRefOrInline(req(cfg, "feature", id), id);
    cf->rootSystem.requiredVerticalSpaceForTree =
        asInt(req(cfg, "required_vertical_space_for_tree", id), id);
    cf->rootSystem.rootRadius = asInt(req(cfg, "root_radius", id), id);
    {
      std::string r = asStr(req(cfg, "root_replaceable", id), id);
      if (!r.empty() && r.front() == '#')
        r.erase(r.begin());
      cf->rootSystem.rootReplaceableTag = normId(r);
    }
    cf->rootSystem.rootStateProvider =
        parseBlockStateProviderJson(req(cfg, "root_state_provider", id), id);
    cf->rootSystem.rootPlacementAttempts =
        asInt(req(cfg, "root_placement_attempts", id), id);
    cf->rootSystem.rootColumnMaxHeight =
        asInt(req(cfg, "root_column_max_height", id), id);
    cf->rootSystem.hangingRootRadius =
        asInt(req(cfg, "hanging_root_radius", id), id);
    cf->rootSystem.hangingRootsVerticalSpan =
        asInt(req(cfg, "hanging_roots_vertical_span", id), id);
    cf->rootSystem.hangingRootStateProvider = parseBlockStateProviderJson(
        req(cfg, "hanging_root_state_provider", id), id);
    cf->rootSystem.hangingRootPlacementAttempts =
        asInt(req(cfg, "hanging_root_placement_attempts", id), id);
    cf->rootSystem.allowedVerticalWaterForTree =
        asInt(req(cfg, "allowed_vertical_water_for_tree", id), id);
    cf->rootSystem.allowedTreePosition =
        parseBlockPredicateJson(req(cfg, "allowed_tree_position", id), id);
  } else if (t == "minecraft:sculk_patch") {
    cf->kind = ConfiguredFeature::Kind::SculkPatch;
    cf->sculkPatch.chargeCount = asInt(req(cfg, "charge_count", id), id);
    cf->sculkPatch.amountPerCharge =
        asInt(req(cfg, "amount_per_charge", id), id);
    cf->sculkPatch.spreadAttempts = asInt(req(cfg, "spread_attempts", id), id);
    cf->sculkPatch.growthRounds = asInt(req(cfg, "growth_rounds", id), id);
    cf->sculkPatch.spreadRounds = asInt(req(cfg, "spread_rounds", id), id);
    cf->sculkPatch.extraRareGrowths =
        parseIntProviderJson(req(cfg, "extra_rare_growths", id), id);
    cf->sculkPatch.catalystChance =
        static_cast<float>(asNum(req(cfg, "catalyst_chance", id), id));
  } else if (t == "minecraft:sea_pickle") {
    cf->kind = ConfiguredFeature::Kind::SeaPickle;
    cf->countProvider = parseIntProviderJson(req(cfg, "count", id), id);
  } else if (t == "minecraft:spike") {
    cf->kind = ConfiguredFeature::Kind::Spike;
    cf->spike.state = parseBlockStateJson(req(cfg, "state", id), id);
    cf->spike.canPlaceOn =
        parseBlockPredicateJson(req(cfg, "can_place_on", id), id);
    cf->spike.canReplace =
        parseBlockPredicateJson(req(cfg, "can_replace", id), id);
  } else if (t == "minecraft:twisting_vines") {
    cf->kind = ConfiguredFeature::Kind::TwistingVines;
    cf->twistingVines.spreadWidth = asInt(req(cfg, "spread_width", id), id);
    cf->twistingVines.spreadHeight = asInt(req(cfg, "spread_height", id), id);
    cf->twistingVines.maxHeight = asInt(req(cfg, "max_height", id), id);
  } else if (t == "minecraft:underwater_magma") {
    cf->kind = ConfiguredFeature::Kind::UnderwaterMagma;
    cf->underwaterMagma.floorSearchRange =
        asInt(req(cfg, "floor_search_range", id), id);
    cf->underwaterMagma.placementRadiusAroundFloor =
        asInt(req(cfg, "placement_radius_around_floor", id), id);
    cf->underwaterMagma.placementProbabilityPerValidPosition =
        static_cast<float>(asNum(
            req(cfg, "placement_probability_per_valid_position", id), id));
  } else if (t == "minecraft:vines") {
    cf->kind = ConfiguredFeature::Kind::Vines;
  } else if (t == "minecraft:void_start_platform") {
    cf->kind = ConfiguredFeature::Kind::VoidStartPlatform;
  } else if (t == "minecraft:weeping_vines") {
    cf->kind = ConfiguredFeature::Kind::WeepingVines;
  } else {
    cf->kind = ConfiguredFeature::Kind::Unsupported;
  }
  return cf;
}

} // namespace redstone::worldgen
