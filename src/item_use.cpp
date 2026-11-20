#include "redstone/item_use.hpp"
#include "redstone/block_tags.hpp"
#include "redstone/entity.hpp"
#include "redstone/fluid_flow.hpp"
#include "redstone/item_tags.hpp"
#include "redstone/random_tick.hpp"
#include "redstone/shapes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace redstone {
namespace {

BlockState copyMatchingProperties(const BlockState &from, const Block &to) {
  BlockState dst = to.defaultState();
  const auto props = from.properties();
  for (const auto &[k, v] : props) {
    if (dst.hasProperty(k))
      dst = dst.with(k, v);
  }
  return dst;
}

void consumeOrGiveRemainder(World &world, ItemStack &stack, ServerPlayer *player,
                            bool creative, std::string_view remainderId) {
  if (creative)
    return;
  --stack.count;
  if (stack.count <= 0) {
    if (!remainderId.empty())
      stack = itemStack(std::string(remainderId), 1);
    else
      stack = {};
    return;
  }
  if (!remainderId.empty() && player) {
    ItemStack rem = itemStack(std::string(remainderId), 1);
    const int accepted = player->onItemPickup ? player->onItemPickup(rem) : 0;
    if (accepted <= 0)
      world.spawnItemAt(player->position, rem.id, rem.count);
  }
}

void shrinkOne(ItemStack &stack, bool creative) {
  if (creative || stack.empty())
    return;
  --stack.count;
  if (stack.count <= 0)
    stack = {};
}

bool hurtItemOne(ItemStack &stack, bool creative) {
  if (creative || stack.empty())
    return false;
  return stack.hurtAndBreak(1, false);
}

bool canLightFlameTarget(const BlockState &state) {
  if (!(state.is(Blocks::CAMPFIRE) || state.is(Blocks::SOUL_CAMPFIRE) ||
        state.block().isCandle() ||
        state.name().ends_with("_candle_cake")))
    return false;
  if (state.getView("waterlogged", "false") == "true")
    return false;
  return state.getView("lit", "false") == "false";
}

bool canSurviveFireAt(World &world, Pos p, bool soul) {
  if (soul) {
    return world.hasBlockTag(world.getBlock(p.relative(Direction::Down)),
                             "minecraft:soul_fire_base_blocks")
        .value_or(false);
  }
  const Pos below = p.relative(Direction::Down);
  const BlockState belowState = world.getBlock(below);
  if (faceSturdy(world, below, belowState, Direction::Up))
    return true;
  for (Direction d : {Direction::Down, Direction::Up, Direction::North,
                      Direction::South, Direction::West, Direction::East}) {
    if (world.getBlock(p.relative(d)).block().metadata().ignitedByLava)
      return true;
  }
  return false;
}

float wrapDegreesF(float deg) {
  float v = std::fmod(deg, 360.0f);
  if (v >= 180.0f)
    v -= 360.0f;
  if (v < -180.0f)
    v += 360.0f;
  return v;
}

struct RayHit {
  bool hit = false;
  Pos pos{};
  Direction face = Direction::Up;
  Vec3 location{};
};

void clipAabbRay(double minX, double minY, double minZ, double maxX,
                 double maxY, double maxZ, Vec3 from, double dx, double dy,
                 double dz, double &bestScale, std::optional<Direction> &dir) {
  auto clipPlane = [&](double da, double db, double dc, double point,
                       double minB, double maxB, double minC, double maxC,
                       Direction hitDir, double fromA, double fromB,
                       double fromC) {
    const double s = (point - fromA) / da;
    const double pb = fromB + s * db;
    const double pc = fromC + s * dc;
    if (0.0 < s && s < bestScale && minB - 1.0e-7 < pb &&
        pb < maxB + 1.0e-7 && minC - 1.0e-7 < pc && pc < maxC + 1.0e-7) {
      bestScale = s;
      dir = hitDir;
    }
  };
  if (dx > 1.0e-7)
    clipPlane(dx, dy, dz, minX, minY, maxY, minZ, maxZ, Direction::West, from.x,
              from.y, from.z);
  else if (dx < -1.0e-7)
    clipPlane(dx, dy, dz, maxX, minY, maxY, minZ, maxZ, Direction::East, from.x,
              from.y, from.z);
  if (dy > 1.0e-7)
    clipPlane(dy, dz, dx, minY, minZ, maxZ, minX, maxX, Direction::Down, from.y,
              from.z, from.x);
  else if (dy < -1.0e-7)
    clipPlane(dy, dz, dx, maxY, minZ, maxZ, minX, maxX, Direction::Up, from.y,
              from.z, from.x);
  if (dz > 1.0e-7)
    clipPlane(dz, dx, dy, minZ, minX, maxX, minY, maxY, Direction::North,
              from.z, from.x, from.y);
  else if (dz < -1.0e-7)
    clipPlane(dz, dx, dy, maxZ, minX, maxX, minY, maxY, Direction::South,
              from.z, from.x, from.y);
}

enum class FluidClipMode { None, SourceOnly, Any };

RayHit povClip(World &world, Vec3 from, float yaw, float pitch, double reach,
               FluidClipMode fluidMode) {
  const double yawRad = -yaw * (std::numbers::pi / 180.0);
  const double pitchRad = pitch * (std::numbers::pi / 180.0);
  const double cosP = std::cos(pitchRad);
  const Vec3 view{std::sin(yawRad) * cosP, -std::sin(pitchRad),
                  std::cos(yawRad) * cosP};
  const Vec3 to = from + view * reach;
  const double totalDx = to.x - from.x;
  const double totalDy = to.y - from.y;
  const double totalDz = to.z - from.z;
  if (totalDx * totalDx + totalDy * totalDy + totalDz * totalDz < 1.0e-12)
    return {};

  const auto testCell = [&](Pos pos) -> RayHit {
    if (!world.isLoaded(pos))
      return {};
    const BlockState state = world.getBlock(pos);
    double bestScale = 1.0;
    std::optional<Direction> bestDir;
    for (const ShapeBox &b : outlineShape(state)) {
      clipAabbRay(pos.x + b.x0 / 16.0, pos.y + b.y0 / 16.0, pos.z + b.z0 / 16.0,
                  pos.x + b.x1 / 16.0, pos.y + b.y1 / 16.0, pos.z + b.z1 / 16.0,
                  from, totalDx, totalDy, totalDz, bestScale, bestDir);
    }
    if (fluidMode != FluidClipMode::None) {
      const FluidState fluid = fluidStateOf(state);
      const bool match = !fluid.isEmpty() &&
                         (fluidMode == FluidClipMode::Any || fluid.isSource());
      if (match) {
        const float height = fluidHeight(world, pos, fluid);
        clipAabbRay(pos.x, pos.y, pos.z, pos.x + 1.0, pos.y + height,
                    pos.z + 1.0, from, totalDx, totalDy, totalDz, bestScale,
                    bestDir);
      }
    }
    if (bestDir) {
      return {true, pos, *bestDir,
              from + Vec3{totalDx * bestScale, totalDy * bestScale,
                          totalDz * bestScale}};
    }
    return {};
  };

  const double toX = to.x + 1.0e-7 * totalDx;
  const double toY = to.y + 1.0e-7 * totalDy;
  const double toZ = to.z + 1.0e-7 * totalDz;
  const double fromX = from.x - 1.0e-7 * totalDx;
  const double fromY = from.y - 1.0e-7 * totalDy;
  const double fromZ = from.z - 1.0e-7 * totalDz;
  int cx = static_cast<int>(std::floor(fromX));
  int cy = static_cast<int>(std::floor(fromY));
  int cz = static_cast<int>(std::floor(fromZ));
  if (RayHit hit = testCell({cx, cy, cz}); hit.hit)
    return hit;

  const double dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
  const auto sign = [](double v) { return (v > 0.0) - (v < 0.0); };
  const auto frac = [](double v) { return v - std::floor(v); };
  const int sx = sign(dx), sy = sign(dy), sz = sign(dz);
  const double tDeltaX = sx == 0 ? 1.0e30 : sx / dx;
  const double tDeltaY = sy == 0 ? 1.0e30 : sy / dy;
  const double tDeltaZ = sz == 0 ? 1.0e30 : sz / dz;
  double tX = tDeltaX * (sx > 0 ? 1.0 - frac(fromX) : frac(fromX));
  double tY = tDeltaY * (sy > 0 ? 1.0 - frac(fromY) : frac(fromY));
  double tZ = tDeltaZ * (sz > 0 ? 1.0 - frac(fromZ) : frac(fromZ));

  for (int step = 0; step < 64 && (tX <= 1.0 || tY <= 1.0 || tZ <= 1.0);
       ++step) {
    if (tX < tY) {
      if (tX < tZ) {
        cx += sx;
        tX += tDeltaX;
      } else {
        cz += sz;
        tZ += tDeltaZ;
      }
    } else if (tY < tZ) {
      cy += sy;
      tY += tDeltaY;
    } else {
      cz += sz;
      tZ += tDeltaZ;
    }
    if (RayHit hit = testCell({cx, cy, cz}); hit.hit)
      return hit;
  }
  return {};
}

const phmap::flat_hash_map<std::string_view, const Block *, AnkerlStringHash> &
axeStrippablesTable() {
  static const phmap::flat_hash_map<std::string_view, const Block *,
                                    AnkerlStringHash>
      m = {
      {"minecraft:oak_wood", &Blocks::STRIPPED_OAK_WOOD},
      {"minecraft:oak_log", &Blocks::STRIPPED_OAK_LOG},
      {"minecraft:dark_oak_wood", &Blocks::STRIPPED_DARK_OAK_WOOD},
      {"minecraft:dark_oak_log", &Blocks::STRIPPED_DARK_OAK_LOG},
      {"minecraft:pale_oak_wood", &Blocks::STRIPPED_PALE_OAK_WOOD},
      {"minecraft:pale_oak_log", &Blocks::STRIPPED_PALE_OAK_LOG},
      {"minecraft:acacia_wood", &Blocks::STRIPPED_ACACIA_WOOD},
      {"minecraft:acacia_log", &Blocks::STRIPPED_ACACIA_LOG},
      {"minecraft:cherry_wood", &Blocks::STRIPPED_CHERRY_WOOD},
      {"minecraft:cherry_log", &Blocks::STRIPPED_CHERRY_LOG},
      {"minecraft:birch_wood", &Blocks::STRIPPED_BIRCH_WOOD},
      {"minecraft:birch_log", &Blocks::STRIPPED_BIRCH_LOG},
      {"minecraft:jungle_wood", &Blocks::STRIPPED_JUNGLE_WOOD},
      {"minecraft:jungle_log", &Blocks::STRIPPED_JUNGLE_LOG},
      {"minecraft:spruce_wood", &Blocks::STRIPPED_SPRUCE_WOOD},
      {"minecraft:spruce_log", &Blocks::STRIPPED_SPRUCE_LOG},
      {"minecraft:warped_stem", &Blocks::STRIPPED_WARPED_STEM},
      {"minecraft:warped_hyphae", &Blocks::STRIPPED_WARPED_HYPHAE},
      {"minecraft:crimson_stem", &Blocks::STRIPPED_CRIMSON_STEM},
      {"minecraft:crimson_hyphae", &Blocks::STRIPPED_CRIMSON_HYPHAE},
      {"minecraft:mangrove_wood", &Blocks::STRIPPED_MANGROVE_WOOD},
      {"minecraft:mangrove_log", &Blocks::STRIPPED_MANGROVE_LOG},
      {"minecraft:bamboo_block", &Blocks::STRIPPED_BAMBOO_BLOCK},
  };
  return m;
}

const std::vector<std::pair<const Block *, const Block *>> &
copperWaxPairs() {
  static const std::vector<std::pair<const Block *, const Block *>> pairs = {
      {&Blocks::COPPER_BLOCK, &Blocks::WAXED_COPPER_BLOCK},
      {&Blocks::EXPOSED_COPPER, &Blocks::WAXED_EXPOSED_COPPER},
      {&Blocks::WEATHERED_COPPER, &Blocks::WAXED_WEATHERED_COPPER},
      {&Blocks::OXIDIZED_COPPER, &Blocks::WAXED_OXIDIZED_COPPER},
      {&Blocks::CUT_COPPER, &Blocks::WAXED_CUT_COPPER},
      {&Blocks::EXPOSED_CUT_COPPER, &Blocks::WAXED_EXPOSED_CUT_COPPER},
      {&Blocks::WEATHERED_CUT_COPPER, &Blocks::WAXED_WEATHERED_CUT_COPPER},
      {&Blocks::OXIDIZED_CUT_COPPER, &Blocks::WAXED_OXIDIZED_CUT_COPPER},
      {&Blocks::CUT_COPPER_SLAB, &Blocks::WAXED_CUT_COPPER_SLAB},
      {&Blocks::EXPOSED_CUT_COPPER_SLAB, &Blocks::WAXED_EXPOSED_CUT_COPPER_SLAB},
      {&Blocks::WEATHERED_CUT_COPPER_SLAB,
       &Blocks::WAXED_WEATHERED_CUT_COPPER_SLAB},
      {&Blocks::OXIDIZED_CUT_COPPER_SLAB,
       &Blocks::WAXED_OXIDIZED_CUT_COPPER_SLAB},
      {&Blocks::CUT_COPPER_STAIRS, &Blocks::WAXED_CUT_COPPER_STAIRS},
      {&Blocks::EXPOSED_CUT_COPPER_STAIRS,
       &Blocks::WAXED_EXPOSED_CUT_COPPER_STAIRS},
      {&Blocks::WEATHERED_CUT_COPPER_STAIRS,
       &Blocks::WAXED_WEATHERED_CUT_COPPER_STAIRS},
      {&Blocks::OXIDIZED_CUT_COPPER_STAIRS,
       &Blocks::WAXED_OXIDIZED_CUT_COPPER_STAIRS},
      {&Blocks::CHISELED_COPPER, &Blocks::WAXED_CHISELED_COPPER},
      {&Blocks::EXPOSED_CHISELED_COPPER, &Blocks::WAXED_EXPOSED_CHISELED_COPPER},
      {&Blocks::WEATHERED_CHISELED_COPPER,
       &Blocks::WAXED_WEATHERED_CHISELED_COPPER},
      {&Blocks::OXIDIZED_CHISELED_COPPER,
       &Blocks::WAXED_OXIDIZED_CHISELED_COPPER},
      {&Blocks::COPPER_DOOR, &Blocks::WAXED_COPPER_DOOR},
      {&Blocks::EXPOSED_COPPER_DOOR, &Blocks::WAXED_EXPOSED_COPPER_DOOR},
      {&Blocks::WEATHERED_COPPER_DOOR, &Blocks::WAXED_WEATHERED_COPPER_DOOR},
      {&Blocks::OXIDIZED_COPPER_DOOR, &Blocks::WAXED_OXIDIZED_COPPER_DOOR},
      {&Blocks::COPPER_TRAPDOOR, &Blocks::WAXED_COPPER_TRAPDOOR},
      {&Blocks::EXPOSED_COPPER_TRAPDOOR, &Blocks::WAXED_EXPOSED_COPPER_TRAPDOOR},
      {&Blocks::WEATHERED_COPPER_TRAPDOOR,
       &Blocks::WAXED_WEATHERED_COPPER_TRAPDOOR},
      {&Blocks::OXIDIZED_COPPER_TRAPDOOR,
       &Blocks::WAXED_OXIDIZED_COPPER_TRAPDOOR},
      {&Blocks::COPPER_BARS, &Blocks::WAXED_COPPER_BARS},
      {&Blocks::EXPOSED_COPPER_BARS, &Blocks::WAXED_EXPOSED_COPPER_BARS},
      {&Blocks::WEATHERED_COPPER_BARS, &Blocks::WAXED_WEATHERED_COPPER_BARS},
      {&Blocks::OXIDIZED_COPPER_BARS, &Blocks::WAXED_OXIDIZED_COPPER_BARS},
      {&Blocks::COPPER_GRATE, &Blocks::WAXED_COPPER_GRATE},
      {&Blocks::EXPOSED_COPPER_GRATE, &Blocks::WAXED_EXPOSED_COPPER_GRATE},
      {&Blocks::WEATHERED_COPPER_GRATE, &Blocks::WAXED_WEATHERED_COPPER_GRATE},
      {&Blocks::OXIDIZED_COPPER_GRATE, &Blocks::WAXED_OXIDIZED_COPPER_GRATE},
      {&Blocks::COPPER_BULB, &Blocks::WAXED_COPPER_BULB},
      {&Blocks::EXPOSED_COPPER_BULB, &Blocks::WAXED_EXPOSED_COPPER_BULB},
      {&Blocks::WEATHERED_COPPER_BULB, &Blocks::WAXED_WEATHERED_COPPER_BULB},
      {&Blocks::OXIDIZED_COPPER_BULB, &Blocks::WAXED_OXIDIZED_COPPER_BULB},
      {&Blocks::COPPER_CHEST, &Blocks::WAXED_COPPER_CHEST},
      {&Blocks::EXPOSED_COPPER_CHEST, &Blocks::WAXED_EXPOSED_COPPER_CHEST},
      {&Blocks::WEATHERED_COPPER_CHEST, &Blocks::WAXED_WEATHERED_COPPER_CHEST},
      {&Blocks::OXIDIZED_COPPER_CHEST, &Blocks::WAXED_OXIDIZED_COPPER_CHEST},
      {&Blocks::COPPER_GOLEM_STATUE, &Blocks::WAXED_COPPER_GOLEM_STATUE},
      {&Blocks::EXPOSED_COPPER_GOLEM_STATUE,
       &Blocks::WAXED_EXPOSED_COPPER_GOLEM_STATUE},
      {&Blocks::WEATHERED_COPPER_GOLEM_STATUE,
       &Blocks::WAXED_WEATHERED_COPPER_GOLEM_STATUE},
      {&Blocks::OXIDIZED_COPPER_GOLEM_STATUE,
       &Blocks::WAXED_OXIDIZED_COPPER_GOLEM_STATUE},
      {&Blocks::LIGHTNING_ROD, &Blocks::WAXED_LIGHTNING_ROD},
      {&Blocks::EXPOSED_LIGHTNING_ROD, &Blocks::WAXED_EXPOSED_LIGHTNING_ROD},
      {&Blocks::WEATHERED_LIGHTNING_ROD, &Blocks::WAXED_WEATHERED_LIGHTNING_ROD},
      {&Blocks::OXIDIZED_LIGHTNING_ROD, &Blocks::WAXED_OXIDIZED_LIGHTNING_ROD},
      {&Blocks::COPPER_LANTERN, &Blocks::WAXED_COPPER_LANTERN},
      {&Blocks::EXPOSED_COPPER_LANTERN, &Blocks::WAXED_EXPOSED_COPPER_LANTERN},
      {&Blocks::WEATHERED_COPPER_LANTERN,
       &Blocks::WAXED_WEATHERED_COPPER_LANTERN},
      {&Blocks::OXIDIZED_COPPER_LANTERN, &Blocks::WAXED_OXIDIZED_COPPER_LANTERN},
      {&Blocks::COPPER_CHAIN, &Blocks::WAXED_COPPER_CHAIN},
      {&Blocks::EXPOSED_COPPER_CHAIN, &Blocks::WAXED_EXPOSED_COPPER_CHAIN},
      {&Blocks::WEATHERED_COPPER_CHAIN, &Blocks::WAXED_WEATHERED_COPPER_CHAIN},
      {&Blocks::OXIDIZED_COPPER_CHAIN, &Blocks::WAXED_OXIDIZED_COPPER_CHAIN},
  };
  return pairs;
}

const std::vector<std::pair<const Block *, const Block *>> &
copperOxidationChain() {
  // Maps oxidized -> weathered -> exposed -> unaffected (for scrape previous).
  static const std::vector<std::pair<const Block *, const Block *>> pairs = {
      {&Blocks::OXIDIZED_COPPER, &Blocks::WEATHERED_COPPER},
      {&Blocks::WEATHERED_COPPER, &Blocks::EXPOSED_COPPER},
      {&Blocks::EXPOSED_COPPER, &Blocks::COPPER_BLOCK},
      {&Blocks::OXIDIZED_CUT_COPPER, &Blocks::WEATHERED_CUT_COPPER},
      {&Blocks::WEATHERED_CUT_COPPER, &Blocks::EXPOSED_CUT_COPPER},
      {&Blocks::EXPOSED_CUT_COPPER, &Blocks::CUT_COPPER},
      {&Blocks::OXIDIZED_CUT_COPPER_SLAB, &Blocks::WEATHERED_CUT_COPPER_SLAB},
      {&Blocks::WEATHERED_CUT_COPPER_SLAB, &Blocks::EXPOSED_CUT_COPPER_SLAB},
      {&Blocks::EXPOSED_CUT_COPPER_SLAB, &Blocks::CUT_COPPER_SLAB},
      {&Blocks::OXIDIZED_CUT_COPPER_STAIRS,
       &Blocks::WEATHERED_CUT_COPPER_STAIRS},
      {&Blocks::WEATHERED_CUT_COPPER_STAIRS, &Blocks::EXPOSED_CUT_COPPER_STAIRS},
      {&Blocks::EXPOSED_CUT_COPPER_STAIRS, &Blocks::CUT_COPPER_STAIRS},
      {&Blocks::OXIDIZED_CHISELED_COPPER, &Blocks::WEATHERED_CHISELED_COPPER},
      {&Blocks::WEATHERED_CHISELED_COPPER, &Blocks::EXPOSED_CHISELED_COPPER},
      {&Blocks::EXPOSED_CHISELED_COPPER, &Blocks::CHISELED_COPPER},
      {&Blocks::OXIDIZED_COPPER_DOOR, &Blocks::WEATHERED_COPPER_DOOR},
      {&Blocks::WEATHERED_COPPER_DOOR, &Blocks::EXPOSED_COPPER_DOOR},
      {&Blocks::EXPOSED_COPPER_DOOR, &Blocks::COPPER_DOOR},
      {&Blocks::OXIDIZED_COPPER_TRAPDOOR, &Blocks::WEATHERED_COPPER_TRAPDOOR},
      {&Blocks::WEATHERED_COPPER_TRAPDOOR, &Blocks::EXPOSED_COPPER_TRAPDOOR},
      {&Blocks::EXPOSED_COPPER_TRAPDOOR, &Blocks::COPPER_TRAPDOOR},
      {&Blocks::OXIDIZED_COPPER_BARS, &Blocks::WEATHERED_COPPER_BARS},
      {&Blocks::WEATHERED_COPPER_BARS, &Blocks::EXPOSED_COPPER_BARS},
      {&Blocks::EXPOSED_COPPER_BARS, &Blocks::COPPER_BARS},
      {&Blocks::OXIDIZED_COPPER_GRATE, &Blocks::WEATHERED_COPPER_GRATE},
      {&Blocks::WEATHERED_COPPER_GRATE, &Blocks::EXPOSED_COPPER_GRATE},
      {&Blocks::EXPOSED_COPPER_GRATE, &Blocks::COPPER_GRATE},
      {&Blocks::OXIDIZED_COPPER_BULB, &Blocks::WEATHERED_COPPER_BULB},
      {&Blocks::WEATHERED_COPPER_BULB, &Blocks::EXPOSED_COPPER_BULB},
      {&Blocks::EXPOSED_COPPER_BULB, &Blocks::COPPER_BULB},
      {&Blocks::OXIDIZED_COPPER_CHEST, &Blocks::WEATHERED_COPPER_CHEST},
      {&Blocks::WEATHERED_COPPER_CHEST, &Blocks::EXPOSED_COPPER_CHEST},
      {&Blocks::EXPOSED_COPPER_CHEST, &Blocks::COPPER_CHEST},
      {&Blocks::OXIDIZED_COPPER_GOLEM_STATUE,
       &Blocks::WEATHERED_COPPER_GOLEM_STATUE},
      {&Blocks::WEATHERED_COPPER_GOLEM_STATUE,
       &Blocks::EXPOSED_COPPER_GOLEM_STATUE},
      {&Blocks::EXPOSED_COPPER_GOLEM_STATUE, &Blocks::COPPER_GOLEM_STATUE},
      {&Blocks::OXIDIZED_LIGHTNING_ROD, &Blocks::WEATHERED_LIGHTNING_ROD},
      {&Blocks::WEATHERED_LIGHTNING_ROD, &Blocks::EXPOSED_LIGHTNING_ROD},
      {&Blocks::EXPOSED_LIGHTNING_ROD, &Blocks::LIGHTNING_ROD},
      {&Blocks::OXIDIZED_COPPER_LANTERN, &Blocks::WEATHERED_COPPER_LANTERN},
      {&Blocks::WEATHERED_COPPER_LANTERN, &Blocks::EXPOSED_COPPER_LANTERN},
      {&Blocks::EXPOSED_COPPER_LANTERN, &Blocks::COPPER_LANTERN},
      {&Blocks::OXIDIZED_COPPER_CHAIN, &Blocks::WEATHERED_COPPER_CHAIN},
      {&Blocks::WEATHERED_COPPER_CHAIN, &Blocks::EXPOSED_COPPER_CHAIN},
      {&Blocks::EXPOSED_COPPER_CHAIN, &Blocks::COPPER_CHAIN},
  };
  return pairs;
}

bool tryBoneMealCrop(World &world, Pos pos) {
  const BlockState state = world.getBlock(pos);
  // Support basic vanilla BonemealableBlock crops currently registered.
  auto advanceAge = [&](int maxAge) -> bool {
    const std::string ageStr = state.get("age", "0");
    int age = 0;
    try {
      age = std::stoi(ageStr);
    } catch (...) {
      return false;
    }
    if (age >= maxAge)
      return false;
    const int bonus =
        maxAge == 7 ? (2 + world.random.nextInt(4)) : 1; // Mth.nextInt(rng,2,5)
    const int nextAge = std::min(maxAge, age + bonus);
    world.setBlock(pos, state.with("age", std::to_string(nextAge)), 3);
    return true;
  };
  if (state.is(Blocks::WHEAT) || state.is(Blocks::CARROTS) ||
      state.is(Blocks::POTATOES) || state.is(Blocks::PUMPKIN_STEM) ||
      state.is(Blocks::MELON_STEM)) {
    return advanceAge(7);
  }
  if (state.is(Blocks::BEETROOTS) || state.is(Blocks::SWEET_BERRY_BUSH)) {
    return advanceAge(3);
  }
  if (state.is(Blocks::COCOA)) {
    return advanceAge(2);
  }
  if (state.is(Blocks::BAMBOO_SAPLING) || state.is(Blocks::BAMBOO)) {
    // Advance upward if air above.
    Pos top = pos;
    for (int i = 0; i < 16; ++i) {
      if (!world.getBlock(top.relative(Direction::Up)).is(Blocks::BAMBOO))
        break;
      top = top.relative(Direction::Up);
    }
    const Pos above = top.relative(Direction::Up);
    if (isAirState(world.getBlock(above))) {
      world.setBlock(above, Blocks::BAMBOO.defaultState(), 3);
      return true;
    }
  }
  // TODO: Sapling TreeGrower, MossBlock spread, CaveVines/Kelp growth,
  // GrassBlock bonemeal vegetation generation require WorldGen/TreeGrower.
  return false;
}

} // namespace

std::optional<BlockState> blockStateForAxeStrip(const BlockState &oldState) {
  const auto &m = axeStrippablesTable();
  const auto it = m.find(oldState.nameView());
  if (it == m.end())
    return std::nullopt;
  return copyMatchingProperties(oldState, *it->second);
}

std::optional<BlockState> blockStateForCopperScrape(const BlockState &oldState) {
  for (const auto &[cur, prev] : copperOxidationChain()) {
    if (oldState.is(*cur))
      return copyMatchingProperties(oldState, *prev);
  }
  return std::nullopt;
}

std::optional<BlockState> blockStateForCopperWaxOn(const BlockState &oldState) {
  for (const auto &[unwaxed, waxed] : copperWaxPairs()) {
    if (oldState.is(*unwaxed))
      return copyMatchingProperties(oldState, *waxed);
  }
  return std::nullopt;
}

std::optional<BlockState> blockStateForCopperWaxOff(const BlockState &oldState) {
  for (const auto &[unwaxed, waxed] : copperWaxPairs()) {
    if (oldState.is(*waxed))
      return copyMatchingProperties(oldState, *unwaxed);
  }
  return std::nullopt;
}

BlockState applyItemBlockStateComponent(const ItemStack &stack,
                                          BlockState placedState) {
  if (!stack.components || stack.components->blockState.empty())
    return placedState;
  for (const auto &[k, v] : stack.components->blockState) {
    if (placedState.hasProperty(k))
      placedState = placedState.with(k, v);
  }
  return placedState;
}

bool placeMinecartOnRail(World &world, Pos railPos, std::string_view itemId) {
  const BlockState state = world.getBlock(railPos);
  if (!isRailBlock(state))
    return false;
  const double raised = railShapeIsSlope(railShapeOf(state)) ? 0.5 : 0.0;
  const Vec3 spawnPos{railPos.x + 0.5, railPos.y + 0.0625 + raised,
                      railPos.z + 0.5};
  if (itemId == "minecraft:minecart") {
    world.spawnMinecart(spawnPos);
    return true;
  }
  if (itemId == "minecraft:chest_minecart") {
    world.addEntity(std::make_shared<MinecartChest>(world, spawnPos));
    return true;
  }
  if (itemId == "minecraft:hopper_minecart") {
    world.addEntity(std::make_shared<MinecartHopper>(world, spawnPos));
    return true;
  }
  if (itemId == "minecraft:tnt_minecart") {
    world.addEntity(std::make_shared<MinecartTNT>(world, spawnPos));
    return true;
  }
  if (itemId == "minecraft:furnace_minecart") {
    world.addEntity(std::make_shared<MinecartFurnace>(world, spawnPos));
    return true;
  }
  // TODO: minecraft:command_block_minecart is unmodeled until command block entity
  // execution is added.
  return false;
}

ItemUseResult useItemOnBlock(World &world, ItemStack &stack,
                             const ItemUseOnParams &params) {
  ItemUseResult result;
  if (stack.empty())
    return result;
  const Item *item = stack.item();
  if (!item)
    return result;

  const Pos pos = params.clickedPos;
  const BlockState clickedState = world.getBlock(pos);

  switch (item->behavior()) {
  case ItemBehaviorKind::Axe: {
    if (params.mainHand && params.offhandBlocksAttacks && !params.secondaryActive)
      return result;
    std::optional<BlockState> next = blockStateForAxeStrip(clickedState);
    if (!next)
      next = blockStateForCopperScrape(clickedState);
    if (!next)
      next = blockStateForCopperWaxOff(clickedState);
    if (!next)
      return result;
    world.setBlock(pos, *next, 11);
    hurtItemOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    result.updatedBlocks.push_back(pos);
    return result;
  }

  case ItemBehaviorKind::Shovel: {
    if (params.clickedFace == Direction::Down)
      return result;
    std::optional<BlockState> updated;
    if (clickedState.is(Blocks::GRASS_BLOCK) || clickedState.is(Blocks::DIRT) ||
        clickedState.is(Blocks::PODZOL) ||
        clickedState.is(Blocks::COARSE_DIRT) ||
        clickedState.is(Blocks::MYCELIUM) ||
        clickedState.is(Blocks::ROOTED_DIRT)) {
      if (isAirState(world.getBlock(pos.relative(Direction::Up))))
        updated = Blocks::DIRT_PATH.defaultState();
    } else if ((clickedState.is(Blocks::CAMPFIRE) ||
                clickedState.is(Blocks::SOUL_CAMPFIRE)) &&
               clickedState.getView("lit", "false") == "true") {
      updated = clickedState.with("lit", "false");
    }
    if (!updated)
      return result;
    world.setBlock(pos, *updated, 11);
    hurtItemOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    result.updatedBlocks.push_back(pos);
    return result;
  }

  case ItemBehaviorKind::Hoe: {
    std::optional<BlockState> updated;
    std::optional<std::string_view> droppedItem;
    const bool airAbove =
        params.clickedFace != Direction::Down &&
        isAirState(world.getBlock(pos.relative(Direction::Up)));
    if ((clickedState.is(Blocks::GRASS_BLOCK) ||
         clickedState.is(Blocks::DIRT_PATH) || clickedState.is(Blocks::DIRT)) &&
        airAbove) {
      updated = Blocks::FARMLAND.defaultState();
    } else if (clickedState.is(Blocks::COARSE_DIRT) && airAbove) {
      updated = Blocks::DIRT.defaultState();
    } else if (clickedState.is(Blocks::ROOTED_DIRT)) {
      updated = Blocks::DIRT.defaultState();
      droppedItem = "minecraft:hanging_roots";
    }
    if (!updated)
      return result;
    world.setBlock(pos, *updated, 11);
    if (droppedItem) {
      const Pos rel = pos.relative(params.clickedFace);
      world.spawnItemAt({rel.x + 0.5, rel.y + 0.5, rel.z + 0.5},
                        std::string(*droppedItem), 1);
    }
    hurtItemOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    result.updatedBlocks.push_back(pos);
    return result;
  }

  case ItemBehaviorKind::Honeycomb: {
    const auto waxed = blockStateForCopperWaxOn(clickedState);
    if (!waxed)
      return result;
    world.setBlock(pos, *waxed, 11);
    shrinkOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    result.updatedBlocks.push_back(pos);
    return result;
  }

  case ItemBehaviorKind::FlintAndSteel:
  case ItemBehaviorKind::FireCharge: {
    const bool isFlint = item->behavior() == ItemBehaviorKind::FlintAndSteel;
    auto spend = [&]() {
      if (isFlint)
        hurtItemOne(stack, params.creative);
      else
        shrinkOne(stack, params.creative);
    };
    if (canLightFlameTarget(clickedState)) {
      world.setBlock(pos, clickedState.with("lit", "true"), 11);
      spend();
      result.outcome = InteractionOutcome::Success;
      result.updatedBlocks.push_back(pos);
      return result;
    }
    if (clickedState.is(Blocks::TNT)) {
      world.primeTntBlock(pos);
      spend();
      result.outcome = InteractionOutcome::Success;
      result.updatedBlocks.push_back(pos);
      return result;
    }
    const Pos rel = pos.relative(params.clickedFace);
    if (!world.isLoaded(rel) || !isAirState(world.getBlock(rel))) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const bool soul = canSurviveFireAt(world, rel, true);
    if (soul || canSurviveFireAt(world, rel, false)) {
      world.setBlock(rel,
                     soul ? Blocks::SOUL_FIRE.defaultState()
                          : Blocks::FIRE.defaultState(),
                     11);
      spend();
      result.outcome = InteractionOutcome::Success;
      result.placedPos = rel;
      result.updatedBlocks.push_back(rel);
      return result;
    }
    result.outcome = InteractionOutcome::Fail;
    return result;
  }

  case ItemBehaviorKind::BoneMeal: {
    if (tryBoneMealCrop(world, pos)) {
      shrinkOne(stack, params.creative);
      result.outcome = InteractionOutcome::Success;
      result.updatedBlocks.push_back(pos);
      return result;
    }
    return result;
  }

  case ItemBehaviorKind::Minecart: {
    if (!isRailBlock(clickedState)) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    if (placeMinecartOnRail(world, pos, item->name())) {
      shrinkOne(stack, params.creative);
      result.outcome = InteractionOutcome::Success;
      return result;
    }
    result.outcome = InteractionOutcome::Fail;
    return result;
  }

  case ItemBehaviorKind::ArmorStand: {
    if (params.clickedFace == Direction::Down) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const bool repl = isReplaceable(clickedState);
    const Pos placePos = repl ? pos : pos.relative(params.clickedFace);
    if (!world.isLoaded(placePos)) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const Pos above = placePos.relative(Direction::Up);
    if (!isReplaceable(world.getBlock(placePos)) ||
        !isReplaceable(world.getBlock(above))) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const Vec3 spawnPos{placePos.x + 0.5, static_cast<double>(placePos.y),
                        placePos.z + 0.5};
    const EntityType *standType = entityType("minecraft:armor_stand");
    if (!standType) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const Aabb box = standType->dimensions().makeBoundingBox(spawnPos);
    if (!world.noCollision(box) || !world.entitiesIn(box).empty()) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    auto stand = std::make_shared<ArmorStand>(world, spawnPos);
    const float snapYaw =
        std::floor((wrapDegreesF(params.playerYaw - 180.0f) + 22.5f) / 45.0f) *
        45.0f;
    stand->yaw = snapYaw;
    stand->pitch = 0.0f;
    world.addEntity(std::move(stand));
    shrinkOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    result.placedPos = placePos;
    return result;
  }

  case ItemBehaviorKind::ItemFrame: {
    const Pos attachPos = pos.relative(params.clickedFace);
    if (!world.isLoaded(attachPos)) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const bool glow = item->name() == "minecraft:glow_item_frame";
    auto frame =
        std::make_shared<ItemFrame>(attachPos, params.clickedFace, glow);
    if (!frame->survives(world)) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    world.addEntity(std::move(frame));
    shrinkOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    result.placedPos = attachPos;
    return result;
  }

  case ItemBehaviorKind::EndCrystal: {
    if (!clickedState.is(Blocks::OBSIDIAN) && !clickedState.is(Blocks::BEDROCK)) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const Pos above = pos.relative(Direction::Up);
    if (!world.isLoaded(above) || !isAirState(world.getBlock(above))) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const Aabb box{static_cast<double>(above.x), static_cast<double>(above.y),
                   static_cast<double>(above.z), above.x + 1.0, above.y + 2.0,
                   above.z + 1.0};
    if (!world.entitiesIn(box).empty()) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    auto crystal = std::make_shared<EndCrystal>(
        world, Vec3{above.x + 0.5, static_cast<double>(above.y), above.z + 0.5});
    crystal->showBottom = false;
    crystal->syncDataFields();
    world.addEntity(std::move(crystal));
    shrinkOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    result.placedPos = above;
    return result;
  }

  case ItemBehaviorKind::SolidBucket: {
    const Pos target =
        isReplaceable(clickedState) ? pos : pos.relative(params.clickedFace);
    if (!world.isLoaded(target)) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const BlockState existing = world.getBlock(target);
    if (!isAirState(existing) && !isReplaceable(existing)) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    world.setBlock(target, Blocks::POWDER_SNOW.defaultState(), 11);
    consumeOrGiveRemainder(world, stack, params.player, params.creative,
                           "minecraft:bucket");
    result.outcome = InteractionOutcome::Success;
    result.placedPos = target;
    result.updatedBlocks.push_back(target);
    return result;
  }

  default:
    return result;
  }
}

InteractionOutcome consumeFoodItem(World &world, ItemStack &stack,
                                   ServerPlayer *player, bool creative) {
  if (stack.empty())
    return InteractionOutcome::Pass;
  const Item *item = stack.item();
  if (!item)
    return InteractionOutcome::Pass;
  if (!item->isFood() && item->useRemainderItem().empty())
    return InteractionOutcome::Pass;

  if (player && item->isFood()) {
    if (!creative && !item->canAlwaysEat() && !player->foodData.needsFood())
      return InteractionOutcome::Pass;
    player->foodData.add(item->nutrition(), item->saturation());
  }
  consumeOrGiveRemainder(world, stack, player, creative,
                         item->useRemainderItem());
  return InteractionOutcome::Consume;
}

ItemUseResult useItemInAir(World &world, ItemStack &stack,
                           const ItemUseParams &params) {
  ItemUseResult result;
  if (stack.empty())
    return result;
  const Item *item = stack.item();
  if (!item)
    return result;

  if (params.player && params.player->cooldowns.isOnCooldown(item->name())) {
    result.outcome = InteractionOutcome::Fail;
    return result;
  }

  auto applyCooldown = [&]() {
    if (params.player && item->useCooldownSeconds() > 0.0f) {
      const int ticks =
          std::max(1, static_cast<int>(std::lround(item->useCooldownSeconds() *
                                                   20.0f)));
      params.player->cooldowns.addCooldown(item->name(), ticks);
    }
  };

  switch (item->behavior()) {
  case ItemBehaviorKind::Boat: {
    const RayHit hit = povClip(world, params.eyePos, params.yaw, params.pitch,
                               params.reach, FluidClipMode::Any);
    if (!hit.hit)
      return result;
    const bool isChest = item->name().find("_chest_") != std::string::npos;
    std::shared_ptr<AbstractBoat> boat;
    if (isChest) {
      boat = std::make_shared<ChestBoat>(world, hit.location,
                                         std::string(item->spawnEntityType()),
                                         item->name());
    } else {
      boat = std::make_shared<Boat>(world, hit.location,
                                    std::string(item->spawnEntityType()),
                                    item->name());
    }
    boat->yaw = params.yaw;
    if (!world.noCollision(boat->boundingBox())) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    world.addEntity(std::move(boat));
    shrinkOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    return result;
  }

  case ItemBehaviorKind::PlaceOnWaterBlock: {
    const RayHit hit = povClip(world, params.eyePos, params.yaw, params.pitch,
                               params.reach, FluidClipMode::SourceOnly);
    if (!hit.hit)
      return result;
    const BlockState hitState = world.getBlock(hit.pos);
    const FluidState fs = fluidStateOf(hitState);
    if (fs.isEmpty() || !Fluids::water().isSame(fs.getType()) ||
        !fs.isSource()) {
      return result;
    }
    const Pos placePos = hit.pos.relative(Direction::Up);
    if (!world.isLoaded(placePos) || !isAirState(world.getBlock(placePos))) {
      result.outcome = InteractionOutcome::Fail;
      return result;
    }
    const Block *block = item->primaryBlock();
    if (!block)
      return result;
    BlockState placed = applyItemBlockStateComponent(stack, block->defaultState());
    world.setBlock(placePos, placed, 11);
    shrinkOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    result.placedPos = placePos;
    result.updatedBlocks.push_back(placePos);
    return result;
  }

  case ItemBehaviorKind::Snowball:
  case ItemBehaviorKind::Egg:
  case ItemBehaviorKind::EnderPearl:
  case ItemBehaviorKind::ExperienceBottle:
  case ItemBehaviorKind::WindCharge: {
    std::shared_ptr<Projectile> proj;
    float power = 1.5f;
    float inaccuracy = 1.0f;
    if (item->behavior() == ItemBehaviorKind::Snowball) {
      auto p = std::make_shared<Snowball>();
      p->item = itemStack(stack.id, 1);
      proj = std::move(p);
    } else if (item->behavior() == ItemBehaviorKind::Egg) {
      auto p = std::make_shared<ThrownEgg>();
      p->item = itemStack(stack.id, 1);
      proj = std::move(p);
    } else if (item->behavior() == ItemBehaviorKind::EnderPearl) {
      auto p = std::make_shared<ThrownEnderpearl>();
      p->item = itemStack(stack.id, 1);
      proj = std::move(p);
    } else if (item->behavior() == ItemBehaviorKind::ExperienceBottle) {
      auto p = std::make_shared<ThrownExperienceBottle>();
      p->item = itemStack(stack.id, 1);
      proj = std::move(p);
      power = 0.7f;
    } else {
      proj = std::make_shared<WindCharge>();
    }
    proj->syncDataFields();
    if (params.player)
      proj->setOwner(params.player);
    proj->setPos(params.eyePos);
    proj->shootFromRotation(world, params.pitch, params.yaw, 0.0f, power,
                            inaccuracy);
    world.addEntity(std::move(proj));
    applyCooldown();
    shrinkOne(stack, params.creative);
    result.outcome = InteractionOutcome::Success;
    return result;
  }

  case ItemBehaviorKind::Bottle: {
    const RayHit hit = povClip(world, params.eyePos, params.yaw, params.pitch,
                               params.reach, FluidClipMode::SourceOnly);
    if (!hit.hit)
      return result;
    const FluidState fs = fluidStateOf(world.getBlock(hit.pos));
    if (!fs.isEmpty() && Fluids::water().isSame(fs.getType()) &&
        fs.isSource()) {
      ItemStack filled = itemStack("minecraft:potion", 1);
      ItemComponentData comp;
      comp.potionId = "minecraft:water";
      filled = withItemComponents(std::move(filled), std::move(comp));
      if (!params.creative) {
        --stack.count;
        if (stack.count <= 0)
          stack = std::move(filled);
        else if (params.player && params.player->onItemPickup)
          (void)params.player->onItemPickup(filled);
      }
      result.outcome = InteractionOutcome::Success;
      return result;
    }
    return result;
  }

  case ItemBehaviorKind::Bucket: {
    const bool isEmpty = item->name() == "minecraft:bucket";
    const RayHit hit =
        povClip(world, params.eyePos, params.yaw, params.pitch, params.reach,
                isEmpty ? FluidClipMode::SourceOnly : FluidClipMode::None);
    if (!hit.hit)
      return result;
    if (isEmpty) {
      const BlockState state = world.getBlock(hit.pos);
      std::string taken;
      if (state.is(Blocks::POWDER_SNOW)) {
        taken = "minecraft:powder_snow_bucket";
        world.setBlock(hit.pos, airState(), 11);
      } else if (state.block().isLiquidContainer() &&
                 state.block().isWaterloggedHost() && waterloggedOf(state)) {
        world.setBlock(hit.pos,
                       state.with(PropertyKind::Waterlogged, std::uint8_t{0}),
                       3);
        taken = "minecraft:water_bucket";
      } else if (isLiquidBlock(state)) {
        const FluidState fluid = fluidStateOf(state);
        if (fluid.isSource() && !fluid.isEmpty()) {
          taken = fluid.is(Fluids::lava()) ? "minecraft:lava_bucket"
                                           : "minecraft:water_bucket";
          world.setBlock(hit.pos, airState(), 11);
        }
      }
      if (taken.empty())
        return result;
      if (!params.creative) {
        --stack.count;
        if (stack.count <= 0) {
          stack = itemStack(taken, 1);
        } else if (params.player && params.player->onItemPickup) {
          (void)params.player->onItemPickup(itemStack(taken, 1));
        }
      }
      result.outcome = InteractionOutcome::Success;
      result.updatedBlocks.push_back(hit.pos);
      return result;
    }
    if (item->name() != "minecraft:water_bucket" &&
        item->name() != "minecraft:lava_bucket") {
      // TODO: MobBucketItem / MilkBucketItem consumable remainders
      if (item->isFood() || !item->useRemainderItem().empty()) {
        result.outcome =
            consumeFoodItem(world, stack, params.player, params.creative);
      }
      return result;
    }
    const bool isWater = item->name() == "minecraft:water_bucket";
    const BlockState clickedState = world.getBlock(hit.pos);
    const Pos initialPos = (isWater && clickedState.block().isLiquidContainer())
                               ? hit.pos
                               : hit.pos.relative(hit.face);
    auto tryEmpty = [&](auto &self, Pos p, bool allowFallback) -> bool {
      if (!world.isLoaded(p))
        return false;
      const BlockState s = world.getBlock(p);
      const bool mayReplace = isReplaceable(s) || !blocksMotion(s);
      const bool canContainer = isWater && s.block().isLiquidContainer() &&
                                canPlaceLiquid(s, Fluids::water());
      const bool canInside =
          s.isAir() || ((mayReplace || canContainer) &&
                        (!params.sneaking || !allowFallback));
      if (!canInside)
        return allowFallback &&
               self(self, hit.pos.relative(hit.face), false);
      if (isWater && s.block().isLiquidContainer() &&
          s.block().isWaterloggedHost()) {
        placeLiquid(world, p, s, fluidStateOf(defaultState("water")));
        result.updatedBlocks.push_back(p);
        return true;
      }
      if (!s.isAir() && mayReplace && !isLiquidBlock(s))
        world.updateOrDestroy(p, s, airState(), 3, 512);
      const BlockState fluidBlock =
          defaultState(isWater ? "water" : "lava");
      const bool ok =
          world.setBlock(p, fluidBlock, 11) || fluidStateOf(s).isSource();
      if (ok)
        result.updatedBlocks.push_back(p);
      return ok;
    };
    if (tryEmpty(tryEmpty, initialPos, true)) {
      consumeOrGiveRemainder(world, stack, params.player, params.creative,
                             "minecraft:bucket");
      result.outcome = InteractionOutcome::Success;
      return result;
    }
    result.outcome = InteractionOutcome::Fail;
    return result;
  }

  default:
    if (item->isFood()) {
      result.outcome =
          consumeFoodItem(world, stack, params.player, params.creative);
      return result;
    }
    // TODO: BowItem / CrossbowItem / TridentItem draw/release, ShieldItem block,
    // SpyglassItem / BrushItem / FishingRodItem / InstrumentItem continuous use.
    return result;
  }
}

} // namespace redstone
