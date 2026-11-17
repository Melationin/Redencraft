#include "redstone/worldgen/feature.hpp"

#include "redstone/block_tags.hpp"
#include "redstone/shapes.hpp"
#include "redstone/worldgen/chunk_generator.hpp"
#include "redstone/worldgen/jigsaw.hpp"
#include "redstone/worldgen/noise_chunk_generator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redstone::worldgen {
namespace {

using feature_detail::regionGetBlockState;
using feature_detail::regionGetFluidState;
using feature_detail::regionGetHeight;
using feature_detail::regionMarkPosForPostprocessing;
using feature_detail::regionScheduleFluidTick;
using feature_detail::regionSetBlockState;

constexpr double kPi = 3.14159265358979323846;
constexpr float kPiF = 3.1415927f;

inline BlockState getBlock(const WorldGenRegion &region, Pos p) noexcept {
  return regionGetBlockState(region, p.x, p.y, p.z);
}

inline FluidState getFluid(const WorldGenRegion &region, Pos p) noexcept {
  return regionGetFluidState(region, p.x, p.y, p.z);
}

inline bool setBlock(WorldGenRegion &region, Pos p, const BlockState &state,
                     int flags = 2) {
  return regionSetBlockState(region, p.x, p.y, p.z, state, flags);
}

inline bool isOutsideBuildHeight(const WorldGenRegion &region, int y) noexcept {
  return y < region.minY() || y >= region.maxY();
}

inline bool isCollisionFullBlock(const BlockState &state) {
  const auto &cs = collisionShape(state);
  return cs.size() == 1 && cs[0].x0 <= 0.0 && cs[0].y0 <= 0.0 &&
         cs[0].z0 <= 0.0 && cs[0].x1 >= 16.0 && cs[0].y1 >= 16.0 &&
         cs[0].z1 >= 16.0;
}

inline bool isBlockSolid(const BlockState &state) {
  return !state.isAir() && !state.block().isLiquid() &&
         (state.block().metadata().conductor || !collisionShape(state).empty());
}

inline bool isSolidRenderApprox(const BlockState &state) {
  return state.block().metadata().conductor && isCollisionFullBlock(state);
}

inline bool isWaterSource(const WorldGenRegion &region, Pos p) noexcept {
  return getFluid(region, p).isSourceOfType(Fluids::water());
}

inline bool isWaterAt(const WorldGenRegion &region, Pos p) noexcept {
  return getFluid(region, p).is(Fluids::water());
}

inline bool isFullFaceSturdy(const BlockState &state, Direction dir) {
  return faceSturdy(state, dir, SupportType::Full);
}

inline std::string_view dirNameLower(Direction d) noexcept {
  switch (d) {
  case Direction::Down:
    return "down";
  case Direction::Up:
    return "up";
  case Direction::North:
    return "north";
  case Direction::South:
    return "south";
  case Direction::West:
    return "west";
  case Direction::East:
    return "east";
  }
  return "north";
}

enum class Axis : std::uint8_t { X, Y, Z };

inline Axis dirAxis(Direction d) noexcept {
  switch (d) {
  case Direction::East:
  case Direction::West:
    return Axis::X;
  case Direction::North:
  case Direction::South:
    return Axis::Z;
  case Direction::Up:
  case Direction::Down:
    return Axis::Y;
  }
  return Axis::Y;
}

inline Pos posAbove(Pos p, int n = 1) noexcept {
  return Pos{p.x, p.y + n, p.z};
}

inline Pos posBelow(Pos p, int n = 1) noexcept {
  return Pos{p.x, p.y - n, p.z};
}

// Java Direction.values() order: DOWN, UP, NORTH, SOUTH, WEST, EAST
constexpr std::array<Direction, 6> kAllDirections{
    Direction::Down,  Direction::Up,   Direction::North,
    Direction::South, Direction::West, Direction::East};

// Java Direction.Plane.HORIZONTAL order: NORTH, EAST, SOUTH, WEST
constexpr std::array<Direction, 4> kHorizDirections{
    Direction::North, Direction::East, Direction::South, Direction::West};

template <typename T> void javaShuffle(std::vector<T> &vec, RandomSource &rng) {
  for (int i = static_cast<int>(vec.size()); i > 1; --i) {
    const int swapTo = rng.nextInt(i);
    std::swap(vec[static_cast<std::size_t>(i - 1)],
              vec[static_cast<std::size_t>(swapTo)]);
  }
}

int ceilFloat(float v) noexcept {
  const int i = static_cast<int>(v);
  return v > static_cast<float>(i) ? i + 1 : i;
}

double clampedMapDouble(double value, double fromMin, double fromMax,
                        double toMin, double toMax) noexcept {
  const double span = fromMax - fromMin;
  const double t =
      span == 0.0 ? 0.0 : std::clamp((value - fromMin) / span, 0.0, 1.0);
  return toMin + t * (toMax - toMin);
}

template <typename Fn>
void forEachBetweenClosed(int minX, int minY, int minZ, int maxX, int maxY,
                          int maxZ, Fn &&fn) {
  // Match Java BlockPos.betweenClosed iteration order: z outer, y middle, x inner.
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        fn(Pos{x, y, z});
      }
    }
  }
}

template <typename Fn>
void forEachWithinManhattan(Pos origin, int reachX, int reachY, int reachZ,
                            Fn &&fn) {
  const int maxDepth = reachX + reachY + reachZ;
  for (int currentDepth = 0; currentDepth <= maxDepth; ++currentDepth) {
    const int maxX = std::min(reachX, currentDepth);
    for (int x = -maxX; x <= maxX; ++x) {
      const int maxY = std::min(reachY, currentDepth - std::abs(x));
      for (int y = -maxY; y <= maxY; ++y) {
        const int zz = currentDepth - std::abs(x) - std::abs(y);
        if (zz <= reachZ) {
          if (!fn(Pos{origin.x + x, origin.y + y, origin.z + zz}))
            return;
          if (zz != 0) {
            if (!fn(Pos{origin.x + x, origin.y + y, origin.z - zz}))
              return;
          }
        }
      }
    }
  }
}

int manhattanDist(Pos a, Pos b) noexcept {
  return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
}

int distSqrInt(Pos a, Pos b) noexcept {
  const int dx = a.x - b.x;
  const int dy = a.y - b.y;
  const int dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz;
}

// ============================================================================
// Column.java helper (for DripstoneCluster, LargeDripstone, UnderwaterMagma)
// ============================================================================

struct ScannedColumn {
  std::optional<int> floor;
  std::optional<int> ceiling;

  std::optional<int> height() const noexcept {
    if (floor && ceiling)
      return *ceiling - *floor - 1;
    return std::nullopt;
  }
};

template <typename InsidePred, typename EdgePred>
std::optional<ScannedColumn> scanColumn(const WorldGenRegion &region, Pos pos,
                                        int searchRange,
                                        InsidePred &&insidePred,
                                        EdgePred &&validEdgePred) {
  if (!insidePred(getBlock(region, pos)))
    return std::nullopt;

  auto scanDir = [&](int dy) -> std::optional<int> {
    Pos cursor = pos;
    for (int i = 1; i < searchRange && insidePred(getBlock(region, cursor));
         ++i) {
      cursor.y += dy;
    }
    return validEdgePred(getBlock(region, cursor)) ? std::optional<int>(cursor.y)
                                                   : std::nullopt;
  };

  ScannedColumn col;
  col.ceiling = scanDir(1);
  col.floor = scanDir(-1);
  return col;
}

// ============================================================================
// 1. SeagrassFeature
// ============================================================================

bool placeSeagrass(const ConfiguredFeature &cf, WorldGenRegion &region,
                   RandomSource &rng, Pos origin) {
  bool placedAny = false;
  const int x1 = rng.nextInt(8);
  const int x2 = rng.nextInt(8);
  const int z1 = rng.nextInt(8);
  const int z2 = rng.nextInt(8);
  const int gx = origin.x + (x1 - x2);
  const int gz = origin.z + (z1 - z2);
  const int y = regionGetHeight(region, HeightmapType::OceanFloor, gx, gz);
  const Pos grassPos{gx, y, gz};

  if (getBlock(region, grassPos).is(Blocks::WATER)) {
    const bool isTall = rng.nextDouble() < static_cast<double>(cf.probability);
    const BlockState state = isTall ? Blocks::TALL_SEAGRASS.defaultState()
                                    : Blocks::SEAGRASS.defaultState();
    const BlockState below = getBlock(region, posBelow(grassPos));
    const bool canSurvive =
        isFullFaceSturdy(below, Direction::Up) &&
        !blockHasTag(below, "minecraft:cannot_support_seagrass")
             .value_or(false) &&
        isWaterSource(region, grassPos);
    if (canSurvive) {
      if (isTall) {
        const BlockState upperState = state.with("half", "upper");
        const Pos above = posAbove(grassPos);
        if (getBlock(region, above).is(Blocks::WATER)) {
          setBlock(region, grassPos, state, 2);
          setBlock(region, above, upperState, 2);
        }
      } else {
        setBlock(region, grassPos, state, 2);
      }
      placedAny = true;
    }
  }
  return placedAny;
}

// ============================================================================
// 2. BambooFeature
// ============================================================================

bool placeBamboo(const ConfiguredFeature &cf, WorldGenRegion &region,
                 RandomSource &rng, Pos origin) {
  static const BlockState kBambooTrunk = Blocks::BAMBOO.defaultState()
                                             .with("age", "1")
                                             .with("leaves", "none")
                                             .with("stage", "0");
  static const BlockState kBambooFinalLarge =
      kBambooTrunk.with("leaves", "large").with("stage", "1");
  static const BlockState kBambooTopLarge =
      kBambooTrunk.with("leaves", "large");
  static const BlockState kBambooTopSmall =
      kBambooTrunk.with("leaves", "small");

  int placed = 0;
  Pos bambooPos = origin;
  if (getBlock(region, bambooPos).isAir()) {
    const BlockState below = getBlock(region, posBelow(bambooPos));
    if (blockHasTag(below, "minecraft:supports_bamboo").value_or(false)) {
      const int height = rng.nextInt(12) + 5;
      if (rng.nextFloat() < cf.probability) {
        const int r = rng.nextInt(4) + 1;
        for (int xx = origin.x - r; xx <= origin.x + r; ++xx) {
          for (int zz = origin.z - r; zz <= origin.z + r; ++zz) {
            const int xd = xx - origin.x;
            const int zd = zz - origin.z;
            if (xd * xd + zd * zd <= r * r) {
              const int py =
                  regionGetHeight(region, HeightmapType::WorldSurface, xx, zz) -
                  1;
              const Pos podzolPos{xx, py, zz};
              if (blockHasTag(getBlock(region, podzolPos),
                              "minecraft:beneath_bamboo_podzol_replaceable")
                      .value_or(false)) {
                setBlock(region, podzolPos, Blocks::PODZOL.defaultState(), 2);
              }
            }
          }
        }
      }

      for (int i = 0; i < height && getBlock(region, bambooPos).isAir(); ++i) {
        setBlock(region, bambooPos, kBambooTrunk, 2);
        bambooPos.y += 1;
      }

      if (bambooPos.y - origin.y >= 3) {
        setBlock(region, bambooPos, kBambooFinalLarge, 2);
        bambooPos.y -= 1;
        setBlock(region, bambooPos, kBambooTopLarge, 2);
        bambooPos.y -= 1;
        setBlock(region, bambooPos, kBambooTopSmall, 2);
      }
    }
    ++placed;
  }
  return placed > 0;
}

// ============================================================================
// 3. BasaltColumnsFeature
// ============================================================================

bool basaltCannotPlaceOn(const BlockState &s) noexcept {
  return s.is(Blocks::LAVA) || s.is(Blocks::BEDROCK) ||
         s.is(Blocks::MAGMA_BLOCK) || s.is(Blocks::SOUL_SAND) ||
         s.is(Blocks::NETHER_BRICKS) || s.is(Blocks::NETHER_BRICK_FENCE) ||
         s.is(Blocks::NETHER_BRICK_STAIRS) || s.is(Blocks::NETHER_WART) ||
         s.is(Blocks::CHEST) || s.is(Blocks::SPAWNER);
}

bool isAirOrLavaOcean(const WorldGenRegion &region, int lavaSeaLevel,
                      Pos pos) noexcept {
  const BlockState s = getBlock(region, pos);
  return s.isAir() || (s.is(Blocks::LAVA) && pos.y <= lavaSeaLevel);
}

bool basaltCanPlaceAt(const WorldGenRegion &region, int lavaSeaLevel,
                      Pos cursor) noexcept {
  if (!isAirOrLavaOcean(region, lavaSeaLevel, cursor))
    return false;
  const BlockState below = getBlock(region, posBelow(cursor));
  return !below.isAir() && !basaltCannotPlaceOn(below);
}

std::optional<Pos> basaltFindSurface(const WorldGenRegion &region,
                                     int lavaSeaLevel, Pos cursor, int limit) {
  while (cursor.y > region.minY() + 1 && limit > 0) {
    --limit;
    if (basaltCanPlaceAt(region, lavaSeaLevel, cursor))
      return cursor;
    cursor.y -= 1;
  }
  return std::nullopt;
}

std::optional<Pos> basaltFindAir(const WorldGenRegion &region, Pos cursor,
                                 int limit) {
  while (cursor.y < region.maxY() && limit > 0) {
    --limit;
    const BlockState s = getBlock(region, cursor);
    if (basaltCannotPlaceOn(s))
      return std::nullopt;
    if (s.isAir())
      return cursor;
    cursor.y += 1;
  }
  return std::nullopt;
}

bool placeBasaltColumns(const ConfiguredFeature &cf, WorldGenRegion &region,
                        const NoiseBasedChunkGenerator &generator,
                        RandomSource &rng, Pos origin) {
  const int lavaSeaLevel = generator.settings().seaLevel;
  if (!basaltCanPlaceAt(region, lavaSeaLevel, origin))
    return false;

  const int columnHeight = cf.basaltColumns.height.sample(rng);
  const bool generateClustered = rng.nextFloat() < 0.9f;
  const int reach = std::min(columnHeight, generateClustered ? 5 : 8);
  const int count = generateClustered ? 50 : 15;
  bool placed = false;

  const int minX = origin.x - reach;
  const int maxX = origin.x + reach;
  const int minZ = origin.z - reach;
  const int maxZ = origin.z + reach;
  const int width = maxX - minX + 1;
  const int depth = maxZ - minZ + 1;

  for (int c = 0; c < count; ++c) {
    const int rx = minX + rng.nextInt(width);
    const int ry = origin.y + rng.nextInt(1);
    const int rz = minZ + rng.nextInt(depth);
    const Pos pos{rx, ry, rz};
    const int blocksToPlaceY = columnHeight - manhattanDist(pos, origin);
    if (blocksToPlaceY >= 0) {
      const int colReach = cf.basaltColumns.reach.sample(rng);
      forEachBetweenClosed(
          pos.x - colReach, pos.y, pos.z - colReach, pos.x + colReach, pos.y,
          pos.z + colReach, [&](Pos p) {
            const int stepLimit = manhattanDist(p, pos);
            const std::optional<Pos> columnPos =
                isAirOrLavaOcean(region, lavaSeaLevel, p)
                    ? basaltFindSurface(region, lavaSeaLevel, p, stepLimit)
                    : basaltFindAir(region, p, stepLimit);
            if (columnPos) {
              int blocksY = blocksToPlaceY - stepLimit / 2;
              Pos cursor = *columnPos;
              while (blocksY >= 0) {
                if (isAirOrLavaOcean(region, lavaSeaLevel, cursor)) {
                  setBlock(region, cursor, Blocks::BASALT.defaultState(), 2);
                  cursor.y += 1;
                  placed = true;
                } else {
                  if (!getBlock(region, cursor).is(Blocks::BASALT))
                    break;
                  cursor.y += 1;
                }
                --blocksY;
              }
            }
          });
    }
  }
  return placed;
}

// ============================================================================
// 4. BasaltPillarFeature
// ============================================================================

bool placeBasaltPillar(WorldGenRegion &region, RandomSource &rng, Pos origin) {
  if (!getBlock(region, origin).isAir() ||
      getBlock(region, posAbove(origin)).isAir()) {
    return false;
  }

  auto placeHangOff = [&](Pos p) -> bool {
    if (rng.nextInt(10) != 0) {
      setBlock(region, p, Blocks::BASALT.defaultState(), 2);
      return true;
    }
    return false;
  };
  auto placeBaseHangOff = [&](Pos p) {
    if (rng.nextBoolean()) {
      setBlock(region, p, Blocks::BASALT.defaultState(), 2);
    }
  };

  Pos pos = origin;
  bool placeNorth = true;
  bool placeSouth = true;
  bool placeWest = true;
  bool placeEast = true;

  while (getBlock(region, pos).isAir()) {
    if (isOutsideBuildHeight(region, pos.y))
      return true;

    setBlock(region, pos, Blocks::BASALT.defaultState(), 2);
    placeNorth = placeNorth && placeHangOff(pos.relative(Direction::North));
    placeSouth = placeSouth && placeHangOff(pos.relative(Direction::South));
    placeWest = placeWest && placeHangOff(pos.relative(Direction::West));
    placeEast = placeEast && placeHangOff(pos.relative(Direction::East));
    pos.y -= 1;
  }

  pos.y += 1;
  placeBaseHangOff(pos.relative(Direction::North));
  placeBaseHangOff(pos.relative(Direction::South));
  placeBaseHangOff(pos.relative(Direction::West));
  placeBaseHangOff(pos.relative(Direction::East));
  pos.y -= 1;

  for (int dx = -3; dx < 4; ++dx) {
    for (int dz = -3; dz < 4; ++dz) {
      const int probability = std::abs(dx) * std::abs(dz);
      if (rng.nextInt(10) < 10 - probability) {
        Pos basePos{pos.x + dx, pos.y, pos.z + dz};
        int maxDrop = 3;
        while (getBlock(region, posBelow(basePos)).isAir()) {
          basePos.y -= 1;
          if (--maxDrop <= 0)
            break;
        }
        if (!getBlock(region, posBelow(basePos)).isAir()) {
          setBlock(region, basePos, Blocks::BASALT.defaultState(), 2);
        }
      }
    }
  }
  return true;
}

// ============================================================================
// 5. BlockBlobFeature
// ============================================================================

bool placeBlockBlob(const ConfiguredFeature &cf, WorldGenRegion &region,
                    RandomSource &rng, Pos origin) {
  while (origin.y > region.minY() + 3 &&
         !cf.blockBlob.canPlaceOn.test(region, posBelow(origin))) {
    origin.y -= 1;
  }
  if (origin.y <= region.minY() + 3)
    return false;

  for (int c = 0; c < 3; ++c) {
    const int xr = rng.nextInt(2);
    const int yr = rng.nextInt(2);
    const int zr = rng.nextInt(2);
    const float tr = static_cast<float>(xr + yr + zr) * 0.333f + 0.5f;
    const double trSq = static_cast<double>(tr * tr);

    forEachBetweenClosed(origin.x - xr, origin.y - yr, origin.z - zr,
                         origin.x + xr, origin.y + yr, origin.z + zr,
                         [&](Pos bp) {
                           if (static_cast<double>(distSqrInt(bp, origin)) <=
                               trSq) {
                             setBlock(region, bp, cf.blockBlob.state, 3);
                           }
                         });

    const int ox = -1 + rng.nextInt(2);
    const int oy = -rng.nextInt(2);
    const int oz = -1 + rng.nextInt(2);
    origin = Pos{origin.x + ox, origin.y + oy, origin.z + oz};
  }
  return true;
}

// ============================================================================
// 6. BlueIceFeature
// ============================================================================

bool placeBlueIce(WorldGenRegion &region,
                  const NoiseBasedChunkGenerator &generator, RandomSource &rng,
                  Pos origin) {
  if (origin.y > generator.settings().seaLevel - 1)
    return false;
  if (!getBlock(region, origin).is(Blocks::WATER) &&
      !getBlock(region, posBelow(origin)).is(Blocks::WATER)) {
    return false;
  }

  bool foundPackedIce = false;
  for (Direction dir : kAllDirections) {
    if (dir != Direction::Down &&
        getBlock(region, origin.relative(dir)).is(Blocks::PACKED_ICE)) {
      foundPackedIce = true;
      break;
    }
  }
  if (!foundPackedIce)
    return false;

  setBlock(region, origin, Blocks::BLUE_ICE.defaultState(), 2);
  for (int i = 0; i < 200; ++i) {
    const int y1 = rng.nextInt(5);
    const int y2 = rng.nextInt(6);
    const int yOff = y1 - y2;
    int xzDiff = 3;
    if (yOff < 2)
      xzDiff += yOff / 2;
    if (xzDiff >= 1) {
      const int dx1 = rng.nextInt(xzDiff);
      const int dx2 = rng.nextInt(xzDiff);
      const int dz1 = rng.nextInt(xzDiff);
      const int dz2 = rng.nextInt(xzDiff);
      const Pos placePos{origin.x + dx1 - dx2, origin.y + yOff,
                         origin.z + dz1 - dz2};
      const BlockState ps = getBlock(region, placePos);
      if (ps.isAir() || ps.is(Blocks::WATER) || ps.is(Blocks::PACKED_ICE) ||
          ps.is(Blocks::ICE)) {
        for (Direction dir : kAllDirections) {
          if (getBlock(region, placePos.relative(dir)).is(Blocks::BLUE_ICE)) {
            setBlock(region, placePos, Blocks::BLUE_ICE.defaultState(), 2);
            break;
          }
        }
      }
    }
  }
  return true;
}

// ============================================================================
// 7. BonusChestFeature
// ============================================================================

bool placeBonusChest(WorldGenRegion &region, RandomSource &rng, Pos origin) {
  const int cx = origin.x >> 4;
  const int cz = origin.z >> 4;
  const int minX = cx << 4;
  const int minZ = cz << 4;

  std::vector<int> xPoses(16);
  std::vector<int> zPoses(16);
  std::iota(xPoses.begin(), xPoses.end(), minX);
  std::iota(zPoses.begin(), zPoses.end(), minZ);
  javaShuffle(xPoses, rng);
  javaShuffle(zPoses, rng);

  for (int x : xPoses) {
    for (int z : zPoses) {
      const int y =
          regionGetHeight(region, HeightmapType::MotionBlockingNoLeaves, x, z);
      const Pos chestPos{x, y, z};
      const BlockState cur = getBlock(region, chestPos);
      if (cur.isAir() || collisionShape(cur).empty()) {
        setBlock(region, chestPos, Blocks::CHEST.defaultState(), 2);
        // TODO(G-3 Feature): `ProtoChunk`/`World` 尚未支持写入 BonusChest 战利品表
        // (`minecraft:chests/spawn_bonus_chest` + `LootTableSeed`)，仅放置箱子与周围火把。
        const BlockState torch = Blocks::TORCH.defaultState();
        for (Direction dir : kHorizDirections) {
          const Pos torchPos = chestPos.relative(dir);
          if (faceSturdy(getBlock(region, posBelow(torchPos)), Direction::Up,
                         SupportType::Center)) {
            setBlock(region, torchPos, torch, 2);
          }
        }
        return true;
      }
    }
  }
  return false;
}

// ============================================================================
// 8. ChorusPlantFeature
// ============================================================================

BlockState chorusStateWithConnections(const WorldGenRegion &region, Pos pos) {
  const BlockState down = getBlock(region, posBelow(pos));
  const BlockState up = getBlock(region, posAbove(pos));
  const BlockState north = getBlock(region, pos.relative(Direction::North));
  const BlockState east = getBlock(region, pos.relative(Direction::East));
  const BlockState south = getBlock(region, pos.relative(Direction::South));
  const BlockState west = getBlock(region, pos.relative(Direction::West));
  auto isConn = [](const BlockState &s) {
    return s.is(Blocks::CHORUS_PLANT) || s.is(Blocks::CHORUS_FLOWER);
  };
  const bool downConn =
      isConn(down) ||
      blockHasTag(down, "minecraft:supports_chorus_plant").value_or(false);
  return Blocks::CHORUS_PLANT.defaultState()
      .with("down", downConn ? "true" : "false")
      .with("up", isConn(up) ? "true" : "false")
      .with("north", isConn(north) ? "true" : "false")
      .with("east", isConn(east) ? "true" : "false")
      .with("south", isConn(south) ? "true" : "false")
      .with("west", isConn(west) ? "true" : "false");
}

bool chorusAllNeighborsEmpty(const WorldGenRegion &region, Pos pos,
                             std::optional<Direction> ignore) {
  for (Direction dir : kHorizDirections) {
    if ((!ignore || dir != *ignore) && !getBlock(region, pos.relative(dir)).isAir())
      return false;
  }
  return true;
}

void chorusGrowTreeRecursive(WorldGenRegion &region, Pos current,
                             RandomSource &rng, Pos startPos,
                             int maxHorizontalSpread, int depth) {
  int height = rng.nextInt(4) + 1;
  if (depth == 0)
    ++height;

  for (int i = 0; i < height; ++i) {
    const Pos target{current.x, current.y + i + 1, current.z};
    if (!chorusAllNeighborsEmpty(region, target, std::nullopt))
      return;
    setBlock(region, target, chorusStateWithConnections(region, target), 2);
    setBlock(region, posBelow(target),
             chorusStateWithConnections(region, posBelow(target)), 2);
  }

  bool placedStem = false;
  if (depth < 4) {
    int stems = rng.nextInt(4);
    if (depth == 0)
      ++stems;
    for (int i = 0; i < stems; ++i) {
      const Direction dir = kHorizDirections[rng.nextInt(4)];
      const Pos target =
          Pos{current.x, current.y + height, current.z}.relative(dir);
      if (std::abs(target.x - startPos.x) < maxHorizontalSpread &&
          std::abs(target.z - startPos.z) < maxHorizontalSpread &&
          getBlock(region, target).isAir() &&
          getBlock(region, posBelow(target)).isAir() &&
          chorusAllNeighborsEmpty(region, target, opposite(dir))) {
        placedStem = true;
        setBlock(region, target, chorusStateWithConnections(region, target), 2);
        const Pos back = target.relative(opposite(dir));
        setBlock(region, back, chorusStateWithConnections(region, back), 2);
        chorusGrowTreeRecursive(region, target, rng, startPos,
                                maxHorizontalSpread, depth + 1);
      }
    }
  }

  if (!placedStem) {
    setBlock(region, Pos{current.x, current.y + height, current.z},
             Blocks::CHORUS_FLOWER.defaultState().with("age", "5"), 2);
  }
}

bool placeChorusPlant(WorldGenRegion &region, RandomSource &rng, Pos origin) {
  if (getBlock(region, origin).isAir() &&
      blockHasTag(getBlock(region, posBelow(origin)),
                  "minecraft:supports_chorus_plant")
          .value_or(false)) {
    setBlock(region, origin, chorusStateWithConnections(region, origin), 2);
    chorusGrowTreeRecursive(region, origin, rng, origin, 8, 0);
    return true;
  }
  return false;
}

// ============================================================================
// 9. DeltaFeature
// ============================================================================

bool deltaCannotReplace(const BlockState &s) noexcept {
  return s.is(Blocks::BEDROCK) || s.is(Blocks::NETHER_BRICKS) ||
         s.is(Blocks::NETHER_BRICK_FENCE) ||
         s.is(Blocks::NETHER_BRICK_STAIRS) || s.is(Blocks::NETHER_WART) ||
         s.is(Blocks::CHEST) || s.is(Blocks::SPAWNER);
}

bool deltaIsClear(const WorldGenRegion &region, Pos pos,
                  const ConfiguredFeature::DeltaConfig &cfg) {
  const BlockState state = getBlock(region, pos);
  if (state.is(cfg.contents.block()))
    return false;
  if (deltaCannotReplace(state))
    return false;
  for (Direction d : kAllDirections) {
    const bool isAir = getBlock(region, pos.relative(d)).isAir();
    if ((isAir && d != Direction::Up) || (!isAir && d == Direction::Up))
      return false;
  }
  return true;
}

bool placeDeltaFeature(const ConfiguredFeature &cf, WorldGenRegion &region,
                       RandomSource &rng, Pos origin) {
  bool anyPlaced = false;
  const bool spawnRim = rng.nextDouble() < 0.9;
  const int rimX = spawnRim ? cf.delta.rimSize.sample(rng) : 0;
  const int rimZ = spawnRim ? cf.delta.rimSize.sample(rng) : 0;
  const bool hasRim = spawnRim && rimX != 0 && rimZ != 0;
  const int radiusX = cf.delta.size.sample(rng);
  const int radiusZ = cf.delta.size.sample(rng);
  const int radiusLimit = std::max(radiusX, radiusZ);

  forEachWithinManhattan(origin, radiusX, 0, radiusZ, [&](Pos pos) {
    if (manhattanDist(pos, origin) > radiusLimit)
      return false;
    if (deltaIsClear(region, pos, cf.delta)) {
      if (hasRim) {
        anyPlaced = true;
        setBlock(region, pos, cf.delta.rim, 2);
      }
      const Pos posOffset{pos.x + rimX, pos.y, pos.z + rimZ};
      if (deltaIsClear(region, posOffset, cf.delta)) {
        anyPlaced = true;
        setBlock(region, posOffset, cf.delta.contents, 2);
      }
    }
    return true;
  });
  return anyPlaced;
}

// ============================================================================
// 10. DesertWellFeature
// ============================================================================

bool placeDesertWell(WorldGenRegion &region, RandomSource &rng, Pos origin) {
  const BlockState sand = Blocks::SAND.defaultState();
  const BlockState sandSlab = Blocks::SANDSTONE_SLAB.defaultState();
  const BlockState sandstone = Blocks::SANDSTONE.defaultState();
  const BlockState water = Blocks::WATER.defaultState();

  origin.y += 1;
  while (getBlock(region, origin).isAir() && origin.y > region.minY() + 2) {
    origin.y -= 1;
  }
  if (!getBlock(region, origin).is(Blocks::SAND))
    return false;

  for (int ox = -2; ox <= 2; ++ox) {
    for (int oz = -2; oz <= 2; ++oz) {
      if (getBlock(region, Pos{origin.x + ox, origin.y - 1, origin.z + oz})
              .isAir() &&
          getBlock(region, Pos{origin.x + ox, origin.y - 2, origin.z + oz})
              .isAir()) {
        return false;
      }
    }
  }

  for (int oy = -2; oy <= 0; ++oy) {
    for (int ox = -2; ox <= 2; ++ox) {
      for (int oz = -2; oz <= 2; ++oz) {
        setBlock(region, Pos{origin.x + ox, origin.y + oy, origin.z + oz},
                 sandstone, 2);
      }
    }
  }

  setBlock(region, origin, water, 2);
  for (Direction d : kHorizDirections) {
    setBlock(region, origin.relative(d), water, 2);
  }

  const Pos sandCenter = posBelow(origin);
  setBlock(region, sandCenter, sand, 2);
  for (Direction d : kHorizDirections) {
    setBlock(region, sandCenter.relative(d), sand, 2);
  }

  for (int ox = -2; ox <= 2; ++ox) {
    for (int oz = -2; oz <= 2; ++oz) {
      if (ox == -2 || ox == 2 || oz == -2 || oz == 2) {
        setBlock(region, Pos{origin.x + ox, origin.y + 1, origin.z + oz},
                 sandstone, 2);
      }
    }
  }

  setBlock(region, Pos{origin.x + 2, origin.y + 1, origin.z}, sandSlab, 2);
  setBlock(region, Pos{origin.x - 2, origin.y + 1, origin.z}, sandSlab, 2);
  setBlock(region, Pos{origin.x, origin.y + 1, origin.z + 2}, sandSlab, 2);
  setBlock(region, Pos{origin.x, origin.y + 1, origin.z - 2}, sandSlab, 2);

  for (int ox = -1; ox <= 1; ++ox) {
    for (int oz = -1; oz <= 1; ++oz) {
      setBlock(region, Pos{origin.x + ox, origin.y + 4, origin.z + oz},
               (ox == 0 && oz == 0) ? sandstone : sandSlab, 2);
    }
  }

  for (int oy = 1; oy <= 3; ++oy) {
    setBlock(region, Pos{origin.x - 1, origin.y + oy, origin.z - 1}, sandstone,
             2);
    setBlock(region, Pos{origin.x - 1, origin.y + oy, origin.z + 1}, sandstone,
             2);
    setBlock(region, Pos{origin.x + 1, origin.y + oy, origin.z - 1}, sandstone,
             2);
    setBlock(region, Pos{origin.x + 1, origin.y + oy, origin.z + 1}, sandstone,
             2);
  }

  const std::array<Pos, 5> waterPositions{
      origin, origin.relative(Direction::East), origin.relative(Direction::South),
      origin.relative(Direction::West), origin.relative(Direction::North)};
  auto placeSusSand = [&](Pos p) {
    setBlock(region, p, Blocks::SUSPICIOUS_SAND.defaultState(), 3);
    // TODO(G-3 Feature): `ProtoChunk`/`World` 尚未支持写入 `BrushableBlockEntity`
    // 考古战利品表 (`minecraft:archaeology/desert_well`)。
  };
  placeSusSand(posBelow(waterPositions[static_cast<std::size_t>(rng.nextInt(5))], 1));
  placeSusSand(posBelow(waterPositions[static_cast<std::size_t>(rng.nextInt(5))], 2));
  return true;
}

// ============================================================================
// 11. DripstoneUtils + DripstoneClusterFeature + LargeDripstoneFeature
// ============================================================================

namespace dripstone_utils {

bool isEmptyOrWaterState(const BlockState &s) noexcept {
  return s.isAir() || s.is(Blocks::WATER);
}

bool isNeitherEmptyNorWaterState(const BlockState &s) noexcept {
  return !s.isAir() && !s.is(Blocks::WATER);
}

bool isEmptyOrWaterOrLavaState(const BlockState &s) noexcept {
  return s.isAir() || s.is(Blocks::WATER) || s.is(Blocks::LAVA);
}

bool isDripstoneBase(const BlockState &s) {
  return s.is(Blocks::DRIPSTONE_BLOCK) ||
         blockHasTag(s, "minecraft:dripstone_replaceable_blocks")
             .value_or(false);
}

bool isDripstoneBaseOrLava(const BlockState &s) {
  return isDripstoneBase(s) || s.is(Blocks::LAVA);
}

double getDripstoneHeight(double xzDistanceFromCenter, double dripstoneRadius,
                          double scale, double bluntness) {
  if (xzDistanceFromCenter < bluntness)
    xzDistanceFromCenter = bluntness;
  const double cutoff = 0.384;
  const double r = xzDistanceFromCenter / dripstoneRadius * cutoff;
  const double part1 = 0.75 * std::pow(r, 4.0 / 3.0);
  const double part2 = std::pow(r, 2.0 / 3.0);
  const double part3 = (1.0 / 3.0) * std::log(r);
  const double hRel = std::max(scale * (part1 - part2 - part3), 0.0);
  return hRel / cutoff * dripstoneRadius;
}

bool isCircleMostlyEmbeddedInStone(const WorldGenRegion &region, Pos center,
                                   int xzRadius) {
  if (isEmptyOrWaterOrLavaState(getBlock(region, center)))
    return false;
  const float angleInc = 6.0f / static_cast<float>(xzRadius);
  for (float angle = 0.0f; angle < kPiF * 2.0f; angle += angleInc) {
    const int dx =
        static_cast<int>(worldgen_math::minecraftCos(angle) * xzRadius);
    const int dz =
        static_cast<int>(worldgen_math::minecraftSin(angle) * xzRadius);
    if (isEmptyOrWaterOrLavaState(
            getBlock(region, Pos{center.x + dx, center.y, center.z + dz}))) {
      return false;
    }
  }
  return true;
}

BlockState createPointedDripstone(Direction direction,
                                  std::string_view thickness) {
  return Blocks::POINTED_DRIPSTONE.defaultState()
      .with("vertical_direction", dirNameLower(direction))
      .with("thickness", thickness);
}

void growPointedDripstone(WorldGenRegion &region, Pos startPos,
                          Direction tipDirection, int height, bool mergedTip) {
  if (!isDripstoneBase(getBlock(region, startPos.relative(opposite(tipDirection)))))
    return;

  Pos pos = startPos;
  auto placeOne = [&](const BlockState &baseState) {
    BlockState st = baseState;
    if (st.is(Blocks::POINTED_DRIPSTONE) && st.hasProperty("waterlogged")) {
      st = st.with("waterlogged", isWaterAt(region, pos) ? "true" : "false");
    }
    setBlock(region, pos, st, 2);
    pos = pos.relative(tipDirection);
  };

  if (height >= 3) {
    placeOne(createPointedDripstone(tipDirection, "base"));
    for (int i = 0; i < height - 3; ++i)
      placeOne(createPointedDripstone(tipDirection, "middle"));
  }
  if (height >= 2)
    placeOne(createPointedDripstone(tipDirection, "frustum"));
  if (height >= 1)
    placeOne(
        createPointedDripstone(tipDirection, mergedTip ? "tip_merge" : "tip"));
}

bool placeDripstoneBlockIfPossible(WorldGenRegion &region, Pos pos) {
  const BlockState s = getBlock(region, pos);
  if (blockHasTag(s, "minecraft:dripstone_replaceable_blocks")
          .value_or(false)) {
    setBlock(region, pos, Blocks::DRIPSTONE_BLOCK.defaultState(), 2);
    return true;
  }
  return false;
}

} // namespace dripstone_utils

bool placeDripstoneCluster(const ConfiguredFeature &cf,
                           WorldGenRegion &region, RandomSource &rng,
                           Pos origin) {
  if (!dripstone_utils::isEmptyOrWaterState(getBlock(region, origin)))
    return false;

  const auto &cfg = cf.dripstoneCluster;
  const int height = cfg.height.sample(rng);
  const float wetness = cfg.wetness.sample(rng);
  const float density = cfg.density.sample(rng);
  const int xRadius = cfg.radius.sample(rng);
  const int zRadius = cfg.radius.sample(rng);

  auto getChance = [&](int dx, int dz) -> double {
    const int xDist = xRadius - std::abs(dx);
    const int zDist = zRadius - std::abs(dz);
    const int distFromEdge = std::min(xDist, zDist);
    return clampedMapDouble(
        static_cast<double>(distFromEdge), 0.0,
        static_cast<double>(
            cfg.maxDistanceFromEdgeAffectingChanceOfDripstoneColumn),
        static_cast<double>(
            cfg.chanceOfDripstoneColumnAtMaxDistanceFromCenter),
        1.0);
  };

  auto canBeAdjToWater = [&](Pos p) {
    const BlockState s = getBlock(region, p);
    return blockHasTag(s, "minecraft:base_stone_overworld").value_or(false) ||
           getFluid(region, p).is(Fluids::water());
  };

  auto canPlacePool = [&](Pos p) {
    const BlockState s = getBlock(region, p);
    if (s.is(Blocks::WATER) || s.is(Blocks::DRIPSTONE_BLOCK) ||
        s.is(Blocks::POINTED_DRIPSTONE)) {
      return false;
    }
    if (getFluid(region, posAbove(p)).is(Fluids::water()))
      return false;
    for (Direction d : kHorizDirections) {
      if (!canBeAdjToWater(p.relative(d)))
        return false;
    }
    return canBeAdjToWater(posBelow(p));
  };

  auto replaceWithDripstone = [&](Pos firstPos, int maxCount, Direction dir) {
    Pos p = firstPos;
    for (int i = 0; i < maxCount; ++i) {
      if (!dripstone_utils::placeDripstoneBlockIfPossible(region, p))
        return;
      p = p.relative(dir);
    }
  };

  auto getDripHeight = [&](int dx, int dz, int maxHeight) -> int {
    if (rng.nextFloat() > density)
      return 0;
    const int distFromCenter = std::abs(dx) + std::abs(dz);
    const float heightMean = static_cast<float>(clampedMapDouble(
        static_cast<double>(distFromCenter), 0.0,
        static_cast<double>(cfg.maxDistanceFromCenterAffectingHeightBias),
        static_cast<double>(maxHeight) / 2.0, 0.0));
    const float sampled =
        std::clamp(heightMean + static_cast<float>(rng.nextGaussian()) *
                                    static_cast<float>(cfg.heightDeviation),
                   0.0f, static_cast<float>(maxHeight));
    return static_cast<int>(sampled);
  };

  for (int dx = -xRadius; dx <= xRadius; ++dx) {
    for (int dz = -zRadius; dz <= zRadius; ++dz) {
      const double chance = getChance(dx, dz);
      const Pos pos{origin.x + dx, origin.y, origin.z + dz};
      auto baseCol = scanColumn(region, pos, cfg.floorToCeilingSearchRange,
                                dripstone_utils::isEmptyOrWaterState,
                                dripstone_utils::isNeitherEmptyNorWaterState);
      if (!baseCol)
        continue;
      const auto ceiling = baseCol->ceiling;
      const auto baseFloor = baseCol->floor;
      if (!ceiling && !baseFloor)
        continue;

      const bool wantPool = rng.nextFloat() < wetness;
      ScannedColumn col = *baseCol;
      if (wantPool && baseFloor &&
          canPlacePool(Pos{pos.x, *baseFloor, pos.z})) {
        col.floor = *baseFloor - 1;
        setBlock(region, Pos{pos.x, *baseFloor, pos.z},
                 Blocks::WATER.defaultState(), 2);
      }
      const auto floor = col.floor;

      const bool wantStalactite = rng.nextDouble() < chance;
      int stalactiteHeight = 0;
      if (ceiling && wantStalactite &&
          !getBlock(region, Pos{pos.x, *ceiling, pos.z}).is(Blocks::LAVA)) {
        const int ceilingThickness = cfg.dripstoneBlockLayerThickness.sample(rng);
        replaceWithDripstone(Pos{pos.x, *ceiling, pos.z}, ceilingThickness,
                             Direction::Up);
        const int maxH =
            floor ? std::min(height, *ceiling - *floor) : height;
        stalactiteHeight = getDripHeight(dx, dz, maxH);
      }

      const bool wantStalagmite = rng.nextDouble() < chance;
      int stalagmiteHeight = 0;
      if (floor && wantStalagmite &&
          !getBlock(region, Pos{pos.x, *floor, pos.z}).is(Blocks::LAVA)) {
        const int floorThickness = cfg.dripstoneBlockLayerThickness.sample(rng);
        replaceWithDripstone(Pos{pos.x, *floor, pos.z}, floorThickness,
                             Direction::Down);
        if (ceiling) {
          stalagmiteHeight = std::max(
              0, stalactiteHeight +
                     worldgen_math::randomBetweenInclusive(
                         rng, -cfg.maxStalagmiteStalactiteHeightDiff,
                         cfg.maxStalagmiteStalactiteHeightDiff));
        } else {
          stalagmiteHeight = getDripHeight(dx, dz, height);
        }
      }

      int actualStalactiteHeight = stalactiteHeight;
      int actualStalagmiteHeight = stalagmiteHeight;
      if (ceiling && floor &&
          *ceiling - stalactiteHeight <= *floor + stalagmiteHeight) {
        const int floorY = *floor;
        const int ceilingY = *ceiling;
        const int lowestStalactiteBottom =
            std::max(ceilingY - stalactiteHeight, floorY + 1);
        const int highestStalagmiteTop =
            std::min(floorY + stalagmiteHeight, ceilingY - 1);
        const int actualStalactiteBottom =
            worldgen_math::randomBetweenInclusive(rng, lowestStalactiteBottom,
                                                  highestStalagmiteTop + 1);
        const int actualStalagmiteTop = actualStalactiteBottom - 1;
        actualStalactiteHeight = ceilingY - actualStalactiteBottom;
        actualStalagmiteHeight = actualStalagmiteTop - floorY;
      }

      const bool mergeTips =
          rng.nextBoolean() && actualStalactiteHeight > 0 &&
          actualStalagmiteHeight > 0 && col.height() &&
          actualStalactiteHeight + actualStalagmiteHeight == *col.height();

      if (ceiling) {
        dripstone_utils::growPointedDripstone(
            region, Pos{pos.x, *ceiling - 1, pos.z}, Direction::Down,
            actualStalactiteHeight, mergeTips);
      }
      if (floor) {
        dripstone_utils::growPointedDripstone(
            region, Pos{pos.x, *floor + 1, pos.z}, Direction::Up,
            actualStalagmiteHeight, mergeTips);
      }
    }
  }
  return true;
}

bool placeLargeDripstone(const ConfiguredFeature &cf, WorldGenRegion &region,
                         RandomSource &rng, Pos origin) {
  if (!dripstone_utils::isEmptyOrWaterState(getBlock(region, origin)))
    return false;
  const auto &cfg = cf.largeDripstone;
  auto col = scanColumn(region, origin, cfg.floorToCeilingSearchRange,
                        dripstone_utils::isEmptyOrWaterState,
                        dripstone_utils::isDripstoneBaseOrLava);
  if (!col || !col->floor || !col->ceiling)
    return false;
  const int colHeight = *col->ceiling - *col->floor - 1;
  if (colHeight < 4)
    return false;

  const int maxRadiusFromHeight = static_cast<int>(
      static_cast<float>(colHeight) * cfg.maxColumnRadiusToCaveHeightRatio);
  const int maxColumnRadius =
      std::clamp(maxRadiusFromHeight, cfg.columnRadius.minInclusive,
                 cfg.columnRadius.maxInclusive);
  const int radius = worldgen_math::randomBetweenInclusive(
      rng, cfg.columnRadius.minInclusive, maxColumnRadius);

  struct LargeDrip {
    Pos root;
    bool pointingUp;
    int radius;
    double bluntness;
    double scale;

    int getHeightAtRadius(float checkRadius) const {
      return static_cast<int>(dripstone_utils::getDripstoneHeight(
          static_cast<double>(checkRadius), static_cast<double>(radius), scale,
          bluntness));
    }
    int getHeight() const { return getHeightAtRadius(0.0f); }
    bool isSuitableForWind(
        const ConfiguredFeature::LargeDripstoneConfig &c) const {
      return radius >= c.minRadiusForWind &&
             bluntness >= static_cast<double>(c.minBluntnessForWind);
    }
  };

  auto makeDrip = [&](Pos root, bool up, const FloatProvider &bluntProv) {
    const double bl = static_cast<double>(bluntProv.sample(rng));
    const double sc = static_cast<double>(cfg.heightScale.sample(rng));
    return LargeDrip{root, up, radius, bl, sc};
  };

  LargeDrip stalactite = makeDrip(Pos{origin.x, *col->ceiling - 1, origin.z},
                                  false, cfg.stalactiteBluntness);
  LargeDrip stalagmite = makeDrip(Pos{origin.x, *col->floor + 1, origin.z},
                                  true, cfg.stalagmiteBluntness);

  bool hasWind = false;
  double windX = 0.0;
  double windZ = 0.0;
  if (stalactite.isSuitableForWind(cfg) && stalagmite.isSuitableForWind(cfg)) {
    hasWind = true;
    const float speed = cfg.windSpeed.sample(rng);
    const float direction = worldgen_math::randomBetween(rng, 0.0f, kPiF);
    windX = static_cast<double>(worldgen_math::minecraftCos(direction) * speed);
    windZ = static_cast<double>(worldgen_math::minecraftSin(direction) * speed);
  }

  auto applyWind = [&](Pos p) -> Pos {
    if (!hasWind)
      return p;
    const int dy = origin.y - p.y;
    const int ox =
        static_cast<int>(std::floor(windX * static_cast<double>(dy)));
    const int oz =
        static_cast<int>(std::floor(windZ * static_cast<double>(dy)));
    return Pos{p.x + ox, p.y, p.z + oz};
  };

  auto moveBackAndShrink = [&](LargeDrip &d) -> bool {
    while (d.radius > 1) {
      Pos newRoot = d.root;
      const int maxTries = std::min(10, d.getHeight());
      for (int i = 0; i < maxTries; ++i) {
        if (getBlock(region, newRoot).is(Blocks::LAVA))
          return false;
        if (dripstone_utils::isCircleMostlyEmbeddedInStone(
                region, applyWind(newRoot), d.radius)) {
          d.root = newRoot;
          return true;
        }
        newRoot.y += (d.pointingUp ? -1 : 1);
      }
      d.radius /= 2;
    }
    return false;
  };

  auto placeDripBlocks = [&](const LargeDrip &d) {
    for (int dx = -d.radius; dx <= d.radius; ++dx) {
      for (int dz = -d.radius; dz <= d.radius; ++dz) {
        const float curR = std::sqrt(static_cast<float>(dx * dx + dz * dz));
        if (curR > static_cast<float>(d.radius))
          continue;
        int h = d.getHeightAtRadius(curR);
        if (h <= 0)
          continue;
        if (rng.nextFloat() < 0.2f) {
          h = static_cast<int>(static_cast<float>(h) *
                               worldgen_math::randomBetween(rng, 0.8f, 1.0f));
        }
        Pos p{d.root.x + dx, d.root.y, d.root.z + dz};
        bool hasBeenOutOfStone = false;
        const int maxY =
            d.pointingUp
                ? regionGetHeight(region, HeightmapType::WorldSurfaceWG, p.x,
                                  p.z)
                : std::numeric_limits<int>::max();
        for (int i = 0; i < h && p.y < maxY; ++i) {
          const Pos adj = applyWind(p);
          if (dripstone_utils::isEmptyOrWaterOrLavaState(
                  getBlock(region, adj))) {
            hasBeenOutOfStone = true;
            setBlock(region, adj, Blocks::DRIPSTONE_BLOCK.defaultState(), 2);
          } else if (hasBeenOutOfStone &&
                     blockHasTag(getBlock(region, adj),
                                 "minecraft:base_stone_overworld")
                         .value_or(false)) {
            break;
          }
          p.y += (d.pointingUp ? 1 : -1);
        }
      }
    }
  };

  if (moveBackAndShrink(stalactite))
    placeDripBlocks(stalactite);
  if (moveBackAndShrink(stalagmite))
    placeDripBlocks(stalagmite);
  return true;
}

// ============================================================================
// 12. EndGatewayFeature & 13. EndPlatformFeature & 14. VoidStartPlatformFeature
// ============================================================================

bool placeEndGateway(const ConfiguredFeature &cf, WorldGenRegion &region,
                     Pos origin) {
  (void)cf.endGateway;
  forEachBetweenClosed(origin.x - 1, origin.y - 2, origin.z - 1, origin.x + 1,
                       origin.y + 2, origin.z + 1, [&](Pos pos) {
                         const bool sameX = pos.x == origin.x;
                         const bool sameY = pos.y == origin.y;
                         const bool sameZ = pos.z == origin.z;
                         const bool end = std::abs(pos.y - origin.y) == 2;
                         if (sameX && sameY && sameZ) {
                           setBlock(region, pos,
                                    Blocks::END_GATEWAY.defaultState(), 2);
                           // TODO(G-3 Feature): `ProtoChunk`/`World` 尚未存储
                           // `TheEndGatewayBlockEntity` 的 `ExitPortal` / `ExactTeleport`。
                         } else if (sameY) {
                           setBlock(region, pos, Blocks::AIR.defaultState(), 2);
                         } else if (end && sameX && sameZ) {
                           setBlock(region, pos, Blocks::BEDROCK.defaultState(),
                                    2);
                         } else if ((sameX || sameZ) && !end) {
                           setBlock(region, pos, Blocks::BEDROCK.defaultState(),
                                    2);
                         } else {
                           setBlock(region, pos, Blocks::AIR.defaultState(), 2);
                         }
                       });
  return true;
}

bool placeEndPlatform(WorldGenRegion &region, Pos origin) {
  for (int dz = -2; dz <= 2; ++dz) {
    for (int dx = -2; dx <= 2; ++dx) {
      for (int dy = -1; dy < 3; ++dy) {
        const Pos blockPos{origin.x + dx, origin.y + dy, origin.z + dz};
        const Block &target = (dy == -1) ? Blocks::OBSIDIAN : Blocks::AIR;
        if (!getBlock(region, blockPos).is(target)) {
          setBlock(region, blockPos, target.defaultState(), 3);
        }
      }
    }
  }
  return true;
}

bool placeVoidStartPlatform(WorldGenRegion &region, Pos origin) {
  constexpr Pos kPlatformOffset{8, 3, 8};
  constexpr int kOriginChunkX = kPlatformOffset.x >> 4;
  constexpr int kOriginChunkZ = kPlatformOffset.z >> 4;
  const int cx = origin.x >> 4;
  const int cz = origin.z >> 4;
  auto checkerDist = [](int xa, int za, int xb, int zb) noexcept {
    return std::max(std::abs(xa - xb), std::abs(za - zb));
  };
  if (checkerDist(cx, cz, kOriginChunkX, kOriginChunkZ) > 1)
    return true;

  const Pos platformOrigin{kPlatformOffset.x, origin.y + kPlatformOffset.y,
                           kPlatformOffset.z};
  const int minX = cx << 4;
  const int maxX = minX + 15;
  const int minZ = cz << 4;
  const int maxZ = minZ + 15;
  for (int z = minZ; z <= maxZ; ++z) {
    for (int x = minX; x <= maxX; ++x) {
      if (checkerDist(platformOrigin.x, platformOrigin.z, x, z) <= 16) {
        const Pos bp{x, platformOrigin.y, z};
        setBlock(region, bp,
                 (bp == platformOrigin) ? Blocks::COBBLESTONE.defaultState()
                                        : Blocks::STONE.defaultState(),
                 2);
      }
    }
  }
  return true;
}

// ============================================================================
// 15. FossilFeature
// ============================================================================

bool placeFossil(const ConfiguredFeature &cf, WorldGenRegion &region,
                 const NoiseBasedChunkGenerator &generator, RandomSource &rng,
                 Pos origin) {
  const auto &cfg = cf.fossil;
  if (cfg.fossilStructures.empty() || cfg.overlayStructures.empty())
    return false;
  const auto &regs = generator.registries();
  const auto *templates = regs.templateCache();
  if (!templates)
    return false;

  const Rotation rotation = static_cast<Rotation>(rng.nextInt(4));
  const int fossilIndex =
      rng.nextInt(static_cast<int>(cfg.fossilStructures.size()));
  const std::string &baseId =
      cfg.fossilStructures[static_cast<std::size_t>(fossilIndex)];
  const std::string &overlayId =
      cfg.overlayStructures[static_cast<std::size_t>(fossilIndex)];

  const StructureTemplate *fossilBase = templates->getOrLoad(baseId);
  const StructureTemplate *fossilOverlay = templates->getOrLoad(overlayId);
  if (!fossilBase || !fossilOverlay)
    return false;

  const int cx = origin.x >> 4;
  const int cz = origin.z >> 4;
  const int minBX = cx << 4;
  const int minBZ = cz << 4;
  const BoundingBox clipBounds{minBX - 16, region.minY(),
                               minBZ - 16, minBX + 15 + 16,
                               region.maxY() - 1, minBZ + 15 + 16};

  const Pos size = fossilBase->rotatedSize(rotation);
  const Pos lowCorner{origin.x - size.x / 2, origin.y, origin.z - size.z / 2};
  int lowestSurfaceY = origin.y;
  for (int xs = 0; xs < size.x; ++xs) {
    for (int zs = 0; zs < size.z; ++zs) {
      lowestSurfaceY = std::min(
          lowestSurfaceY,
          regionGetHeight(region, HeightmapType::OceanFloorWG,
                          lowCorner.x + xs, lowCorner.z + zs));
    }
  }

  const int targetY =
      std::max(lowestSurfaceY - 15 - rng.nextInt(10), region.minY() + 10);
  const Pos targetPos = StructureTemplate::getZeroPositionWithTransform(
      Pos{lowCorner.x, targetY, lowCorner.z}, Mirror::None, rotation,
      fossilBase->size.x, fossilBase->size.z);

  const BoundingBox bb =
      fossilBase->boundingBox(targetPos, rotation, Pos{0, 0, 0}, Mirror::None);
  int emptyCorners = 0;
  const std::array<Pos, 8> corners{
      Pos{bb.maxX, bb.maxY, bb.maxZ}, Pos{bb.minX, bb.maxY, bb.maxZ},
      Pos{bb.maxX, bb.minY, bb.maxZ}, Pos{bb.minX, bb.minY, bb.maxZ},
      Pos{bb.maxX, bb.maxY, bb.minZ}, Pos{bb.minX, bb.maxY, bb.minZ},
      Pos{bb.maxX, bb.minY, bb.minZ}, Pos{bb.minX, bb.minY, bb.minZ},
  };
  for (const Pos cp : corners) {
    const BlockState s = getBlock(region, cp);
    if (s.isAir() || s.is(Blocks::LAVA) || s.is(Blocks::WATER))
      ++emptyCorners;
  }
  if (emptyCorners > cfg.maxEmptyCornersAllowed)
    return false;

  placeStructureTemplateWithProcessors(
      baseId, cfg.fossilProcessors, targetPos, rotation, Mirror::None,
      Pos{0, 0, 0}, clipBounds, LiquidSettings::ApplyWaterlogging, *templates,
      regs, region, rng, true, 260);
  placeStructureTemplateWithProcessors(
      overlayId, cfg.overlayProcessors, targetPos, rotation, Mirror::None,
      Pos{0, 0, 0}, clipBounds, LiquidSettings::ApplyWaterlogging, *templates,
      regs, region, rng, true, 260);
  return true;
}

// ============================================================================
// 16. GeodeFeature
// ============================================================================

bool placeGeode(const ConfiguredFeature &cf, WorldGenRegion &region,
                RandomSource &rng, Pos origin) {
  const auto &cfg = cf.geode;
  const int numPoints = cfg.distributionPoints.sample(rng);
  LegacyRandomSource noiseRng(region.seed());
  const NormalNoise noise =
      NormalNoise::create(noiseRng, NoiseParameters{-4, {1.0}});

  const double crackSizeAdjustment =
      static_cast<double>(numPoints) /
      static_cast<double>(cfg.outerWallDistance.maxInclusive);
  const double innerAir = 1.0 / std::sqrt(cfg.filling);
  const double innermostBlockLayer =
      1.0 / std::sqrt(cfg.innerLayer + crackSizeAdjustment);
  const double innerCrust =
      1.0 / std::sqrt(cfg.middleLayer + crackSizeAdjustment);
  const double outerCrust =
      1.0 / std::sqrt(cfg.outerLayer + crackSizeAdjustment);
  const double crackSize =
      1.0 /
      std::sqrt(cfg.baseCrackSize + rng.nextDouble() / 2.0 +
                (numPoints > 3 ? crackSizeAdjustment : 0.0));
  const bool shouldGenerateCrack = rng.nextFloat() < cfg.generateCrackChance;

  std::vector<std::pair<Pos, int>> points;
  points.reserve(static_cast<std::size_t>(numPoints));
  int numInvalidPoints = 0;
  for (int i = 0; i < numPoints; ++i) {
    const int x = cfg.outerWallDistance.sample(rng);
    const int y = cfg.outerWallDistance.sample(rng);
    const int z = cfg.outerWallDistance.sample(rng);
    const Pos pos{origin.x + x, origin.y + y, origin.z + z};
    const BlockState state = getBlock(region, pos);
    if (state.isAir() ||
        blockHasTag(state, cfg.invalidBlocksTag).value_or(false)) {
      if (++numInvalidPoints > cfg.invalidBlocksThreshold)
        return false;
    }
    points.emplace_back(pos, cfg.pointOffset.sample(rng));
  }

  std::vector<Pos> crackPoints;
  if (shouldGenerateCrack) {
    const int offsetIndex = rng.nextInt(4);
    const int crackOffset = numPoints * 2 + 1;
    if (offsetIndex == 0) {
      crackPoints.push_back(Pos{origin.x + crackOffset, origin.y + 7, origin.z});
      crackPoints.push_back(Pos{origin.x + crackOffset, origin.y + 5, origin.z});
      crackPoints.push_back(Pos{origin.x + crackOffset, origin.y + 1, origin.z});
    } else if (offsetIndex == 1) {
      crackPoints.push_back(Pos{origin.x, origin.y + 7, origin.z + crackOffset});
      crackPoints.push_back(Pos{origin.x, origin.y + 5, origin.z + crackOffset});
      crackPoints.push_back(Pos{origin.x, origin.y + 1, origin.z + crackOffset});
    } else if (offsetIndex == 2) {
      crackPoints.push_back(
          Pos{origin.x + crackOffset, origin.y + 7, origin.z + crackOffset});
      crackPoints.push_back(
          Pos{origin.x + crackOffset, origin.y + 5, origin.z + crackOffset});
      crackPoints.push_back(
          Pos{origin.x + crackOffset, origin.y + 1, origin.z + crackOffset});
    } else {
      crackPoints.push_back(Pos{origin.x, origin.y + 7, origin.z});
      crackPoints.push_back(Pos{origin.x, origin.y + 5, origin.z});
      crackPoints.push_back(Pos{origin.x, origin.y + 1, origin.z});
    }
  }

  auto safeSetGeode = [&](Pos p, const BlockState &state) {
    if (!blockHasTag(getBlock(region, p), cfg.cannotReplaceTag)
             .value_or(false)) {
      setBlock(region, p, state, 2);
    }
  };

  std::vector<Pos> potentialCrystalPlacements;
  forEachBetweenClosed(
      origin.x + cfg.minGenOffset, origin.y + cfg.minGenOffset,
      origin.z + cfg.minGenOffset, origin.x + cfg.maxGenOffset,
      origin.y + cfg.maxGenOffset, origin.z + cfg.maxGenOffset,
      [&](Pos pointInside) {
        const double noiseOffset =
            noise.getValue(pointInside.x, pointInside.y, pointInside.z) *
            cfg.noiseMultiplier;
        double distSumShell = 0.0;
        double distSumCrack = 0.0;
        for (const auto &[pt, off] : points) {
          distSumShell +=
              1.0 / std::sqrt(static_cast<double>(distSqrInt(pointInside, pt) +
                                                  off)) +
              noiseOffset;
        }
        for (const Pos cp : crackPoints) {
          distSumCrack +=
              1.0 / std::sqrt(static_cast<double>(distSqrInt(pointInside, cp) +
                                                  cfg.crackPointOffset)) +
              noiseOffset;
        }

        if (distSumShell < outerCrust)
          return;
        if (shouldGenerateCrack && distSumCrack >= crackSize &&
            distSumShell < innerAir) {
          safeSetGeode(pointInside, Blocks::AIR.defaultState());
          for (Direction dir : kAllDirections) {
            const Pos adj = pointInside.relative(dir);
            const FluidState adjFs = getFluid(region, adj);
            if (!adjFs.isEmpty()) {
              regionScheduleFluidTick(region, adj.x, adj.y, adj.z,
                                      adjFs.getType(), 0);
            }
          }
        } else if (distSumShell >= innerAir) {
          if (auto st = cfg.fillingProvider.getOptionalState(region, rng,
                                                             pointInside)) {
            safeSetGeode(pointInside, *st);
          }
        } else if (distSumShell >= innermostBlockLayer) {
          const bool useAlt =
              rng.nextFloat() < static_cast<float>(cfg.useAlternateLayer0Chance);
          const auto &prov =
              useAlt ? cfg.alternateInnerLayerProvider : cfg.innerLayerProvider;
          if (auto st = prov.getOptionalState(region, rng, pointInside))
            safeSetGeode(pointInside, *st);
          if ((!cfg.placementsRequireLayer0Alternate || useAlt) &&
              rng.nextFloat() <
                  static_cast<float>(cfg.usePotentialPlacementsChance)) {
            potentialCrystalPlacements.push_back(pointInside);
          }
        } else if (distSumShell >= innerCrust) {
          if (auto st = cfg.middleLayerProvider.getOptionalState(region, rng,
                                                                 pointInside)) {
            safeSetGeode(pointInside, *st);
          }
        } else if (distSumShell >= outerCrust) {
          if (auto st = cfg.outerLayerProvider.getOptionalState(region, rng,
                                                                pointInside)) {
            safeSetGeode(pointInside, *st);
          }
        }
      });

  if (!cfg.innerPlacements.empty()) {
    for (const Pos crystalPos : potentialCrystalPlacements) {
      BlockState st = cfg.innerPlacements[static_cast<std::size_t>(
          rng.nextInt(static_cast<int>(cfg.innerPlacements.size())))];
      for (Direction dir : kAllDirections) {
        if (st.hasProperty("facing"))
          st = st.with("facing", dirNameLower(dir));
        const Pos placePos = crystalPos.relative(dir);
        const BlockState placeState = getBlock(region, placePos);
        if (st.hasProperty("waterlogged")) {
          st = st.with("waterlogged",
                       getFluid(region, placePos).isSource() ? "true" : "false");
        }
        if (placeState.isAir() ||
            (placeState.is(Blocks::WATER) &&
             getFluid(region, placePos).isSource())) {
          safeSetGeode(placePos, st);
          break;
        }
      }
    }
  }
  return true;
}

// ============================================================================
// 17. IcebergFeature
// ============================================================================

bool isIcebergState(const BlockState &s) noexcept {
  return s.is(Blocks::PACKED_ICE) || s.is(Blocks::SNOW_BLOCK) ||
         s.is(Blocks::BLUE_ICE);
}

int icebergHeightDependentRadiusRound(RandomSource &rng, int yOff, int height,
                                      int width) {
  const float k = 3.5f - rng.nextFloat();
  float scale =
      (1.0f - static_cast<float>(yOff * yOff) /
                  (static_cast<float>(height) * k)) *
      static_cast<float>(width);
  if (height > 15 + rng.nextInt(5)) {
    const int tempYOff = yOff < 3 + rng.nextInt(6) ? yOff / 2 : yOff;
    scale = (1.0f - static_cast<float>(tempYOff) /
                        (static_cast<float>(height) * k * 0.4f)) *
            static_cast<float>(width);
  }
  return ceilFloat(scale / 2.0f);
}

int icebergHeightDependentRadiusEllipse(int yOff, int height, int width) {
  const float scale = (1.0f - static_cast<float>(yOff * yOff) /
                                  static_cast<float>(height)) *
                      static_cast<float>(width);
  return ceilFloat(scale / 2.0f);
}

int icebergHeightDependentRadiusSteep(RandomSource &rng, int yOff, int height,
                                      int width) {
  const float k = 1.0f + rng.nextFloat() / 2.0f;
  const float scale =
      (1.0f - static_cast<float>(yOff) / (static_cast<float>(height) * k)) *
      static_cast<float>(width);
  return ceilFloat(scale / 2.0f);
}

double icebergSignedDistanceEllipse(int xo, int zo, Pos localOrigin, int a,
                                    int c, double angle) {
  const double dx = static_cast<double>(xo - localOrigin.x);
  const double dz = static_cast<double>(zo - localOrigin.z);
  const double ca = std::cos(angle);
  const double sa = std::sin(angle);
  const double t1 = (dx * ca - dz * sa) / static_cast<double>(a);
  const double t2 = (dx * sa + dz * ca) / static_cast<double>(c);
  return t1 * t1 + t2 * t2 - 1.0;
}

double icebergSignedDistanceCircle(int xo, int zo, Pos localOrigin, int radius,
                                   RandomSource &rng) {
  const float off =
      10.0f * std::clamp(rng.nextFloat(), 0.2f, 0.8f) / static_cast<float>(radius);
  const double dx = static_cast<double>(xo - localOrigin.x);
  const double dz = static_cast<double>(zo - localOrigin.z);
  return static_cast<double>(off) + dx * dx + dz * dz -
         static_cast<double>(radius * radius);
}

int icebergGetEllipseC(int yOff, int height, int shapeEllipseC) noexcept {
  int c = shapeEllipseC;
  if (yOff > 0 && height - yOff <= 3)
    c = shapeEllipseC - (4 - (height - yOff));
  return c;
}

bool placeIceberg(const ConfiguredFeature &cf, WorldGenRegion &region,
                  const NoiseBasedChunkGenerator &generator, RandomSource &rng,
                  Pos origin) {
  origin = Pos{origin.x, generator.settings().seaLevel, origin.z};
  const bool snowOnTop = rng.nextDouble() > 0.7;
  const BlockState mainState = cf.simpleBlockState;
  const double shapeAngle = rng.nextDouble() * 2.0 * kPi;
  const int shapeEllipseA = 11 - rng.nextInt(5);
  const int shapeEllipseC = 3 + rng.nextInt(3);
  const bool isEllipse = rng.nextDouble() > 0.7;
  int overWaterHeight = isEllipse ? rng.nextInt(6) + 6 : rng.nextInt(15) + 3;
  if (!isEllipse && rng.nextDouble() > 0.9)
    overWaterHeight += rng.nextInt(19) + 7;
  const int underWaterHeight = std::min(overWaterHeight + rng.nextInt(11), 18);
  const int w1 = rng.nextInt(7);
  const int w2 = rng.nextInt(5);
  const int width = std::min(overWaterHeight + w1 - w2, 11);
  const int a = isEllipse ? shapeEllipseA : 11;

  auto setIcebergBlock = [&](Pos pos, int hDiff, int height) {
    const BlockState state = getBlock(region, pos);
    if (state.isAir() || state.is(Blocks::SNOW_BLOCK) || state.is(Blocks::ICE) ||
        state.is(Blocks::WATER)) {
      const bool randomness = !isEllipse || rng.nextDouble() > 0.05;
      const int divisor = isEllipse ? 3 : 2;
      if (snowOnTop && !state.is(Blocks::WATER) &&
          hDiff <= rng.nextInt(std::max(1, height / divisor)) +
                       static_cast<double>(height) * 0.6 &&
          randomness) {
        setBlock(region, pos, Blocks::SNOW_BLOCK.defaultState(), 2);
      } else {
        setBlock(region, pos, mainState, 2);
      }
    }
  };

  auto genBlock = [&](int height, int xo, int yOff, int zo, int radius,
                      int curA) {
    const double signedDist =
        isEllipse
            ? icebergSignedDistanceEllipse(
                  xo, zo, Pos{0, 0, 0}, curA,
                  icebergGetEllipseC(yOff, height, shapeEllipseC), shapeAngle)
            : icebergSignedDistanceCircle(xo, zo, Pos{0, 0, 0}, radius, rng);
    if (signedDist < 0.0) {
      const Pos pos{origin.x + xo, origin.y + yOff, origin.z + zo};
      const double compareVal = isEllipse ? -0.5 : -6.0 - rng.nextInt(3);
      if (signedDist > compareVal && rng.nextDouble() > 0.9)
        return;
      setIcebergBlock(pos, height - yOff, height);
    }
  };

  for (int xo = -a; xo < a; ++xo) {
    for (int zo = -a; zo < a; ++zo) {
      for (int yOff = 0; yOff < overWaterHeight; ++yOff) {
        const int radius =
            isEllipse
                ? icebergHeightDependentRadiusEllipse(yOff, overWaterHeight,
                                                      width)
                : icebergHeightDependentRadiusRound(rng, yOff, overWaterHeight,
                                                    width);
        if (isEllipse || xo < radius)
          genBlock(overWaterHeight, xo, yOff, zo, radius, a);
      }
    }
  }

  // smooth()
  {
    const int sa = isEllipse ? shapeEllipseA : width / 2;
    for (int x = -sa; x <= sa; ++x) {
      for (int z = -sa; z <= sa; ++z) {
        for (int yOff = 0; yOff <= overWaterHeight; ++yOff) {
          const Pos pos{origin.x + x, origin.y + yOff, origin.z + z};
          const BlockState s = getBlock(region, pos);
          if (isIcebergState(s) || s.is(Blocks::SNOW)) {
            if (getBlock(region, posBelow(pos)).isAir()) {
              setBlock(region, pos, Blocks::AIR.defaultState(), 2);
              setBlock(region, posAbove(pos), Blocks::AIR.defaultState(), 2);
            } else if (isIcebergState(s)) {
              int counter = 0;
              for (Direction d : {Direction::West, Direction::East,
                                  Direction::North, Direction::South}) {
                if (!isIcebergState(getBlock(region, pos.relative(d))))
                  ++counter;
              }
              if (counter >= 3)
                setBlock(region, pos, Blocks::AIR.defaultState(), 2);
            }
          }
        }
      }
    }
  }

  for (int xo = -a; xo < a; ++xo) {
    for (int zo = -a; zo < a; ++zo) {
      for (int yOff = -1; yOff > -underWaterHeight; --yOff) {
        const int newA =
            isEllipse
                ? ceilFloat(static_cast<float>(a) *
                            (1.0f - static_cast<float>(yOff * yOff) /
                                        (static_cast<float>(underWaterHeight) *
                                         8.0f)))
                : a;
        const int radius = icebergHeightDependentRadiusSteep(
            rng, -yOff, underWaterHeight, width);
        if (xo < radius)
          genBlock(underWaterHeight, xo, yOff, zo, radius, newA);
      }
    }
  }

  const bool doCutOut =
      isEllipse ? rng.nextDouble() > 0.1 : rng.nextDouble() > 0.7;
  if (doCutOut) {
    const int randomSignX = rng.nextBoolean() ? -1 : 1;
    const int randomSignZ = rng.nextBoolean() ? -1 : 1;
    int xOff = rng.nextInt(std::max(width / 2 - 2, 1));
    if (rng.nextBoolean())
      xOff = width / 2 + 1 - rng.nextInt(std::max(width - width / 2 - 1, 1));
    int zOff = rng.nextInt(std::max(width / 2 - 2, 1));
    if (rng.nextBoolean())
      zOff = width / 2 + 1 - rng.nextInt(std::max(width - width / 2 - 1, 1));
    if (isEllipse)
      xOff = zOff = rng.nextInt(std::max(shapeEllipseA - 5, 1));

    const Pos localOrigin{randomSignX * xOff, 0, randomSignZ * zOff};
    const double angle =
        isEllipse ? shapeAngle + kPi / 2.0 : rng.nextDouble() * 2.0 * kPi;

    auto carve = [&](int radius, int yOff, bool underWater) {
      const int ca = radius + 1 + shapeEllipseA / 3;
      const int cc = std::min(radius - 3, 3) + shapeEllipseC / 2 - 1;
      for (int xo = -ca; xo < ca; ++xo) {
        for (int zo = -ca; zo < ca; ++zo) {
          if (icebergSignedDistanceEllipse(xo, zo, localOrigin, ca, cc, angle) <
              0.0) {
            const Pos pos{origin.x + xo, origin.y + yOff, origin.z + zo};
            const BlockState state = getBlock(region, pos);
            if (isIcebergState(state) || state.is(Blocks::SNOW_BLOCK)) {
              if (underWater) {
                setBlock(region, pos, Blocks::WATER.defaultState(), 2);
              } else {
                setBlock(region, pos, Blocks::AIR.defaultState(), 2);
                if (getBlock(region, posAbove(pos)).is(Blocks::SNOW))
                  setBlock(region, posAbove(pos), Blocks::AIR.defaultState(), 2);
              }
            }
          }
        }
      }
    };

    for (int yOff = 0; yOff < overWaterHeight - 3; ++yOff) {
      const int radius =
          icebergHeightDependentRadiusRound(rng, yOff, overWaterHeight, width);
      carve(radius, yOff, false);
    }
    const int cutDownLimit = -overWaterHeight + rng.nextInt(5);
    for (int yOff = -1; yOff > cutDownLimit; --yOff) {
      const int radius =
          icebergHeightDependentRadiusSteep(rng, -yOff, overWaterHeight, width);
      carve(radius, yOff, true);
    }
  }
  return true;
}

// ============================================================================
// 18. KelpFeature
// ============================================================================

bool placeKelp(WorldGenRegion &region, RandomSource &rng, Pos origin) {
  int placed = 0;
  const int y =
      regionGetHeight(region, HeightmapType::OceanFloor, origin.x, origin.z);
  Pos kelpPos{origin.x, y, origin.z};
  if (getBlock(region, kelpPos).is(Blocks::WATER)) {
    const BlockState stateTop = Blocks::KELP.defaultState();
    const BlockState state = Blocks::KELP_PLANT.defaultState();
    const int height = 1 + rng.nextInt(10);

    auto canSurviveKelp = [&](Pos p) {
      const BlockState below = getBlock(region, posBelow(p));
      return !blockHasTag(below, "minecraft:cannot_support_kelp")
                  .value_or(false) &&
             (below.is(Blocks::KELP) || below.is(Blocks::KELP_PLANT) ||
              isFullFaceSturdy(below, Direction::Up));
    };

    for (int h = 0; h <= height; ++h) {
      if (getBlock(region, kelpPos).is(Blocks::WATER) &&
          getBlock(region, posAbove(kelpPos)).is(Blocks::WATER) &&
          canSurviveKelp(kelpPos)) {
        if (h == height) {
          setBlock(region, kelpPos,
                   stateTop.with("age", std::to_string(rng.nextInt(4) + 20)),
                   2);
          ++placed;
        } else {
          setBlock(region, kelpPos, state, 2);
        }
      } else if (h > 0) {
        const Pos below = posBelow(kelpPos);
        if (canSurviveKelp(below) &&
            !getBlock(region, posBelow(below)).is(Blocks::KELP)) {
          setBlock(region, below,
                   stateTop.with("age", std::to_string(rng.nextInt(4) + 20)),
                   2);
          ++placed;
        }
        break;
      }
      kelpPos.y += 1;
    }
  }
  return placed > 0;
}

// ============================================================================
// 19. MonsterRoomFeature
// ============================================================================

BlockState reorientChest(const WorldGenRegion &region, Pos blockPos,
                         BlockState blockState) {
  std::optional<Direction> solidNeighbor;
  for (Direction dir : kHorizDirections) {
    const BlockState st = getBlock(region, blockPos.relative(dir));
    if (st.is(Blocks::CHEST))
      return blockState;
    if (isSolidRenderApprox(st)) {
      if (solidNeighbor.has_value()) {
        solidNeighbor.reset();
        break;
      }
      solidNeighbor = dir;
    }
  }
  if (solidNeighbor)
    return blockState.with("facing", dirNameLower(opposite(*solidNeighbor)));

  Direction lockDir =
      directionFromString(blockState.getView("facing", "north"));
  if (isSolidRenderApprox(getBlock(region, blockPos.relative(lockDir)))) {
    lockDir = opposite(lockDir);
    if (isSolidRenderApprox(getBlock(region, blockPos.relative(lockDir)))) {
      auto cw = [](Direction d) {
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
      };
      lockDir = cw(lockDir);
      if (isSolidRenderApprox(getBlock(region, blockPos.relative(lockDir))))
        lockDir = opposite(lockDir);
    }
  }
  return blockState.with("facing", dirNameLower(lockDir));
}

bool placeMonsterRoom(WorldGenRegion &region, RandomSource &rng, Pos origin) {
  const int xr = rng.nextInt(2) + 2;
  const int minX = -xr - 1;
  const int maxX = xr + 1;
  const int zr = rng.nextInt(2) + 2;
  const int minZ = -zr - 1;
  const int maxZ = zr + 1;
  int holeCount = 0;

  for (int dx = minX; dx <= maxX; ++dx) {
    for (int dy = -1; dy <= 4; ++dy) {
      for (int dz = minZ; dz <= maxZ; ++dz) {
        const Pos holePos{origin.x + dx, origin.y + dy, origin.z + dz};
        const bool solid = isBlockSolid(getBlock(region, holePos));
        if ((dy == -1 || dy == 4) && !solid)
          return false;
        if ((dx == minX || dx == maxX || dz == minZ || dz == maxZ) &&
            dy == 0 && getBlock(region, holePos).isAir() &&
            getBlock(region, posAbove(holePos)).isAir()) {
          ++holeCount;
        }
      }
    }
  }

  if (holeCount < 1 || holeCount > 5)
    return false;

  const BlockState caveAir = Blocks::CAVE_AIR.defaultState();
  auto safeSetMonsterRoom = [&](Pos p, const BlockState &st) {
    if (!blockHasTag(getBlock(region, p), "minecraft:features_cannot_replace")
             .value_or(false)) {
      setBlock(region, p, st, 2);
    }
  };

  for (int dx = minX; dx <= maxX; ++dx) {
    for (int dy = 3; dy >= -1; --dy) {
      for (int dz = minZ; dz <= maxZ; ++dz) {
        const Pos wallBlock{origin.x + dx, origin.y + dy, origin.z + dz};
        const BlockState wallState = getBlock(region, wallBlock);
        if (dx == minX || dy == -1 || dz == minZ || dx == maxX || dy == 4 ||
            dz == maxZ) {
          if (wallBlock.y >= region.minY() &&
              !isBlockSolid(getBlock(region, posBelow(wallBlock)))) {
            setBlock(region, wallBlock, caveAir, 2);
          } else if (isBlockSolid(wallState) && !wallState.is(Blocks::CHEST)) {
            if (dy == -1 && rng.nextInt(4) != 0) {
              safeSetMonsterRoom(wallBlock,
                                 Blocks::MOSSY_COBBLESTONE.defaultState());
            } else {
              safeSetMonsterRoom(wallBlock, Blocks::COBBLESTONE.defaultState());
            }
          }
        } else if (!wallState.is(Blocks::CHEST) &&
                   !wallState.is(Blocks::SPAWNER)) {
          safeSetMonsterRoom(wallBlock, caveAir);
        }
      }
    }
  }

  for (int cc = 0; cc < 2; ++cc) {
    for (int i = 0; i < 3; ++i) {
      const int xc = origin.x + rng.nextInt(xr * 2 + 1) - xr;
      const int yc = origin.y;
      const int zc = origin.z + rng.nextInt(zr * 2 + 1) - zr;
      const Pos chestPos{xc, yc, zc};
      if (getBlock(region, chestPos).isAir()) {
        int wallCount = 0;
        for (Direction dir : kHorizDirections) {
          if (isBlockSolid(getBlock(region, chestPos.relative(dir))))
            ++wallCount;
        }
        if (wallCount == 1) {
          safeSetMonsterRoom(
              chestPos,
              reorientChest(region, chestPos, Blocks::CHEST.defaultState()));
          // TODO(G-3 Feature): `ProtoChunk`/`World` 尚未支持写入地牢箱子战利品表
          // (`minecraft:chests/simple_dungeon`)。
          break;
        }
      }
    }
  }

  safeSetMonsterRoom(origin, Blocks::SPAWNER.defaultState());
  (void)rng.nextInt(4); // MOBS: [SKELETON, ZOMBIE, ZOMBIE, SPIDER]
  // TODO(G-3 Feature): `ProtoChunk`/`World` 尚未支持写入 `SpawnerBlockEntity`
  // 的刷怪笼实体类型 (`SpawnData` / `SpawnPotentials`)。
  return true;
}

// ============================================================================
// 20. MultifaceGrowthFeature + MultifaceSpreader (used by MultifaceGrowth & Sculk)
// ============================================================================

namespace multiface_detail {

bool multifaceHasFace(const BlockState &state, Direction dir) {
  const std::string_view prop = dirNameLower(dir);
  return state.hasProperty(prop) && state.getView(prop, "false") == "true";
}

bool multifaceHasAnyFace(const BlockState &state) {
  for (Direction d : kAllDirections) {
    if (multifaceHasFace(state, d))
      return true;
  }
  return false;
}

bool multifaceCanAttachTo(const WorldGenRegion &region, Pos neighbourPos,
                          Direction dirTowardsNeighbour) {
  return isFullFaceSturdy(getBlock(region, neighbourPos),
                          opposite(dirTowardsNeighbour));
}

bool multifaceIsValidPlacement(const WorldGenRegion &region, const Block &block,
                               const BlockState &oldState, Pos placementPos,
                               Direction placementDir) {
  if (!oldState.is(block) || !multifaceHasFace(oldState, placementDir)) {
    return multifaceCanAttachTo(region, placementPos.relative(placementDir),
                                placementDir);
  }
  return false;
}

std::optional<BlockState>
multifaceGetStateForPlacement(const Block &block, const BlockState &oldState,
                              const WorldGenRegion &region, Pos placementPos,
                              Direction placementDir) {
  if (!multifaceIsValidPlacement(region, block, oldState, placementPos,
                                 placementDir)) {
    return std::nullopt;
  }
  BlockState newState = block.defaultState();
  if (oldState.is(block)) {
    newState = oldState;
  } else if (isWaterSource(region, placementPos)) {
    newState = newState.with("waterlogged", "true");
  }
  return newState.with(dirNameLower(placementDir), "true");
}

struct SpreadPos {
  Pos pos{};
  Direction face = Direction::Down;
};

enum class SpreadType : std::uint8_t {
  SamePosition,
  SamePlane,
  WrapAround,
};

SpreadPos getSpreadPos(SpreadType t, Pos pos, Direction spreadDir,
                       Direction fromFace) noexcept {
  switch (t) {
  case SpreadType::SamePosition:
    return SpreadPos{pos, spreadDir};
  case SpreadType::SamePlane:
    return SpreadPos{pos.relative(spreadDir), fromFace};
  case SpreadType::WrapAround:
    return SpreadPos{pos.relative(spreadDir).relative(fromFace),
                     opposite(spreadDir)};
  }
  return SpreadPos{pos, spreadDir};
}

bool multifaceCanSpreadInto(const WorldGenRegion &region, const Block &block,
                            bool isSculkVein, Pos sourcePos,
                            const SpreadPos &sp) {
  const BlockState existing = getBlock(region, sp.pos);
  if (isSculkVein) {
    const BlockState against = getBlock(region, sp.pos.relative(sp.face));
    if (against.is(Blocks::SCULK) || against.is(Blocks::SCULK_CATALYST) ||
        against.is(Blocks::MOVING_PISTON)) {
      return false;
    }
    if (manhattanDist(sourcePos, sp.pos) == 2) {
      const Pos neigh = sourcePos.relative(opposite(sp.face));
      if (isFullFaceSturdy(getBlock(region, neigh), sp.face))
        return false;
    }
    const FluidState fs = getFluid(region, sp.pos);
    if (!fs.isEmpty() && !fs.is(Fluids::water()))
      return false;
    if (blockHasTag(existing, "minecraft:fire").value_or(false))
      return false;
    const bool replaceable =
        existing.isAir() || existing.is(block) ||
        (existing.is(Blocks::WATER) && fs.isSource()) ||
        existing.block().metadata().replaceable;
    return replaceable &&
           multifaceIsValidPlacement(region, block, existing, sp.pos, sp.face);
  } else {
    const bool replaceable =
        existing.isAir() || existing.is(block) ||
        (existing.is(Blocks::WATER) && getFluid(region, sp.pos).isSource());
    return replaceable &&
           multifaceIsValidPlacement(region, block, existing, sp.pos, sp.face);
  }
}

std::optional<SpreadPos> spreadFromFaceTowardDirection(
    const BlockState &state, WorldGenRegion &region, const Block &block,
    bool isSculkVein, bool otherBlockValidAsSource, Pos pos,
    Direction fromFace, Direction spreadDir,
    const std::vector<SpreadType> &spreadTypes, bool postProcess, int flags) {
  if (dirAxis(spreadDir) == dirAxis(fromFace))
    return std::nullopt;
  const bool canFrom =
      (otherBlockValidAsSource && !state.is(block)) ||
      (multifaceHasFace(state, fromFace) && !multifaceHasFace(state, spreadDir));
  if (!canFrom)
    return std::nullopt;

  for (SpreadType st : spreadTypes) {
    const SpreadPos sp = getSpreadPos(st, pos, spreadDir, fromFace);
    if (multifaceCanSpreadInto(region, block, isSculkVein, pos, sp)) {
      const BlockState oldS = getBlock(region, sp.pos);
      if (auto ns = multifaceGetStateForPlacement(block, oldS, region, sp.pos,
                                                  sp.face)) {
        if (postProcess)
          regionMarkPosForPostprocessing(region, sp.pos.x, sp.pos.y, sp.pos.z);
        if (setBlock(region, sp.pos, *ns, flags))
          return sp;
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

bool spreadFromFaceTowardRandomDirection(const BlockState &state,
                                         WorldGenRegion &region,
                                         const Block &block, bool isSculkVein,
                                         Pos pos, Direction startingFace,
                                         RandomSource &rng, bool postProcess) {
  std::vector<Direction> dirs(kAllDirections.begin(), kAllDirections.end());
  javaShuffle(dirs, rng);
  static const std::vector<SpreadType> kDefaultOrder{
      SpreadType::SamePosition, SpreadType::SamePlane, SpreadType::WrapAround};
  for (Direction spreadDir : dirs) {
    if (spreadFromFaceTowardDirection(state, region, block, isSculkVein, false,
                                      pos, startingFace, spreadDir,
                                      kDefaultOrder, postProcess, 2)) {
      return true;
    }
  }
  return false;
}

long spreadAll(const BlockState &state, WorldGenRegion &region,
               const Block &block, bool isSculkVein,
               bool otherBlockValidAsSource, Pos pos,
               const std::vector<SpreadType> &spreadTypes, bool postProcess) {
  long count = 0;
  for (Direction faceDir : kAllDirections) {
    const bool canFrom = (otherBlockValidAsSource && !state.is(block)) ||
                         multifaceHasFace(state, faceDir);
    if (!canFrom)
      continue;
    for (Direction spreadDir : kAllDirections) {
      if (spreadFromFaceTowardDirection(
              state, region, block, isSculkVein, otherBlockValidAsSource, pos,
              faceDir, spreadDir, spreadTypes, postProcess, 2)) {
        ++count;
      }
    }
  }
  return count;
}

} // namespace multiface_detail

bool placeMultifaceGrowth(const ConfiguredFeature &cf, WorldGenRegion &region,
                          RandomSource &rng, Pos origin) {
  const auto &cfg = cf.multifaceGrowth;
  const Block &placeBlock = blockByName(cfg.blockId);
  const bool isSculkVein = placeBlock.is(Blocks::SCULK_VEIN);
  auto isAirOrWater = [](const BlockState &s) noexcept {
    return s.isAir() || s.is(Blocks::WATER);
  };
  if (!isAirOrWater(getBlock(region, origin)))
    return false;

  std::vector<Direction> validDirections;
  if (cfg.canPlaceOnCeiling)
    validDirections.push_back(Direction::Up);
  if (cfg.canPlaceOnFloor)
    validDirections.push_back(Direction::Down);
  if (cfg.canPlaceOnWall) {
    for (Direction d : kHorizDirections)
      validDirections.push_back(d);
  }

  auto canPlaceOnBlock = [&](const BlockState &s) {
    if (!cfg.canBePlacedOnTag.empty() &&
        blockHasTag(s, cfg.canBePlacedOnTag).value_or(false)) {
      return true;
    }
    const std::string_view name = s.nameView();
    for (const auto &id : cfg.canBePlacedOnIds) {
      if (name == id)
        return true;
    }
    return false;
  };

  auto tryPlaceGrowth = [&](Pos pos, const BlockState &oldState,
                            const std::vector<Direction> &dirs) -> bool {
    for (Direction placementDir : dirs) {
      const BlockState neigh = getBlock(region, pos.relative(placementDir));
      if (canPlaceOnBlock(neigh)) {
        auto newState = multiface_detail::multifaceGetStateForPlacement(
            placeBlock, oldState, region, pos, placementDir);
        if (!newState)
          return false;
        setBlock(region, pos, *newState, 3);
        regionMarkPosForPostprocessing(region, pos.x, pos.y, pos.z);
        if (rng.nextFloat() < cfg.chanceOfSpreading) {
          multiface_detail::spreadFromFaceTowardRandomDirection(
              *newState, region, placeBlock, isSculkVein, pos, placementDir,
              rng, true);
        }
        return true;
      }
    }
    return false;
  };

  std::vector<Direction> searchDirections = validDirections;
  javaShuffle(searchDirections, rng);
  if (tryPlaceGrowth(origin, getBlock(region, origin), searchDirections))
    return true;

  for (Direction searchDir : searchDirections) {
    std::vector<Direction> placementDirs;
    const Direction excl = opposite(searchDir);
    for (Direction d : validDirections) {
      if (d != excl)
        placementDirs.push_back(d);
    }
    javaShuffle(placementDirs, rng);
    for (int i = 0; i < cfg.searchRange; ++i) {
      // Match vanilla 26.1 MultifaceGrowthFeature.java:234 (`pos.setWithOffset(origin, searchDirection)`).
      const Pos pos = origin.relative(searchDir);
      const BlockState state = getBlock(region, pos);
      if (!isAirOrWater(state) && !state.is(placeBlock))
        break;
      if (tryPlaceGrowth(pos, state, placementDirs))
        return true;
    }
  }
  return false;
}

// ============================================================================
// 21. RootSystemFeature
// ============================================================================

bool placeRootSystem(const ConfiguredFeature &cf, WorldGenRegion &region,
                     const NoiseBasedChunkGenerator &generator,
                     std::int64_t worldSeed, RandomSource &rng, Pos origin) {
  if (!getBlock(region, origin).isAir())
    return false;

  const auto &cfg = cf.rootSystem;
  const auto &regs = generator.registries();

  auto isAllowedTreeSpace = [&](const BlockState &s, Pos p,
                                int blocksAboveOrigin) {
    if (s.isAir())
      return true;
    return (blocksAboveOrigin + 1) <= cfg.allowedVerticalWaterForTree &&
           getFluid(region, p).is(Fluids::water());
  };

  auto spaceForTree = [&](Pos p) {
    for (int i = 1; i <= cfg.requiredVerticalSpaceForTree; ++i) {
      const Pos up{p.x, p.y + i, p.z};
      if (!isAllowedTreeSpace(getBlock(region, up), up, i))
        return false;
    }
    return true;
  };

  auto placeRootedDirt = [&](Pos rowOrigin) {
    for (int i = 0; i < cfg.rootPlacementAttempts; ++i) {
      const int dx1 = rng.nextInt(cfg.rootRadius);
      const int dx2 = rng.nextInt(cfg.rootRadius);
      const int dz1 = rng.nextInt(cfg.rootRadius);
      const int dz2 = rng.nextInt(cfg.rootRadius);
      const Pos wp{rowOrigin.x + dx1 - dx2, rowOrigin.y,
                   rowOrigin.z + dz1 - dz2};
      if (blockHasTag(getBlock(region, wp), cfg.rootReplaceableTag)
              .value_or(false)) {
        if (auto st = cfg.rootStateProvider.getOptionalState(region, rng, wp)) {
          setBlock(region, wp, *st, 2);
        }
      }
    }
  };

  auto placeDirt = [&](int targetHeight) {
    for (int y = origin.y; y < targetHeight; ++y) {
      placeRootedDirt(Pos{origin.x, y, origin.z});
    }
  };

  auto placeRoots = [&]() {
    for (int i = 0; i < cfg.hangingRootPlacementAttempts; ++i) {
      const int dx1 = rng.nextInt(cfg.hangingRootRadius);
      const int dx2 = rng.nextInt(cfg.hangingRootRadius);
      const int dy1 = rng.nextInt(cfg.hangingRootsVerticalSpan);
      const int dy2 = rng.nextInt(cfg.hangingRootsVerticalSpan);
      const int dz1 = rng.nextInt(cfg.hangingRootRadius);
      const int dz2 = rng.nextInt(cfg.hangingRootRadius);
      const Pos wp{origin.x + dx1 - dx2, origin.y + dy1 - dy2,
                   origin.z + dz1 - dz2};
      if (getBlock(region, wp).isAir()) {
        if (auto stOpt = cfg.hangingRootStateProvider.getOptionalState(
                region, rng, wp)) {
          BlockState st = *stOpt;
          if (st.hasProperty("waterlogged") && isWaterAt(region, wp))
            st = st.with("waterlogged", "true");
          if (isFullFaceSturdy(getBlock(region, posAbove(wp)), Direction::Down))
            setBlock(region, wp, st, 2);
        }
      }
    }
  };

  const PlacedFeature *treePf = nullptr;
  if (cfg.treeFeature) {
    if (cfg.treeFeature->inlineFeature ||
        !cfg.treeFeature->featureRefId.empty()) {
      treePf = cfg.treeFeature.get();
    } else if (!cfg.treeFeature->id.empty()) {
      treePf = regs.findPlacedFeature(cfg.treeFeature->id);
    }
  }

  bool placedTree = false;
  Pos workingPos = origin;
  for (int y = 0; y < cfg.rootColumnMaxHeight; ++y) {
    workingPos.y += 1;
    if (cfg.allowedTreePosition.test(region, workingPos) &&
        spaceForTree(workingPos)) {
      const Pos belowPos = posBelow(workingPos);
      if (getFluid(region, belowPos).is(Fluids::lava()) ||
          !isBlockSolid(getBlock(region, belowPos))) {
        break;
      }
      if (treePf &&
          treePf->place(region, generator, worldSeed, rng, workingPos, false)) {
        placeDirt(origin.y + y);
        placedTree = true;
        break;
      }
    }
  }

  if (placedTree)
    placeRoots();
  return true;
}

// ============================================================================
// 22. SculkPatchFeature + SculkSpreader
// ============================================================================

namespace sculk_detail {

constexpr int kGrowthSpawnCost = 50;
constexpr int kNoGrowthRadius = 1;
constexpr int kChargeDecayRate = 5;
constexpr int kAdditionalDecayRate = 10;
constexpr int kMaxCursors = 32;

const std::vector<Pos> &nonCornerNeighbours() {
  static const std::vector<Pos> kOffsets = []() {
    std::vector<Pos> out;
    out.reserve(18);
    forEachBetweenClosed(-1, -1, -1, 1, 1, 1, [&](Pos p) {
      if ((p.x == 0 || p.y == 0 || p.z == 0) && !(p.x == 0 && p.y == 0 && p.z == 0))
        out.push_back(p);
    });
    return out;
  }();
  return kOffsets;
}

void sculkVeinOnDischarged(WorldGenRegion &region, BlockState state, Pos pos) {
  if (!state.is(Blocks::SCULK_VEIN))
    return;
  for (Direction dir : kAllDirections) {
    const std::string_view prop = dirNameLower(dir);
    if (state.getView(prop, "false") == "true" &&
        getBlock(region, pos.relative(dir)).is(Blocks::SCULK)) {
      state = state.with(prop, "false");
    }
  }
  if (!multiface_detail::multifaceHasAnyFace(state)) {
    const FluidState fs = getFluid(region, pos);
    state = (fs.isEmpty() ? Blocks::AIR : Blocks::WATER).defaultState();
  }
  setBlock(region, pos, state, 3);
}

bool sculkVeinHasSubstrateAccess(const WorldGenRegion &region,
                                 const BlockState &state, Pos pos) {
  if (!state.is(Blocks::SCULK_VEIN))
    return false;
  for (Direction dir : kAllDirections) {
    if (multiface_detail::multifaceHasFace(state, dir) &&
        blockHasTag(getBlock(region, pos.relative(dir)),
                    "minecraft:sculk_replaceable")
            .value_or(false)) {
      return true;
    }
  }
  return false;
}

bool sculkVeinRegrow(WorldGenRegion &region, Pos pos,
                     const std::vector<Direction> &faces) {
  bool hasFace = false;
  BlockState newState = Blocks::SCULK_VEIN.defaultState();
  for (Direction f : faces) {
    if (multiface_detail::multifaceCanAttachTo(region, pos.relative(f), f)) {
      newState = newState.with(dirNameLower(f), "true");
      hasFace = true;
    }
  }
  if (!hasFace)
    return false;
  if (!getFluid(region, pos).isEmpty())
    newState = newState.with("waterlogged", "true");
  setBlock(region, pos, newState, 3);
  return true;
}

struct ChargeCursor {
  Pos pos{};
  int charge = 0;
  int updateDelay = 0;
  int decayDelay = 1;
  bool hasFacings = false;
  std::vector<Direction> facings;
};

int sculkBlockAttemptUseCharge(const ChargeCursor &cursor, WorldGenRegion &region,
                               Pos originPos, RandomSource &rng) {
  const int charge = cursor.charge;
  if (charge == 0 || rng.nextInt(kChargeDecayRate) != 0)
    return charge;
  const Pos chargePos = cursor.pos;
  const int distSq = distSqrInt(chargePos, originPos);
  const bool closeToCatalyst = distSq < (kNoGrowthRadius * kNoGrowthRadius);

  auto canPlaceGrowth = [&]() {
    const BlockState above = getBlock(region, posAbove(chargePos));
    if (above.isAir() ||
        (above.is(Blocks::WATER) && getFluid(region, posAbove(chargePos)).is(Fluids::water()))) {
      int growthCount = 0;
      for (int z = chargePos.z - 4; z <= chargePos.z + 4; ++z) {
        for (int y = chargePos.y; y <= chargePos.y + 2; ++y) {
          for (int x = chargePos.x - 4; x <= chargePos.x + 4; ++x) {
            const BlockState s = getBlock(region, Pos{x, y, z});
            if (s.is(Blocks::SCULK_SENSOR) || s.is(Blocks::SCULK_SHRIEKER))
              ++growthCount;
            if (growthCount > 2)
              return false;
          }
        }
      }
      return true;
    }
    return false;
  };

  if (!closeToCatalyst && canPlaceGrowth()) {
    if (rng.nextInt(kGrowthSpawnCost) < charge) {
      const Pos growthPos = posAbove(chargePos);
      BlockState gs =
          (rng.nextInt(11) == 0)
              ? Blocks::SCULK_SHRIEKER.defaultState().with("can_summon", "true")
              : Blocks::SCULK_SENSOR.defaultState();
      if (gs.hasProperty("waterlogged") && !getFluid(region, growthPos).isEmpty())
        gs = gs.with("waterlogged", "true");
      setBlock(region, growthPos, gs, 3);
    }
    return std::max(0, charge - kGrowthSpawnCost);
  }
  if (rng.nextInt(kAdditionalDecayRate) != 0)
    return charge;

  if (closeToCatalyst)
    return charge - 1;
  const float outerDist =
      std::sqrt(static_cast<float>(distSq)) - static_cast<float>(kNoGrowthRadius);
  const float maxReach = static_cast<float>(24 - kNoGrowthRadius);
  const float distFactor =
      std::min(1.0f, (outerDist * outerDist) / (maxReach * maxReach));
  return charge - std::max(1, static_cast<int>(static_cast<float>(charge) *
                                               distFactor * 0.5f));
}

int sculkVeinAttemptUseCharge(const ChargeCursor &cursor, WorldGenRegion &region,
                              RandomSource &rng, bool spreadVeins) {
  if (spreadVeins) {
    const Pos pos = cursor.pos;
    const BlockState state = getBlock(region, pos);
    std::vector<Direction> dirs(kAllDirections.begin(), kAllDirections.end());
    javaShuffle(dirs, rng);
    for (Direction support : dirs) {
      if (!multiface_detail::multifaceHasFace(state, support))
        continue;
      const Pos supportPos = pos.relative(support);
      const BlockState supportState = getBlock(region, supportPos);
      if (blockHasTag(supportState, "minecraft:sculk_replaceable_world_gen")
              .value_or(false)) {
        const BlockState defaultSculk = Blocks::SCULK.defaultState();
        setBlock(region, supportPos, defaultSculk, 3);
        static const std::vector<multiface_detail::SpreadType> kAllSpread{
            multiface_detail::SpreadType::SamePosition,
            multiface_detail::SpreadType::SamePlane,
            multiface_detail::SpreadType::WrapAround};
        multiface_detail::spreadAll(defaultSculk, region, Blocks::SCULK_VEIN,
                                    true, true, supportPos, kAllSpread, true);
        const Direction skip = opposite(support);
        for (Direction vb : kAllDirections) {
          if (vb == skip)
            continue;
          const Pos vp = supportPos.relative(vb);
          const BlockState pvb = getBlock(region, vp);
          if (pvb.is(Blocks::SCULK_VEIN))
            sculkVeinOnDischarged(region, pvb, vp);
        }
        return cursor.charge - 1;
      }
    }
  }
  return rng.nextInt(kChargeDecayRate) == 0 ? (cursor.charge / 2)
                                            : cursor.charge;
}

std::optional<Pos> getValidMovementPos(const WorldGenRegion &region, Pos pos,
                                       RandomSource &rng) {
  std::vector<Pos> offsets = nonCornerNeighbours();
  javaShuffle(offsets, rng);

  auto isUnobstructed = [&](Pos from, Direction d) {
    const Pos tp = from.relative(d);
    return !isFullFaceSturdy(getBlock(region, tp), opposite(d));
  };
  auto isMoveUnobstructed = [&](Pos from, Pos to) {
    if (manhattanDist(from, to) == 1)
      return true;
    const int dx = to.x - from.x;
    const int dy = to.y - from.y;
    const int dz = to.z - from.z;
    const Direction dX = dx < 0 ? Direction::West : Direction::East;
    const Direction dY = dy < 0 ? Direction::Down : Direction::Up;
    const Direction dZ = dz < 0 ? Direction::North : Direction::South;
    if (dx == 0)
      return isUnobstructed(from, dY) || isUnobstructed(from, dZ);
    if (dy == 0)
      return isUnobstructed(from, dX) || isUnobstructed(from, dZ);
    return isUnobstructed(from, dX) || isUnobstructed(from, dY);
  };

  Pos sculkPosition = pos;
  for (const Pos off : offsets) {
    const Pos neighbour{pos.x + off.x, pos.y + off.y, pos.z + off.z};
    const BlockState tr = getBlock(region, neighbour);
    if ((tr.is(Blocks::SCULK) || tr.is(Blocks::SCULK_VEIN)) &&
        isMoveUnobstructed(pos, neighbour)) {
      sculkPosition = neighbour;
      if (sculkVeinHasSubstrateAccess(region, tr, neighbour))
        break;
    }
  }
  return (sculkPosition == pos) ? std::nullopt
                                : std::optional<Pos>(sculkPosition);
}

void updateCursor(ChargeCursor &cursor, WorldGenRegion &region, Pos originPos,
                  RandomSource &rng, bool spreadVeins) {
  if (cursor.charge <= 0)
    return;
  if (cursor.updateDelay > 0) {
    --cursor.updateDelay;
    return;
  }

  BlockState currentState = getBlock(region, cursor.pos);
  auto isSculk = [&]() { return currentState.is(Blocks::SCULK); };
  auto isVein = [&]() { return currentState.is(Blocks::SCULK_VEIN); };

  if (spreadVeins) {
    bool spreadOk = false;
    if (isSculk() || isVein()) {
      static const std::vector<multiface_detail::SpreadType> kAllSpread{
          multiface_detail::SpreadType::SamePosition,
          multiface_detail::SpreadType::SamePlane,
          multiface_detail::SpreadType::WrapAround};
      spreadOk =
          multiface_detail::spreadAll(currentState, region, Blocks::SCULK_VEIN,
                                      true, true, cursor.pos, kAllSpread, true) > 0;
    } else {
      if (!cursor.hasFacings) {
        static const std::vector<multiface_detail::SpreadType> kSamePos{
            multiface_detail::SpreadType::SamePosition};
        spreadOk = multiface_detail::spreadAll(
                       currentState, region, Blocks::SCULK_VEIN, true, true,
                       cursor.pos, kSamePos, true) > 0;
      } else if (!cursor.facings.empty()) {
        if (currentState.isAir() ||
            getFluid(region, cursor.pos).is(Fluids::water())) {
          spreadOk = sculkVeinRegrow(region, cursor.pos, cursor.facings);
        }
      }
    }
    if (spreadOk && !isSculk()) {
      currentState = getBlock(region, cursor.pos);
    }
  }

  if (isSculk()) {
    cursor.charge = sculkBlockAttemptUseCharge(cursor, region, originPos, rng);
  } else if (isVein()) {
    cursor.charge =
        sculkVeinAttemptUseCharge(cursor, region, rng, spreadVeins);
  } else {
    cursor.charge = (cursor.decayDelay > 0) ? cursor.charge : 0;
  }

  if (cursor.charge <= 0) {
    if (isVein())
      sculkVeinOnDischarged(region, currentState, cursor.pos);
    return;
  }

  if (auto transferPos = getValidMovementPos(region, cursor.pos, rng)) {
    if (isVein())
      sculkVeinOnDischarged(region, currentState, cursor.pos);
    cursor.pos = *transferPos;
    const int dx = cursor.pos.x - originPos.x;
    const int dz = cursor.pos.z - originPos.z;
    if (dx * dx + dz * dz >= 15 * 15) {
      cursor.charge = 0;
      return;
    }
    currentState = getBlock(region, cursor.pos);
  }

  if (isSculk() || isVein()) {
    cursor.hasFacings = true;
    cursor.facings.clear();
    if (isVein()) {
      for (Direction d : kAllDirections) {
        if (multiface_detail::multifaceHasFace(currentState, d))
          cursor.facings.push_back(d);
      }
    }
  }
  cursor.decayDelay =
      (isSculk() || isVein()) ? 1 : std::max(cursor.decayDelay - 1, 0);
  cursor.updateDelay = 1;
}

} // namespace sculk_detail

bool placeSculkPatch(const ConfiguredFeature &cf, WorldGenRegion &region,
                     RandomSource &rng, Pos origin) {
  auto canSpreadFrom = [&]() {
    const BlockState start = getBlock(region, origin);
    if (start.is(Blocks::SCULK) || start.is(Blocks::SCULK_VEIN))
      return true;
    if (!start.isAir() &&
        !(start.is(Blocks::WATER) && getFluid(region, origin).isSource())) {
      return false;
    }
    for (Direction d : kAllDirections) {
      if (isCollisionFullBlock(getBlock(region, origin.relative(d))))
        return true;
    }
    return false;
  };

  if (!canSpreadFrom())
    return false;

  const auto &cfg = cf.sculkPatch;
  std::vector<sculk_detail::ChargeCursor> cursors;
  const int totalRounds = cfg.spreadRounds + cfg.growthRounds;
  for (int round = 0; round < totalRounds; ++round) {
    for (int i = 0; i < cfg.chargeCount; ++i) {
      int rem = cfg.amountPerCharge;
      while (rem > 0) {
        const int cur = std::min(rem, 1000);
        if (static_cast<int>(cursors.size()) < sculk_detail::kMaxCursors) {
          sculk_detail::ChargeCursor c;
          c.pos = origin;
          c.charge = cur;
          cursors.push_back(std::move(c));
        }
        rem -= cur;
      }
    }

    const bool spreadVeins = round < cfg.spreadRounds;
    for (int attempt = 0; attempt < cfg.spreadAttempts; ++attempt) {
      if (cursors.empty())
        break;
      std::vector<sculk_detail::ChargeCursor> nextCursors;
      for (auto &c : cursors) {
        const int chess = std::max({std::abs(c.pos.x - origin.x),
                                    std::abs(c.pos.y - origin.y),
                                    std::abs(c.pos.z - origin.z)});
        if (chess > 1024)
          continue;
        sculk_detail::updateCursor(c, region, origin, rng, spreadVeins);
        if (c.charge > 0)
          nextCursors.push_back(std::move(c));
      }
      cursors = std::move(nextCursors);
    }
    cursors.clear();
  }

  if (rng.nextFloat() <= cfg.catalystChance &&
      isCollisionFullBlock(getBlock(region, posBelow(origin)))) {
    setBlock(region, origin, Blocks::SCULK_CATALYST.defaultState(), 3);
  }

  const int extraGrowths = cfg.extraRareGrowths.sample(rng);
  for (int i = 0; i < extraGrowths; ++i) {
    const int ox = rng.nextInt(5) - 2;
    const int oz = rng.nextInt(5) - 2;
    const Pos cand{origin.x + ox, origin.y, origin.z + oz};
    if (getBlock(region, cand).isAir() &&
        isFullFaceSturdy(getBlock(region, posBelow(cand)), Direction::Up)) {
      setBlock(
          region, cand,
          Blocks::SCULK_SHRIEKER.defaultState().with("can_summon", "true"), 3);
    }
  }
  return true;
}

// ============================================================================
// 23. SeaPickleFeature
// ============================================================================

bool placeSeaPickle(const ConfiguredFeature &cf, WorldGenRegion &region,
                    RandomSource &rng, Pos origin) {
  int placed = 0;
  const int count = cf.countProvider.sample(rng);
  for (int i = 0; i < count; ++i) {
    const int x1 = rng.nextInt(8);
    const int x2 = rng.nextInt(8);
    const int z1 = rng.nextInt(8);
    const int z2 = rng.nextInt(8);
    const int px = origin.x + (x1 - x2);
    const int pz = origin.z + (z1 - z2);
    const int y = regionGetHeight(region, HeightmapType::OceanFloor, px, pz);
    const Pos picklePos{px, y, pz};
    const int pickles = rng.nextInt(4) + 1;
    const BlockState pickleState =
        Blocks::SEA_PICKLE.defaultState().with("pickles",
                                               std::to_string(pickles));
    const BlockState below = getBlock(region, posBelow(picklePos));
    const auto &bcs = collisionShape(below);
    const bool hasUpFace = std::any_of(
        bcs.begin(), bcs.end(), [](const auto &box) { return box.y1 >= 16.0; });
    const bool mayPlace = hasUpFace || isFullFaceSturdy(below, Direction::Up);
    if (getBlock(region, picklePos).is(Blocks::WATER) && mayPlace) {
      setBlock(region, picklePos, pickleState, 2);
      ++placed;
    }
  }
  return placed > 0;
}

// ============================================================================
// 24. SpikeFeature
// ============================================================================

bool placeSpike(const ConfiguredFeature &cf, WorldGenRegion &region,
                RandomSource &rng, Pos origin) {
  while (getBlock(region, origin).isAir() && origin.y > region.minY() + 2) {
    origin.y -= 1;
  }
  const auto &cfg = cf.spike;
  if (!cfg.canPlaceOn.test(region, origin))
    return false;

  origin.y += rng.nextInt(4);
  const int height = rng.nextInt(4) + 7;
  const int width = height / 4 + rng.nextInt(2);
  if (width > 1 && rng.nextInt(60) == 0)
    origin.y += 10 + rng.nextInt(30);

  for (int yOff = 0; yOff < height; ++yOff) {
    const float scale =
        (1.0f - static_cast<float>(yOff) / static_cast<float>(height)) *
        static_cast<float>(width);
    const int newWidth = ceilFloat(scale);
    for (int xo = -newWidth; xo <= newWidth; ++xo) {
      const float dx = static_cast<float>(std::abs(xo)) - 0.25f;
      for (int zo = -newWidth; zo <= newWidth; ++zo) {
        const float dz = static_cast<float>(std::abs(zo)) - 0.25f;
        if (((xo == 0 && zo == 0) || !(dx * dx + dz * dz > scale * scale)) &&
            ((xo != -newWidth && xo != newWidth && zo != -newWidth &&
              zo != newWidth) ||
             !(rng.nextFloat() > 0.75f))) {
          const Pos posOff{origin.x + xo, origin.y + yOff, origin.z + zo};
          const BlockState s = getBlock(region, posOff);
          if (s.isAir() || cfg.canReplace.test(region, posOff))
            setBlock(region, posOff, cfg.state, 2);

          if (yOff != 0 && newWidth > 1) {
            const Pos negOff{origin.x + xo, origin.y - yOff, origin.z + zo};
            const BlockState ns = getBlock(region, negOff);
            if (ns.isAir() || cfg.canReplace.test(region, negOff))
              setBlock(region, negOff, cfg.state, 2);
          }
        }
      }
    }
  }

  int pillarWidth = std::clamp(width - 1, 0, 1);
  for (int xo = -pillarWidth; xo <= pillarWidth; ++xo) {
    for (int zo = -pillarWidth; zo <= pillarWidth; ++zo) {
      Pos cursor{origin.x + xo, origin.y - 1, origin.z + zo};
      int runLength = 50;
      if (std::abs(xo) == 1 && std::abs(zo) == 1)
        runLength = rng.nextInt(5);
      while (cursor.y > 50) {
        const BlockState sx = getBlock(region, cursor);
        if (!sx.isAir() && !cfg.canReplace.test(region, cursor) &&
            !(sx == cfg.state)) {
          break;
        }
        setBlock(region, cursor, cfg.state, 2);
        cursor.y -= 1;
        if (--runLength <= 0) {
          cursor.y -= rng.nextInt(5) + 1;
          runLength = rng.nextInt(5);
        }
      }
    }
  }
  return true;
}

// ============================================================================
// 25. TwistingVinesFeature & 26. WeepingVinesFeature
// ============================================================================

bool placeTwistingVines(const ConfiguredFeature &cf, WorldGenRegion &region,
                        RandomSource &rng, Pos origin) {
  auto isInvalidLoc = [&](Pos p) {
    if (!getBlock(region, p).isAir())
      return true;
    const BlockState below = getBlock(region, posBelow(p));
    return !below.is(Blocks::NETHERRACK) && !below.is(Blocks::WARPED_NYLIUM) &&
           !below.is(Blocks::WARPED_WART_BLOCK);
  };
  if (isInvalidLoc(origin))
    return false;

  const int sw = cf.twistingVines.spreadWidth;
  const int sh = cf.twistingVines.spreadHeight;
  const int mh = cf.twistingVines.maxHeight;
  for (int i = 0; i < sw * sw; ++i) {
    const int dx = worldgen_math::randomBetweenInclusive(rng, -sw, sw);
    const int dy = worldgen_math::randomBetweenInclusive(rng, -sh, sh);
    const int dz = worldgen_math::randomBetweenInclusive(rng, -sw, sw);
    Pos p{origin.x + dx, origin.y + dy, origin.z + dz};

    bool foundGround = false;
    while (true) {
      p.y -= 1;
      if (isOutsideBuildHeight(region, p.y)) {
        foundGround = false;
        break;
      }
      if (!getBlock(region, p).isAir()) {
        p.y += 1;
        foundGround = true;
        break;
      }
    }
    if (!foundGround || isInvalidLoc(p))
      continue;

    int vineHeight = worldgen_math::randomBetweenInclusive(rng, 1, mh);
    if (rng.nextInt(6) == 0)
      vineHeight *= 2;
    if (rng.nextInt(5) == 0)
      vineHeight = 1;

    for (int h = 1; h <= vineHeight; ++h) {
      if (getBlock(region, p).isAir()) {
        if (h == vineHeight || !getBlock(region, posAbove(p)).isAir()) {
          const int age = worldgen_math::randomBetweenInclusive(rng, 17, 25);
          setBlock(region, p,
                   Blocks::TWISTING_VINES.defaultState().with(
                       "age", std::to_string(age)),
                   2);
          break;
        }
        setBlock(region, p, Blocks::TWISTING_VINES_PLANT.defaultState(), 2);
      }
      p.y += 1;
    }
  }
  return true;
}

bool placeWeepingVines(WorldGenRegion &region, RandomSource &rng, Pos origin) {
  if (!getBlock(region, origin).isAir())
    return false;
  const BlockState above = getBlock(region, posAbove(origin));
  if (!above.is(Blocks::NETHERRACK) && !above.is(Blocks::NETHER_WART_BLOCK))
    return false;

  // placeRoofNetherWart
  setBlock(region, origin, Blocks::NETHER_WART_BLOCK.defaultState(), 2);
  for (int i = 0; i < 200; ++i) {
    const int dx1 = rng.nextInt(6);
    const int dx2 = rng.nextInt(6);
    const int dy1 = rng.nextInt(2);
    const int dy2 = rng.nextInt(5);
    const int dz1 = rng.nextInt(6);
    const int dz2 = rng.nextInt(6);
    const Pos pp{origin.x + dx1 - dx2, origin.y + dy1 - dy2,
                 origin.z + dz1 - dz2};
    if (getBlock(region, pp).isAir()) {
      int neighbours = 0;
      for (Direction d : kAllDirections) {
        const BlockState ns = getBlock(region, pp.relative(d));
        if (ns.is(Blocks::NETHERRACK) || ns.is(Blocks::NETHER_WART_BLOCK))
          ++neighbours;
        if (neighbours > 1)
          break;
      }
      if (neighbours == 1)
        setBlock(region, pp, Blocks::NETHER_WART_BLOCK.defaultState(), 2);
    }
  }

  // placeRoofWeepingVines
  for (int i = 0; i < 100; ++i) {
    const int dx1 = rng.nextInt(8);
    const int dx2 = rng.nextInt(8);
    const int dy1 = rng.nextInt(2);
    const int dy2 = rng.nextInt(7);
    const int dz1 = rng.nextInt(8);
    const int dz2 = rng.nextInt(8);
    Pos pp{origin.x + dx1 - dx2, origin.y + dy1 - dy2, origin.z + dz1 - dz2};
    if (getBlock(region, pp).isAir()) {
      const BlockState pa = getBlock(region, posAbove(pp));
      if (pa.is(Blocks::NETHERRACK) || pa.is(Blocks::NETHER_WART_BLOCK)) {
        int vineHeight = worldgen_math::randomBetweenInclusive(rng, 1, 8);
        if (rng.nextInt(6) == 0)
          vineHeight *= 2;
        if (rng.nextInt(5) == 0)
          vineHeight = 1;
        for (int h = 0; h <= vineHeight; ++h) {
          if (getBlock(region, pp).isAir()) {
            if (h == vineHeight || !getBlock(region, posBelow(pp)).isAir()) {
              const int age =
                  worldgen_math::randomBetweenInclusive(rng, 17, 25);
              setBlock(region, pp,
                       Blocks::WEEPING_VINES.defaultState().with(
                           "age", std::to_string(age)),
                       2);
              break;
            }
            setBlock(region, pp, Blocks::WEEPING_VINES_PLANT.defaultState(), 2);
          }
          pp.y -= 1;
        }
      }
    }
  }
  return true;
}

// ============================================================================
// 27. UnderwaterMagmaFeature
// ============================================================================

bool placeUnderwaterMagma(const ConfiguredFeature &cf, WorldGenRegion &region,
                          RandomSource &rng, Pos origin) {
  const auto &cfg = cf.underwaterMagma;
  auto isWater = [](const BlockState &s) noexcept {
    return s.is(Blocks::WATER);
  };
  auto notWater = [](const BlockState &s) noexcept {
    return !s.is(Blocks::WATER);
  };
  auto col = scanColumn(region, origin, cfg.floorSearchRange, isWater, notWater);
  if (!col || !col->floor)
    return false;

  const Pos floorPos{origin.x, *col->floor, origin.z};
  const int r = cfg.placementRadiusAroundFloor;
  auto isVisibleFromOutside = [&](Pos p, Direction coveredDir) {
    return !isFullFaceSturdy(getBlock(region, p), coveredDir);
  };
  auto isValid = [&](Pos p) {
    const BlockState s = getBlock(region, p);
    if (s.is(Blocks::WATER) || s.isAir())
      return false;
    if (isVisibleFromOutside(posBelow(p), Direction::Up))
      return false;
    for (Direction d : kHorizDirections) {
      if (isVisibleFromOutside(p.relative(d), opposite(d)))
        return false;
    }
    return true;
  };

  int placed = 0;
  forEachBetweenClosed(floorPos.x - r, floorPos.y - r, floorPos.z - r,
                       floorPos.x + r, floorPos.y + r, floorPos.z + r,
                       [&](Pos p) {
                         if (rng.nextFloat() <
                                 cfg.placementProbabilityPerValidPosition &&
                             isValid(p)) {
                           setBlock(region, p,
                                    Blocks::MAGMA_BLOCK.defaultState(), 2);
                           ++placed;
                         }
                       });
  return placed > 0;
}

// ============================================================================
// 28. VinesFeature
// ============================================================================

bool placeVines(WorldGenRegion &region, Pos origin) {
  if (!getBlock(region, origin).isAir())
    return false;
  for (Direction d : kAllDirections) {
    if (d != Direction::Down &&
        multiface_detail::multifaceCanAttachTo(region, origin.relative(d), d)) {
      setBlock(region, origin,
               Blocks::VINE.defaultState().with(dirNameLower(d), "true"), 2);
      return true;
    }
  }
  return false;
}

} // namespace

bool placeExtraConfiguredFeature(const ConfiguredFeature &cf,
                                 WorldGenRegion &region,
                                 const NoiseBasedChunkGenerator &generator,
                                 std::int64_t worldSeed, RandomSource &rng,
                                 Pos origin) {
  switch (cf.kind) {
  case ConfiguredFeature::Kind::Seagrass:
    return placeSeagrass(cf, region, rng, origin);
  case ConfiguredFeature::Kind::Bamboo:
    return placeBamboo(cf, region, rng, origin);
  case ConfiguredFeature::Kind::BasaltColumns:
    return placeBasaltColumns(cf, region, generator, rng, origin);
  case ConfiguredFeature::Kind::BasaltPillar:
    return placeBasaltPillar(region, rng, origin);
  case ConfiguredFeature::Kind::BlockBlob:
    return placeBlockBlob(cf, region, rng, origin);
  case ConfiguredFeature::Kind::BlueIce:
    return placeBlueIce(region, generator, rng, origin);
  case ConfiguredFeature::Kind::BonusChest:
    return placeBonusChest(region, rng, origin);
  case ConfiguredFeature::Kind::ChorusPlant:
    return placeChorusPlant(region, rng, origin);
  case ConfiguredFeature::Kind::DeltaFeature:
    return placeDeltaFeature(cf, region, rng, origin);
  case ConfiguredFeature::Kind::DesertWell:
    return placeDesertWell(region, rng, origin);
  case ConfiguredFeature::Kind::DripstoneCluster:
    return placeDripstoneCluster(cf, region, rng, origin);
  case ConfiguredFeature::Kind::EndGateway:
    return placeEndGateway(cf, region, origin);
  case ConfiguredFeature::Kind::EndPlatform:
    return placeEndPlatform(region, origin);
  case ConfiguredFeature::Kind::Fossil:
    return placeFossil(cf, region, generator, rng, origin);
  case ConfiguredFeature::Kind::Geode:
    return placeGeode(cf, region, rng, origin);
  case ConfiguredFeature::Kind::Iceberg:
    return placeIceberg(cf, region, generator, rng, origin);
  case ConfiguredFeature::Kind::Kelp:
    return placeKelp(region, rng, origin);
  case ConfiguredFeature::Kind::LargeDripstone:
    return placeLargeDripstone(cf, region, rng, origin);
  case ConfiguredFeature::Kind::MonsterRoom:
    return placeMonsterRoom(region, rng, origin);
  case ConfiguredFeature::Kind::MultifaceGrowth:
    return placeMultifaceGrowth(cf, region, rng, origin);
  case ConfiguredFeature::Kind::RootSystem:
    return placeRootSystem(cf, region, generator, worldSeed, rng, origin);
  case ConfiguredFeature::Kind::SculkPatch:
    return placeSculkPatch(cf, region, rng, origin);
  case ConfiguredFeature::Kind::SeaPickle:
    return placeSeaPickle(cf, region, rng, origin);
  case ConfiguredFeature::Kind::Spike:
    return placeSpike(cf, region, rng, origin);
  case ConfiguredFeature::Kind::TwistingVines:
    return placeTwistingVines(cf, region, rng, origin);
  case ConfiguredFeature::Kind::UnderwaterMagma:
    return placeUnderwaterMagma(cf, region, rng, origin);
  case ConfiguredFeature::Kind::Vines:
    return placeVines(region, origin);
  case ConfiguredFeature::Kind::VoidStartPlatform:
    return placeVoidStartPlatform(region, origin);
  case ConfiguredFeature::Kind::WeepingVines:
    return placeWeepingVines(region, rng, origin);
  default:
    return false;
  }
}

} // namespace redstone::worldgen
