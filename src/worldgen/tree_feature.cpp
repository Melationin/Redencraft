#include "redstone/worldgen/feature.hpp"

#include "redstone/block_tags.hpp"
#include "redstone/hash.hpp"
#include "redstone/worldgen/chunk_generator.hpp"
#include "redstone/worldgen/noise_chunk_generator.hpp"
#include "redstone/worldgen/noise_router.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace redstone::worldgen {
namespace {

using feature_detail::blockMatchesFaceSturdyUp;
using feature_detail::regionGetBlockState;
using feature_detail::regionGetFluidState;
using feature_detail::regionGetHeight;
using feature_detail::regionSetBlockState;

constexpr double kPi = 3.14159265358979323846;

struct PosHash {
  std::size_t operator()(Pos p) const noexcept {
    std::size_t h = static_cast<std::size_t>(p.x) * 73856093ULL;
    h ^= static_cast<std::size_t>(p.y) * 19349663ULL;
    h ^= static_cast<std::size_t>(p.z) * 83492791ULL;
    return h;
  }
};

struct PosSet {
  std::vector<Pos> order;
  phmap::flat_hash_set<Pos, PosHash> seen;

  bool add(Pos p) {
    if (seen.insert(p).second) {
      order.push_back(p);
      return true;
    }
    return false;
  }
  bool contains(Pos p) const noexcept { return seen.count(p) != 0; }
  bool empty() const noexcept { return order.empty(); }
};

int floorFloat(float v) noexcept {
  const int i = static_cast<int>(v);
  return v < static_cast<float>(i) ? i - 1 : i;
}

int floorDouble(double v) noexcept {
  const int i = static_cast<int>(v);
  return v < static_cast<double>(i) ? i - 1 : i;
}

int manhattanDist(Pos a, Pos b) noexcept {
  return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
}

Direction randomHorizontalDir(RandomSource &rng) {
  static constexpr std::array<Direction, 4> kHoriz{
      Direction::North, Direction::East, Direction::South, Direction::West};
  return kHoriz[rng.nextInt(4)];
}

Direction clockwiseDir(Direction d) noexcept {
  switch (d) {
  case Direction::North:
    return Direction::East;
  case Direction::East:
    return Direction::South;
  case Direction::South:
    return Direction::West;
  case Direction::West:
    return Direction::North;
  default:
    return d;
  }
}

Direction oppositeDir(Direction d) noexcept {
  switch (d) {
  case Direction::North:
    return Direction::South;
  case Direction::South:
    return Direction::North;
  case Direction::East:
    return Direction::West;
  case Direction::West:
    return Direction::East;
  case Direction::Up:
    return Direction::Down;
  case Direction::Down:
    return Direction::Up;
  }
  return d;
}

std::string_view dirAxisName(Direction d) noexcept {
  switch (d) {
  case Direction::East:
  case Direction::West:
    return "x";
  case Direction::North:
  case Direction::South:
    return "z";
  default:
    return "y";
  }
}

Pos stepPos(Pos p, Direction d, int n = 1) noexcept {
  const Pos unit = Pos{0, 0, 0}.relative(d);
  return Pos{p.x + unit.x * n, p.y + unit.y * n, p.z + unit.z * n};
}

std::string_view dirNameLower(Direction d) noexcept {
  switch (d) {
  case Direction::North:
    return "north";
  case Direction::South:
    return "south";
  case Direction::West:
    return "west";
  case Direction::East:
    return "east";
  case Direction::Up:
    return "up";
  case Direction::Down:
    return "down";
  }
  return "north";
}

bool matchesHolderSet(const BlockState &s, std::string_view tag,
                      const std::vector<std::string> &ids) {
  if (!tag.empty() && blockHasTag(s, tag).value_or(false))
    return true;
  if (!ids.empty()) {
    const std::string_view name = s.nameView();
    for (const auto &id : ids) {
      if (name == id)
        return true;
    }
  }
  return false;
}

bool validTreePos(const WorldGenRegion &region, Pos p) {
  const BlockState s = regionGetBlockState(region, p.x, p.y, p.z);
  return s.isAir() ||
         blockHasTag(s, "minecraft:replaceable_by_trees").value_or(false);
}

bool trunkValidTreePos(const ConfiguredFeature::TreeConfig &cfg,
                       const WorldGenRegion &region, Pos p) {
  if (validTreePos(region, p))
    return true;
  if (!cfg.trunkCanGrowThroughTag.empty() ||
      !cfg.trunkCanGrowThroughIds.empty()) {
    const BlockState s = regionGetBlockState(region, p.x, p.y, p.z);
    return matchesHolderSet(s, cfg.trunkCanGrowThroughTag,
                            cfg.trunkCanGrowThroughIds);
  }
  return false;
}

bool isTrunkFree(const ConfiguredFeature::TreeConfig &cfg,
                 const WorldGenRegion &region, Pos p) {
  if (trunkValidTreePos(cfg, region, p))
    return true;
  const BlockState s = regionGetBlockState(region, p.x, p.y, p.z);
  return blockHasTag(s, "minecraft:logs").value_or(false);
}

bool isVineAt(const WorldGenRegion &region, Pos p) {
  return regionGetBlockState(region, p.x, p.y, p.z).is(Blocks::VINE);
}

bool isAirOrLeavesAt(const WorldGenRegion &region, Pos p) {
  const BlockState s = regionGetBlockState(region, p.x, p.y, p.z);
  return s.isAir() || blockHasTag(s, "minecraft:leaves").value_or(false);
}

void placeBelowTrunkBlock(const ConfiguredFeature::TreeConfig &cfg,
                          WorldGenRegion &region, PosSet &trunks,
                          RandomSource &rng, Pos pos) {
  std::optional<BlockState> st;
  if (cfg.belowTrunkProvider) {
    st = cfg.belowTrunkProvider->getOptionalState(region, rng, pos);
  } else {
    st = cfg.dirtProvider.getOptionalState(region, rng, pos);
  }
  if (st) {
    if (regionSetBlockState(region, pos.x, pos.y, pos.z, *st, 19))
      trunks.add(pos);
  }
}

bool placeTreeLog(const ConfiguredFeature::TreeConfig &cfg,
                  WorldGenRegion &region, PosSet &trunks, RandomSource &rng,
                  Pos pos,
                  std::optional<std::string_view> axisOverride = std::nullopt) {
  if (!trunkValidTreePos(cfg, region, pos))
    return false;
  BlockState logState = cfg.trunkProvider.getState(rng, pos);
  if (axisOverride && logState.hasProperty("axis")) {
    logState = logState.with("axis", *axisOverride);
  }
  if (regionSetBlockState(region, pos.x, pos.y, pos.z, logState, 19))
    trunks.add(pos);
  return true;
}

void placeTreeLogIfFree(const ConfiguredFeature::TreeConfig &cfg,
                        WorldGenRegion &region, PosSet &trunks,
                        RandomSource &rng, Pos pos) {
  if (isTrunkFree(cfg, region, pos))
    placeTreeLog(cfg, region, trunks, rng, pos);
}

struct FoliageAttachment {
  Pos pos;
  int radiusOffset = 0;
  bool doubleTrunk = false;
};

int featureSizeAtHeight(const ConfiguredFeature::TreeConfig &cfg,
                        int treeHeight, int yo) noexcept {
  if (cfg.isThreeLayersSize) {
    if (yo < cfg.minSizeLimit)
      return cfg.lowerSize;
    return yo >= treeHeight - cfg.upperSizeLimit ? cfg.upperSize
                                                 : cfg.middleSize;
  }
  return yo < cfg.minSizeLimit ? cfg.lowerSize : cfg.upperSize;
}

int getMaxFreeTreeHeight(const ConfiguredFeature::TreeConfig &cfg,
                         const WorldGenRegion &region, int maxTreeHeight,
                         Pos treePos) {
  for (int y = 0; y <= maxTreeHeight + 1; ++y) {
    const int r = featureSizeAtHeight(cfg, maxTreeHeight, y);
    for (int x = -r; x <= r; ++x) {
      for (int z = -r; z <= r; ++z) {
        const Pos cp{treePos.x + x, treePos.y + y, treePos.z + z};
        if (!isTrunkFree(cfg, region, cp) ||
            (!cfg.ignoreVines && isVineAt(region, cp))) {
          return y - 2;
        }
      }
    }
  }
  return maxTreeHeight;
}

// ============================================================================
// RootPlacers (`MangroveRootPlacer`)
// ============================================================================

bool canPlaceMangroveRoot(const ConfiguredFeature::RootPlacerConfig &rp,
                          const WorldGenRegion &region, Pos pos) {
  if (validTreePos(region, pos))
    return true;
  const BlockState s = regionGetBlockState(region, pos.x, pos.y, pos.z);
  return matchesHolderSet(s, rp.canGrowThroughTag, rp.canGrowThroughIds);
}

BlockState potentiallyWaterlogged(const WorldGenRegion &region, Pos pos,
                                  BlockState state) {
  if (state.hasProperty("waterlogged")) {
    const FluidState fs = regionGetFluidState(region, pos.x, pos.y, pos.z);
    state = state.with("waterlogged",
                       fs.isSourceOfType(Fluids::water()) ? "true" : "false");
  }
  return state;
}

std::vector<Pos> potentialMangroveRootPositions(
    const ConfiguredFeature::RootPlacerConfig &rp, Pos pos, Direction prevDir,
    RandomSource &rng, Pos rootOrigin) {
  const Pos below = pos.relative(Direction::Down);
  const Pos nextTo = pos.relative(prevDir);
  const int width = manhattanDist(pos, rootOrigin);
  if (width > rp.maxRootWidth - 3 && width <= rp.maxRootWidth) {
    if (rng.nextFloat() < rp.randomSkewChance)
      return {below, nextTo.relative(Direction::Down)};
    return {below};
  }
  if (width > rp.maxRootWidth)
    return {below};
  if (rng.nextFloat() < rp.randomSkewChance)
    return {below};
  return rng.nextBoolean() ? std::vector<Pos>{nextTo}
                           : std::vector<Pos>{below};
}

bool simulateMangroveRoots(const ConfiguredFeature::RootPlacerConfig &rp,
                           const WorldGenRegion &region, RandomSource &rng,
                           Pos rootPos, Direction dir, Pos rootOrigin,
                           std::vector<Pos> &rootPositions, int layer) {
  if (layer == rp.maxRootLength ||
      static_cast<int>(rootPositions.size()) > rp.maxRootLength) {
    return false;
  }
  for (const Pos &pos :
       potentialMangroveRootPositions(rp, rootPos, dir, rng, rootOrigin)) {
    if (canPlaceMangroveRoot(rp, region, pos)) {
      rootPositions.push_back(pos);
      if (!simulateMangroveRoots(rp, region, rng, pos, dir, rootOrigin,
                                 rootPositions, layer + 1)) {
        return false;
      }
    }
  }
  return true;
}

bool placeMangroveRoots(const ConfiguredFeature::RootPlacerConfig &rp,
                        WorldGenRegion &region, PosSet &roots,
                        RandomSource &rng, Pos origin, Pos trunkOrigin) {
  std::vector<Pos> rootPositions;
  Pos columnPos = origin;
  while (columnPos.y < trunkOrigin.y) {
    if (!canPlaceMangroveRoot(rp, region, columnPos))
      return false;
    columnPos = columnPos.relative(Direction::Up);
  }
  rootPositions.push_back(trunkOrigin.relative(Direction::Down));
  for (Direction dir :
       {Direction::North, Direction::South, Direction::West, Direction::East}) {
    const Pos pos = trunkOrigin.relative(dir);
    std::vector<Pos> dirPositions;
    if (!simulateMangroveRoots(rp, region, rng, pos, dir, trunkOrigin,
                               dirPositions, 0)) {
      return false;
    }
    rootPositions.insert(rootPositions.end(), dirPositions.begin(),
                         dirPositions.end());
    rootPositions.push_back(pos);
  }

  for (const Pos &rootPos : rootPositions) {
    const BlockState cur =
        regionGetBlockState(region, rootPos.x, rootPos.y, rootPos.z);
    if (matchesHolderSet(cur, rp.muddyRootsInTag, rp.muddyRootsInIds)) {
      BlockState mr = rp.muddyRootsProvider.getState(rng, rootPos);
      mr = potentiallyWaterlogged(region, rootPos, mr);
      if (regionSetBlockState(region, rootPos.x, rootPos.y, rootPos.z, mr, 19))
        roots.add(rootPos);
    } else if (canPlaceMangroveRoot(rp, region, rootPos)) {
      BlockState rState = rp.rootProvider.getState(rng, rootPos);
      rState = potentiallyWaterlogged(region, rootPos, rState);
      if (regionSetBlockState(region, rootPos.x, rootPos.y, rootPos.z, rState,
                              19)) {
        roots.add(rootPos);
      }
      if (rp.aboveRootProvider.has_value()) {
        const Pos above = rootPos.relative(Direction::Up);
        if (rng.nextFloat() < rp.aboveRootPlacementChance &&
            regionGetBlockState(region, above.x, above.y, above.z).isAir()) {
          BlockState ab = rp.aboveRootProvider->getState(rng, above);
          ab = potentiallyWaterlogged(region, above, ab);
          if (regionSetBlockState(region, above.x, above.y, above.z, ab, 19))
            roots.add(above);
        }
      }
    }
  }
  return true;
}

// ============================================================================
// TrunkPlacers (all 9 in 26.1)
// ============================================================================

float fancyTreeShape(int height, int y) noexcept {
  if (static_cast<float>(y) < static_cast<float>(height) * 0.3f)
    return -1.0f;
  const float radius = static_cast<float>(height) / 2.0f;
  const float adjacent = radius - static_cast<float>(y);
  float distance = std::sqrt(std::max(0.0f, radius * radius - adjacent * adjacent));
  if (adjacent == 0.0f)
    distance = radius;
  else if (std::abs(adjacent) >= radius)
    return 0.0f;
  return distance * 0.5f;
}

std::string_view fancyLogAxis(Pos startPos, Pos blockPos) noexcept {
  const int xdiff = std::abs(blockPos.x - startPos.x);
  const int zdiff = std::abs(blockPos.z - startPos.z);
  const int maxdiff = std::max(xdiff, zdiff);
  if (maxdiff > 0)
    return xdiff == maxdiff ? "x" : "z";
  return "y";
}

bool fancyMakeLimb(const ConfiguredFeature::TreeConfig &cfg,
                   WorldGenRegion &region, PosSet &trunks, RandomSource &rng,
                   Pos startPos, Pos endPos, bool doPlace) {
  if (!doPlace && startPos.x == endPos.x && startPos.y == endPos.y &&
      startPos.z == endPos.z) {
    return true;
  }
  const int dx = endPos.x - startPos.x;
  const int dy = endPos.y - startPos.y;
  const int dz = endPos.z - startPos.z;
  const int steps = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
  if (steps == 0) {
    if (doPlace)
      placeTreeLog(cfg, region, trunks, rng, startPos, fancyLogAxis(startPos, startPos));
    else if (!isTrunkFree(cfg, region, startPos))
      return false;
    return true;
  }
  const float sx = static_cast<float>(dx) / static_cast<float>(steps);
  const float sy = static_cast<float>(dy) / static_cast<float>(steps);
  const float sz = static_cast<float>(dz) / static_cast<float>(steps);
  for (int i = 0; i <= steps; ++i) {
    const Pos bp{startPos.x + floorFloat(0.5f + static_cast<float>(i) * sx),
                 startPos.y + floorFloat(0.5f + static_cast<float>(i) * sy),
                 startPos.z + floorFloat(0.5f + static_cast<float>(i) * sz)};
    if (doPlace) {
      placeTreeLog(cfg, region, trunks, rng, bp, fancyLogAxis(startPos, bp));
    } else if (!isTrunkFree(cfg, region, bp)) {
      return false;
    }
  }
  return true;
}

std::vector<FoliageAttachment>
placeFancyTrunk(const ConfiguredFeature::TreeConfig &cfg, WorldGenRegion &region,
                PosSet &trunks, RandomSource &rng, int treeHeight, Pos origin) {
  const int height = treeHeight + 2;
  const int trunkHeight = floorDouble(static_cast<double>(height) * 0.618);
  placeBelowTrunkBlock(cfg, region, trunks, rng,
                       origin.relative(Direction::Down));
  const int clustersPerY =
      std::min(1, floorDouble(1.382 + std::pow(static_cast<double>(height) / 13.0, 2.0)));
  const int trunkTop = origin.y + trunkHeight;

  struct FoliageCoord {
    FoliageAttachment attachment;
    int branchBase;
  };
  std::vector<FoliageCoord> coords;
  coords.push_back(
      {FoliageAttachment{Pos{origin.x, origin.y + height - 5, origin.z}, 0, false},
       trunkTop});

  for (int relY = height - 5; relY >= 0; --relY) {
    const float shape = fancyTreeShape(height, relY);
    if (shape < 0.0f)
      continue;
    for (int i = 0; i < clustersPerY; ++i) {
      const double radius =
          1.0 * static_cast<double>(shape) * (rng.nextFloat() + 0.328);
      const double angle =
          static_cast<double>(rng.nextFloat()) * 2.0 * kPi;
      const double x = radius * std::sin(angle) + 0.5;
      const double z = radius * std::cos(angle) + 0.5;
      const Pos checkStart{origin.x + floorDouble(x), origin.y + relY - 1,
                           origin.z + floorDouble(z)};
      const Pos checkEnd{checkStart.x, checkStart.y + 5, checkStart.z};
      if (fancyMakeLimb(cfg, region, trunks, rng, checkStart, checkEnd, false)) {
        const int dx = origin.x - checkStart.x;
        const int dz = origin.z - checkStart.z;
        const double branchH =
            static_cast<double>(checkStart.y) -
            std::sqrt(static_cast<double>(dx * dx + dz * dz)) * 0.381;
        const int branchTop = branchH > static_cast<double>(trunkTop)
                                  ? trunkTop
                                  : static_cast<int>(branchH);
        const Pos branchBase{origin.x, branchTop, origin.z};
        if (fancyMakeLimb(cfg, region, trunks, rng, branchBase, checkStart,
                          false)) {
          coords.push_back(
              {FoliageAttachment{checkStart, 0, false}, branchBase.y});
        }
      }
    }
  }

  fancyMakeLimb(cfg, region, trunks, rng, origin,
                Pos{origin.x, origin.y + trunkHeight, origin.z}, true);
  for (const auto &c : coords) {
    const Pos base{origin.x, c.branchBase, origin.z};
    if ((base.x != c.attachment.pos.x || base.y != c.attachment.pos.y ||
         base.z != c.attachment.pos.z) &&
        static_cast<double>(c.branchBase - origin.y) >=
            static_cast<double>(height) * 0.2) {
      fancyMakeLimb(cfg, region, trunks, rng, base, c.attachment.pos, true);
    }
  }
  std::vector<FoliageAttachment> attachments;
  for (const auto &c : coords) {
    if (static_cast<double>(c.branchBase - origin.y) >=
        static_cast<double>(height) * 0.2) {
      attachments.push_back(c.attachment);
    }
  }
  return attachments;
}

std::vector<FoliageAttachment>
placeCherryTrunk(const ConfiguredFeature::TreeConfig &cfg,
                 WorldGenRegion &region, PosSet &trunks, RandomSource &rng,
                 int treeHeight, Pos origin) {
  placeBelowTrunkBlock(cfg, region, trunks, rng,
                       origin.relative(Direction::Down));
  auto sampleStart = [&](int minV, int maxV) {
    return minV + rng.nextInt(std::max(1, maxV - minV + 1));
  };
  const int firstBranchOff =
      std::max(0, treeHeight - 1 + sampleStart(cfg.branchStartMin, cfg.branchStartMax));
  int secondBranchOff = std::max(
      0, treeHeight - 1 +
             sampleStart(cfg.branchStartMin, std::max(cfg.branchStartMin, cfg.branchStartMax - 1)));
  if (secondBranchOff >= firstBranchOff)
    ++secondBranchOff;

  const int branchCnt = cfg.branchCount.sample(rng);
  const bool hasMiddle = (branchCnt == 3);
  const bool hasBoth = (branchCnt >= 2);
  int trunkH = firstBranchOff + 1;
  if (hasMiddle)
    trunkH = treeHeight;
  else if (hasBoth)
    trunkH = std::max(firstBranchOff, secondBranchOff) + 1;

  for (int y = 0; y < trunkH; ++y) {
    placeTreeLog(cfg, region, trunks, rng, Pos{origin.x, origin.y + y, origin.z});
  }

  std::vector<FoliageAttachment> attachments;
  if (hasMiddle) {
    attachments.push_back(
        FoliageAttachment{Pos{origin.x, origin.y + trunkH, origin.z}, 0, false});
  }

  const Direction treeDir = randomHorizontalDir(rng);
  const std::string_view sidewaysAxis = dirAxisName(treeDir);

  auto generateBranch = [&](Direction branchDir, int offsetFromOrigin,
                            bool middleContinues) {
    Pos logPos{origin.x, origin.y + offsetFromOrigin, origin.z};
    const int branchEndOff = treeHeight - 1 + cfg.branchEndOffset.sample(rng);
    const bool extendAway = middleContinues || branchEndOff < offsetFromOrigin;
    const int distToTrunk =
        cfg.branchHorizontalLength.sample(rng) + (extendAway ? 1 : 0);
    Pos branchEnd = stepPos(origin, branchDir, distToTrunk);
    branchEnd.y += branchEndOff;
    const int horizSteps = extendAway ? 2 : 1;
    for (int i = 0; i < horizSteps; ++i) {
      logPos = logPos.relative(branchDir);
      placeTreeLog(cfg, region, trunks, rng, logPos, sidewaysAxis);
    }
    const Direction vDir =
        branchEnd.y > logPos.y ? Direction::Up : Direction::Down;
    while (true) {
      const int d = manhattanDist(logPos, branchEnd);
      if (d == 0) {
        return FoliageAttachment{branchEnd.relative(Direction::Up), 0, false};
      }
      const float chanceVert =
          static_cast<float>(std::abs(branchEnd.y - logPos.y)) /
          static_cast<float>(d);
      const bool growVert = rng.nextFloat() < chanceVert;
      logPos = logPos.relative(growVert ? vDir : branchDir);
      placeTreeLog(cfg, region, trunks, rng, logPos,
                   growVert ? std::optional<std::string_view>{}
                            : std::optional<std::string_view>{sidewaysAxis});
    }
  };

  attachments.push_back(generateBranch(
      treeDir, firstBranchOff, firstBranchOff < trunkH - 1));
  if (hasBoth) {
    attachments.push_back(generateBranch(
        oppositeDir(treeDir), secondBranchOff, secondBranchOff < trunkH - 1));
  }
  return attachments;
}

std::vector<FoliageAttachment>
placeUpwardsBranchingTrunk(const ConfiguredFeature::TreeConfig &cfg,
                           WorldGenRegion &region, PosSet &trunks,
                           RandomSource &rng, int treeHeight, Pos origin) {
  std::vector<FoliageAttachment> attachments;
  for (int hPos = 0; hPos < treeHeight; ++hPos) {
    const int curY = origin.y + hPos;
    const Pos curPos{origin.x, curY, origin.z};
    if (placeTreeLog(cfg, region, trunks, rng, curPos) &&
        hPos < treeHeight - 1 &&
        rng.nextFloat() < cfg.placeBranchPerLogProbability) {
      const Direction branchDir = randomHorizontalDir(rng);
      const int branchLen = cfg.extraBranchLength.sample(rng);
      const int branchPos =
          std::max(0, branchLen - cfg.extraBranchLength.sample(rng) - 1);
      int branchSteps = cfg.extraBranchSteps.sample(rng);
      int heightAlong = curY + branchPos;
      int lx = origin.x, lz = origin.z;
      int bi = branchPos;
      const Pos step = Pos{0, 0, 0}.relative(branchDir);
      while (bi < treeHeight && branchSteps > 0) {
        if (bi >= 1) {
          const int py = curY + bi;
          lx += step.x;
          lz += step.z;
          heightAlong = py;
          const Pos lp{lx, py, lz};
          if (placeTreeLog(cfg, region, trunks, rng, lp))
            heightAlong = py + 1;
          attachments.push_back(FoliageAttachment{lp, 0, false});
        }
        ++bi;
        --branchSteps;
      }
      if (heightAlong - curY > 1) {
        const Pos fp{lx, heightAlong, lz};
        attachments.push_back(FoliageAttachment{fp, 0, false});
        attachments.push_back(FoliageAttachment{Pos{fp.x, fp.y - 2, fp.z}, 0, false});
      }
    }
    if (hPos == treeHeight - 1) {
      attachments.push_back(
          FoliageAttachment{Pos{origin.x, curY + 1, origin.z}, 0, false});
    }
  }
  return attachments;
}

std::vector<FoliageAttachment>
placeTrunk(const ConfiguredFeature::TreeConfig &cfg, WorldGenRegion &region,
           PosSet &trunks, RandomSource &rng, int treeHeight, Pos origin) {
  const std::string &tp = cfg.trunkPlacerType;
  if (tp == "minecraft:fancy_trunk_placer")
    return placeFancyTrunk(cfg, region, trunks, rng, treeHeight, origin);
  if (tp == "minecraft:cherry_trunk_placer")
    return placeCherryTrunk(cfg, region, trunks, rng, treeHeight, origin);
  if (tp == "minecraft:upwards_branching_trunk_placer")
    return placeUpwardsBranchingTrunk(cfg, region, trunks, rng, treeHeight,
                                      origin);

  if (tp == "minecraft:bending_trunk_placer") {
    const Direction dir = randomHorizontalDir(rng);
    const int logH = treeHeight - 1;
    Pos pos = origin;
    placeBelowTrunkBlock(cfg, region, trunks, rng,
                         pos.relative(Direction::Down));
    std::vector<FoliageAttachment> pts;
    for (int i = 0; i <= logH; ++i) {
      if (i + 1 >= logH + rng.nextInt(2))
        pos = pos.relative(dir);
      if (validTreePos(region, pos))
        placeTreeLog(cfg, region, trunks, rng, pos);
      if (i >= cfg.minHeightForLeaves)
        pts.push_back(FoliageAttachment{pos, 0, false});
      pos = pos.relative(Direction::Up);
    }
    const int dirLen = cfg.bendLength.sample(rng);
    for (int i = 0; i <= dirLen; ++i) {
      if (validTreePos(region, pos))
        placeTreeLog(cfg, region, trunks, rng, pos);
      pts.push_back(FoliageAttachment{pos, 0, false});
      pos = pos.relative(dir);
    }
    return pts;
  }

  if (tp == "minecraft:forking_trunk_placer") {
    placeBelowTrunkBlock(cfg, region, trunks, rng,
                         origin.relative(Direction::Down));
    std::vector<FoliageAttachment> pts;
    const Direction leanDir = randomHorizontalDir(rng);
    const int leanH = treeHeight - rng.nextInt(4) - 1;
    int leanSteps = 3 - rng.nextInt(3);
    int tx = origin.x, tz = origin.z;
    std::optional<int> ey;
    for (int yo = 0; yo < treeHeight; ++yo) {
      const int yy = origin.y + yo;
      if (yo >= leanH && leanSteps > 0) {
        const Pos d = Pos{tx, yy, tz}.relative(leanDir);
        tx = d.x;
        tz = d.z;
        --leanSteps;
      }
      if (placeTreeLog(cfg, region, trunks, rng, Pos{tx, yy, tz}))
        ey = yy + 1;
    }
    if (ey)
      pts.push_back(FoliageAttachment{Pos{tx, *ey, tz}, 1, false});

    tx = origin.x;
    tz = origin.z;
    const Direction branchDir = randomHorizontalDir(rng);
    if (branchDir != leanDir) {
      const int branchPos = leanH - rng.nextInt(2) - 1;
      int branchSteps = 1 + rng.nextInt(3);
      ey.reset();
      for (int yo = branchPos; yo < treeHeight && branchSteps > 0;
           --branchSteps, ++yo) {
        if (yo >= 1) {
          const int yyx = origin.y + yo;
          const Pos d = Pos{tx, yyx, tz}.relative(branchDir);
          tx = d.x;
          tz = d.z;
          if (placeTreeLog(cfg, region, trunks, rng, Pos{tx, yyx, tz}))
            ey = yyx + 1;
        }
      }
      if (ey)
        pts.push_back(FoliageAttachment{Pos{tx, *ey, tz}, 0, false});
    }
    return pts;
  }

  if (tp == "minecraft:dark_oak_trunk_placer") {
    std::vector<FoliageAttachment> pts;
    const Pos b = origin.relative(Direction::Down);
    placeBelowTrunkBlock(cfg, region, trunks, rng, b);
    placeBelowTrunkBlock(cfg, region, trunks, rng, Pos{b.x + 1, b.y, b.z});
    placeBelowTrunkBlock(cfg, region, trunks, rng, Pos{b.x, b.y, b.z + 1});
    placeBelowTrunkBlock(cfg, region, trunks, rng, Pos{b.x + 1, b.y, b.z + 1});
    const Direction leanDir = randomHorizontalDir(rng);
    const int leanH = treeHeight - rng.nextInt(4);
    int leanSteps = 2 - rng.nextInt(3);
    int tx = origin.x, tz = origin.z;
    const int ey = origin.y + treeHeight - 1;
    for (int dy = 0; dy < treeHeight; ++dy) {
      if (dy >= leanH && leanSteps > 0) {
        const Pos d = Pos{tx, 0, tz}.relative(leanDir);
        tx = d.x;
        tz = d.z;
        --leanSteps;
      }
      const int yy = origin.y + dy;
      const Pos bp{tx, yy, tz};
      if (isAirOrLeavesAt(region, bp)) {
        placeTreeLog(cfg, region, trunks, rng, bp);
        placeTreeLog(cfg, region, trunks, rng, Pos{bp.x + 1, bp.y, bp.z});
        placeTreeLog(cfg, region, trunks, rng, Pos{bp.x, bp.y, bp.z + 1});
        placeTreeLog(cfg, region, trunks, rng, Pos{bp.x + 1, bp.y, bp.z + 1});
      }
    }
    pts.push_back(FoliageAttachment{Pos{tx, ey, tz}, 0, true});
    for (int ox = -1; ox <= 2; ++ox) {
      for (int oz = -1; oz <= 2; ++oz) {
        if ((ox < 0 || ox > 1 || oz < 0 || oz > 1) && rng.nextInt(3) <= 0) {
          const int len = rng.nextInt(3) + 2;
          for (int by = 0; by < len; ++by) {
            placeTreeLog(cfg, region, trunks, rng,
                         Pos{origin.x + ox, ey - by - 1, origin.z + oz});
          }
          pts.push_back(
              FoliageAttachment{Pos{origin.x + ox, ey, origin.z + oz}, 0, false});
        }
      }
    }
    return pts;
  }

  if (tp == "minecraft:giant_trunk_placer" ||
      tp == "minecraft:mega_jungle_trunk_placer") {
    const Pos b = origin.relative(Direction::Down);
    placeBelowTrunkBlock(cfg, region, trunks, rng, b);
    placeBelowTrunkBlock(cfg, region, trunks, rng, Pos{b.x + 1, b.y, b.z});
    placeBelowTrunkBlock(cfg, region, trunks, rng, Pos{b.x, b.y, b.z + 1});
    placeBelowTrunkBlock(cfg, region, trunks, rng, Pos{b.x + 1, b.y, b.z + 1});
    for (int hh = 0; hh < treeHeight; ++hh) {
      placeTreeLogIfFree(cfg, region, trunks, rng,
                         Pos{origin.x, origin.y + hh, origin.z});
      if (hh < treeHeight - 1) {
        placeTreeLogIfFree(cfg, region, trunks, rng,
                           Pos{origin.x + 1, origin.y + hh, origin.z});
        placeTreeLogIfFree(cfg, region, trunks, rng,
                           Pos{origin.x + 1, origin.y + hh, origin.z + 1});
        placeTreeLogIfFree(cfg, region, trunks, rng,
                           Pos{origin.x, origin.y + hh, origin.z + 1});
      }
    }
    std::vector<FoliageAttachment> pts{
        FoliageAttachment{Pos{origin.x, origin.y + treeHeight, origin.z}, 0, true}};
    if (tp == "minecraft:mega_jungle_trunk_placer") {
      for (int bh = treeHeight - 2 - rng.nextInt(4); bh > treeHeight / 2;
           bh -= 2 + rng.nextInt(4)) {
        const float angle = rng.nextFloat() * static_cast<float>(2.0 * kPi);
        int bx = 0, bz = 0;
        for (int bIdx = 0; bIdx < 5; ++bIdx) {
          bx = static_cast<int>(1.5f + worldgen_math::minecraftCos(angle) *
                                           static_cast<float>(bIdx));
          bz = static_cast<int>(1.5f + worldgen_math::minecraftSin(angle) *
                                           static_cast<float>(bIdx));
          placeTreeLog(cfg, region, trunks, rng,
                       Pos{origin.x + bx, origin.y + bh - 3 + bIdx / 2,
                           origin.z + bz});
        }
        pts.push_back(FoliageAttachment{
            Pos{origin.x + bx, origin.y + bh, origin.z + bz}, -2, false});
      }
    }
    return pts;
  }

  // Default: `minecraft:straight_trunk_placer`
  placeBelowTrunkBlock(cfg, region, trunks, rng,
                       origin.relative(Direction::Down));
  for (int y = 0; y < treeHeight; ++y) {
    placeTreeLog(cfg, region, trunks, rng,
                 Pos{origin.x, origin.y + y, origin.z});
  }
  return {FoliageAttachment{Pos{origin.x, origin.y + treeHeight, origin.z}, 0,
                            false}};
}

// ============================================================================
// FoliagePlacers (all 11 in 26.1)
// ============================================================================

bool tryPlaceLeaf(const ConfiguredFeature::TreeConfig &cfg,
                  WorldGenRegion &region, PosSet &foliage, RandomSource &rng,
                  Pos pos) {
  const BlockState cur = regionGetBlockState(region, pos.x, pos.y, pos.z);
  const bool isPersistent =
      cur.hasProperty("persistent") && cur.getView("persistent") == "true";
  if (isPersistent || !validTreePos(region, pos))
    return false;
  BlockState leaf = cfg.foliageProvider.getState(rng, pos);
  if (leaf.hasProperty("waterlogged")) {
    const FluidState fs = regionGetFluidState(region, pos.x, pos.y, pos.z);
    leaf = leaf.with("waterlogged",
                     fs.isSourceOfType(Fluids::water()) ? "true" : "false");
  }
  if (regionSetBlockState(region, pos.x, pos.y, pos.z, leaf, 19)) {
    foliage.add(pos);
    return true;
  }
  return false;
}

template <typename SkipFn>
void placeLeavesRow(const ConfiguredFeature::TreeConfig &cfg,
                    WorldGenRegion &region, PosSet &foliage, RandomSource &rng,
                    Pos origin, int currentRadius, int y, bool doubleTrunk,
                    SkipFn &&shouldSkip) {
  const int off = doubleTrunk ? 1 : 0;
  for (int dx = -currentRadius; dx <= currentRadius + off; ++dx) {
    for (int dz = -currentRadius; dz <= currentRadius + off; ++dz) {
      if (!shouldSkip(dx, y, dz)) {
        tryPlaceLeaf(cfg, region, foliage, rng,
                     Pos{origin.x + dx, origin.y + y, origin.z + dz});
      }
    }
  }
}

int absMinCoord(int d, bool doubleTrunk) noexcept {
  return doubleTrunk ? std::min(std::abs(d), std::abs(d - 1)) : std::abs(d);
}

void placeLeavesWithHanging(const ConfiguredFeature::TreeConfig &cfg,
                            WorldGenRegion &region, PosSet &foliage,
                            RandomSource &rng, Pos origin, int currentRadius,
                            int y, bool doubleTrunk, float hangChance,
                            float hangExtChance) {
  auto skip = [&](int dx, int yy, int dz) {
    const int adx = absMinCoord(dx, doubleTrunk);
    const int adz = absMinCoord(dz, doubleTrunk);
    if (yy == -1 && (adx == currentRadius || adz == currentRadius) &&
        rng.nextFloat() < cfg.wideBottomLayerHoleChance) {
      return true;
    }
    const bool corner = (adx == currentRadius && adz == currentRadius);
    const bool wideLayer = currentRadius > 2;
    return wideLayer ? (corner || (adx + adz > currentRadius * 2 - 2 &&
                                   rng.nextFloat() < cfg.cornerHoleChance))
                     : (corner && rng.nextFloat() < cfg.cornerHoleChance);
  };
  placeLeavesRow(cfg, region, foliage, rng, origin, currentRadius, y,
                 doubleTrunk, skip);

  const int off = doubleTrunk ? 1 : 0;
  const Pos logPos{origin.x, origin.y - 1, origin.z};
  for (Direction alongEdge :
       {Direction::North, Direction::East, Direction::South, Direction::West}) {
    const Direction toEdge = clockwiseDir(alongEdge);
    const int offsetToEdge =
        (toEdge == Direction::South || toEdge == Direction::East)
            ? currentRadius + off
            : currentRadius;
    Pos cursor = stepPos(
        stepPos(Pos{origin.x, origin.y + y - 1, origin.z}, toEdge, offsetToEdge),
        alongEdge, -currentRadius);
    for (int offAlong = -currentRadius; offAlong < currentRadius + off;
         ++offAlong) {
      const bool leavesAbove = foliage.contains(cursor.relative(Direction::Up));
      auto tryExt = [&](float chance) {
        if (manhattanDist(cursor, logPos) >= 7)
          return false;
        if (rng.nextFloat() > chance)
          return false;
        return tryPlaceLeaf(cfg, region, foliage, rng, cursor);
      };
      if (leavesAbove && tryExt(hangChance)) {
        cursor = cursor.relative(Direction::Down);
        tryExt(hangExtChance);
        cursor = cursor.relative(Direction::Up);
      }
      cursor = cursor.relative(alongEdge);
    }
  }
}

void createFoliageForAttachment(const ConfiguredFeature::TreeConfig &cfg,
                                WorldGenRegion &region, PosSet &foliage,
                                RandomSource &rng,
                                const FoliageAttachment &att, int foliageHeight,
                                int leafRadius) {
  const int offset = cfg.foliageOffset.sample(rng);
  const std::string &fpType = cfg.foliagePlacerType;

  if (fpType == "minecraft:blob_foliage_placer") {
    for (int yo = offset; yo >= offset - foliageHeight; --yo) {
      const int cr = std::max(leafRadius + att.radiusOffset - 1 - yo / 2, 0);
      placeLeavesRow(cfg, region, foliage, rng, att.pos, cr, yo,
                     att.doubleTrunk, [&](int dx, int y, int dz) {
                       const int adx = absMinCoord(dx, att.doubleTrunk);
                       const int adz = absMinCoord(dz, att.doubleTrunk);
                       return adx == cr && adz == cr &&
                              (rng.nextInt(2) == 0 || y == 0);
                     });
    }
  } else if (fpType == "minecraft:bush_foliage_placer") {
    for (int yo = offset; yo >= offset - foliageHeight; --yo) {
      const int cr = leafRadius + att.radiusOffset - 1 - yo;
      placeLeavesRow(cfg, region, foliage, rng, att.pos, cr, yo,
                     att.doubleTrunk, [&](int dx, int, int dz) {
                       const int adx = absMinCoord(dx, att.doubleTrunk);
                       const int adz = absMinCoord(dz, att.doubleTrunk);
                       return adx == cr && adz == cr && rng.nextInt(2) == 0;
                     });
    }
  } else if (fpType == "minecraft:fancy_foliage_placer") {
    for (int yo = offset; yo >= offset - foliageHeight; --yo) {
      const int cr =
          leafRadius + (yo != offset && yo != offset - foliageHeight ? 1 : 0);
      placeLeavesRow(cfg, region, foliage, rng, att.pos, cr, yo,
                     att.doubleTrunk, [&](int dx, int, int dz) {
                       const float fx =
                           static_cast<float>(absMinCoord(dx, att.doubleTrunk)) +
                           0.5f;
                       const float fz =
                           static_cast<float>(absMinCoord(dz, att.doubleTrunk)) +
                           0.5f;
                       return fx * fx + fz * fz >
                              static_cast<float>(cr * cr);
                     });
    }
  } else if (fpType == "minecraft:spruce_foliage_placer") {
    int cr = rng.nextInt(2), maxR = 1, minR = 0;
    for (int yo = offset; yo >= -foliageHeight; --yo) {
      placeLeavesRow(cfg, region, foliage, rng, att.pos, cr, yo,
                     att.doubleTrunk, [&](int dx, int, int dz) {
                       const int adx = absMinCoord(dx, att.doubleTrunk);
                       const int adz = absMinCoord(dz, att.doubleTrunk);
                       return adx == cr && adz == cr && cr > 0;
                     });
      if (cr >= maxR) {
        cr = minR;
        minR = 1;
        maxR = std::min(maxR + 1, leafRadius + att.radiusOffset);
      } else {
        ++cr;
      }
    }
  } else if (fpType == "minecraft:pine_foliage_placer") {
    int cr = 0;
    for (int yo = offset; yo >= offset - foliageHeight; --yo) {
      placeLeavesRow(cfg, region, foliage, rng, att.pos, cr, yo,
                     att.doubleTrunk, [&](int dx, int, int dz) {
                       const int adx = absMinCoord(dx, att.doubleTrunk);
                       const int adz = absMinCoord(dz, att.doubleTrunk);
                       return adx == cr && adz == cr && cr > 0;
                     });
      if (cr >= 1 && yo == offset - foliageHeight + 1)
        --cr;
      else if (cr < leafRadius + att.radiusOffset)
        ++cr;
    }
  } else if (fpType == "minecraft:mega_pine_foliage_placer") {
    int prevR = 0;
    for (int yy = att.pos.y - foliageHeight + offset; yy <= att.pos.y + offset;
         ++yy) {
      const int yo = att.pos.y - yy;
      const int smoothR =
          leafRadius + att.radiusOffset +
          floorFloat(static_cast<float>(yo) /
                     static_cast<float>(std::max(1, foliageHeight)) * 3.5f);
      const int jaggedR =
          (yo > 0 && smoothR == prevR && (yy & 1) == 0) ? smoothR + 1 : smoothR;
      placeLeavesRow(
          cfg, region, foliage, rng, Pos{att.pos.x, yy, att.pos.z}, jaggedR, 0,
          att.doubleTrunk, [&](int dx, int, int dz) {
            const int adx = absMinCoord(dx, att.doubleTrunk);
            const int adz = absMinCoord(dz, att.doubleTrunk);
            return adx + adz >= 7 || adx * adx + adz * adz > jaggedR * jaggedR;
          });
      prevR = smoothR;
    }
  } else if (fpType == "minecraft:jungle_foliage_placer" ||
             fpType == "minecraft:mega_jungle_foliage_placer") {
    const int leafH = att.doubleTrunk ? foliageHeight : 1 + rng.nextInt(2);
    for (int yo = offset; yo >= offset - leafH; --yo) {
      const int cr = leafRadius + att.radiusOffset + 1 - yo;
      placeLeavesRow(
          cfg, region, foliage, rng, att.pos, cr, yo, att.doubleTrunk,
          [&](int dx, int, int dz) {
            const int adx = absMinCoord(dx, att.doubleTrunk);
            const int adz = absMinCoord(dz, att.doubleTrunk);
            return adx + adz >= 7 || adx * adx + adz * adz > cr * cr;
          });
    }
  } else if (fpType == "minecraft:acacia_foliage_placer") {
    const Pos fp{att.pos.x, att.pos.y + offset, att.pos.z};
    auto placeAcaciaRow = [&](int r, int yOff) {
      placeLeavesRow(cfg, region, foliage, rng, fp, r, yOff, att.doubleTrunk,
                     [&](int dx, int y, int dz) {
                       const int adx = absMinCoord(dx, att.doubleTrunk);
                       const int adz = absMinCoord(dz, att.doubleTrunk);
                       return y == 0
                                  ? ((adx > 1 || adz > 1) && adx != 0 && adz != 0)
                                  : (adx == r && adz == r && r > 0);
                     });
    };
    placeAcaciaRow(leafRadius + att.radiusOffset, -1 - foliageHeight);
    placeAcaciaRow(leafRadius - 1, -foliageHeight);
    placeAcaciaRow(leafRadius + att.radiusOffset - 1, 0);
  } else if (fpType == "minecraft:dark_oak_foliage_placer") {
    const Pos fp{att.pos.x, att.pos.y + offset, att.pos.z};
    auto placeDarkRow = [&](int r, int yOff) {
      placeLeavesRow(
          cfg, region, foliage, rng, fp, r, yOff, att.doubleTrunk,
          [&](int dx, int y, int dz) {
            if (y == 0 && att.doubleTrunk &&
                ((dx == -r || dx >= r) && (dz == -r || dz >= r))) {
              return true;
            }
            const int adx = absMinCoord(dx, att.doubleTrunk);
            const int adz = absMinCoord(dz, att.doubleTrunk);
            if (y == -1 && !att.doubleTrunk)
              return adx == r && adz == r;
            if (y == 1)
              return adx + adz > r * 2 - 2;
            return false;
          });
    };
    if (att.doubleTrunk) {
      placeDarkRow(leafRadius + 2, -1);
      placeDarkRow(leafRadius + 3, 0);
      placeDarkRow(leafRadius + 2, 1);
      if (rng.nextBoolean())
        placeDarkRow(leafRadius, 2);
    } else {
      placeDarkRow(leafRadius + 2, -1);
      placeDarkRow(leafRadius + 1, 0);
    }
  } else if (fpType == "minecraft:cherry_foliage_placer") {
    const Pos fp{att.pos.x, att.pos.y + offset, att.pos.z};
    const int cr = leafRadius + att.radiusOffset - 1;
    auto cherrySkip = [&](int r, int dx, int y, int dz) {
      const int adx = absMinCoord(dx, att.doubleTrunk);
      const int adz = absMinCoord(dz, att.doubleTrunk);
      if (y == -1 && (adx == r || adz == r) &&
          rng.nextFloat() < cfg.wideBottomLayerHoleChance) {
        return true;
      }
      const bool corner = (adx == r && adz == r);
      const bool wideLayer = r > 2;
      return wideLayer ? (corner || (adx + adz > r * 2 - 2 &&
                                     rng.nextFloat() < cfg.cornerHoleChance))
                       : (corner && rng.nextFloat() < cfg.cornerHoleChance);
    };
    placeLeavesRow(cfg, region, foliage, rng, fp, std::max(0, cr - 2),
                   foliageHeight - 3, att.doubleTrunk, [&](int dx, int y, int dz) {
                     return cherrySkip(std::max(0, cr - 2), dx, y, dz);
                   });
    placeLeavesRow(cfg, region, foliage, rng, fp, std::max(0, cr - 1),
                   foliageHeight - 4, att.doubleTrunk, [&](int dx, int y, int dz) {
                     return cherrySkip(std::max(0, cr - 1), dx, y, dz);
                   });
    for (int y = foliageHeight - 5; y >= 0; --y) {
      placeLeavesRow(cfg, region, foliage, rng, fp, cr, y, att.doubleTrunk,
                     [&](int dx, int yIn, int dz) {
                       return cherrySkip(cr, dx, yIn, dz);
                     });
    }
    placeLeavesWithHanging(cfg, region, foliage, rng, fp, cr, -1,
                           att.doubleTrunk, cfg.hangingLeavesChance,
                           cfg.hangingLeavesExtensionChance);
    placeLeavesWithHanging(cfg, region, foliage, rng, fp, std::max(0, cr - 1),
                           -2, att.doubleTrunk, cfg.hangingLeavesChance,
                           cfg.hangingLeavesExtensionChance);
  } else if (fpType == "minecraft:random_spread_foliage_placer") {
    const int rad = std::max(1, leafRadius);
    const int fh = std::max(1, foliageHeight);
    for (int i = 0; i < cfg.leafPlacementAttempts; ++i) {
      const int rx = rng.nextInt(rad) - rng.nextInt(rad);
      const int ry = rng.nextInt(fh) - rng.nextInt(fh);
      const int rz = rng.nextInt(rad) - rng.nextInt(rad);
      tryPlaceLeaf(cfg, region, foliage, rng,
                   Pos{att.pos.x + rx, att.pos.y + ry, att.pos.z + rz});
    }
  }
}

// ============================================================================
// TreeDecorators (all 10 in 26.1)
// ============================================================================

std::vector<Pos> sortedByY(const PosSet &s) {
  std::vector<Pos> out = s.order;
  std::stable_sort(out.begin(), out.end(),
                   [](Pos a, Pos b) { return a.y < b.y; });
  return out;
}

std::vector<Pos> lowestTrunkOrRoot(const std::vector<Pos> &logs,
                                   const std::vector<Pos> &roots) {
  std::vector<Pos> out;
  if (roots.empty()) {
    out = logs;
  } else if (!logs.empty() && roots.front().y == logs.front().y) {
    out = logs;
    out.insert(out.end(), roots.begin(), roots.end());
  } else {
    out = roots;
  }
  return out;
}

template <typename T> void javaUtilShuffle(std::vector<T> &vec, RandomSource &rng) {
  for (int i = static_cast<int>(vec.size()); i > 1; --i) {
    std::swap(vec[static_cast<std::size_t>(i - 1)],
              vec[static_cast<std::size_t>(rng.nextInt(i))]);
  }
}

void runTreeDecorators(
    const std::vector<ConfiguredFeature::TreeDecoratorConfig> &decorators,
    WorldGenRegion &region, const NoiseBasedChunkGenerator *generator,
    std::int64_t worldSeed, RandomSource &rng, PosSet &trunks, PosSet &foliage,
    PosSet &roots, PosSet &decorations) {
  if (decorators.empty())
    return;
  const std::vector<Pos> logsList = sortedByY(trunks);
  const std::vector<Pos> leavesList = sortedByY(foliage);
  const std::vector<Pos> rootsList = sortedByY(roots);

  auto setDec = [&](Pos p, const BlockState &s) {
    if (regionSetBlockState(region, p.x, p.y, p.z, s, 19))
      decorations.add(p);
  };
  auto isAirAt = [&](Pos p) {
    return regionGetBlockState(region, p.x, p.y, p.z).isAir();
  };
  auto placeVine = [&](Pos p, std::string_view faceProp) {
    setDec(p, Blocks::VINE.defaultState().with(faceProp, "true"));
  };
  auto addHangingVine = [&](Pos p, std::string_view faceProp) {
    placeVine(p, faceProp);
    Pos cur = p.relative(Direction::Down);
    for (int rem = 4; rem > 0 && isAirAt(cur); --rem) {
      placeVine(cur, faceProp);
      cur = cur.relative(Direction::Down);
    }
  };
  auto addMossHanger = [&](Pos p) {
    while (isAirAt(p.relative(Direction::Down)) && !(rng.nextFloat() < 0.5f)) {
      setDec(p, Blocks::PALE_HANGING_MOSS.defaultState().with("tip", "false"));
      p = p.relative(Direction::Down);
    }
    setDec(p, Blocks::PALE_HANGING_MOSS.defaultState().with("tip", "true"));
  };

  for (const auto &dec : decorators) {
    using K = ConfiguredFeature::TreeDecoratorConfig::Kind;
    switch (dec.kind) {
    case K::Beehive: {
      if (logsList.empty() || rng.nextFloat() >= dec.probability)
        break;
      const int hiveY =
          !leavesList.empty()
              ? std::max(leavesList.front().y - 1, logsList.front().y + 1)
              : std::min(logsList.front().y + 1 + rng.nextInt(3),
                         logsList.back().y);
      static constexpr std::array<Direction, 3> kSpawnDirs{
          Direction::South, Direction::West, Direction::East};
      std::vector<Pos> candidates;
      for (const Pos &lp : logsList) {
        if (lp.y == hiveY) {
          for (Direction sd : kSpawnDirs)
            candidates.push_back(lp.relative(sd));
        }
      }
      if (candidates.empty())
        break;
      javaUtilShuffle(candidates, rng);
      for (const Pos &cand : candidates) {
        if (isAirAt(cand) && isAirAt(cand.relative(Direction::South))) {
          setDec(cand,
                 Blocks::BEE_NEST.defaultState().with("facing", "south"));
          // TODO(G-3 Feature): `ProtoChunk` 尚未存储 `BeehiveBlockEntity`
          // 蜂群 occupant NBT，此处消耗 `random.nextInt` 与原版保持一致后跳过 BE。
          const int numBees = 2 + rng.nextInt(2);
          for (int c = 0; c < numBees; ++c)
            (void)rng.nextInt(599);
          break;
        }
      }
      break;
    }
    case K::PlaceOnGround: {
      const std::vector<Pos> lowest = lowestTrunkOrRoot(logsList, rootsList);
      if (lowest.empty())
        break;
      const int minY = lowest.front().y;
      int minX = lowest.front().x, maxX = minX;
      int minZ = lowest.front().z, maxZ = minZ;
      for (const Pos &lp : lowest) {
        if (lp.y == minY) {
          minX = std::min(minX, lp.x);
          maxX = std::max(maxX, lp.x);
          minZ = std::min(minZ, lp.z);
          maxZ = std::max(maxZ, lp.z);
        }
      }
      const int bbMinX = minX - dec.radius, bbMaxX = maxX + dec.radius;
      const int bbMinY = minY - dec.height, bbMaxY = minY + dec.height;
      const int bbMinZ = minZ - dec.radius, bbMaxZ = maxZ + dec.radius;
      for (int i = 0; i < dec.tries; ++i) {
        const int px = worldgen_math::randomBetweenInclusive(rng, bbMinX, bbMaxX);
        const int py = worldgen_math::randomBetweenInclusive(rng, bbMinY, bbMaxY);
        const int pz = worldgen_math::randomBetweenInclusive(rng, bbMinZ, bbMaxZ);
        const Pos above{px, py + 1, pz};
        const BlockState ab = regionGetBlockState(region, above.x, above.y, above.z);
        const BlockState gr = regionGetBlockState(region, px, py, pz);
        const int mbNoLeavesY = regionGetHeight(
            region, HeightmapType::MotionBlockingNoLeaves, px, pz);
        if ((ab.isAir() || ab.is(Blocks::VINE)) && gr.block().metadata().conductor &&
            mbNoLeavesY <= above.y) {
          setDec(above, dec.blockProvider.getState(rng, above));
        }
      }
      break;
    }
    case K::LeaveVine: {
      for (const Pos &pos : leavesList) {
        if (rng.nextFloat() < dec.probability) {
          const Pos west = pos.relative(Direction::West);
          if (isAirAt(west))
            addHangingVine(west, "east");
        }
        if (rng.nextFloat() < dec.probability) {
          const Pos east = pos.relative(Direction::East);
          if (isAirAt(east))
            addHangingVine(east, "west");
        }
        if (rng.nextFloat() < dec.probability) {
          const Pos north = pos.relative(Direction::North);
          if (isAirAt(north))
            addHangingVine(north, "south");
        }
        if (rng.nextFloat() < dec.probability) {
          const Pos south = pos.relative(Direction::South);
          if (isAirAt(south))
            addHangingVine(south, "north");
        }
      }
      break;
    }
    case K::TrunkVine: {
      for (const Pos &pos : logsList) {
        if (rng.nextInt(3) > 0) {
          const Pos west = pos.relative(Direction::West);
          if (isAirAt(west))
            placeVine(west, "east");
        }
        if (rng.nextInt(3) > 0) {
          const Pos east = pos.relative(Direction::East);
          if (isAirAt(east))
            placeVine(east, "west");
        }
        if (rng.nextInt(3) > 0) {
          const Pos north = pos.relative(Direction::North);
          if (isAirAt(north))
            placeVine(north, "south");
        }
        if (rng.nextInt(3) > 0) {
          const Pos south = pos.relative(Direction::South);
          if (isAirAt(south))
            placeVine(south, "north");
        }
      }
      break;
    }
    case K::AlterGround: {
      const std::vector<Pos> lowest = lowestTrunkOrRoot(logsList, rootsList);
      if (lowest.empty())
        break;
      const int minY = lowest.front().y;
      auto placeBlockAt = [&](Pos p) {
        for (int dy = 2; dy >= -3; --dy) {
          const Pos cur{p.x, p.y + dy, p.z};
          if (auto repl = dec.blockProvider.getOptionalState(region, rng, cur)) {
            setDec(cur, *repl);
            break;
          }
          if (!isAirAt(cur) && dy < 0)
            break;
        }
      };
      auto placeCircle = [&](Pos p) {
        for (int xx = -2; xx <= 2; ++xx) {
          for (int zz = -2; zz <= 2; ++zz) {
            if (std::abs(xx) != 2 || std::abs(zz) != 2)
              placeBlockAt(Pos{p.x + xx, p.y, p.z + zz});
          }
        }
      };
      for (const Pos &lp : lowest) {
        if (lp.y != minY)
          continue;
        placeCircle(Pos{lp.x - 1, lp.y, lp.z - 1});
        placeCircle(Pos{lp.x + 2, lp.y, lp.z - 1});
        placeCircle(Pos{lp.x - 1, lp.y, lp.z + 2});
        placeCircle(Pos{lp.x + 2, lp.y, lp.z + 2});
        for (int i = 0; i < 5; ++i) {
          const int pl = rng.nextInt(64);
          const int xx = pl % 8;
          const int zz = pl / 8;
          if (xx == 0 || xx == 7 || zz == 0 || zz == 7)
            placeCircle(Pos{lp.x - 3 + xx, lp.y, lp.z - 3 + zz});
        }
      }
      break;
    }
    case K::AttachedToLeaves: {
      if (dec.directions.empty())
        break;
      std::vector<Pos> shuffled = leavesList;
      javaUtilShuffle(shuffled, rng);
      PosSet blacklist;
      for (const Pos &leafPos : shuffled) {
        const Direction dir =
            dec.directions[rng.nextInt(static_cast<int>(dec.directions.size()))];
        const Pos placementPos = leafPos.relative(dir);
        if (!blacklist.contains(placementPos) &&
            rng.nextFloat() < dec.probability) {
          bool emptyOk = true;
          for (int i = 1; i <= dec.requiredEmptyBlocks; ++i) {
            if (!isAirAt(stepPos(leafPos, dir, i))) {
              emptyOk = false;
              break;
            }
          }
          if (emptyOk) {
            for (int bx = -dec.exclusionRadiusXZ; bx <= dec.exclusionRadiusXZ;
                 ++bx) {
              for (int by = -dec.exclusionRadiusY; by <= dec.exclusionRadiusY;
                   ++by) {
                for (int bz = -dec.exclusionRadiusXZ;
                     bz <= dec.exclusionRadiusXZ; ++bz) {
                  blacklist.add(Pos{placementPos.x + bx, placementPos.y + by,
                                    placementPos.z + bz});
                }
              }
            }
            setDec(placementPos, dec.blockProvider.getState(rng, placementPos));
          }
        }
      }
      break;
    }
    case K::AttachedToLogs: {
      if (dec.directions.empty())
        break;
      std::vector<Pos> shuffled = logsList;
      javaUtilShuffle(shuffled, rng);
      for (const Pos &lp : shuffled) {
        const Direction dir =
            dec.directions[rng.nextInt(static_cast<int>(dec.directions.size()))];
        const Pos pp = lp.relative(dir);
        if (rng.nextFloat() <= dec.probability && isAirAt(pp)) {
          setDec(pp, dec.blockProvider.getState(rng, pp));
        }
      }
      break;
    }
    case K::PaleMoss: {
      std::vector<Pos> shuffled = logsList;
      javaUtilShuffle(shuffled, rng);
      if (shuffled.empty())
        break;
      const Pos origin = *std::min_element(
          logsList.begin(), logsList.end(),
          [](Pos a, Pos b) { return a.y < b.y; });
      if (rng.nextFloat() < dec.groundProbability && generator != nullptr) {
        if (const ConfiguredFeature *cf =
                generator->registries().findConfiguredFeature(
                    "minecraft:pale_moss_patch")) {
          (void)cf->place(region, *generator, worldSeed, rng,
                          origin.relative(Direction::Up));
        }
      }
      for (const Pos &pos : logsList) {
        if (rng.nextFloat() < dec.trunkProbability) {
          const Pos down = pos.relative(Direction::Down);
          if (isAirAt(down))
            addMossHanger(down);
        }
      }
      for (const Pos &pos : leavesList) {
        if (rng.nextFloat() < dec.leavesProbability) {
          const Pos down = pos.relative(Direction::Down);
          if (isAirAt(down))
            addMossHanger(down);
        }
      }
      break;
    }
    case K::CreakingHeart: {
      if (logsList.empty() || rng.nextFloat() >= dec.probability)
        break;
      std::vector<Pos> heartPlacements = logsList;
      javaUtilShuffle(heartPlacements, rng);
      for (const Pos &pos : heartPlacements) {
        bool allLogs = true;
        for (Direction d : {Direction::Down, Direction::Up, Direction::North,
                            Direction::South, Direction::West, Direction::East}) {
          const Pos nb = pos.relative(d);
          if (!blockHasTag(regionGetBlockState(region, nb.x, nb.y, nb.z),
                           "minecraft:logs")
                   .value_or(false)) {
            allLogs = false;
            break;
          }
        }
        if (allLogs) {
          setDec(pos, Blocks::CREAKING_HEART.defaultState()
                          .with("creaking_heart_state", "dormant")
                          .with("natural", "true"));
          break;
        }
      }
      break;
    }
    case K::Cocoa: {
      if (logsList.empty() || rng.nextFloat() >= dec.probability)
        break;
      const int treeY = logsList.front().y;
      for (const Pos &pos : logsList) {
        if (pos.y - treeY > 2)
          continue;
        for (Direction dir : {Direction::North, Direction::South,
                              Direction::West, Direction::East}) {
          if (rng.nextFloat() <= 0.25f) {
            const Pos cocoaPos = pos.relative(oppositeDir(dir));
            if (isAirAt(cocoaPos)) {
              setDec(cocoaPos, Blocks::COCOA.defaultState()
                                   .with("age", std::to_string(rng.nextInt(3)))
                                   .with("facing", dirNameLower(dir)));
            }
          }
        }
      }
      break;
    }
    }
  }
}

void updateTreeLeaves(WorldGenRegion &region, int minX, int minY, int minZ,
                      int maxX, int maxY, int maxZ, const PosSet &trunks,
                      const PosSet &decorations, const PosSet &roots) {
  const int spanX = maxX - minX + 1;
  const int spanY = maxY - minY + 1;
  const int spanZ = maxZ - minZ + 1;
  std::vector<std::uint8_t> visited(
      static_cast<std::size_t>(spanX * spanY * spanZ), 0);
  auto shapeIdx = [&](int x, int y, int z) -> std::size_t {
    return static_cast<std::size_t>((x - minX) * spanY * spanZ +
                                    (y - minY) * spanZ + (z - minZ));
  };
  auto inBounds = [&](Pos p) {
    return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY &&
           p.z >= minZ && p.z <= maxZ;
  };
  for (const Pos &p : decorations.order) {
    if (inBounds(p))
      visited[shapeIdx(p.x, p.y, p.z)] = 1;
  }
  for (const Pos &p : roots.order) {
    if (inBounds(p))
      visited[shapeIdx(p.x, p.y, p.z)] = 1;
  }

  std::array<std::vector<Pos>, 7> buckets;
  std::array<phmap::flat_hash_set<Pos, PosHash>, 7> queued;
  for (const Pos &lp : trunks.order) {
    if (queued[0].insert(lp).second)
      buckets[0].push_back(lp);
  }
  std::array<std::size_t, 7> cursors{};
  int smallestDist = 0;
  while (smallestDist < 7) {
    while (cursors[static_cast<std::size_t>(smallestDist)] >=
           buckets[static_cast<std::size_t>(smallestDist)].size()) {
      ++smallestDist;
      if (smallestDist >= 7)
        return;
    }
    const Pos p =
        buckets[static_cast<std::size_t>(smallestDist)]
               [cursors[static_cast<std::size_t>(smallestDist)]++];
    if (!inBounds(p))
      continue;
    if (visited[shapeIdx(p.x, p.y, p.z)])
      continue;
    if (smallestDist != 0) {
      const BlockState st = regionGetBlockState(region, p.x, p.y, p.z);
      if (st.hasProperty("distance")) {
        regionSetBlockState(region, p.x, p.y, p.z,
                            st.with("distance", std::to_string(smallestDist)),
                            19);
      }
    }
    visited[shapeIdx(p.x, p.y, p.z)] = 1;
    for (Direction d : {Direction::Down, Direction::Up, Direction::North,
                        Direction::South, Direction::West, Direction::East}) {
      const Pos nb = p.relative(d);
      if (!inBounds(nb))
        continue;
      if (visited[shapeIdx(nb.x, nb.y, nb.z)])
        continue;
      const BlockState nbState = regionGetBlockState(region, nb.x, nb.y, nb.z);
      std::optional<int> dist;
      if (blockHasTag(nbState, "minecraft:logs").value_or(false))
        dist = 0;
      else if (nbState.hasProperty("distance")) {
        const std::string_view dv = nbState.getView("distance");
        if (!dv.empty() && dv[0] >= '0' && dv[0] <= '7')
          dist = dv[0] - '0';
      }
      if (dist.has_value()) {
        const int newDist = std::min(*dist, smallestDist + 1);
        if (newDist < 7) {
          if (queued[static_cast<std::size_t>(newDist)].insert(nb).second) {
            buckets[static_cast<std::size_t>(newDist)].push_back(nb);
          }
          smallestDist = std::min(smallestDist, newDist);
        }
      }
    }
  }
}

void placeWeepingVinesColumn(WorldGenRegion &region, RandomSource &rng, Pos start,
                             int length, int minAge, int maxAge) {
  Pos cur = start;
  for (int i = 0; i <= length; ++i) {
    if (regionGetBlockState(region, cur.x, cur.y, cur.z).isAir()) {
      const BlockState below =
          regionGetBlockState(region, cur.x, cur.y - 1, cur.z);
      if (i == length || !below.isAir()) {
        const int age = worldgen_math::randomBetweenInclusive(rng, minAge, maxAge);
        regionSetBlockState(
            region, cur.x, cur.y, cur.z,
            Blocks::WEEPING_VINES.defaultState().with("age", std::to_string(age)),
            2);
        break;
      }
      regionSetBlockState(region, cur.x, cur.y, cur.z,
                          Blocks::WEEPING_VINES_PLANT.defaultState(), 2);
    }
    cur = cur.relative(Direction::Down);
  }
}

} // namespace

bool placeTreeFeature(const ConfiguredFeature::TreeConfig &cfg,
                      WorldGenRegion &region,
                      const NoiseBasedChunkGenerator &generator,
                      std::int64_t worldSeed, RandomSource &rng, Pos origin) {
  const int treeHeight = cfg.baseHeight + rng.nextInt(cfg.heightRandA + 1) +
                         rng.nextInt(cfg.heightRandB + 1);
  int foliageHeight = cfg.foliageHeight;
  const std::string &fpType = cfg.foliagePlacerType;
  if (fpType == "minecraft:spruce_foliage_placer")
    foliageHeight = std::max(4, treeHeight - cfg.trunkHeightProvider.sample(rng));
  else if (fpType == "minecraft:pine_foliage_placer" ||
           fpType == "minecraft:random_spread_foliage_placer")
    foliageHeight = cfg.trunkHeightProvider.sample(rng);
  else if (fpType == "minecraft:cherry_foliage_placer")
    foliageHeight = cfg.cherryHeight.sample(rng);
  else if (fpType == "minecraft:mega_pine_foliage_placer")
    foliageHeight = cfg.crownHeight.sample(rng);
  else if (fpType == "minecraft:acacia_foliage_placer")
    foliageHeight = 0;
  else if (fpType == "minecraft:dark_oak_foliage_placer")
    foliageHeight = 4;

  const int trunkHeight = treeHeight - foliageHeight;
  int leafRadius = cfg.foliageRadius.sample(rng);
  if (fpType == "minecraft:pine_foliage_placer")
    leafRadius += rng.nextInt(std::max(trunkHeight + 1, 1));

  const Pos trunkOrigin =
      cfg.rootPlacer.has_value()
          ? Pos{origin.x, origin.y + cfg.rootPlacer->trunkOffsetY.sample(rng),
                origin.z}
          : origin;

  const int minY = std::min(origin.y, trunkOrigin.y);
  const int maxY = std::max(origin.y, trunkOrigin.y) + treeHeight + 1;
  if (minY < region.minY() + 1 || maxY > region.maxY())
    return false;

  const int clippedHeight =
      getMaxFreeTreeHeight(cfg, region, treeHeight, trunkOrigin);
  if (clippedHeight < treeHeight &&
      (!cfg.minClippedHeight.has_value() ||
       clippedHeight < *cfg.minClippedHeight)) {
    return false;
  }

  PosSet roots, trunks, foliage, decorations;
  if (cfg.rootPlacer.has_value()) {
    if (!placeMangroveRoots(*cfg.rootPlacer, region, roots, rng, origin,
                            trunkOrigin)) {
      return false;
    }
  }

  const auto attachments =
      placeTrunk(cfg, region, trunks, rng, clippedHeight, trunkOrigin);
  for (const auto &att : attachments) {
    createFoliageForAttachment(cfg, region, foliage, rng, att, foliageHeight,
                               leafRadius);
  }

  if (trunks.empty() && foliage.empty())
    return false;

  runTreeDecorators(cfg.decorators, region, &generator, worldSeed, rng, trunks,
                    foliage, roots, decorations);

  int bMinX = std::numeric_limits<int>::max(), bMinY = bMinX, bMinZ = bMinX;
  int bMaxX = std::numeric_limits<int>::min(), bMaxY = bMaxX, bMaxZ = bMaxX;
  auto enc = [&](const PosSet &s) {
    for (const Pos &p : s.order) {
      bMinX = std::min(bMinX, p.x);
      bMinY = std::min(bMinY, p.y);
      bMinZ = std::min(bMinZ, p.z);
      bMaxX = std::max(bMaxX, p.x);
      bMaxY = std::max(bMaxY, p.y);
      bMaxZ = std::max(bMaxZ, p.z);
    }
  };
  enc(roots);
  enc(trunks);
  enc(foliage);
  enc(decorations);
  if (bMinX <= bMaxX) {
    updateTreeLeaves(region, bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ, trunks,
                     decorations, roots);
    return true;
  }
  return false;
}

bool placeFallenTreeFeature(const ConfiguredFeature::FallenTreeConfig &cfg,
                            WorldGenRegion &region, RandomSource &rng,
                            Pos origin) {
  auto placeLogBlock = [&](Pos pos,
                           std::optional<std::string_view> axis) -> Pos {
    BlockState st = cfg.trunkProvider.getState(rng, pos);
    if (axis && st.hasProperty("axis"))
      st = st.with("axis", *axis);
    regionSetBlockState(region, pos.x, pos.y, pos.z, st, 3);
    return pos;
  };
  auto isOverSolid = [&](Pos p) {
    return blockMatchesFaceSturdyUp(
        regionGetBlockState(region, p.x, p.y - 1, p.z));
  };

  PosSet stumpSet, dummyEmpty;
  stumpSet.add(placeLogBlock(origin, std::nullopt));
  if (!cfg.stumpDecorators.empty()) {
    runTreeDecorators(cfg.stumpDecorators, region, nullptr, 0, rng, stumpSet,
                      dummyEmpty, dummyEmpty, dummyEmpty);
  }

  const Direction dir = randomHorizontalDir(rng);
  const int logLength = cfg.logLength.sample(rng) - 2;
  Pos logStart = stepPos(origin, dir, 2 + rng.nextInt(2));
  logStart.y += 1;
  for (int i = 0; i < 6; ++i) {
    if (validTreePos(region, logStart) && isOverSolid(logStart))
      break;
    logStart.y -= 1;
  }

  Pos cursor = logStart;
  int gap = 0;
  for (int i = 0; i < logLength; ++i) {
    if (!validTreePos(region, cursor))
      return true;
    if (!isOverSolid(cursor)) {
      if (++gap > 2)
        return true;
    } else {
      gap = 0;
    }
    cursor = cursor.relative(dir);
  }

  PosSet fallenLogs;
  cursor = logStart;
  const std::string_view axis = dirAxisName(dir);
  for (int i = 0; i < logLength; ++i) {
    fallenLogs.add(placeLogBlock(cursor, axis));
    cursor = cursor.relative(dir);
  }
  if (!cfg.logDecorators.empty()) {
    runTreeDecorators(cfg.logDecorators, region, nullptr, 0, rng, fallenLogs,
                      dummyEmpty, dummyEmpty, dummyEmpty);
  }
  return true;
}

bool placeHugeMushroomFeature(bool isRed,
                              const ConfiguredFeature::HugeMushroomConfig &cfg,
                              WorldGenRegion &region, RandomSource &rng,
                              Pos origin) {
  int treeH = rng.nextInt(3) + 4;
  if (rng.nextInt(12) == 0)
    treeH *= 2;
  if (origin.y < region.minY() + 1 || origin.y + treeH + 1 > region.maxY())
    return false;
  if (!cfg.canPlaceOn.test(region, origin.relative(Direction::Down)))
    return false;

  auto radiusForHeight = [&](int yo) -> int {
    if (isRed) {
      return (yo < treeH && yo >= treeH - 3) || yo == treeH ? cfg.foliageRadius
                                                            : 0;
    }
    return yo <= 3 ? 0 : cfg.foliageRadius;
  };
  for (int dy = 0; dy <= treeH; ++dy) {
    const int r = radiusForHeight(dy);
    for (int dx = -r; dx <= r; ++dx) {
      for (int dz = -r; dz <= r; ++dz) {
        const BlockState s =
            regionGetBlockState(region, origin.x + dx, origin.y + dy,
                                origin.z + dz);
        if (!s.isAir() && !blockHasTag(s, "minecraft:leaves").value_or(false))
          return false;
      }
    }
  }

  auto placeMush = [&](Pos pos, const BlockState &newState) {
    const BlockState cur = regionGetBlockState(region, pos.x, pos.y, pos.z);
    if (cur.isAir() ||
        blockHasTag(cur, "minecraft:replaceable_by_mushrooms").value_or(false)) {
      regionSetBlockState(region, pos.x, pos.y, pos.z, newState, 2);
    }
  };

  if (isRed) {
    for (int dy = treeH - 3; dy <= treeH; ++dy) {
      const int rad = dy < treeH ? cfg.foliageRadius : cfg.foliageRadius - 1;
      const int center = cfg.foliageRadius - 2;
      for (int dx = -rad; dx <= rad; ++dx) {
        for (int dz = -rad; dz <= rad; ++dz) {
          const bool minX = (dx == -rad), maxX = (dx == rad);
          const bool minZ = (dz == -rad), maxZ = (dz == rad);
          const bool xEdge = minX || maxX, zEdge = minZ || maxZ;
          if (dy >= treeH || xEdge != zEdge) {
            BlockState st = cfg.capProvider.getState(rng, origin);
            if (st.hasProperty("west") && st.hasProperty("east") &&
                st.hasProperty("north") && st.hasProperty("south") &&
                st.hasProperty("up")) {
              st = st.with("up", dy >= treeH - 1 ? "true" : "false")
                       .with("west", dx < -center ? "true" : "false")
                       .with("east", dx > center ? "true" : "false")
                       .with("north", dz < -center ? "true" : "false")
                       .with("south", dz > center ? "true" : "false");
            }
            placeMush(Pos{origin.x + dx, origin.y + dy, origin.z + dz}, st);
          }
        }
      }
    }
  } else {
    const int rad = cfg.foliageRadius;
    for (int dx = -rad; dx <= rad; ++dx) {
      for (int dz = -rad; dz <= rad; ++dz) {
        const bool minX = (dx == -rad), maxX = (dx == rad);
        const bool minZ = (dz == -rad), maxZ = (dz == rad);
        const bool xEdge = minX || maxX, zEdge = minZ || maxZ;
        if (!xEdge || !zEdge) {
          const bool west = minX || (zEdge && dx == 1 - rad);
          const bool east = maxX || (zEdge && dx == rad - 1);
          const bool north = minZ || (xEdge && dz == 1 - rad);
          const bool south = maxZ || (xEdge && dz == rad - 1);
          BlockState st = cfg.capProvider.getState(rng, origin);
          if (st.hasProperty("west") && st.hasProperty("east") &&
              st.hasProperty("north") && st.hasProperty("south")) {
            st = st.with("west", west ? "true" : "false")
                     .with("east", east ? "true" : "false")
                     .with("north", north ? "true" : "false")
                     .with("south", south ? "true" : "false");
          }
          placeMush(Pos{origin.x + dx, origin.y + treeH, origin.z + dz}, st);
        }
      }
    }
  }

  for (int dy = 0; dy < treeH; ++dy) {
    placeMush(Pos{origin.x, origin.y + dy, origin.z},
              cfg.stemProvider.getState(rng, origin));
  }
  return true;
}

bool placeHugeFungusFeature(const ConfiguredFeature::HugeFungusConfig &cfg,
                            WorldGenRegion &region,
                            const NoiseBasedChunkGenerator &generator,
                            RandomSource &rng, Pos origin) {
  const BlockState below =
      regionGetBlockState(region, origin.x, origin.y - 1, origin.z);
  if (!below.sameBlock(cfg.validBaseState))
    return false;

  int totalH = worldgen_math::randomBetweenInclusive(rng, 4, 13);
  if (rng.nextInt(12) == 0)
    totalH *= 2;
  if (!cfg.planted) {
    if (origin.y + totalH + 1 >= generator.height())
      return false;
  }

  const bool isHuge = !cfg.planted && rng.nextFloat() < 0.06f;
  regionSetBlockState(region, origin.x, origin.y, origin.z,
                      Blocks::AIR.defaultState(), 2);

  auto isRepl = [&](Pos p, bool checkPlants) {
    const BlockState s = regionGetBlockState(region, p.x, p.y, p.z);
    if (s.isAir() || s.block().metadata().replaceable)
      return true;
    return checkPlants ? cfg.replaceableBlocks.test(region, p) : false;
  };

  // Stem
  const int stemRad = isHuge ? 1 : 0;
  for (int dx = -stemRad; dx <= stemRad; ++dx) {
    for (int dz = -stemRad; dz <= stemRad; ++dz) {
      const bool corner = isHuge && std::abs(dx) == stemRad &&
                          std::abs(dz) == stemRad;
      for (int dy = 0; dy < totalH; ++dy) {
        const Pos bp{origin.x + dx, origin.y + dy, origin.z + dz};
        if (isRepl(bp, true)) {
          if (cfg.planted || !corner || rng.nextFloat() < 0.1f)
            regionSetBlockState(region, bp.x, bp.y, bp.z, cfg.stemState, 2);
        }
      }
    }
  }

  // Hat
  const bool placeVines = cfg.hatState.is(Blocks::NETHER_WART_BLOCK);
  const int hatH = std::min(rng.nextInt(1 + totalH / 3) + 5, totalH);
  const int hatStart = totalH - hatH;
  for (int dy = hatStart; dy <= totalH; ++dy) {
    int radius = dy < totalH - rng.nextInt(3) ? 2 : 1;
    if (hatH > 8 && dy < hatStart + 4)
      radius = 3;
    if (isHuge)
      ++radius;
    for (int dx = -radius; dx <= radius; ++dx) {
      for (int dz = -radius; dz <= radius; ++dz) {
        const bool edgeX = (dx == -radius || dx == radius);
        const bool edgeZ = (dz == -radius || dz == radius);
        const bool inside = !edgeX && !edgeZ && dy != totalH;
        const bool corner = edgeX && edgeZ;
        const bool isBottom = dy < hatStart + 3;
        const Pos bp{origin.x + dx, origin.y + dy, origin.z + dz};
        if (!isRepl(bp, false))
          continue;
        auto tryVines = [&](Pos p) {
          const Pos belowPos = p.relative(Direction::Down);
          if (regionGetBlockState(region, belowPos.x, belowPos.y, belowPos.z)
                  .isAir()) {
            int gh = worldgen_math::randomBetweenInclusive(rng, 1, 5);
            if (rng.nextInt(7) == 0)
              gh *= 2;
            placeWeepingVinesColumn(region, rng, belowPos, gh, 23, 25);
          }
        };
        auto placeHatBlock = [&](float decorP, float hatP, float vineP) {
          if (rng.nextFloat() < decorP) {
            regionSetBlockState(region, bp.x, bp.y, bp.z, cfg.decorState, 2);
          } else if (rng.nextFloat() < hatP) {
            regionSetBlockState(region, bp.x, bp.y, bp.z, cfg.hatState, 2);
            if (rng.nextFloat() < vineP)
              tryVines(bp);
          }
        };
        if (isBottom) {
          if (!inside) {
            if (regionGetBlockState(region, bp.x, bp.y - 1, bp.z)
                    .sameBlock(cfg.hatState)) {
              regionSetBlockState(region, bp.x, bp.y, bp.z, cfg.hatState, 2);
            } else if (rng.nextFloat() < 0.15f) {
              regionSetBlockState(region, bp.x, bp.y, bp.z, cfg.hatState, 2);
              if (placeVines && rng.nextInt(11) == 0)
                tryVines(bp);
            }
          }
        } else if (inside) {
          placeHatBlock(0.1f, 0.2f, placeVines ? 0.1f : 0.0f);
        } else if (corner) {
          placeHatBlock(0.01f, 0.7f, placeVines ? 0.083f : 0.0f);
        } else {
          placeHatBlock(0.0005f, 0.98f, placeVines ? 0.07f : 0.0f);
        }
      }
    }
  }
  return true;
}

} // namespace redstone::worldgen
