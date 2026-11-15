#include "redstone/random_tick.hpp"
#include "redstone/fluid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace redstone {
namespace {
constexpr std::array<Direction, 6> kAllDirections{
    Direction::Down, Direction::Up, Direction::North,
    Direction::South, Direction::West, Direction::East};
constexpr std::array<Direction, 4> kHorizontalDirections{
    Direction::North, Direction::East, Direction::South, Direction::West};

std::string_view intDigitView(int value) {
  static constexpr std::array<std::string_view, 26> kInts{
      "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
      "9",  "10", "11", "12", "13", "14", "15", "16", "17",
      "18", "19", "20", "21", "22", "23", "24", "25"};
  return (value >= 0 && value < static_cast<int>(kInts.size()))
             ? kInts[static_cast<std::size_t>(value)]
             : std::string_view{};
}

int parseIntView(std::string_view view, int fallback = 0) {
  if (view.empty())
    return fallback;
  int out = 0;
  for (char c : view) {
    if (c < '0' || c > '9')
      return fallback;
    out = out * 10 + (c - '0');
  }
  return out;
}

Direction clockwise(Direction d) {
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

Direction counterClockwise(Direction d) {
  switch (d) {
  case Direction::North:
    return Direction::West;
  case Direction::West:
    return Direction::South;
  case Direction::South:
    return Direction::East;
  case Direction::East:
    return Direction::North;
  default:
    return d;
  }
}

bool fireCanBurn(const BlockState &state) {
  if (state.hasProperty("waterlogged") &&
      state.getView("waterlogged") == "true")
    return false;
  const Block &b = state.block();
  // FireBlock.bootStrap(): every block registered with igniteOdds > 0 also has
  // metadata().ignitedByLava == true OR is an explicit plant/wool/wood/etc.
  if (b.metadata().ignitedByLava)
    return true;
  return &b == &Blocks::DRIED_KELP_BLOCK || &b == &Blocks::SCAFFOLDING ||
         &b == &Blocks::LECTERN || &b == &Blocks::COMPOSTER ||
         &b == &Blocks::SWEET_BERRY_BUSH || &b == &Blocks::BEEHIVE ||
         &b == &Blocks::BEE_NEST || &b == &Blocks::AZALEA_LEAVES ||
         &b == &Blocks::FLOWERING_AZALEA_LEAVES || &b == &Blocks::CAVE_VINES ||
         &b == &Blocks::CAVE_VINES_PLANT || &b == &Blocks::SPORE_BLOSSOM ||
         &b == &Blocks::AZALEA || &b == &Blocks::FLOWERING_AZALEA ||
         &b == &Blocks::BIG_DRIPLEAF || &b == &Blocks::BIG_DRIPLEAF_STEM ||
         &b == &Blocks::SMALL_DRIPLEAF || &b == &Blocks::HANGING_ROOTS ||
         &b == &Blocks::GLOW_LICHEN || &b == &Blocks::FIREFLY_BUSH ||
         &b == &Blocks::BUSH || &b == &Blocks::PALE_MOSS_BLOCK ||
         &b == &Blocks::PALE_MOSS_CARPET || &b == &Blocks::PALE_HANGING_MOSS;
}

BlockState transferProperties(const BlockState &from, const Block &toBlock) {
  BlockState out = toBlock.defaultState();
  for (const auto &prop : from.block().stateDefinition().properties()) {
    if (out.hasProperty(prop.name()))
      out = out.with(prop.name(), from.getView(prop.name()));
  }
  return out;
}

// ---------------------------------------------------------------------------
// SpreadingSnowyBlock (GRASS_BLOCK, MYCELIUM) & NyliumBlock
// ---------------------------------------------------------------------------
RandomTickResult spreadingSnowyRandomTick(RandomTickAccess &level, Pos pos,
                                          const BlockState &state,
                                          Random &random) {
  const Pos above = pos.relative(Direction::Up);
  const auto survives = level.canSpreadGrassSurvive(state, above);
  if (!survives)
    return RandomTickResult::Unsupported;
  if (!*survives) {
    level.setBlockAndUpdate(pos, Blocks::DIRT.defaultState());
    return RandomTickResult::Applied;
  }
  const auto brightness = level.maxLocalRawBrightness(above);
  if (!brightness)
    return RandomTickResult::Unsupported;
  if (*brightness < 9)
    return RandomTickResult::Applied;
  const BlockState spreadDefault = state.block().defaultState();
  for (int i = 0; i < 4; ++i) {
    const int dx = random.nextInt(3) - 1;
    const int dy = random.nextInt(5) - 3;
    const int dz = random.nextInt(3) - 1;
    const Pos target{pos.x + dx, pos.y + dy, pos.z + dz};
    if (!level.getBlock(target).is(Blocks::DIRT))
      continue;
    switch (level.canSpreadGrassTarget(spreadDefault,
                                       target.relative(Direction::Up))) {
    case RandomTickAccess::SpreadTargetResult::Unsupported:
      return RandomTickResult::Unsupported;
    case RandomTickAccess::SpreadTargetResult::CannotSpread:
      continue;
    case RandomTickAccess::SpreadTargetResult::SpreadSnowy:
      level.setBlockAndUpdate(target, spreadDefault.with("snowy", "true"));
      break;
    case RandomTickAccess::SpreadTargetResult::SpreadClear:
      level.setBlockAndUpdate(target, spreadDefault.with("snowy", "false"));
      break;
    }
  }
  return RandomTickResult::Applied;
}

RandomTickResult nyliumRandomTick(RandomTickAccess &level, Pos pos,
                                  const BlockState &state, Random &) {
  const auto blocked =
      level.lightBlockIntoUp(state, pos.relative(Direction::Up));
  if (!blocked)
    return RandomTickResult::Unsupported;
  if (*blocked >= 15)
    level.setBlockAndUpdate(pos, Blocks::NETHERRACK.defaultState());
  return RandomTickResult::Applied;
}

// ---------------------------------------------------------------------------
// Leaves, Saplings, Mangrove Propagule, Mushrooms
// ---------------------------------------------------------------------------
RandomTickResult mushroomRandomTick(RandomTickAccess &level, Pos pos,
                                    const BlockState &state, Random &random) {
  if (random.nextInt(25) != 0)
    return RandomTickResult::Applied;
  int remaining = 5;
  for (int z = -4; z <= 4; ++z)
    for (int y = -1; y <= 1; ++y)
      for (int x = -4; x <= 4; ++x)
        if (level.getBlock({pos.x + x, pos.y + y, pos.z + z}).sameBlock(state) &&
            --remaining <= 0)
          return RandomTickResult::Applied;
  const auto offset = [&](Pos base) {
    const int x = random.nextInt(3) - 1;
    const int y1 = random.nextInt(2);
    const int y2 = random.nextInt(2);
    const int z = random.nextInt(3) - 1;
    return Pos{base.x + x, base.y + y1 - y2, base.z + z};
  };
  auto candidate = offset(pos);
  for (int i = 0; i < 4; ++i) {
    if (level.getBlock(candidate).isAir()) {
      const auto survives = level.canMushroomSurvive(candidate, state);
      if (!survives)
        return RandomTickResult::Unsupported;
      if (*survives)
        pos = candidate;
    }
    candidate = offset(pos);
  }
  if (level.getBlock(candidate).isAir()) {
    const auto survives = level.canMushroomSurvive(candidate, state);
    if (!survives)
      return RandomTickResult::Unsupported;
    if (*survives)
      level.setBlock(candidate, state, 2);
  }
  return RandomTickResult::Applied;
}

RandomTickResult leavesRandomTick(RandomTickAccess &, Pos,
                                  const BlockState &state, Random &) {
  if (state.getView("distance") != "7" || state.getView("persistent") != "false")
    return RandomTickResult::Applied;
  // TODO: LeavesBlock.randomTick drops resources before removing the block.
  return RandomTickResult::Unsupported;
}

RandomTickResult advanceTree(RandomTickAccess &level, Pos pos,
                             const BlockState &state, Random &random) {
  if (state.getView("stage") == "0") {
    level.setBlock(pos, state.with("stage", "1"), 260);
    return RandomTickResult::Applied;
  }
  const auto generated = level.growSaplingTree(pos, state, random);
  return generated ? RandomTickResult::Applied : RandomTickResult::Unsupported;
}

RandomTickResult saplingRandomTick(RandomTickAccess &level, Pos pos,
                                   const BlockState &state, Random &random) {
  const auto brightness =
      level.maxLocalRawBrightness(pos.relative(Direction::Up));
  if (!brightness)
    return RandomTickResult::Unsupported;
  if (*brightness < 9 || random.nextInt(7) != 0)
    return RandomTickResult::Applied;
  return advanceTree(level, pos, state, random);
}

RandomTickResult mangrovePropaguleRandomTick(RandomTickAccess &level, Pos pos,
                                             const BlockState &state,
                                             Random &random) {
  if (state.getView("hanging") != "true") {
    if (random.nextInt(7) == 0)
      return advanceTree(level, pos, state, random);
    return RandomTickResult::Applied;
  }
  const int age = parseIntView(state.getView("age"), 4);
  if (age < 4)
    level.setBlock(pos, state.with("age", intDigitView(age + 1)), 2);
  return RandomTickResult::Applied;
}

// ---------------------------------------------------------------------------
// Snow, Ice, Lava, Redstone Ore, Dried Ghast
// ---------------------------------------------------------------------------
RandomTickResult snowLayerRandomTick(RandomTickAccess &level, Pos pos,
                                     const BlockState &, Random &) {
  const auto blockLight = level.blockBrightness(pos);
  if (!blockLight)
    return RandomTickResult::Unsupported;
  if (*blockLight <= 11)
    return RandomTickResult::Applied;
  // TODO: SnowLayerBlock.randomTick drops snowball loot before removing.
  return RandomTickResult::Unsupported;
}

RandomTickResult iceRandomTick(RandomTickAccess &level, Pos pos,
                               const BlockState &state, Random &) {
  const auto blockLight = level.blockBrightness(pos);
  if (!blockLight)
    return RandomTickResult::Unsupported;
  const int dampening = static_cast<int>(lightPropertiesOf(state).dampening);
  if (*blockLight <= 11 - dampening)
    return RandomTickResult::Applied;
  if (level.waterEvaporates()) {
    level.removeBlock(pos, false);
  } else {
    level.setBlockAndUpdate(pos, Blocks::WATER.defaultState());
    level.neighborChanged(pos, Blocks::WATER.name());
  }
  return RandomTickResult::Applied;
}

RandomTickResult lavaRandomTick(RandomTickAccess &level, Pos pos,
                                const BlockState &state, Random &random) {
  return fluidStateOf(state).randomTick(level, pos, random);
}

RandomTickResult redstoneOreRandomTick(RandomTickAccess &level, Pos pos,
                                       const BlockState &state, Random &) {
  if (state.getView("lit") == "true")
    level.setBlock(pos, state.with("lit", "false"), 3);
  return RandomTickResult::Applied;
}

RandomTickResult driedGhastRandomTick(RandomTickAccess &level, Pos pos,
                                      const BlockState &state, Random &) {
  const bool wet = state.getView("waterlogged") == "true" ||
                   parseIntView(state.getView("hydration"), 0) > 0;
  if (wet && !level.hasScheduledTick(pos, Blocks::DRIED_GHAST.name()))
    level.scheduleTick(pos, Blocks::DRIED_GHAST.name(), 5000);
  return RandomTickResult::Applied;
}

// ---------------------------------------------------------------------------
// VineBlock (Multiface attachment + neighbor spread)
// ---------------------------------------------------------------------------
std::string_view vineFaceProperty(Direction d) {
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
  default:
    return {};
  }
}

bool vineIsAcceptableNeighbour(RandomTickAccess &level, Pos neighbourPos,
                               Direction directionToNeighbour) {
  const BlockState neighbourState = level.getBlock(neighbourPos);
  return level.isFaceFullBlockSupportOrCollision(neighbourPos, neighbourState,
                                                 directionToNeighbour);
}

bool vineCanSupportAtFace(RandomTickAccess &level, Pos pos,
                          Direction direction) {
  if (direction == Direction::Down)
    return false;
  const Pos rel = pos.relative(direction);
  if (vineIsAcceptableNeighbour(level, rel, direction))
    return true;
  if (direction == Direction::Up)
    return false;
  const std::string_view prop = vineFaceProperty(direction);
  const BlockState above = level.getBlock(pos.relative(Direction::Up));
  return above.is(Blocks::VINE) && above.getView(prop) == "true";
}

bool vineHasHorizontalConnection(const BlockState &state) {
  return state.getView("north") == "true" || state.getView("east") == "true" ||
         state.getView("south") == "true" || state.getView("west") == "true";
}

bool vineCanSpread(RandomTickAccess &level, Pos pos) {
  int max = 5;
  for (int x = pos.x - 4; x <= pos.x + 4; ++x) {
    for (int y = pos.y - 1; y <= pos.y + 1; ++y) {
      for (int z = pos.z - 4; z <= pos.z + 4; ++z) {
        if (level.getBlock({x, y, z}).is(Blocks::VINE) && --max <= 0)
          return false;
      }
    }
  }
  return true;
}

BlockState vineCopyRandomFaces(const BlockState &from, BlockState to,
                               Random &random) {
  for (Direction d : kHorizontalDirections) {
    if (random.nextInt(2) == 0) {
      const std::string_view prop = vineFaceProperty(d);
      if (from.getView(prop) == "true")
        to = to.with(prop, "true");
    }
  }
  return to;
}

RandomTickResult vineRandomTick(RandomTickAccess &level, Pos pos,
                                const BlockState &state, Random &random) {
  if (!level.spreadVinesEnabled() || random.nextInt(4) != 0)
    return RandomTickResult::Applied;
  const Direction testDir = kAllDirections[static_cast<std::size_t>(random.nextInt(6))];
  const Pos abovePos = pos.relative(Direction::Up);
  const bool testIsHorizontal =
      testDir != Direction::Down && testDir != Direction::Up;
  if (testIsHorizontal && state.getView(vineFaceProperty(testDir)) != "true") {
    if (!vineCanSpread(level, pos))
      return RandomTickResult::Applied;
    const Pos testPos = pos.relative(testDir);
    const BlockState edgeState = level.getBlock(testPos);
    if (edgeState.isAir()) {
      const Direction cw = clockwise(testDir);
      const Direction ccw = counterClockwise(testDir);
      const std::string_view cwProp = vineFaceProperty(cw);
      const std::string_view ccwProp = vineFaceProperty(ccw);
      const bool cwHas = state.getView(cwProp) == "true";
      const bool ccwHas = state.getView(ccwProp) == "true";
      const Pos cwTestPos = testPos.relative(cw);
      const Pos ccwTestPos = testPos.relative(ccw);
      const BlockState def = Blocks::VINE.defaultState();
      if (cwHas && vineIsAcceptableNeighbour(level, cwTestPos, cw)) {
        level.setBlock(testPos, def.with(cwProp, "true"), 2);
      } else if (ccwHas && vineIsAcceptableNeighbour(level, ccwTestPos, ccw)) {
        level.setBlock(testPos, def.with(ccwProp, "true"), 2);
      } else {
        const Direction opp = opposite(testDir);
        const std::string_view oppProp = vineFaceProperty(opp);
        if (cwHas && level.getBlock(cwTestPos).isAir() &&
            vineIsAcceptableNeighbour(level, pos.relative(cw), opp)) {
          level.setBlock(cwTestPos, def.with(oppProp, "true"), 2);
        } else if (ccwHas && level.getBlock(ccwTestPos).isAir() &&
                   vineIsAcceptableNeighbour(level, pos.relative(ccw), opp)) {
          level.setBlock(ccwTestPos, def.with(oppProp, "true"), 2);
        } else if (random.nextFloat() < 0.05f &&
                   vineIsAcceptableNeighbour(
                       level, testPos.relative(Direction::Up), Direction::Up)) {
          level.setBlock(testPos, def.with("up", "true"), 2);
        }
      }
    } else if (vineIsAcceptableNeighbour(level, testPos, testDir)) {
      level.setBlock(pos, state.with(vineFaceProperty(testDir), "true"), 2);
    }
    return RandomTickResult::Applied;
  }

  if (testDir == Direction::Up && pos.y < level.maxBlockY()) {
    if (vineCanSupportAtFace(level, pos, testDir)) {
      level.setBlock(pos, state.with("up", "true"), 2);
      return RandomTickResult::Applied;
    }
    if (level.getBlock(abovePos).isAir()) {
      if (!vineCanSpread(level, pos))
        return RandomTickResult::Applied;
      BlockState aboveState = state;
      for (Direction d : kHorizontalDirections) {
        if (random.nextInt(2) == 0 ||
            !vineIsAcceptableNeighbour(level, abovePos.relative(d), d)) {
          aboveState = aboveState.with(vineFaceProperty(d), "false");
        }
      }
      if (vineHasHorizontalConnection(aboveState))
        level.setBlock(abovePos, aboveState, 2);
      return RandomTickResult::Applied;
    }
  }

  if (pos.y > level.minY()) {
    const Pos belowPos = pos.relative(Direction::Down);
    const BlockState belowState = level.getBlock(belowPos);
    if (belowState.isAir() || belowState.is(Blocks::VINE)) {
      const BlockState before =
          belowState.isAir() ? Blocks::VINE.defaultState() : belowState;
      const BlockState after = vineCopyRandomFaces(state, before, random);
      if (before != after && vineHasHorizontalConnection(after))
        level.setBlock(belowPos, after, 2);
    }
  }
  return RandomTickResult::Applied;
}

// ---------------------------------------------------------------------------
// PointedDripstoneBlock
// ---------------------------------------------------------------------------
bool dripIsPointedWithDir(const BlockState &s, std::string_view dir) {
  return s.is(Blocks::POINTED_DRIPSTONE) && s.getView("vertical_direction") == dir;
}

bool dripIsTip(const BlockState &s, bool includeMerged) {
  if (!s.is(Blocks::POINTED_DRIPSTONE))
    return false;
  const auto thickness = s.getView("thickness");
  return thickness == "tip" || (includeMerged && thickness == "tip_merge");
}

bool dripIsUnmergedTipWithDir(const BlockState &s, std::string_view dir) {
  return dripIsTip(s, false) && s.getView("vertical_direction") == dir;
}

bool dripCanDrip(const BlockState &s) {
  return dripIsPointedWithDir(s, "down") && s.getView("thickness") == "tip" &&
         s.getView("waterlogged") != "true";
}

bool dripIsStalactiteStartPos(RandomTickAccess &level, const BlockState &s,
                              Pos pos) {
  return dripIsPointedWithDir(s, "down") &&
         !level.getBlock(pos.relative(Direction::Up))
              .is(Blocks::POINTED_DRIPSTONE);
}

bool dripCanDripThrough(RandomTickAccess &level, Pos pos, const BlockState &s) {
  if (s.isAir())
    return true;
  if (level.isSolidRender(s))
    return false;
  if (!fluidStateOf(s).isEmpty())
    return false;
  return !level.hasCollisionIntersecting(pos, 7.0, 0.0, 7.0, 9.0, 16.0, 9.0);
}

bool dripValidPlacement(RandomTickAccess &level, Pos pos,
                        Direction tipDirection) {
  const Pos behindPos = pos.relative(opposite(tipDirection));
  const BlockState behindState = level.getBlock(behindPos);
  const std::string_view dirView =
      tipDirection == Direction::Down ? "down" : "up";
  return level.isFaceSturdy(behindPos, behindState, tipDirection) ||
         dripIsPointedWithDir(behindState, dirView);
}

template <class PathPred, class TargetPred>
std::optional<Pos> findBlockVertical(RandomTickAccess &level, Pos pos,
                                     Direction stepDir, PathPred &&pathPred,
                                     TargetPred &&targetPred, int maxSteps) {
  Pos cur = pos;
  for (int i = 1; i < maxSteps; ++i) {
    cur = cur.relative(stepDir);
    const BlockState state = level.getBlock(cur);
    if (targetPred(state))
      return cur;
    if (!level.isInsideBuildHeight(cur.y) || !pathPred(cur, state))
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<Pos> dripFindTip(RandomTickAccess &level, const BlockState &s,
                               Pos pos, int maxSearch, bool includeMerged) {
  if (dripIsTip(s, includeMerged))
    return pos;
  const std::string_view tipDirView = s.getView("vertical_direction");
  const Direction searchDir =
      tipDirView == "down" ? Direction::Down : Direction::Up;
  return findBlockVertical(
      level, pos, searchDir,
      [&](Pos, const BlockState &cur) {
        return dripIsPointedWithDir(cur, tipDirView);
      },
      [&](const BlockState &cur) { return dripIsTip(cur, includeMerged); },
      maxSearch);
}

void createDripstone(RandomTickAccess &level, Pos pos,
                     std::string_view tipDirection, std::string_view thickness) {
  const auto isWater = level.isWaterFluid(pos);
  const bool waterlogged =
      isWater && *isWater && fluidStateOf(level.getBlock(pos)).getType().is(Fluids::water());
  const BlockState next = Blocks::POINTED_DRIPSTONE.defaultState()
                              .with("vertical_direction", tipDirection)
                              .with("thickness", thickness)
                              .with("waterlogged", waterlogged ? "true" : "false");
  level.setBlock(pos, next, 3);
}

void dripCreateMergedTips(RandomTickAccess &level, const BlockState &tipState,
                          Pos tipPos) {
  Pos stalactitePos = tipPos;
  Pos stalagmitePos = tipPos;
  if (tipState.getView("vertical_direction") == "up")
    stalactitePos = tipPos.relative(Direction::Up);
  else
    stalagmitePos = tipPos.relative(Direction::Down);
  createDripstone(level, stalactitePos, "down", "tip_merge");
  createDripstone(level, stalagmitePos, "up", "tip_merge");
}

void dripGrow(RandomTickAccess &level, Pos growFromPos,
              Direction growToDirection) {
  const Pos targetPos = growFromPos.relative(growToDirection);
  const BlockState existing = level.getBlock(targetPos);
  const std::string_view oppView =
      growToDirection == Direction::Down ? "up" : "down";
  const std::string_view toView =
      growToDirection == Direction::Down ? "down" : "up";
  if (dripIsUnmergedTipWithDir(existing, oppView)) {
    dripCreateMergedTips(level, existing, targetPos);
  } else if (existing.isAir() || existing.is(Blocks::WATER)) {
    createDripstone(level, targetPos, toView, "tip");
  }
}

bool dripCanTipGrow(RandomTickAccess &level, const BlockState &tipState,
                    Pos tipPos) {
  const Direction growDir = tipState.getView("vertical_direction") == "down"
                                ? Direction::Down
                                : Direction::Up;
  const Pos growPos = tipPos.relative(growDir);
  const BlockState atGrow = level.getBlock(growPos);
  if (!fluidStateOf(atGrow).isEmpty())
    return false;
  if (atGrow.isAir())
    return true;
  const std::string_view oppView =
      growDir == Direction::Down ? "up" : "down";
  return dripIsUnmergedTipWithDir(atGrow, oppView);
}

bool dripCanReceiveFluid(const BlockState &cauldronState, const Fluid &fluid) {
  if (cauldronState.is(Blocks::CAULDRON))
    return true;
  if (cauldronState.is(Blocks::WATER_CAULDRON))
    return fluid.is(Fluids::water());
  return false;
}

void dripMaybeTransferFluid(RandomTickAccess &level, const BlockState &state,
                            Pos pos, float randomValue) {
  if (randomValue > 0.17578125f && randomValue > 0.05859375f)
    return;
  if (!dripIsStalactiteStartPos(level, state, pos))
    return;
  const std::string_view tipDirView = state.getView("vertical_direction");
  const Direction oppDir =
      tipDirView == "down" ? Direction::Up : Direction::Down;
  const auto rootPos = findBlockVertical(
      level, pos, oppDir,
      [&](Pos, const BlockState &cur) {
        return dripIsPointedWithDir(cur, tipDirView);
      },
      [](const BlockState &cur) {
        return !cur.is(Blocks::POINTED_DRIPSTONE);
      },
      11);
  if (!rootPos)
    return;
  const Pos abovePos = rootPos->relative(Direction::Up);
  const BlockState aboveState = level.getBlock(abovePos);
  const Fluid *fluid = &Fluids::empty();
  const bool mudWater = aboveState.is(Blocks::MUD) && !level.waterEvaporates();
  if (mudWater) {
    fluid = &Fluids::water();
  } else {
    fluid = &fluidStateOf(aboveState).getType();
  }
  float transferProb = 0.0f;
  if (fluid->is(Fluids::water()))
    transferProb = 0.17578125f;
  else if (fluid->is(Fluids::lava()))
    transferProb = 0.05859375f;
  else
    return;
  if (randomValue >= transferProb)
    return;
  const auto tipPos = dripFindTip(level, state, pos, 11, false);
  if (!tipPos)
    return;
  if (mudWater && fluid->is(Fluids::water())) {
    level.setBlockAndUpdate(abovePos, Blocks::CLAY.defaultState());
    return;
  }
  const auto cauldronPos = findBlockVertical(
      level, *tipPos, Direction::Down,
      [&](Pos p, const BlockState &s) { return dripCanDripThrough(level, p, s); },
      [&](const BlockState &s) { return dripCanReceiveFluid(s, *fluid); },
      11);
  if (cauldronPos) {
    const int fallDistance = tipPos->y - cauldronPos->y;
    const int delay = 50 + fallDistance;
    level.scheduleTick(*cauldronPos, level.getBlock(*cauldronPos).name(),
                       delay);
  }
}

void dripGrowStalactiteOrStalagmite(RandomTickAccess &level,
                                    const BlockState &startState, Pos startPos,
                                    Random &random) {
  const BlockState rootState =
      level.getBlock(startPos.relative(Direction::Up));
  const BlockState twoAboveState =
      level.getBlock({startPos.x, startPos.y + 2, startPos.z});
  const FluidState fs = fluidStateOf(twoAboveState);
  if (!rootState.is(Blocks::DRIPSTONE_BLOCK) || !fs.getType().is(Fluids::water()) ||
      !fs.isSource())
    return;
  const auto tipPos = dripFindTip(level, startState, startPos, 7, false);
  if (!tipPos)
    return;
  const BlockState tipState = level.getBlock(*tipPos);
  if (!dripCanDrip(tipState) || !dripCanTipGrow(level, tipState, *tipPos))
    return;
  if (random.nextInt(2) == 0) {
    dripGrow(level, *tipPos, Direction::Down);
    return;
  }
  Pos p = *tipPos;
  for (int i = 0; i < 10; ++i) {
    p = p.relative(Direction::Down);
    const BlockState s = level.getBlock(p);
    if (!fluidStateOf(s).isEmpty())
      return;
    if (dripIsUnmergedTipWithDir(s, "up") && dripCanTipGrow(level, s, p)) {
      dripGrow(level, p, Direction::Up);
      return;
    }
    const Pos below = p.relative(Direction::Down);
    if (dripValidPlacement(level, p, Direction::Up) &&
        !fluidStateOf(level.getBlock(below)).getType().is(Fluids::water())) {
      dripGrow(level, below, Direction::Up);
      return;
    }
    if (!dripCanDripThrough(level, p, s))
      return;
  }
}

RandomTickResult pointedDripstoneRandomTick(RandomTickAccess &level, Pos pos,
                                            const BlockState &state,
                                            Random &random) {
  dripMaybeTransferFluid(level, state, pos, random.nextFloat());
  if (random.nextFloat() < 0.011377778f &&
      dripIsStalactiteStartPos(level, state, pos)) {
    dripGrowStalactiteOrStalagmite(level, state, pos, random);
  }
  return RandomTickResult::Applied;
}

// ---------------------------------------------------------------------------
// GrowingPlantHeadBlock (KELP, CAVE_VINES, WEEPING_VINES, TWISTING_VINES)
// ---------------------------------------------------------------------------
RandomTickResult growingPlantHeadRandomTick(RandomTickAccess &level, Pos pos,
                                            const BlockState &state,
                                            Random &random, Direction growthDir,
                                            double probability,
                                            bool caveVines) {
  const int age = parseIntView(state.getView("age"), 25);
  if (age >= 25 || random.nextDouble() >= probability)
    return RandomTickResult::Applied;
  const Pos growthPos = pos.relative(growthDir);
  const BlockState target = level.getBlock(growthPos);
  const bool canGrow =
      state.is(Blocks::KELP)
          ? target.is(Blocks::WATER)
          : (target.isAir() ||
             (!caveVines && target.is(Blocks::WATER) && false));
  // NetherVines.isValidGrowthState is strictly state.isAir(); CaveVines isAir().
  if (!canGrow)
    return RandomTickResult::Applied;
  BlockState next = state.with("age", intDigitView(age + 1));
  if (caveVines)
    next = next.with("berries", random.nextFloat() < 0.11f ? "true" : "false");
  level.setBlockAndUpdate(growthPos, next);
  return RandomTickResult::Applied;
}

// ---------------------------------------------------------------------------
// Bamboo, Sugar Cane, Cactus, Cocoa, Nether Wart, Sweet Berry, Budding Amethyst
// ---------------------------------------------------------------------------
RandomTickResult bambooStalkRandomTick(RandomTickAccess &level, Pos pos,
                                       const BlockState &state,
                                       Random &random) {
  if (state.getView("stage") != "0")
    return RandomTickResult::Applied;
  if (random.nextInt(3) != 0)
    return RandomTickResult::Applied;
  const Pos above = pos.relative(Direction::Up);
  if (!level.getBlock(above).isAir())
    return RandomTickResult::Applied;
  const auto brightness = level.maxLocalRawBrightness(above);
  if (!brightness)
    return RandomTickResult::Unsupported;
  if (*brightness < 9)
    return RandomTickResult::Applied;
  int heightBelow = 0;
  while (heightBelow < 16 &&
         level.getBlock({pos.x, pos.y - (heightBelow + 1), pos.z})
             .is(Blocks::BAMBOO)) {
    ++heightBelow;
  }
  const int height = heightBelow + 1;
  if (height >= 16)
    return RandomTickResult::Applied;
  const BlockState below = level.getBlock(pos.relative(Direction::Down));
  const Pos twoBelowPos{pos.x, pos.y - 2, pos.z};
  const BlockState twoBelow = level.getBlock(twoBelowPos);
  std::string_view leaves = "none";
  if (height >= 1) {
    if (!below.is(Blocks::BAMBOO) || below.getView("leaves") == "none") {
      leaves = "small";
    } else if (below.is(Blocks::BAMBOO) && below.getView("leaves") != "none") {
      leaves = "large";
      if (twoBelow.is(Blocks::BAMBOO)) {
        level.setBlock(pos.relative(Direction::Down),
                       below.with("leaves", "small"), 3);
        level.setBlock(twoBelowPos, twoBelow.with("leaves", "none"), 3);
      }
    }
  }
  const std::string_view age =
      (state.getView("age") != "1" && !twoBelow.is(Blocks::BAMBOO)) ? "0" : "1";
  const std::string_view stage =
      ((height < 11 || random.nextFloat() >= 0.25f) && height != 15) ? "0"
                                                                     : "1";
  level.setBlock(above, Blocks::BAMBOO.defaultState()
                            .with("age", age)
                            .with("leaves", leaves)
                            .with("stage", stage),
                 3);
  return RandomTickResult::Applied;
}

RandomTickResult bambooSaplingRandomTick(RandomTickAccess &level, Pos pos,
                                         const BlockState &, Random &random) {
  if (random.nextInt(3) != 0)
    return RandomTickResult::Applied;
  const Pos above = pos.relative(Direction::Up);
  if (!level.getBlock(above).isAir())
    return RandomTickResult::Applied;
  const auto brightness = level.maxLocalRawBrightness(above);
  if (!brightness)
    return RandomTickResult::Unsupported;
  if (*brightness >= 9) {
    level.setBlock(above, Blocks::BAMBOO.defaultState().with("leaves", "small"),
                   3);
  }
  return RandomTickResult::Applied;
}

RandomTickResult buddingAmethystRandomTick(RandomTickAccess &level, Pos pos,
                                           const BlockState &, Random &random) {
  if (random.nextInt(5) != 0)
    return RandomTickResult::Applied;
  const Direction dir = kAllDirections[static_cast<std::size_t>(random.nextInt(6))];
  const Pos growPos = pos.relative(dir);
  const BlockState rel = level.getBlock(growPos);
  const Block *nextStage = nullptr;
  const FluidState relFluid = fluidStateOf(rel);
  if (rel.isAir() || (rel.is(Blocks::WATER) && relFluid.isFull())) {
    nextStage = &Blocks::SMALL_AMETHYST_BUD;
  } else if (rel.is(Blocks::SMALL_AMETHYST_BUD) &&
             rel.getView("facing") == directionName(dir)) {
    nextStage = &Blocks::MEDIUM_AMETHYST_BUD;
  } else if (rel.is(Blocks::MEDIUM_AMETHYST_BUD) &&
             rel.getView("facing") == directionName(dir)) {
    nextStage = &Blocks::LARGE_AMETHYST_BUD;
  } else if (rel.is(Blocks::LARGE_AMETHYST_BUD) &&
             rel.getView("facing") == directionName(dir)) {
    nextStage = &Blocks::AMETHYST_CLUSTER;
  }
  if (nextStage) {
    const bool waterlogged = relFluid.getType().is(Fluids::water());
    level.setBlockAndUpdate(
        growPos, nextStage->defaultState()
                     .with("facing", directionName(dir))
                     .with("waterlogged", waterlogged ? "true" : "false"));
  }
  return RandomTickResult::Applied;
}

RandomTickResult cocoaRandomTick(RandomTickAccess &level, Pos pos,
                                 const BlockState &state, Random &random) {
  if (random.nextInt(5) != 0)
    return RandomTickResult::Applied;
  const int age = parseIntView(state.getView("age"), 2);
  if (age < 2)
    level.setBlock(pos, state.with("age", intDigitView(age + 1)), 2);
  return RandomTickResult::Applied;
}

RandomTickResult sugarCaneRandomTick(RandomTickAccess &level, Pos pos,
                                     const BlockState &state, Random &) {
  const Pos above = pos.relative(Direction::Up);
  if (!level.getBlock(above).isAir())
    return RandomTickResult::Applied;
  int height = 1;
  while (level.getBlock({pos.x, pos.y - height, pos.z}).is(Blocks::SUGAR_CANE))
    ++height;
  if (height >= 3)
    return RandomTickResult::Applied;
  const int age = parseIntView(state.getView("age"), 0);
  if (age == 15) {
    level.setBlockAndUpdate(above, Blocks::SUGAR_CANE.defaultState());
    level.setBlock(pos, state.with("age", "0"), 260);
  } else {
    level.setBlock(pos, state.with("age", intDigitView(age + 1)), 260);
  }
  return RandomTickResult::Applied;
}

bool cactusCanSurvive(RandomTickAccess &level, Pos pos) {
  for (Direction d : kHorizontalDirections) {
    const Pos nPos = pos.relative(d);
    const BlockState n = level.getBlock(nPos);
    if (blocksMotion(n) ||
        fluidStateOf(n).getType().is(Fluids::lava()))
      return false;
  }
  const BlockState below = level.getBlock(pos.relative(Direction::Down));
  const auto supports = level.hasBlockTag(below, "minecraft:supports_cactus");
  const bool validBelow = below.is(Blocks::CACTUS) || (supports && *supports);
  return validBelow &&
         !isLiquidBlock(level.getBlock(pos.relative(Direction::Up)));
}

RandomTickResult cactusRandomTick(RandomTickAccess &level, Pos pos,
                                  const BlockState &state, Random &random) {
  const Pos above = pos.relative(Direction::Up);
  if (!level.getBlock(above).isAir())
    return RandomTickResult::Applied;
  int height = 1;
  const int age = parseIntView(state.getView("age"), 0);
  while (level.getBlock({pos.x, pos.y - height, pos.z}).is(Blocks::CACTUS)) {
    if (++height == 3 && age == 15)
      return RandomTickResult::Applied;
  }
  if (age == 8 && cactusCanSurvive(level, above)) {
    const double flowerChance = height >= 3 ? 0.25 : 0.1;
    if (random.nextDouble() <= flowerChance)
      level.setBlockAndUpdate(above, Blocks::CACTUS_FLOWER.defaultState());
  } else if (age == 15 && height < 3) {
    level.setBlockAndUpdate(above, Blocks::CACTUS.defaultState());
    const BlockState zeroed = state.with("age", "0");
    level.setBlock(pos, zeroed, 260);
    level.neighborChanged(above, Blocks::CACTUS.name());
  }
  if (age < 15)
    level.setBlock(pos, state.with("age", intDigitView(age + 1)), 260);
  return RandomTickResult::Applied;
}

RandomTickResult sweetBerryBushRandomTick(RandomTickAccess &level, Pos pos,
                                          const BlockState &state,
                                          Random &random) {
  const int age = parseIntView(state.getView("age"), 3);
  if (age >= 3 || random.nextInt(5) != 0)
    return RandomTickResult::Applied;
  const auto brightness =
      level.maxLocalRawBrightness(pos.relative(Direction::Up));
  if (!brightness)
    return RandomTickResult::Unsupported;
  if (*brightness >= 9)
    level.setBlock(pos, state.with("age", intDigitView(age + 1)), 2);
  return RandomTickResult::Applied;
}

RandomTickResult netherWartRandomTick(RandomTickAccess &level, Pos pos,
                                      const BlockState &state, Random &random) {
  const int age = parseIntView(state.getView("age"), 3);
  if (age < 3 && random.nextInt(10) == 0)
    level.setBlock(pos, state.with("age", intDigitView(age + 1)), 2);
  return RandomTickResult::Applied;
}

// ---------------------------------------------------------------------------
// Farmland & Crops / Stems / Pitcher Crop
// ---------------------------------------------------------------------------
bool farmlandIsNearWater(RandomTickAccess &level, Pos pos) {
  for (int x = pos.x - 4; x <= pos.x + 4; ++x) {
    for (int y = pos.y; y <= pos.y + 1; ++y) {
      for (int z = pos.z - 4; z <= pos.z + 4; ++z) {
        const auto water = level.isWaterFluid({x, y, z});
        if (water && *water)
          return true;
      }
    }
  }
  return false;
}

RandomTickResult farmlandRandomTick(RandomTickAccess &level, Pos pos,
                                    const BlockState &state, Random &) {
  const int moisture = parseIntView(state.getView("moisture"), 0);
  if (farmlandIsNearWater(level, pos)) {
    if (moisture < 7)
      level.setBlock(pos, state.with("moisture", "7"), 2);
    return RandomTickResult::Applied;
  }
  const auto raining = level.isRainingAt(pos.relative(Direction::Up));
  if (!raining) {
    // TODO: FarmlandBlock.randomTick requires weather / biome precipitation
    // when not near water.
    return RandomTickResult::Unsupported;
  }
  if (*raining) {
    if (moisture < 7)
      level.setBlock(pos, state.with("moisture", "7"), 2);
    return RandomTickResult::Applied;
  }
  if (moisture > 0) {
    level.setBlock(pos, state.with("moisture", intDigitView(moisture - 1)), 2);
  } else {
    const auto maintains = level.hasBlockTag(
        level.getBlock(pos.relative(Direction::Up)),
        "minecraft:maintains_farmland");
    if (!maintains || !*maintains)
      level.setBlockAndUpdate(pos, Blocks::DIRT.defaultState());
  }
  return RandomTickResult::Applied;
}

float cropGrowthSpeed(RandomTickAccess &level, Pos pos, const Block &crop) {
  float speed = 1.0f;
  const Pos below = pos.relative(Direction::Down);
  for (int xx = -1; xx <= 1; ++xx) {
    for (int zz = -1; zz <= 1; ++zz) {
      float blockSpeed = 0.0f;
      const BlockState s = level.getBlock({below.x + xx, below.y, below.z + zz});
      const auto grows = level.hasBlockTag(s, "minecraft:grows_crops");
      if (grows && *grows) {
        blockSpeed = 1.0f;
        if (parseIntView(s.getView("moisture"), 0) > 0)
          blockSpeed = 3.0f;
      }
      if (xx != 0 || zz != 0)
        blockSpeed /= 4.0f;
      speed += blockSpeed;
    }
  }
  const Pos north = pos.relative(Direction::North);
  const Pos south = pos.relative(Direction::South);
  const Pos west = pos.relative(Direction::West);
  const Pos east = pos.relative(Direction::East);
  const bool horiz =
      level.getBlock(west).is(crop) || level.getBlock(east).is(crop);
  const bool vert =
      level.getBlock(north).is(crop) || level.getBlock(south).is(crop);
  if (horiz && vert) {
    speed /= 2.0f;
  } else {
    const bool diag =
        level.getBlock(west.relative(Direction::North)).is(crop) ||
        level.getBlock(east.relative(Direction::North)).is(crop) ||
        level.getBlock(east.relative(Direction::South)).is(crop) ||
        level.getBlock(west.relative(Direction::South)).is(crop);
    if (diag)
      speed /= 2.0f;
  }
  return speed;
}

RandomTickResult cropRandomTick(RandomTickAccess &level, Pos pos,
                                const BlockState &state, Random &random,
                                int maxAge, bool threeTickMod,
                                const Block *torchflowerDone = nullptr) {
  if (threeTickMod && random.nextInt(3) == 0)
    return RandomTickResult::Applied;
  const auto brightness = level.maxLocalRawBrightness(pos);
  if (!brightness)
    return RandomTickResult::Unsupported;
  if (*brightness < 9)
    return RandomTickResult::Applied;
  const int age = parseIntView(state.getView("age"), maxAge);
  if (age >= maxAge)
    return RandomTickResult::Applied;
  const float speed = cropGrowthSpeed(level, pos, state.block());
  if (random.nextInt(static_cast<int>(25.0f / speed) + 1) == 0) {
    const int nextAge = age + 1;
    const BlockState nextState =
        (torchflowerDone && nextAge == maxAge)
            ? torchflowerDone->defaultState()
            : state.with("age", intDigitView(nextAge));
    level.setBlock(pos, nextState, 2);
  }
  return RandomTickResult::Applied;
}

RandomTickResult pitcherCropRandomTick(RandomTickAccess &level, Pos pos,
                                       const BlockState &state,
                                       Random &random) {
  const float speed = cropGrowthSpeed(level, pos, Blocks::PITCHER_CROP);
  if (random.nextInt(static_cast<int>(25.0f / speed) + 1) != 0)
    return RandomTickResult::Applied;
  const int age = parseIntView(state.getView("age"), 4);
  if (age >= 4)
    return RandomTickResult::Applied;
  const auto brightness = level.maxLocalRawBrightness(pos);
  if (!brightness)
    return RandomTickResult::Unsupported;
  const int updatedAge = std::min(age + 1, 4);
  const Pos above = pos.relative(Direction::Up);
  const BlockState aboveState = level.getBlock(above);
  const bool canGrow =
      *brightness >= 8 && level.isInsideBuildHeight(above.y) &&
      (updatedAge < 3 || aboveState.isAir() ||
       aboveState.is(Blocks::PITCHER_CROP));
  if (canGrow) {
    const BlockState newLower =
        state.with("age", intDigitView(updatedAge));
    level.setBlock(pos, newLower, 2);
    if (updatedAge >= 3)
      level.setBlock(above, newLower.with("half", "upper"), 3);
  }
  return RandomTickResult::Applied;
}

RandomTickResult stemRandomTick(RandomTickAccess &level, Pos pos,
                                const BlockState &state, Random &random,
                                const Block &fruit, const Block &attachedStem,
                                std::string_view fruitSupportTag) {
  const auto brightness = level.maxLocalRawBrightness(pos);
  if (!brightness)
    return RandomTickResult::Unsupported;
  if (*brightness < 9)
    return RandomTickResult::Applied;
  const float speed = cropGrowthSpeed(level, pos, state.block());
  if (random.nextInt(static_cast<int>(25.0f / speed) + 1) != 0)
    return RandomTickResult::Applied;
  const int age = parseIntView(state.getView("age"), 0);
  if (age < 7) {
    level.setBlock(pos, state.with("age", intDigitView(age + 1)), 2);
    return RandomTickResult::Applied;
  }
  const Direction dir =
      kHorizontalDirections[static_cast<std::size_t>(random.nextInt(4))];
  const Pos rel = pos.relative(dir);
  const BlockState below = level.getBlock(rel.relative(Direction::Down));
  const auto canSupport = level.hasBlockTag(below, fruitSupportTag);
  if (level.getBlock(rel).isAir() && canSupport && *canSupport) {
    level.setBlockAndUpdate(rel, fruit.defaultState());
    level.setBlockAndUpdate(
        pos, attachedStem.defaultState().with("facing", directionName(dir)));
  }
  return RandomTickResult::Applied;
}

// ---------------------------------------------------------------------------
// ChangeOverTimeBlock / WeatheringCopper
// ---------------------------------------------------------------------------
struct CopperStageEntry {
  const Block *block;
  std::uint8_t age; // 0=unaffected, 1=exposed, 2=weathered, 3=oxidized
  const Block *next;
};

constexpr CopperStageEntry kCopperStages[] = {
    {&Blocks::COPPER_BLOCK, 0, &Blocks::EXPOSED_COPPER},
    {&Blocks::EXPOSED_COPPER, 1, &Blocks::WEATHERED_COPPER},
    {&Blocks::WEATHERED_COPPER, 2, &Blocks::OXIDIZED_COPPER},
    {&Blocks::OXIDIZED_COPPER, 3, nullptr},

    {&Blocks::CUT_COPPER, 0, &Blocks::EXPOSED_CUT_COPPER},
    {&Blocks::EXPOSED_CUT_COPPER, 1, &Blocks::WEATHERED_CUT_COPPER},
    {&Blocks::WEATHERED_CUT_COPPER, 2, &Blocks::OXIDIZED_CUT_COPPER},
    {&Blocks::OXIDIZED_CUT_COPPER, 3, nullptr},

    {&Blocks::CHISELED_COPPER, 0, &Blocks::EXPOSED_CHISELED_COPPER},
    {&Blocks::EXPOSED_CHISELED_COPPER, 1, &Blocks::WEATHERED_CHISELED_COPPER},
    {&Blocks::WEATHERED_CHISELED_COPPER, 2, &Blocks::OXIDIZED_CHISELED_COPPER},
    {&Blocks::OXIDIZED_CHISELED_COPPER, 3, nullptr},

    {&Blocks::CUT_COPPER_SLAB, 0, &Blocks::EXPOSED_CUT_COPPER_SLAB},
    {&Blocks::EXPOSED_CUT_COPPER_SLAB, 1, &Blocks::WEATHERED_CUT_COPPER_SLAB},
    {&Blocks::WEATHERED_CUT_COPPER_SLAB, 2, &Blocks::OXIDIZED_CUT_COPPER_SLAB},
    {&Blocks::OXIDIZED_CUT_COPPER_SLAB, 3, nullptr},

    {&Blocks::CUT_COPPER_STAIRS, 0, &Blocks::EXPOSED_CUT_COPPER_STAIRS},
    {&Blocks::EXPOSED_CUT_COPPER_STAIRS, 1, &Blocks::WEATHERED_CUT_COPPER_STAIRS},
    {&Blocks::WEATHERED_CUT_COPPER_STAIRS, 2, &Blocks::OXIDIZED_CUT_COPPER_STAIRS},
    {&Blocks::OXIDIZED_CUT_COPPER_STAIRS, 3, nullptr},

    {&Blocks::COPPER_DOOR, 0, &Blocks::EXPOSED_COPPER_DOOR},
    {&Blocks::EXPOSED_COPPER_DOOR, 1, &Blocks::WEATHERED_COPPER_DOOR},
    {&Blocks::WEATHERED_COPPER_DOOR, 2, &Blocks::OXIDIZED_COPPER_DOOR},
    {&Blocks::OXIDIZED_COPPER_DOOR, 3, nullptr},

    {&Blocks::COPPER_TRAPDOOR, 0, &Blocks::EXPOSED_COPPER_TRAPDOOR},
    {&Blocks::EXPOSED_COPPER_TRAPDOOR, 1, &Blocks::WEATHERED_COPPER_TRAPDOOR},
    {&Blocks::WEATHERED_COPPER_TRAPDOOR, 2, &Blocks::OXIDIZED_COPPER_TRAPDOOR},
    {&Blocks::OXIDIZED_COPPER_TRAPDOOR, 3, nullptr},

    {&Blocks::COPPER_BARS, 0, &Blocks::EXPOSED_COPPER_BARS},
    {&Blocks::EXPOSED_COPPER_BARS, 1, &Blocks::WEATHERED_COPPER_BARS},
    {&Blocks::WEATHERED_COPPER_BARS, 2, &Blocks::OXIDIZED_COPPER_BARS},
    {&Blocks::OXIDIZED_COPPER_BARS, 3, nullptr},

    {&Blocks::COPPER_GRATE, 0, &Blocks::EXPOSED_COPPER_GRATE},
    {&Blocks::EXPOSED_COPPER_GRATE, 1, &Blocks::WEATHERED_COPPER_GRATE},
    {&Blocks::WEATHERED_COPPER_GRATE, 2, &Blocks::OXIDIZED_COPPER_GRATE},
    {&Blocks::OXIDIZED_COPPER_GRATE, 3, nullptr},

    {&Blocks::COPPER_BULB, 0, &Blocks::EXPOSED_COPPER_BULB},
    {&Blocks::EXPOSED_COPPER_BULB, 1, &Blocks::WEATHERED_COPPER_BULB},
    {&Blocks::WEATHERED_COPPER_BULB, 2, &Blocks::OXIDIZED_COPPER_BULB},
    {&Blocks::OXIDIZED_COPPER_BULB, 3, nullptr},

    {&Blocks::COPPER_LANTERN, 0, &Blocks::EXPOSED_COPPER_LANTERN},
    {&Blocks::EXPOSED_COPPER_LANTERN, 1, &Blocks::WEATHERED_COPPER_LANTERN},
    {&Blocks::WEATHERED_COPPER_LANTERN, 2, &Blocks::OXIDIZED_COPPER_LANTERN},
    {&Blocks::OXIDIZED_COPPER_LANTERN, 3, nullptr},

    {&Blocks::COPPER_CHEST, 0, &Blocks::EXPOSED_COPPER_CHEST},
    {&Blocks::EXPOSED_COPPER_CHEST, 1, &Blocks::WEATHERED_COPPER_CHEST},
    {&Blocks::WEATHERED_COPPER_CHEST, 2, &Blocks::OXIDIZED_COPPER_CHEST},
    {&Blocks::OXIDIZED_COPPER_CHEST, 3, nullptr},

    {&Blocks::COPPER_GOLEM_STATUE, 0, &Blocks::EXPOSED_COPPER_GOLEM_STATUE},
    {&Blocks::EXPOSED_COPPER_GOLEM_STATUE, 1, &Blocks::WEATHERED_COPPER_GOLEM_STATUE},
    {&Blocks::WEATHERED_COPPER_GOLEM_STATUE, 2, &Blocks::OXIDIZED_COPPER_GOLEM_STATUE},
    {&Blocks::OXIDIZED_COPPER_GOLEM_STATUE, 3, nullptr},

    {&Blocks::LIGHTNING_ROD, 0, &Blocks::EXPOSED_LIGHTNING_ROD},
    {&Blocks::EXPOSED_LIGHTNING_ROD, 1, &Blocks::WEATHERED_LIGHTNING_ROD},
    {&Blocks::WEATHERED_LIGHTNING_ROD, 2, &Blocks::OXIDIZED_LIGHTNING_ROD},
    {&Blocks::OXIDIZED_LIGHTNING_ROD, 3, nullptr},

    {&Blocks::COPPER_CHAIN, 0, &Blocks::EXPOSED_COPPER_CHAIN},
    {&Blocks::EXPOSED_COPPER_CHAIN, 1, &Blocks::WEATHERED_COPPER_CHAIN},
    {&Blocks::WEATHERED_COPPER_CHAIN, 2, &Blocks::OXIDIZED_COPPER_CHAIN},
    {&Blocks::OXIDIZED_COPPER_CHAIN, 3, nullptr},
};

const CopperStageEntry *findCopperStage(const Block &block) {
  for (const auto &entry : kCopperStages) {
    if (entry.block == &block)
      return &entry;
  }
  return nullptr;
}

RandomTickResult weatheringCopperRandomTick(RandomTickAccess &level, Pos pos,
                                            const BlockState &state,
                                            Random &random) {
  const CopperStageEntry *own = findCopperStage(state.block());
  if (!own || !own->next)
    return RandomTickResult::Applied;
  if (random.nextFloat() >= 0.05688889f)
    return RandomTickResult::Applied;
  int sameAgeCount = 0;
  int olderCount = 0;
  for (int dx = -4; dx <= 4; ++dx) {
    const int remX = 4 - std::abs(dx);
    for (int dy = -remX; dy <= remX; ++dy) {
      const int remY = remX - std::abs(dy);
      for (int dz = -remY; dz <= remY; ++dz) {
        if (dx == 0 && dy == 0 && dz == 0)
          continue;
        const CopperStageEntry *neighbor = findCopperStage(
            level.getBlock({pos.x + dx, pos.y + dy, pos.z + dz}).block());
        if (!neighbor)
          continue;
        if (neighbor->age < own->age)
          return RandomTickResult::Applied;
        if (neighbor->age > own->age)
          ++olderCount;
        else
          ++sameAgeCount;
      }
    }
  }
  const float chance = static_cast<float>(olderCount + 1) /
                       static_cast<float>(olderCount + sameAgeCount + 1);
  const float modifier = (own->age == 0) ? 0.75f : 1.0f;
  const float actualChance = chance * chance * modifier;
  if (random.nextFloat() < actualChance)
    level.setBlockAndUpdate(pos, transferProperties(state, *own->next));
  return RandomTickResult::Applied;
}
} // namespace

BlockState baseFireStateForPlacement(const RandomTickAccess &level, Pos pos) {
  const Pos belowPos = pos.relative(Direction::Down);
  const BlockState belowState = level.getBlock(belowPos);
  const auto soulBase =
      level.hasBlockTag(belowState, "minecraft:soul_fire_base_blocks");
  if (soulBase && *soulBase)
    return Blocks::SOUL_FIRE.defaultState();
  if (!fireCanBurn(belowState) &&
      !level.isFaceSturdy(belowPos, belowState, Direction::Up)) {
    BlockState result = Blocks::FIRE.defaultState();
    for (Direction dir : kAllDirections) {
      const std::string_view prop = vineFaceProperty(dir);
      if (!prop.empty()) {
        const bool burn = fireCanBurn(level.getBlock(pos.relative(dir)));
        result = result.with(prop, burn ? "true" : "false");
      }
    }
    return result;
  }
  return Blocks::FIRE.defaultState();
}

namespace detail {
void bindRandomTickBehaviour(Block &block) {
  if (&block == &Blocks::BROWN_MUSHROOM || &block == &Blocks::RED_MUSHROOM) {
    block.behaviours_.randomTick = &mushroomRandomTick;
  } else if (block.isLeaves()) {
    block.behaviours_.randomTick = &leavesRandomTick;
  } else if (&block == &Blocks::LAVA) {
    block.behaviours_.randomTick = &lavaRandomTick;
  } else if (&block == &Blocks::OAK_SAPLING || &block == &Blocks::SPRUCE_SAPLING ||
             &block == &Blocks::BIRCH_SAPLING || &block == &Blocks::JUNGLE_SAPLING ||
             &block == &Blocks::ACACIA_SAPLING || &block == &Blocks::CHERRY_SAPLING ||
             &block == &Blocks::DARK_OAK_SAPLING || &block == &Blocks::PALE_OAK_SAPLING) {
    block.behaviours_.randomTick = &saplingRandomTick;
  } else if (&block == &Blocks::MANGROVE_PROPAGULE) {
    block.behaviours_.randomTick = &mangrovePropaguleRandomTick;
  } else if (&block == &Blocks::GRASS_BLOCK || &block == &Blocks::MYCELIUM) {
    block.behaviours_.randomTick = &spreadingSnowyRandomTick;
  } else if (&block == &Blocks::CRIMSON_NYLIUM ||
             &block == &Blocks::WARPED_NYLIUM) {
    block.behaviours_.randomTick = &nyliumRandomTick;
  } else if (&block == &Blocks::SNOW) {
    block.behaviours_.randomTick = &snowLayerRandomTick;
  } else if (&block == &Blocks::ICE || &block == &Blocks::FROSTED_ICE) {
    block.behaviours_.randomTick = &iceRandomTick;
  } else if (&block == &Blocks::VINE) {
    block.behaviours_.randomTick = &vineRandomTick;
  } else if (&block == &Blocks::POINTED_DRIPSTONE) {
    block.behaviours_.randomTick = &pointedDripstoneRandomTick;
  } else if (&block == &Blocks::KELP) {
    block.behaviours_.randomTick = [](RandomTickAccess &l, Pos p,
                                      const BlockState &s, Random &r) {
      return growingPlantHeadRandomTick(l, p, s, r, Direction::Up, 0.14, false);
    };
  } else if (&block == &Blocks::CAVE_VINES) {
    block.behaviours_.randomTick = [](RandomTickAccess &l, Pos p,
                                      const BlockState &s, Random &r) {
      return growingPlantHeadRandomTick(l, p, s, r, Direction::Down, 0.1, true);
    };
  } else if (&block == &Blocks::WEEPING_VINES) {
    block.behaviours_.randomTick = [](RandomTickAccess &l, Pos p,
                                      const BlockState &s, Random &r) {
      return growingPlantHeadRandomTick(l, p, s, r, Direction::Down, 0.1,
                                        false);
    };
  } else if (&block == &Blocks::TWISTING_VINES) {
    block.behaviours_.randomTick = [](RandomTickAccess &l, Pos p,
                                      const BlockState &s, Random &r) {
      return growingPlantHeadRandomTick(l, p, s, r, Direction::Up, 0.1, false);
    };
  } else if (&block == &Blocks::BAMBOO) {
    block.behaviours_.randomTick = &bambooStalkRandomTick;
  } else if (&block == &Blocks::BAMBOO_SAPLING) {
    block.behaviours_.randomTick = &bambooSaplingRandomTick;
  } else if (&block == &Blocks::BUDDING_AMETHYST) {
    block.behaviours_.randomTick = &buddingAmethystRandomTick;
  } else if (&block == &Blocks::COCOA) {
    block.behaviours_.randomTick = &cocoaRandomTick;
  } else if (&block == &Blocks::SUGAR_CANE) {
    block.behaviours_.randomTick = &sugarCaneRandomTick;
  } else if (&block == &Blocks::CACTUS) {
    block.behaviours_.randomTick = &cactusRandomTick;
  } else if (&block == &Blocks::SWEET_BERRY_BUSH) {
    block.behaviours_.randomTick = &sweetBerryBushRandomTick;
  } else if (&block == &Blocks::NETHER_WART) {
    block.behaviours_.randomTick = &netherWartRandomTick;
  } else if (&block == &Blocks::FARMLAND) {
    block.behaviours_.randomTick = &farmlandRandomTick;
  } else if (&block == &Blocks::WHEAT || &block == &Blocks::CARROTS ||
             &block == &Blocks::POTATOES) {
    block.behaviours_.randomTick = [](RandomTickAccess &l, Pos p,
                                      const BlockState &s, Random &r) {
      return cropRandomTick(l, p, s, r, 7, false);
    };
  } else if (&block == &Blocks::BEETROOTS) {
    block.behaviours_.randomTick = [](RandomTickAccess &l, Pos p,
                                      const BlockState &s, Random &r) {
      return cropRandomTick(l, p, s, r, 3, true);
    };
  } else if (&block == &Blocks::TORCHFLOWER_CROP) {
    block.behaviours_.randomTick = [](RandomTickAccess &l, Pos p,
                                      const BlockState &s, Random &r) {
      return cropRandomTick(l, p, s, r, 2, true, &Blocks::TORCHFLOWER);
    };
  } else if (&block == &Blocks::PITCHER_CROP) {
    block.behaviours_.randomTick = &pitcherCropRandomTick;
  } else if (&block == &Blocks::MELON_STEM) {
    block.behaviours_.randomTick = [](RandomTickAccess &l, Pos p,
                                      const BlockState &s, Random &r) {
      return stemRandomTick(l, p, s, r, Blocks::MELON,
                            Blocks::ATTACHED_MELON_STEM,
                            "minecraft:supports_melon_stem_fruit");
    };
  } else if (&block == &Blocks::PUMPKIN_STEM) {
    block.behaviours_.randomTick = [](RandomTickAccess &l, Pos p,
                                      const BlockState &s, Random &r) {
      return stemRandomTick(l, p, s, r, Blocks::PUMPKIN,
                            Blocks::ATTACHED_PUMPKIN_STEM,
                            "minecraft:supports_pumpkin_stem_fruit");
    };
  } else if (&block == &Blocks::REDSTONE_ORE ||
             &block == &Blocks::DEEPSLATE_REDSTONE_ORE) {
    block.behaviours_.randomTick = &redstoneOreRandomTick;
  } else if (&block == &Blocks::DRIED_GHAST) {
    block.behaviours_.randomTick = &driedGhastRandomTick;
  } else if (const auto *copper = findCopperStage(block);
             copper && copper->next) {
    block.behaviours_.randomTick = &weatheringCopperRandomTick;
  }
  // TODO: TURTLE_EGG (EntityType.TURTLE + EnvironmentAttributes),
  // NETHER_PORTAL (Difficulty + ZOMBIFIED_PIGLIN spawn + EnvironmentAttributes),
  // CHORUS_FLOWER (ChorusPlantBlock.getStateWithConnections),
  // OPEN/CLOSED_EYEBLOSSOM & POTTED_*_EYEBLOSSOM (EnvironmentAttributes).
}
} // namespace detail

std::optional<bool> RandomTickAccess::canSpreadGrassSurvive(BlockState state,
                                                            Pos above) const {
  const BlockState aboveState = getBlock(above);
  if (aboveState.is(Blocks::SNOW)) {
    if (!aboveState.hasProperty("layers"))
      return std::nullopt;
    if (aboveState.getView("layers") == "1")
      return true;
  }
  const auto full = isFullFluid(above);
  if (!full)
    return std::nullopt;
  if (*full)
    return false;
  const auto blocked = lightBlockIntoUp(state, above);
  if (!blocked)
    return std::nullopt;
  return *blocked < 15;
}

RandomTickAccess::SpreadTargetResult
RandomTickAccess::canSpreadGrassTarget(BlockState spreadDefault,
                                       Pos targetAbove) const {
  const auto survives = canSpreadGrassSurvive(spreadDefault, targetAbove);
  if (!survives)
    return SpreadTargetResult::Unsupported;
  if (!*survives)
    return SpreadTargetResult::CannotSpread;
  const auto water = isWaterFluid(targetAbove);
  if (!water)
    return SpreadTargetResult::Unsupported;
  if (*water)
    return SpreadTargetResult::CannotSpread;
  const auto snowy = isSnowySetting(targetAbove);
  if (!snowy)
    return SpreadTargetResult::Unsupported;
  return *snowy ? SpreadTargetResult::SpreadSnowy
                : SpreadTargetResult::SpreadClear;
}

RandomTickResult Block::randomTick(RandomTickAccess &level, Pos pos,
                                   const BlockState &state,
                                   Random &random) const {
  if (behaviours_.randomTick)
    return behaviours_.randomTick(level, pos, state, random);
  // TODO: implement each remaining BlockBehaviour.randomTick override.
  return RandomTickResult::Unsupported;
}

std::optional<bool> BlockState::isRandomlyTicking() const {
  if (!isSchemaState())
    return std::nullopt;
  return stateIsRandomlyTicking(*this);
}

RandomTickResult BlockState::randomTick(RandomTickAccess &level, Pos pos,
                                        Random &random) const {
  // Fast path: non-randomly-ticking states skip handler dispatch immediately.
  if (isSchemaState() && !stateIsRandomlyTicking(*this))
    return RandomTickResult::Applied;
  return block().randomTick(level, pos, *this, random);
}
} // namespace redstone
