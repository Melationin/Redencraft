#include "redstone/worldgen/surface_rules.hpp"

#include "redstone/worldgen/noise_router.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace redstone::worldgen {
namespace {

BlockState stateFromNameOrDefault(const char *name) {
  return BlockState(std::string(name));
}

std::int64_t packColumn(int x, int z) noexcept {
  return (static_cast<std::int64_t>(x) & 0xFFFFFFFFLL) |
         ((static_cast<std::int64_t>(z) & 0xFFFFFFFFLL) << 32);
}

int floorInt(double v) noexcept {
  const int i = static_cast<int>(v);
  return v < static_cast<double>(i) ? i - 1 : i;
}

float lerpFloat(float t, float a, float b) noexcept { return a + t * (b - a); }

float lerp2Float(float t1, float t2, int a00, int a10, int a01,
                 int a11) noexcept {
  return lerpFloat(t2, lerpFloat(t1, static_cast<float>(a00), static_cast<float>(a10)),
                   lerpFloat(t1, static_cast<float>(a01), static_cast<float>(a11)));
}

double mapDouble(double value, double fromMin, double fromMax, double toMin,
                 double toMax) noexcept {
  return worldgen_math::lerp((value - fromMin) / (fromMax - fromMin), toMin,
                             toMax);
}

int nextIntBetweenInclusive(RandomSource &rng, int minVal, int maxInclusive) {
  return rng.nextInt(maxInclusive - minVal + 1) + minVal;
}

std::array<BlockState, 192> generateClayBands(RandomSource &rng) {
  std::array<BlockState, 192> bands{};
  const BlockState terracotta = stateFromNameOrDefault("minecraft:terracotta");
  const BlockState orange =
      stateFromNameOrDefault("minecraft:orange_terracotta");
  const BlockState yellow =
      stateFromNameOrDefault("minecraft:yellow_terracotta");
  const BlockState brown = stateFromNameOrDefault("minecraft:brown_terracotta");
  const BlockState red = stateFromNameOrDefault("minecraft:red_terracotta");
  const BlockState white = stateFromNameOrDefault("minecraft:white_terracotta");
  const BlockState lightGray =
      stateFromNameOrDefault("minecraft:light_gray_terracotta");

  bands.fill(terracotta);
  for (int i = 0; i < 192; ++i) {
    i += rng.nextInt(5) + 1;
    if (i < 192)
      bands[static_cast<std::size_t>(i)] = orange;
  }

  auto makeBands = [&](int baseWidth, const BlockState &state) {
    const int bandCount = nextIntBetweenInclusive(rng, 6, 15);
    for (int i = 0; i < bandCount; ++i) {
      const int width = baseWidth + rng.nextInt(3);
      const int start = rng.nextInt(192);
      for (int p = 0; start + p < 192 && p < width; ++p)
        bands[static_cast<std::size_t>(start + p)] = state;
    }
  };
  makeBands(1, yellow);
  makeBands(2, brown);
  makeBands(1, red);

  const int whiteCount = nextIntBetweenInclusive(rng, 9, 15);
  int ix = 0;
  for (int start = 0; ix < whiteCount && start < 192;
       start += rng.nextInt(16) + 4) {
    bands[static_cast<std::size_t>(start)] = white;
    if (start - 1 > 0 && rng.nextBoolean())
      bands[static_cast<std::size_t>(start - 1)] = lightGray;
    if (start + 1 < 192 && rng.nextBoolean())
      bands[static_cast<std::size_t>(start + 1)] = lightGray;
    ++ix;
  }
  return bands;
}

bool isStoneBlock(const BlockState &state) {
  return !state.isAir() && fluidStateOf(state).isEmpty();
}

struct CondCacheEntry {
  std::int64_t lastUpdate = std::numeric_limits<std::int64_t>::min();
  bool result = false;
};

struct PreparedCondNode {
  const SurfaceConditionNode *src = nullptr;
  std::shared_ptr<const NormalNoise> noise;
  PositionalRandomFactory randomFactory;
  int resolvedTrueBelow = 0;
  int resolvedFalseAbove = 0;
  int resolvedAnchorY = 0;
  int innerNotIdx = -1;
  std::vector<std::uint64_t> biomeMask;
};

struct PreparedRuleNode {
  const SurfaceRuleNode *src = nullptr;
  int condIdx = -1;
  int thenRuleIdx = -1;
  std::uint32_t seqStart = 0;
  std::uint32_t seqCount = 0;
};

struct RuleEvalContext {
  const SurfaceSystem &system;
  RandomState &randomState;
  ChunkAccess &chunk;
  NoiseChunk &noiseChunk;
  const BiomeManager &biomeManager;
  const BiomeManager::NoiseBiomeReader &noiseBiomeReader;
  const WorldgenRegistries &registries;
  int genMinY = -64;
  int genDepth = 384;

  bool hasLastCellOrigin = false;
  std::int64_t lastCellOrigin = 0;
  std::array<int, 4> preliminarySurfaceCache{};

  std::int64_t lastUpdateXZ = std::numeric_limits<std::int64_t>::min() + 1;
  int blockX = 0;
  int blockZ = 0;
  int surfaceDepth = 0;
  std::int64_t lastSurfaceDepth2Update = std::numeric_limits<std::int64_t>::min();
  double surfaceSecondary = 0.0;
  std::int64_t lastMinSurfaceLevelUpdate = std::numeric_limits<std::int64_t>::min();
  int minSurfaceLevel = 0;

  std::int64_t lastUpdateY = std::numeric_limits<std::int64_t>::min() + 1;
  int blockY = 0;
  int waterHeight = std::numeric_limits<int>::min();
  int stoneDepthBelow = 0;
  int stoneDepthAbove = 0;

  bool biomeCached = false;
  std::uint16_t cachedBiomeIdNum = WorldgenRegistries::kInvalidBiomeId;
  std::string_view cachedBiomeId;

  // 4×4×4 Quart 角点缓存：同一纵列 y 连续下降时跨 Y 带复用顶部 4 角点，
  // 且当 8 角点同群系时完全跳过 getFiddledDistance 与底层群系查询。
  bool hasCornerCube = false;
  int cornerParentX = 0;
  int cornerParentY = 0;
  int cornerParentZ = 0;
  bool cornerUniform = false;
  std::array<std::string_view, 8> cornerNames{};
  std::array<std::uint16_t, 8> cornerIds{};

  std::vector<CondCacheEntry> condCache;

  RuleEvalContext(const SurfaceSystem &sys, RandomState &rs, ChunkAccess &c,
                  NoiseChunk &nc, const BiomeManager &bm,
                  const BiomeManager::NoiseBiomeReader &nbr,
                  const WorldgenRegistries &reg, int minY, int depth,
                  std::size_t condCount)
      : system(sys), randomState(rs), chunk(c), noiseChunk(nc),
        biomeManager(bm), noiseBiomeReader(nbr), registries(reg),
        genMinY(minY), genDepth(depth), condCache(condCount) {
    cornerIds.fill(WorldgenRegistries::kInvalidBiomeId);
  }

  void updateXZ(int x, int z) {
    ++lastUpdateXZ;
    ++lastUpdateY;
    blockX = x;
    blockZ = z;
    surfaceDepth = system.getSurfaceDepth(x, z);
    biomeCached = false;
  }

  void updateY(int above, int below, int waterH, int x, int y, int z) {
    ++lastUpdateY;
    biomeCached = false;
    blockY = y;
    waterHeight = waterH;
    stoneDepthBelow = below;
    stoneDepthAbove = above;
    (void)x;
    (void)z;
  }

  static int floorDiv4(int v) noexcept { return v >= 0 ? (v >> 2) : ((v - 3) / 4); }

  void ensureBiomeResolved() {
    if (biomeCached)
      return;
    const int absX = blockX - 2;
    const int absY = blockY - 2;
    const int absZ = blockZ - 2;
    const int pX = floorDiv4(absX);
    const int pY = floorDiv4(absY);
    const int pZ = floorDiv4(absZ);

    if (!hasCornerCube || pX != cornerParentX || pZ != cornerParentZ ||
        pY != cornerParentY) {
      if (hasCornerCube && pX == cornerParentX && pZ == cornerParentZ &&
          pY == cornerParentY - 1) {
        // 原 (yEven=true -> pY_old) 变为新的 (yEven=false -> pY_new + 1)
        constexpr int kMoveIdx[4] = {0, 1, 4, 5};
        for (int idx : kMoveIdx) {
          cornerNames[static_cast<std::size_t>(idx | 2)] =
              cornerNames[static_cast<std::size_t>(idx)];
          cornerIds[static_cast<std::size_t>(idx | 2)] =
              cornerIds[static_cast<std::size_t>(idx)];
          const int cx = (idx & 4) == 0 ? pX : pX + 1;
          const int cz = (idx & 1) == 0 ? pZ : pZ + 1;
          const std::string_view name = noiseBiomeReader(cx, pY, cz);
          cornerNames[static_cast<std::size_t>(idx)] = name;
          cornerIds[static_cast<std::size_t>(idx)] = registries.findBiomeId(name);
        }
      } else {
        for (int i = 0; i < 8; ++i) {
          const int cx = (i & 4) == 0 ? pX : pX + 1;
          const int cy = (i & 2) == 0 ? pY : pY + 1;
          const int cz = (i & 1) == 0 ? pZ : pZ + 1;
          const std::string_view name = noiseBiomeReader(cx, cy, cz);
          cornerNames[static_cast<std::size_t>(i)] = name;
          cornerIds[static_cast<std::size_t>(i)] = registries.findBiomeId(name);
        }
      }
      hasCornerCube = true;
      cornerParentX = pX;
      cornerParentY = pY;
      cornerParentZ = pZ;
      const std::uint16_t id0 = cornerIds[0];
      if (id0 != WorldgenRegistries::kInvalidBiomeId) {
        cornerUniform = true;
        for (int i = 1; i < 8; ++i) {
          if (cornerIds[static_cast<std::size_t>(i)] != id0) {
            cornerUniform = false;
            break;
          }
        }
      } else {
        const std::string_view n0 = cornerNames[0];
        cornerUniform = true;
        for (int i = 1; i < 8; ++i) {
          if (cornerNames[static_cast<std::size_t>(i)] != n0) {
            cornerUniform = false;
            break;
          }
        }
      }
    }

    if (cornerUniform) {
      cachedBiomeId = cornerNames[0];
      cachedBiomeIdNum = cornerIds[0];
      biomeCached = true;
      return;
    }

    const int minI = BiomeManager::pickCornerIndex(
        biomeManager.biomeZoomSeed(), pX, pY, pZ, absX, absY, absZ);
    cachedBiomeId = cornerNames[static_cast<std::size_t>(minI)];
    cachedBiomeIdNum = cornerIds[static_cast<std::size_t>(minI)];
    biomeCached = true;
  }

  std::string_view currentBiomeId() {
    ensureBiomeResolved();
    return cachedBiomeId;
  }

  std::uint16_t currentBiomeNum() {
    ensureBiomeResolved();
    return cachedBiomeIdNum;
  }

  double getSurfaceSecondary() {
    if (lastSurfaceDepth2Update != lastUpdateXZ) {
      lastSurfaceDepth2Update = lastUpdateXZ;
      surfaceSecondary = system.getSurfaceSecondary(blockX, blockZ);
    }
    return surfaceSecondary;
  }

  int getMinSurfaceLevel() {
    if (lastMinSurfaceLevelUpdate != lastUpdateXZ) {
      lastMinSurfaceLevelUpdate = lastUpdateXZ;
      const int cellX = blockX >> 4;
      const int cellZ = blockZ >> 4;
      const std::int64_t origin = packColumn(cellX, cellZ);
      if (!hasLastCellOrigin || lastCellOrigin != origin) {
        hasLastCellOrigin = true;
        lastCellOrigin = origin;
        preliminarySurfaceCache[0] =
            noiseChunk.preliminarySurfaceLevel(cellX << 4, cellZ << 4);
        preliminarySurfaceCache[1] =
            noiseChunk.preliminarySurfaceLevel((cellX + 1) << 4, cellZ << 4);
        preliminarySurfaceCache[2] =
            noiseChunk.preliminarySurfaceLevel(cellX << 4, (cellZ + 1) << 4);
        preliminarySurfaceCache[3] = noiseChunk.preliminarySurfaceLevel(
            (cellX + 1) << 4, (cellZ + 1) << 4);
      }
      const int prelim = floorInt(static_cast<double>(lerp2Float(
          static_cast<float>(blockX & 15) / 16.0f,
          static_cast<float>(blockZ & 15) / 16.0f, preliminarySurfaceCache[0],
          preliminarySurfaceCache[1], preliminarySurfaceCache[2],
          preliminarySurfaceCache[3])));
      minSurfaceLevel = prelim + surfaceDepth - 8;
    }
    return minSurfaceLevel;
  }
};

bool testPreparedCondition(const std::vector<PreparedCondNode> &conds,
                           int condIdx, RuleEvalContext &ctx);

bool computePreparedXZ(const PreparedCondNode &c, RuleEvalContext &ctx) {
  const SurfaceConditionNode &src = *c.src;
  if (src.kind == SurfaceConditionKind::NoiseThreshold) {
    const double v =
        c.noise->getValue(static_cast<double>(ctx.blockX), 0.0,
                          static_cast<double>(ctx.blockZ));
    return v >= src.minThreshold && v <= src.maxThreshold;
  }
  const int lx = ctx.blockX & 15;
  const int lz = ctx.blockZ & 15;
  const int zNorth = std::max(lz - 1, 0);
  const int zSouth = std::min(lz + 1, 15);
  const int hNorth =
      ctx.chunk.getHeight(HeightmapType::WorldSurfaceWG, lx, zNorth);
  const int hSouth =
      ctx.chunk.getHeight(HeightmapType::WorldSurfaceWG, lx, zSouth);
  if (hSouth >= hNorth + 4)
    return true;
  const int xWest = std::max(lx - 1, 0);
  const int xEast = std::min(lx + 1, 15);
  const int hWest =
      ctx.chunk.getHeight(HeightmapType::WorldSurfaceWG, xWest, lz);
  const int hEast =
      ctx.chunk.getHeight(HeightmapType::WorldSurfaceWG, xEast, lz);
  return hWest >= hEast + 4;
}

bool computePreparedY(const PreparedCondNode &c, RuleEvalContext &ctx) {
  const SurfaceConditionNode &src = *c.src;
  switch (src.kind) {
  case SurfaceConditionKind::Biome: {
    const std::uint16_t bid = ctx.currentBiomeNum();
    if (bid != WorldgenRegistries::kInvalidBiomeId)
      return WorldgenRegistries::biomeMatchesMask(bid, c.biomeMask);
    const std::string_view id = ctx.currentBiomeId();
    return std::find(src.biomes.begin(), src.biomes.end(), id) !=
           src.biomes.end();
  }
  case SurfaceConditionKind::VerticalGradient: {
    const int y = ctx.blockY;
    if (y <= c.resolvedTrueBelow)
      return true;
    if (y >= c.resolvedFalseAbove)
      return false;
    const double prob = mapDouble(
        static_cast<double>(y), static_cast<double>(c.resolvedTrueBelow),
        static_cast<double>(c.resolvedFalseAbove), 1.0, 0.0);
    AnyRandomSource rng = c.randomFactory.at(ctx.blockX, y, ctx.blockZ);
    return static_cast<double>(rng.nextFloat()) < prob;
  }
  case SurfaceConditionKind::YAbove: {
    const int lhs =
        ctx.blockY + (src.addStoneDepth ? ctx.stoneDepthAbove : 0);
    const int rhs =
        c.resolvedAnchorY + ctx.surfaceDepth * src.surfaceDepthMultiplier;
    return lhs >= rhs;
  }
  case SurfaceConditionKind::Water: {
    if (ctx.waterHeight == std::numeric_limits<int>::min())
      return true;
    const int lhs =
        ctx.blockY + (src.addStoneDepth ? ctx.stoneDepthAbove : 0);
    const int rhs =
        ctx.waterHeight + src.offset +
        ctx.surfaceDepth * src.surfaceDepthMultiplier;
    return lhs >= rhs;
  }
  case SurfaceConditionKind::Temperature: {
    const std::uint16_t bid = ctx.currentBiomeNum();
    const BiomeClimateProperties *clim =
        bid != WorldgenRegistries::kInvalidBiomeId
            ? ctx.registries.biomeClimateById(bid)
            : ctx.registries.findBiomeClimate(ctx.currentBiomeId());
    const BiomeClimateProperties def{};
    const BiomeClimateProperties &props = clim ? *clim : def;
    return props.coldEnoughToSnow(ctx.blockX, ctx.blockY, ctx.blockZ,
                                  ctx.system.seaLevel());
  }
  case SurfaceConditionKind::StoneDepth: {
    const int depth = src.ceiling ? ctx.stoneDepthBelow : ctx.stoneDepthAbove;
    const int depthAdd = src.addSurfaceDepth ? ctx.surfaceDepth : 0;
    const int secAdd =
        src.secondaryDepthRange == 0
            ? 0
            : static_cast<int>(mapDouble(
                  ctx.getSurfaceSecondary(), -1.0, 1.0, 0.0,
                  static_cast<double>(src.secondaryDepthRange)));
    return depth <= 1 + src.offset + depthAdd + secAdd;
  }
  default:
    return false;
  }
}

bool testPreparedCondition(const std::vector<PreparedCondNode> &conds,
                           int condIdx, RuleEvalContext &ctx) {
  const PreparedCondNode &c = conds[static_cast<std::size_t>(condIdx)];
  const SurfaceConditionNode &src = *c.src;
  switch (src.kind) {
  case SurfaceConditionKind::Hole:
    return ctx.surfaceDepth <= 0;
  case SurfaceConditionKind::AbovePreliminarySurface:
    return ctx.blockY >= ctx.getMinSurfaceLevel();
  case SurfaceConditionKind::Not:
    return c.innerNotIdx >= 0 ? !testPreparedCondition(conds, c.innerNotIdx, ctx)
                              : true;
  case SurfaceConditionKind::Biome:
  case SurfaceConditionKind::VerticalGradient:
  case SurfaceConditionKind::YAbove:
  case SurfaceConditionKind::Water:
  case SurfaceConditionKind::Temperature:
  case SurfaceConditionKind::StoneDepth: {
    CondCacheEntry &slot = ctx.condCache[static_cast<std::size_t>(condIdx)];
    if (slot.lastUpdate == ctx.lastUpdateY)
      return slot.result;
    slot.lastUpdate = ctx.lastUpdateY;
    slot.result = computePreparedY(c, ctx);
    return slot.result;
  }
  case SurfaceConditionKind::NoiseThreshold:
  case SurfaceConditionKind::Steep: {
    CondCacheEntry &slot = ctx.condCache[static_cast<std::size_t>(condIdx)];
    if (slot.lastUpdate == ctx.lastUpdateXZ)
      return slot.result;
    slot.lastUpdate = ctx.lastUpdateXZ;
    slot.result = computePreparedXZ(c, ctx);
    return slot.result;
  }
  }
  return false;
}

std::optional<BlockState>
applyPreparedRule(const std::vector<PreparedRuleNode> &rules,
                  const std::vector<PreparedCondNode> &conds,
                  const std::vector<int> &seqChildren, int ruleIdx,
                  RuleEvalContext &ctx, int x, int y, int z) {
  const PreparedRuleNode &r = rules[static_cast<std::size_t>(ruleIdx)];
  const SurfaceRuleNode &src = *r.src;
  switch (src.kind) {
  case SurfaceRuleKind::Bandlands:
    return ctx.system.getBand(x, y, z);
  case SurfaceRuleKind::Block:
    return src.blockState;
  case SurfaceRuleKind::Sequence: {
    const std::uint32_t end = r.seqStart + r.seqCount;
    for (std::uint32_t i = r.seqStart; i < end; ++i) {
      if (auto res = applyPreparedRule(rules, conds, seqChildren,
                                       seqChildren[i], ctx, x, y, z);
          res.has_value()) {
        return res;
      }
    }
    return std::nullopt;
  }
  case SurfaceRuleKind::Condition:
    if (r.condIdx >= 0 && !testPreparedCondition(conds, r.condIdx, ctx))
      return std::nullopt;
    return r.thenRuleIdx >= 0
               ? applyPreparedRule(rules, conds, seqChildren, r.thenRuleIdx, ctx,
                                   x, y, z)
               : std::nullopt;
  }
  return std::nullopt;
}

void setSurfaceColumnBlock(ChunkAccess &chunk, int x, int y, int z,
                           const BlockState &state) {
  if (chunk.isOutsideBuildHeight(y))
    return;
  chunk.setBlockState(x, y, z, state, 3);
  if (!fluidStateOf(state).isEmpty())
    chunk.markPosForPostprocessing(x, y, z);
}

} // namespace

struct SurfaceSystem::PreparedProgram {
  std::vector<PreparedCondNode> condNodes;
  std::vector<PreparedRuleNode> ruleNodes;
  std::vector<int> seqChildren;
  int rootRuleIdx = -1;

  int compileCond(const SurfaceConditionNode &node, RandomState &rs,
                  const WorldgenRegistries &reg, int genMinY, int genDepth) {
    const int idx = static_cast<int>(condNodes.size());
    condNodes.emplace_back();
    condNodes[static_cast<std::size_t>(idx)].src = &node;
    switch (node.kind) {
    case SurfaceConditionKind::Biome:
      reg.buildBiomeSelectorMask(
          node.biomes, condNodes[static_cast<std::size_t>(idx)].biomeMask);
      break;
    case SurfaceConditionKind::NoiseThreshold:
      condNodes[static_cast<std::size_t>(idx)].noise =
          rs.getOrCreateNoise(node.noiseId);
      break;
    case SurfaceConditionKind::VerticalGradient:
      condNodes[static_cast<std::size_t>(idx)].resolvedTrueBelow =
          node.trueAtAndBelow.resolveY(genMinY, genDepth);
      condNodes[static_cast<std::size_t>(idx)].resolvedFalseAbove =
          node.falseAtAndAbove.resolveY(genMinY, genDepth);
      condNodes[static_cast<std::size_t>(idx)].randomFactory =
          rs.getOrCreateRandomFactory(node.randomName);
      break;
    case SurfaceConditionKind::YAbove:
      condNodes[static_cast<std::size_t>(idx)].resolvedAnchorY =
          node.anchor.resolveY(genMinY, genDepth);
      break;
    case SurfaceConditionKind::Not:
      if (node.target) {
        const int inner = compileCond(*node.target, rs, reg, genMinY, genDepth);
        condNodes[static_cast<std::size_t>(idx)].innerNotIdx = inner;
      }
      break;
    default:
      break;
    }
    return idx;
  }

  int compileRule(const SurfaceRuleNode &node, RandomState &rs,
                  const WorldgenRegistries &reg, int genMinY, int genDepth) {
    const int idx = static_cast<int>(ruleNodes.size());
    ruleNodes.emplace_back();
    ruleNodes[static_cast<std::size_t>(idx)].src = &node;
    switch (node.kind) {
    case SurfaceRuleKind::Sequence: {
      std::vector<int> kids;
      kids.reserve(node.sequence.size());
      for (const auto &child : node.sequence) {
        if (child)
          kids.push_back(compileRule(*child, rs, reg, genMinY, genDepth));
      }
      const std::uint32_t start = static_cast<std::uint32_t>(seqChildren.size());
      seqChildren.insert(seqChildren.end(), kids.begin(), kids.end());
      ruleNodes[static_cast<std::size_t>(idx)].seqStart = start;
      ruleNodes[static_cast<std::size_t>(idx)].seqCount =
          static_cast<std::uint32_t>(kids.size());
      break;
    }
    case SurfaceRuleKind::Condition: {
      const int cIdx =
          node.ifTrue ? compileCond(*node.ifTrue, rs, reg, genMinY, genDepth)
                      : -1;
      const int tIdx =
          node.thenRun ? compileRule(*node.thenRun, rs, reg, genMinY, genDepth)
                       : -1;
      ruleNodes[static_cast<std::size_t>(idx)].condIdx = cIdx;
      ruleNodes[static_cast<std::size_t>(idx)].thenRuleIdx = tIdx;
      break;
    }
    case SurfaceRuleKind::Bandlands:
    case SurfaceRuleKind::Block:
      break;
    }
    return idx;
  }
};

namespace surface_rules {

SurfaceConditionSourcePtr biome(std::vector<std::string> biomes) {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::Biome;
  n->biomes = std::move(biomes);
  return n;
}

SurfaceConditionSourcePtr noiseThreshold(std::string noiseId, double minThresh,
                                         double maxThresh) {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::NoiseThreshold;
  n->noiseId = std::move(noiseId);
  n->minThreshold = minThresh;
  n->maxThreshold = maxThresh;
  return n;
}

SurfaceConditionSourcePtr verticalGradient(std::string randomName,
                                           VerticalAnchor trueAtAndBelow,
                                           VerticalAnchor falseAtAndAbove) {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::VerticalGradient;
  n->randomName = std::move(randomName);
  n->trueAtAndBelow = trueAtAndBelow;
  n->falseAtAndAbove = falseAtAndAbove;
  return n;
}

SurfaceConditionSourcePtr yAbove(VerticalAnchor anchor,
                                 int surfaceDepthMultiplier,
                                 bool addStoneDepth) {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::YAbove;
  n->anchor = anchor;
  n->surfaceDepthMultiplier = surfaceDepthMultiplier;
  n->addStoneDepth = addStoneDepth;
  return n;
}

SurfaceConditionSourcePtr water(int offset, int surfaceDepthMultiplier,
                                bool addStoneDepth) {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::Water;
  n->offset = offset;
  n->surfaceDepthMultiplier = surfaceDepthMultiplier;
  n->addStoneDepth = addStoneDepth;
  return n;
}

SurfaceConditionSourcePtr temperature() {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::Temperature;
  return n;
}

SurfaceConditionSourcePtr steep() {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::Steep;
  return n;
}

SurfaceConditionSourcePtr hole() {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::Hole;
  return n;
}

SurfaceConditionSourcePtr abovePreliminarySurface() {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::AbovePreliminarySurface;
  return n;
}

SurfaceConditionSourcePtr stoneDepth(int offset, bool addSurfaceDepth,
                                     int secondaryDepthRange, bool ceiling) {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::StoneDepth;
  n->offset = offset;
  n->addSurfaceDepth = addSurfaceDepth;
  n->secondaryDepthRange = secondaryDepthRange;
  n->ceiling = ceiling;
  return n;
}

SurfaceConditionSourcePtr invert(SurfaceConditionSourcePtr target) {
  auto n = std::make_shared<SurfaceConditionNode>();
  n->kind = SurfaceConditionKind::Not;
  n->target = std::move(target);
  return n;
}

SurfaceRuleSourcePtr bandlands() {
  auto n = std::make_shared<SurfaceRuleNode>();
  n->kind = SurfaceRuleKind::Bandlands;
  return n;
}

SurfaceRuleSourcePtr block(BlockState state) {
  auto n = std::make_shared<SurfaceRuleNode>();
  n->kind = SurfaceRuleKind::Block;
  n->blockState = state;
  return n;
}

SurfaceRuleSourcePtr sequence(std::vector<SurfaceRuleSourcePtr> rules) {
  if (rules.size() == 1)
    return rules.front();
  auto n = std::make_shared<SurfaceRuleNode>();
  n->kind = SurfaceRuleKind::Sequence;
  n->sequence = std::move(rules);
  return n;
}

SurfaceRuleSourcePtr condition(SurfaceConditionSourcePtr ifTrue,
                               SurfaceRuleSourcePtr thenRun) {
  auto n = std::make_shared<SurfaceRuleNode>();
  n->kind = SurfaceRuleKind::Condition;
  n->ifTrue = std::move(ifTrue);
  n->thenRun = std::move(thenRun);
  return n;
}

} // namespace surface_rules

SurfaceSystem::SurfaceSystem(RandomState &randomState, BlockState defaultBlock,
                             int seaLevel,
                             const PositionalRandomFactory &noiseRandom)
    : defaultBlock_(defaultBlock), seaLevel_(seaLevel),
      noiseRandom_(noiseRandom),
      programMutex_(std::make_unique<std::mutex>()) {
  clayBandsOffsetNoise_ =
      randomState.getOrCreateNoise("minecraft:clay_bands_offset");
  AnyRandomSource bandsRng = noiseRandom_.fromHashOf("minecraft:clay_bands");
  clayBands_ = generateClayBands(bandsRng);
  surfaceNoise_ = randomState.getOrCreateNoise("minecraft:surface");
  surfaceSecondaryNoise_ =
      randomState.getOrCreateNoise("minecraft:surface_secondary");
  badlandsPillarNoise_ =
      randomState.getOrCreateNoise("minecraft:badlands_pillar");
  badlandsPillarRoofNoise_ =
      randomState.getOrCreateNoise("minecraft:badlands_pillar_roof");
  badlandsSurfaceNoise_ =
      randomState.getOrCreateNoise("minecraft:badlands_surface");
  icebergPillarNoise_ = randomState.getOrCreateNoise("minecraft:iceberg_pillar");
  icebergPillarRoofNoise_ =
      randomState.getOrCreateNoise("minecraft:iceberg_pillar_roof");
  icebergSurfaceNoise_ =
      randomState.getOrCreateNoise("minecraft:iceberg_surface");
}

int SurfaceSystem::getSurfaceDepth(int blockX, int blockZ) const {
  const double noiseVal = surfaceNoise_->getValue(
      static_cast<double>(blockX), 0.0, static_cast<double>(blockZ));
  AnyRandomSource rng = noiseRandom_.at(blockX, 0, blockZ);
  return static_cast<int>(noiseVal * 2.75 + 3.0 + rng.nextDouble() * 0.25);
}

double SurfaceSystem::getSurfaceSecondary(int blockX, int blockZ) const {
  return surfaceSecondaryNoise_->getValue(static_cast<double>(blockX), 0.0,
                                          static_cast<double>(blockZ));
}

BlockState SurfaceSystem::getBand(int worldX, int y, int worldZ) const {
  const int offset = static_cast<int>(std::llround(
      clayBandsOffsetNoise_->getValue(static_cast<double>(worldX), 0.0,
                                      static_cast<double>(worldZ)) *
      4.0));
  const int raw = y + offset + 192;
  const int mod = raw % 192;
  return clayBands_[static_cast<std::size_t>(mod < 0 ? mod + 192 : mod)];
}

void SurfaceSystem::erodedBadlandsExtension(ChunkAccess &chunk, int blockX,
                                            int blockZ, int height) const {
  const double pillarBuffer = std::min(
      std::abs(badlandsSurfaceNoise_->getValue(static_cast<double>(blockX), 0.0,
                                               static_cast<double>(blockZ)) *
               8.25),
      badlandsPillarNoise_->getValue(static_cast<double>(blockX) * 0.2, 0.0,
                                     static_cast<double>(blockZ) * 0.2) *
          15.0);
  if (pillarBuffer <= 0.0)
    return;

  const double pillarFloor = std::abs(
      badlandsPillarRoofNoise_->getValue(static_cast<double>(blockX) * 0.75, 0.0,
                                         static_cast<double>(blockZ) * 0.75) *
      1.5);
  const double extensionTop =
      64.0 + std::min(pillarBuffer * pillarBuffer * 2.5,
                      std::ceil(pillarFloor * 50.0) + 24.0);
  const int startY = floorInt(extensionTop);
  if (height > startY)
    return;

  for (int y = startY; y >= chunk.minY(); --y) {
    const BlockState oldState = chunk.getBlockState(blockX, y, blockZ);
    if (oldState.sameBlock(defaultBlock_))
      break;
    if (oldState.is(Blocks::WATER))
      return;
  }
  for (int y = startY;
       y >= chunk.minY() && chunk.getBlockState(blockX, y, blockZ).isAir();
       --y) {
    chunk.setBlockState(blockX, y, blockZ, defaultBlock_, 3);
  }
}

void SurfaceSystem::frozenOceanExtension(
    int minSurfaceLevel, const BiomeClimateProperties &biomeClimate,
    ChunkAccess &chunk, int blockX, int blockZ, int height) const {
  const double iceberg = std::min(
      std::abs(icebergSurfaceNoise_->getValue(static_cast<double>(blockX), 0.0,
                                              static_cast<double>(blockZ)) *
               8.25),
      icebergPillarNoise_->getValue(static_cast<double>(blockX) * 1.28, 0.0,
                                    static_cast<double>(blockZ) * 1.28) *
          15.0);
  if (iceberg <= 1.8)
    return;

  const double icebergRoof = std::abs(
      icebergPillarRoofNoise_->getValue(static_cast<double>(blockX) * 1.17, 0.0,
                                        static_cast<double>(blockZ) * 1.17) *
      1.5);
  double top =
      std::min(iceberg * iceberg * 1.2, std::ceil(icebergRoof * 40.0) + 14.0);
  if (biomeClimate.shouldMeltFrozenOceanIcebergSlightly(blockX, seaLevel_,
                                                       blockZ, seaLevel_)) {
    top -= 2.0;
  }

  double extensionBottom;
  if (top > 2.0) {
    extensionBottom = static_cast<double>(seaLevel_) - top - 7.0;
    top += static_cast<double>(seaLevel_);
  } else {
    top = 0.0;
    extensionBottom = 0.0;
  }

  const double extensionTop = top;
  AnyRandomSource rng = noiseRandom_.at(blockX, 0, blockZ);
  const int maxSnowDepth = 2 + rng.nextInt(4);
  const int minSnowHeight = seaLevel_ + 18 + rng.nextInt(10);
  int snowDepth = 0;

  const BlockState packedIce = stateFromNameOrDefault("minecraft:packed_ice");
  const BlockState snowBlock = stateFromNameOrDefault("minecraft:snow_block");

  for (int y = std::max(height, static_cast<int>(top) + 1); y >= minSurfaceLevel;
       --y) {
    const BlockState cur = chunk.getBlockState(blockX, y, blockZ);
    if ((cur.isAir() && y < static_cast<int>(extensionTop) &&
         rng.nextDouble() > 0.01) ||
        (cur.is(Blocks::WATER) &&
         y > static_cast<int>(extensionBottom) && y < seaLevel_ &&
         extensionBottom != 0.0 && rng.nextDouble() > 0.15)) {
      if (snowDepth <= maxSnowDepth && y > minSnowHeight) {
        chunk.setBlockState(blockX, y, blockZ, snowBlock, 3);
        ++snowDepth;
      } else {
        chunk.setBlockState(blockX, y, blockZ, packedIce, 3);
      }
    }
  }
}

std::shared_ptr<const SurfaceSystem::PreparedProgram>
SurfaceSystem::getOrCompileProgram(
    RandomState &randomState, const WorldgenRegistries &registries, int genMinY,
    int genDepth, const SurfaceRuleSourcePtr &ruleSource) const {
  const ProgramCacheKey key{ruleSource.get(), &registries, genMinY, genDepth};
  std::lock_guard<std::mutex> lock(*programMutex_);
  for (const auto &entry : cachedPrograms_) {
    if (entry.key == key)
      return entry.program;
  }
  auto prog = std::make_shared<PreparedProgram>();
  prog->rootRuleIdx =
      prog->compileRule(*ruleSource, randomState, registries, genMinY, genDepth);
  cachedPrograms_.push_back(CachedProgramEntry{key, prog});
  return prog;
}

void SurfaceSystem::buildSurface(
    RandomState &randomState, const BiomeManager &biomeManager,
    const BiomeManager::NoiseBiomeReader &noiseBiomeReader,
    const WorldgenRegistries &registries, bool useLegacyRandom, int genMinY,
    int genDepth, ChunkAccess &chunk, NoiseChunk &noiseChunk,
    const SurfaceRuleSourcePtr &ruleSource) const {
  if (!ruleSource)
    return;

  const auto program = getOrCompileProgram(randomState, registries, genMinY,
                                           genDepth, ruleSource);
  const int minBlockX = chunk.pos().x * 16;
  const int minBlockZ = chunk.pos().z * 16;
  RuleEvalContext ctx(*this, randomState, chunk, noiseChunk, biomeManager,
                      noiseBiomeReader, registries, genMinY, genDepth,
                      program->condNodes.size());

  const std::uint16_t erodedBadlandsId =
      registries.findBiomeId("minecraft:eroded_badlands");
  const std::uint16_t frozenOceanId =
      registries.findBiomeId("minecraft:frozen_ocean");
  const std::uint16_t deepFrozenOceanId =
      registries.findBiomeId("minecraft:deep_frozen_ocean");

  for (int x = 0; x < 16; ++x) {
    for (int z = 0; z < 16; ++z) {
      const int blockX = minBlockX + x;
      const int blockZ = minBlockZ + z;
      const int startingHeight =
          chunk.getHeight(HeightmapType::WorldSurfaceWG, x, z) + 1;
      const std::string_view surfaceBiome = biomeManager.getBiome(
          blockX, useLegacyRandom ? 0 : startingHeight, blockZ,
          noiseBiomeReader);
      const std::uint16_t surfaceBiomeId = registries.findBiomeId(surfaceBiome);

      if ((erodedBadlandsId != WorldgenRegistries::kInvalidBiomeId &&
           surfaceBiomeId == erodedBadlandsId) ||
          surfaceBiome == "minecraft:eroded_badlands") {
        erodedBadlandsExtension(chunk, blockX, blockZ, startingHeight);
      }

      const int height =
          chunk.getHeight(HeightmapType::WorldSurfaceWG, x, z) + 1;
      ctx.updateXZ(blockX, blockZ);
      int stoneAboveDepth = 0;
      int waterHeight = std::numeric_limits<int>::min();
      int nextCeilingStoneY = std::numeric_limits<int>::max();
      const int endY = chunk.minY();

      for (int y = height; y >= endY; --y) {
        const BlockState old = chunk.getBlockState(blockX, y, blockZ);
        if (old.isAir()) {
          stoneAboveDepth = 0;
          waterHeight = std::numeric_limits<int>::min();
        } else if (!fluidStateOf(old).isEmpty()) {
          if (waterHeight == std::numeric_limits<int>::min())
            waterHeight = y + 1;
        } else {
          if (nextCeilingStoneY >= y) {
            nextCeilingStoneY = kWayBelowMinY;
            for (int lookaheadY = y - 1; lookaheadY >= endY - 1; --lookaheadY) {
              const BlockState nextState =
                  chunk.getBlockState(blockX, lookaheadY, blockZ);
              if (!isStoneBlock(nextState)) {
                nextCeilingStoneY = lookaheadY + 1;
                break;
              }
            }
          }

          ++stoneAboveDepth;
          const int stoneBelowDepth = y - nextCeilingStoneY + 1;
          ctx.updateY(stoneAboveDepth, stoneBelowDepth, waterHeight, blockX, y,
                      blockZ);
          if (old == defaultBlock_ && program->rootRuleIdx >= 0) {
            if (auto replacement = applyPreparedRule(
                    program->ruleNodes, program->condNodes,
                    program->seqChildren, program->rootRuleIdx, ctx, blockX, y,
                    blockZ);
                replacement.has_value()) {
              setSurfaceColumnBlock(chunk, blockX, y, blockZ, *replacement);
            }
          }
        }
      }

      const bool isFrozenOcean =
          (surfaceBiomeId != WorldgenRegistries::kInvalidBiomeId)
              ? (surfaceBiomeId == frozenOceanId ||
                 surfaceBiomeId == deepFrozenOceanId)
              : (surfaceBiome == "minecraft:frozen_ocean" ||
                 surfaceBiome == "minecraft:deep_frozen_ocean");
      if (isFrozenOcean) {
        const BiomeClimateProperties *clim =
            surfaceBiomeId != WorldgenRegistries::kInvalidBiomeId
                ? registries.biomeClimateById(surfaceBiomeId)
                : registries.findBiomeClimate(surfaceBiome);
        const BiomeClimateProperties def{};
        frozenOceanExtension(ctx.getMinSurfaceLevel(), clim ? *clim : def,
                             chunk, blockX, blockZ, startingHeight);
      }
    }
  }
}

std::optional<BlockState> SurfaceSystem::topMaterial(
    RandomState &randomState, const BiomeManager &biomeManager,
    const BiomeManager::NoiseBiomeReader &noiseBiomeReader,
    const WorldgenRegistries &registries, int genMinY, int genDepth,
    ChunkAccess &chunk, NoiseChunk &noiseChunk,
    const SurfaceRuleSourcePtr &ruleSource, int blockX, int blockY, int blockZ,
    bool underFluid) const {
  if (!ruleSource)
    return std::nullopt;
  const auto program = getOrCompileProgram(randomState, registries, genMinY,
                                           genDepth, ruleSource);
  if (program->rootRuleIdx < 0)
    return std::nullopt;
  RuleEvalContext ctx(*this, randomState, chunk, noiseChunk, biomeManager,
                      noiseBiomeReader, registries, genMinY, genDepth,
                      program->condNodes.size());
  ctx.updateXZ(blockX, blockZ);
  ctx.updateY(1, 1,
              underFluid ? blockY + 1 : std::numeric_limits<int>::min(),
              blockX, blockY, blockZ);
  return applyPreparedRule(program->ruleNodes, program->condNodes,
                           program->seqChildren, program->rootRuleIdx, ctx,
                           blockX, blockY, blockZ);
}

} // namespace redstone::worldgen
