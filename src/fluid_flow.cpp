#include "redstone/fluid_flow.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "redstone/entity.hpp"
#include "redstone/shapes.hpp"

// net/minecraft/world/level/material/FlowingFluid.java
//
// The spread algorithm is a direct transcription of the Java methods; the helper
// predicates that vanilla gets from BlockState/LiquidBlockContainer are adapted
// to this registry and marked with TODO where a dependency is not ported yet.
namespace redstone {
namespace {

// WaterFluid/LavaFluid getFlowing()/getSource(): the two registry entries that
// form one fluid family.
struct FluidPair {
  const Fluid *source = nullptr;
  const Fluid *flowing = nullptr;
};
FluidPair familyOf(const Fluid &fluid) {
  if (fluid.isSame(Fluids::water()))
    return {&Fluids::water(), &Fluids::flowingWater()};
  if (fluid.isSame(Fluids::lava()))
    return {&Fluids::lava(), &Fluids::flowingLava()};
  return {&fluid, &fluid};
}

// FlowingFluid.getDropOff / getSlopeFindDistance / getTickDelay and
// canConvertToSource. The per-environment differences (fast lava, gamerules) are
// task F-4; these are the registered base values.
int dropOff(const Fluid &fluid) { return fluid.dropOff(); }
int slopeFindDistance(const Fluid &fluid) { return fluid.slopeFindDistance(); }
int tickDelay(const Fluid &fluid) { return fluid.tickDelay(); }

// FlowingFluid.canHoldAnyFluid (FlowingFluid.java:410-427): a
// LiquidBlockContainer always holds fluid whatever its collision shape says,
// otherwise a non-blocking-motion state does, minus the block classes the
// vanilla test excludes. Doors, signs, ladders, sugar cane, bubble columns,
// portals and structure voids are not registered here, so the exclusion list
// has no member in this subset yet.
bool canHoldAnyFluid(const BlockState &state) {
  if (state.block().isLiquidContainer())
    return true;
  return !blocksMotion(state);
}

// FlowingFluid.canHoldSpecificFluid (FlowingFluid.java:433-435): a container
// answers for itself through canPlaceLiquid, every other block that holds any
// fluid holds this one.
bool canHoldSpecificFluid(World &, Pos, const BlockState &state,
                          const Fluid &newFluid) {
  if (!state.block().isLiquidContainer())
    return true;
  // SimpleWaterloggedBlock.canPlaceLiquid(level, pos, state, type) is
  // `type == Fluids.WATER`, independent of the state and of the level.
  return newFluid.is(Fluids::water());
}

// FlowingFluid.canHoldFluid: canHoldAnyFluid(state) && canHoldSpecificFluid
// (FlowingFluid.java:429-431), used by isWaterHole.
bool canHoldFluid(World &world, Pos pos, const BlockState &state,
                  const Fluid &newFluid) {
  return canHoldAnyFluid(state) &&
         canHoldSpecificFluid(world, pos, state, newFluid);
}

// Shapes.mergedFaceOccludes for the axis-aligned shapes this registry uses.
// Both shapes are (possibly empty) unions of boxes on a 1/16 grid, so the union
// of their face rectangles covers the shared face exactly when every 1/16 cell
// of that face is covered. Boxes that do not touch the face do not contribute.
bool mergedFaceOccludes(const VoxelShape &source, const VoxelShape &target,
                        Direction direction) {
  // The source contributes the face on `direction`; the target contributes the
  // face on the opposite side, so the two meet on the shared plane
  // (Shapes.java:236-252 picks which shape is "first" by the axis direction).
  bool covered[16][16] = {};

  const auto mark = [&](const VoxelShape &shape, Direction faceDir) {
    for (const ShapeBox &box : shape) {
      double face = 0.0, u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0;
      switch (faceDir) {
      case Direction::Up:
        face = box.y1;
        u0 = box.x0;
        u1 = box.x1;
        v0 = box.z0;
        v1 = box.z1;
        break;
      case Direction::Down:
        face = box.y0;
        u0 = box.x0;
        u1 = box.x1;
        v0 = box.z0;
        v1 = box.z1;
        break;
      case Direction::North:
        face = box.z0;
        u0 = box.x0;
        u1 = box.x1;
        v0 = box.y0;
        v1 = box.y1;
        break;
      case Direction::South:
        face = box.z1;
        u0 = box.x0;
        u1 = box.x1;
        v0 = box.y0;
        v1 = box.y1;
        break;
      case Direction::West:
        face = box.x0;
        u0 = box.z0;
        u1 = box.z1;
        v0 = box.y0;
        v1 = box.y1;
        break;
      case Direction::East:
        face = box.x1;
        u0 = box.z0;
        u1 = box.z1;
        v0 = box.y0;
        v1 = box.y1;
        break;
      }
      // Only boxes touching this side of the block occlude the face.
      const double limit =
          (faceDir == Direction::Up || faceDir == Direction::South ||
           faceDir == Direction::East)
              ? 16.0
              : 0.0;
      if (std::abs(face - limit) > 1e-6)
        continue;
      for (int i = 0; i < 16; ++i) {
        const double cu = i + 0.5;
        if (cu < u0 || cu > u1)
          continue;
        for (int j = 0; j < 16; ++j) {
          const double cv = j + 0.5;
          if (cv >= v0 && cv <= v1)
            covered[i][j] = true;
        }
      }
    }
  };
  mark(source, direction);
  mark(target, opposite(direction));
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 16; ++j)
      if (!covered[i][j])
        return false;
  return true;
}

bool isFullCube(const VoxelShape &shape) {
  return shape.size() == 1 && shape[0].x0 == 0 && shape[0].y0 == 0 &&
         shape[0].z0 == 0 && shape[0].x1 == 16 && shape[0].y1 == 16 &&
         shape[0].z1 == 16;
}

// FlowingFluid.OCCLUSION_CACHE: immutable state pairs are keyed by both ids and
// the directed face. The Java map is a bounded ThreadLocal LRU; a fixed direct
// mapped table gives the same safe memoization with constant-time probing and no
// allocation. Moving pistons depend on position/progress and never enter it.
struct SharedFaceCacheEntry {
  BlockState::Id source = 0;
  BlockState::Id target = 0;
  Direction direction = Direction::Down;
  bool pass = false;
  bool valid = false;
};

inline constexpr std::size_t kSharedFaceCacheSize = 256;

std::array<SharedFaceCacheEntry, kSharedFaceCacheSize> &
sharedFaceCache() {
  static thread_local std::array<SharedFaceCacheEntry, kSharedFaceCacheSize>
      cache{};
  return cache;
}

std::size_t sharedFaceSlot(BlockState::Id source, BlockState::Id target,
                           Direction direction) {
  std::uint64_t key = static_cast<std::uint64_t>(source) *
                      0x9E3779B185EBCA87ULL;
  key ^= static_cast<std::uint64_t>(target) + 0x517CC1B727220A95ULL +
         (key << 6) + (key >> 2);
  key ^= static_cast<std::uint64_t>(direction) * 0x94D049BB133111EBULL;
  key ^= key >> 30;
  key *= 0xBF58476D1CE4E5B9ULL;
  key ^= key >> 27;
  return static_cast<std::size_t>(key) & (kSharedFaceCacheSize - 1);
}

// FlowingFluid.isSourceBlockOfThisType.
bool isSourceBlockOfThisType(const FluidState &state, const Fluid &self) {
  return state.getType().isSame(self) && state.isSource();
}

// FlowingFluid.canBeReplacedWith, dispatched on the fluid *currently at the
// target position* (FluidState.java:117-119). EmptyFluid always allows it, which
// is what makes air replaceable; water yields only downward and only to a
// non-water fluid, lava only to water once its surface is high enough.
bool canBeReplacedWith(const FluidState &state, World &world, Pos pos,
                       const Fluid &other, Direction direction) {
  const Fluid &self = state.getType();
  if (self.isEmpty())
    return true;
  // Both branches test the WATER *tag*, not the family: WaterFluid wants
  // !other.is(FluidTags.WATER), LavaFluid wants other.is(FluidTags.WATER).
  if (self.isSame(Fluids::water()))
    return direction == Direction::Down &&
           !fluidHasTag(other, FluidTag::Water);
  if (self.isSame(Fluids::lava()))
    return fluidHeight(world, pos, state) >= 0.44444445f &&
           fluidHasTag(other, FluidTag::Water);
  return false;
}

// FlowingFluid.getSpreadDelay (base) overridden by LavaFluid: the delay is
// multiplied by 4 when the surface rises without falling, unless the level's
// random source rolls 0 (LavaFluid.java:179-191).
int computeSpreadDelay(World &world, Pos pos, const Fluid &self,
                       const FluidState &oldState, const FluidState &newState) {
  int result = tickDelay(self);
  if (!self.isSame(Fluids::lava()))
    return result;
  if (oldState.isEmpty() || newState.isEmpty())
    return result;
  if (oldState.isFalling() || newState.isFalling())
    return result;
  if (fluidHeight(world, pos, newState) <= fluidHeight(world, pos, oldState))
    return result;
  if (world.random.nextInt(4) != 0)
    result *= 4;
  return result;
}

// Level.getGameRuleBooleanValue(WATER_SOURCE_CONVERSION / LAVA_SOURCE_CONVERSION).
// The world owns the switches because vanilla reads them from the gamerules.
bool fluidCanConvertToSource(World &world, const Fluid &self) {
  if (self.isSame(Fluids::water()))
    return world.waterSourceConversion;
  if (self.isSame(Fluids::lava()))
    return world.lavaSourceConversion;
  return false;
}

// FlowingFluid.getSlopeDistance visits at most one cell beyond the registered
// slope radius (the hole test happens before the pass bound). Water's radius 4
// is the largest current fluid, so a 11x11 table covers every cache key without
// allocating a node or a map. The vanilla short key packs x/z + 128; here the
// same relative-coordinate identity is used directly, which also keeps negative
// world coordinates unambiguous.
inline constexpr int kMaxSlopeFindDistance = 4;
inline constexpr int kSpreadCacheRadius = kMaxSlopeFindDistance + 1;
inline constexpr int kSpreadCacheWidth = kSpreadCacheRadius * 2 + 1;
inline constexpr std::size_t kSpreadCacheCells =
    static_cast<std::size_t>(kSpreadCacheWidth * kSpreadCacheWidth);

class SpreadContext {
public:
  SpreadContext(World &world, Pos origin, const Fluid &self)
      : world_(world), origin_(origin), self_(self),
        radius_(slopeFindDistance(self) + 1) {
    knownStates_.fill(false);
    knownHoles_.fill(false);
  }

  // Return by value. Out-of-table coordinates are a correctness fallback for a
  // future fluid with a larger radius; a shared mutable scratch reference would
  // alias the parent state when recursion overwrites it.
  BlockState getBlockState(Pos pos) {
    const int index = indexOf(pos);
    if (index < 0)
      return world_.getBlock(pos);
    const auto cell = static_cast<std::size_t>(index);
    if (!knownStates_[cell]) {
      states_[cell] = world_.getBlock(pos);
      knownStates_[cell] = true;
    }
    return states_[cell];
  }

  bool isHole(Pos pos) {
    const int index = indexOf(pos);
    if (index >= 0) {
      const auto cell = static_cast<std::size_t>(index);
      if (knownHoles_[cell])
        return holes_[cell];
    }
    const BlockState state = getBlockState(pos);
    const Pos below = pos.relative(Direction::Down);
    const BlockState belowState = world_.getBlock(below);
    const bool hole = isWaterHole(pos, state, below, belowState);
    if (index >= 0) {
      const auto cell = static_cast<std::size_t>(index);
      holes_[cell] = hole;
      knownHoles_[cell] = true;
    }
    return hole;
  }

private:
  int indexOf(Pos pos) const {
    const int dx = pos.x - origin_.x;
    const int dz = pos.z - origin_.z;
    if (radius_ > kSpreadCacheRadius || dx < -radius_ || dx > radius_ ||
        dz < -radius_ || dz > radius_)
      return -1;
    return (dx + radius_) * kSpreadCacheWidth + (dz + radius_);
  }
  // Declared below; the context only forwards.
  bool isWaterHole(Pos topPos, const BlockState &topState, Pos bottomPos,
                   const BlockState &bottomState);

  World &world_;
  Pos origin_;
  const Fluid &self_;
  int radius_;
  std::array<BlockState, kSpreadCacheCells> states_{};
  std::array<bool, kSpreadCacheCells> knownStates_{};
  std::array<bool, kSpreadCacheCells> holes_{};
  std::array<bool, kSpreadCacheCells> knownHoles_{};
};

bool canMaybePassThrough(World &world, const Fluid &self, Pos sourcePos,
                         const BlockState &sourceState, Direction direction,
                         Pos testPos, const BlockState &testState,
                         const FluidState &testFluidState);

bool canPassThrough(World &world, const Fluid &self, Pos sourcePos,
                    const BlockState &sourceState, Direction direction,
                    Pos testPos, const BlockState &testState,
                    const FluidState &testFluidState) {
  return canMaybePassThrough(world, self, sourcePos, sourceState, direction,
                             testPos, testState, testFluidState) &&
         canHoldSpecificFluid(world, testPos, testState, self);
}

bool canMaybePassThrough(World &world, const Fluid &self, Pos sourcePos,
                         const BlockState &sourceState, Direction direction,
                         Pos testPos, const BlockState &testState,
                         const FluidState &testFluidState) {
  return !isSourceBlockOfThisType(testFluidState, self) &&
         canHoldAnyFluid(testState) &&
         fluidCanPassThroughWall(world, direction, sourcePos, sourceState,
                                 testPos, testState);
}

// FlowingFluid.isWaterHole.
bool isWaterHoleImpl(World &world, const Fluid &self, Pos topPos,
                     const BlockState &topState, Pos bottomPos,
                     const BlockState &bottomState) {
  if (!fluidCanPassThroughWall(world, Direction::Down, topPos, topState,
                               bottomPos, bottomState))
    return false;
  const FluidState below = fluidStateOf(bottomState);
  if (below.getType().isSame(self))
    return true;
  // FlowingFluid.java:313-319 tests canHoldFluid(level, bottomPos,
  // bottomState, this.getFlowing()), i.e. canHoldAnyFluid followed by the
  // container's own canPlaceLiquid for the FLOWING variant of this family.
  const FluidPair pair = familyOf(self);
  return canHoldFluid(world, bottomPos, bottomState, *pair.flowing);
}

bool SpreadContext::isWaterHole(Pos topPos, const BlockState &topState,
                                Pos bottomPos, const BlockState &bottomState) {
  return isWaterHoleImpl(world_, self_, topPos, topState, bottomPos, bottomState);
}

// FlowingFluid.getSlopeDistance: shortest walk to a hole, bounded by the
// fluid's slope search distance.
int getSlopeDistance(World &world, const Fluid &self, Pos pos, int pass,
                     Direction from, const BlockState &state,
                     SpreadContext &context) {
  int lowest = 1000;
  for (Direction direction : {Direction::North, Direction::South,
                              Direction::West, Direction::East}) {
    if (direction == from)
      continue;
    const Pos testPos = pos.relative(direction);
    const BlockState testState = context.getBlockState(testPos);
    const FluidState testFluidState = fluidStateOf(testState);
    if (!canPassThrough(world, self, pos, state, direction, testPos, testState,
                        testFluidState))
      continue;
    if (context.isHole(testPos))
      return pass;
    if (pass < slopeFindDistance(self)) {
      const int v = getSlopeDistance(world, self, testPos, pass + 1,
                                     opposite(direction), testState, context);
      if (v < lowest)
        lowest = v;
    }
  }
  return lowest;
}

FluidState getFlowing(const Fluid &self, int amount, bool falling) {
  return FluidState(*familyOf(self).flowing, static_cast<std::uint8_t>(amount),
                    falling);
}
FluidState getSource(const Fluid &self, bool falling) {
  return FluidState(*familyOf(self).source, 8, falling);
}

// FlowingFluid.sourceNeighborCount.
int sourceNeighborCount(World &world, const Fluid &self, Pos pos) {
  int count = 0;
  for (Direction direction : {Direction::North, Direction::South,
                              Direction::West, Direction::East})
    if (isSourceBlockOfThisType(fluidStateOf(world.getBlock(pos.relative(direction))),
                                self))
      ++count;
  return count;
}

void spreadTo(World &world, Pos pos, const BlockState &state,
              Direction direction, const FluidState &target, const Fluid &self) {
  // LavaFluid.spreadTo: lava falling onto water turns the water into stone and
  // stops there instead of writing lava (LavaFluid.java:202-216).
  if (direction == Direction::Down && fluidHasTag(self, FluidTag::Lava) &&
      fluidHasTag(fluidStateOf(world.getBlock(pos)).getType(), FluidTag::Water)) {
    if (isLiquidBlock(state))
      world.setBlock(pos, Blocks::STONE.defaultState(), 3);
    // TODO: fizz (levelEvent 1501) needs the event/sound layer.
    return;
  }
  // FlowingFluid.java:271-281: a LiquidBlockContainer takes the fluid through
  // placeLiquid and the block survives; only every other state goes through
  // beforeDestroyingBlock and the legacy write. A container that refuses the
  // fluid (a wet host, or anything but the exact water source) leaves the
  // block untouched.
  if (state.block().isLiquidContainer()) {
    placeLiquid(world, pos, state, target);
    return;
  }
  if (!state.isAir()) {
    // FlowingFluid.beforeDestroyingBlock. Water drops the block's loot before
    // the fluid is written; lava only fizzles (WaterFluid.java:81-85,
    // LavaFluid.java:144-146).
    if (self.isSame(Fluids::water())) {
      for (const auto &drop : blockDrops(state))
        world.spawnItem(pos, drop.id, drop.count);
    }
    // TODO: lava's fizz (levelEvent 1501) needs the event/sound layer.
  }
  world.setBlock(pos, target.getType().createLegacyBlock(target), 3);
}

// FlowingFluid.getSpread uses an EnumMap. The only populated directions are
// NORTH, SOUTH, WEST and EAST, visited and emitted in that order, so four inline
// slots preserve EnumMap's observable iteration order without tree allocation.
class SpreadResult {
public:
  void clear() { size_ = 0; }
  void append(Direction direction, const FluidState &fluid) {
    if (size_ == directions_.size())
      return;
    directions_[size_] = direction;
    fluids_[size_] = fluid;
    ++size_;
  }
  std::size_t size() const { return size_; }
  Direction direction(std::size_t i) const { return directions_[i]; }
  const FluidState &fluid(std::size_t i) const { return fluids_[i]; }

private:
  std::array<Direction, 4> directions_{};
  std::array<FluidState, 4> fluids_{};
  std::size_t size_ = 0;
};

// FlowingFluid.getSpread.
SpreadResult getSpread(World &world, Pos pos, const BlockState &state,
                       const Fluid &self);

void spreadToSides(World &world, Pos pos, const FluidState &state,
                   const BlockState &blockState, const Fluid &self) {
  int neighbor = state.getAmount() - dropOff(self);
  if (state.isFalling())
    neighbor = 7;
  if (neighbor <= 0)
    return;
  const auto spreads = getSpread(world, pos, blockState, self);
  for (std::size_t i = 0; i < spreads.size(); ++i) {
    const Direction spread = spreads.direction(i);
    const FluidState &newNeighborFluid = spreads.fluid(i);
    const Pos neighborPos = pos.relative(spread);
    spreadTo(world, neighborPos, world.getBlock(neighborPos), spread,
             newNeighborFluid, self);
  }
}

SpreadResult getSpread(World &world, Pos pos, const BlockState &state,
                       const Fluid &self) {
  int lowest = 1000;
  SpreadResult result;
  SpreadContext context(world, pos, self);
  for (Direction direction : {Direction::North, Direction::South,
                              Direction::West, Direction::East}) {
    const Pos testPos = pos.relative(direction);
    const BlockState &testState = world.getBlock(testPos);
    const FluidState testFluidState = fluidStateOf(testState);
    if (!canMaybePassThrough(world, self, pos, state, direction, testPos,
                             testState, testFluidState))
      continue;
    const FluidState newFluid = fluidNewLiquid(world, testPos, testState, self);
    if (!canHoldSpecificFluid(world, testPos, testState, newFluid.getType()))
      continue;
    int distance;
    if (context.isHole(testPos))
      distance = 0;
    else
      distance = getSlopeDistance(world, self, testPos, 1, opposite(direction),
                                  testState, context);
    if (distance < lowest)
      result.clear();
    if (distance <= lowest) {
      if (canBeReplacedWith(testFluidState, world, testPos,
                            newFluid.getType(), direction))
        result.append(direction, newFluid);
      lowest = distance;
    }
  }
  return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
bool fluidCanPassThroughWall(World &world, Direction direction, Pos sourcePos,
                             const BlockState &sourceState, Pos targetPos,
                             const BlockState &targetState) {
  // FlowingFluid.canPassThroughWall queries the target shape first and returns
  // immediately for a full target. Only a moving piston has a position-aware
  // shape, so every other state uses its cached immutable shape.
  const bool sourceDynamic = sourceState.is(Blocks::MOVING_PISTON);
  const bool targetDynamic = targetState.is(Blocks::MOVING_PISTON);
  VoxelShape sourceTmp, targetTmp;
  const VoxelShape *targetShape = &collisionShape(targetState);
  if (targetDynamic) {
    targetTmp = collisionShape(world, targetPos, targetState);
    targetShape = &targetTmp;
  }
  if (isFullCube(*targetShape))
    return false;
  const VoxelShape *sourceShape = &collisionShape(sourceState);
  if (sourceDynamic) {
    sourceTmp = collisionShape(world, sourcePos, sourceState);
    sourceShape = &sourceTmp;
  }
  if (isFullCube(*sourceShape))
    return false;
  if (sourceShape->empty() && targetShape->empty())
    return true;

  const bool cacheable = !sourceDynamic && !targetDynamic;
  SharedFaceCacheEntry *entry = nullptr;
  if (cacheable) {
    entry = &sharedFaceCache()[sharedFaceSlot(sourceState.id(), targetState.id(),
                                              direction)];
    if (entry->valid && entry->source == sourceState.id() &&
        entry->target == targetState.id() && entry->direction == direction)
      return entry->pass;
  }
  const bool pass = !mergedFaceOccludes(*sourceShape, *targetShape, direction);
  if (entry) {
    *entry = SharedFaceCacheEntry{sourceState.id(), targetState.id(), direction,
                                  pass, true};
  }
  return pass;
}

bool fluidShapesSealSharedFace(const VoxelShape &source,
                               const VoxelShape &target, Direction direction) {
  return mergedFaceOccludes(source, target, direction);
}

float fluidHeight(World &world, Pos pos, const FluidState &state) {
  // EmptyFluid.getHeight is the constant 0.0F, so an empty state never reports a
  // full block just because the block above is also empty.
  if (state.isEmpty())
    return 0.0f;
  // FlowingFluid.hasSameAbove: a same-family fluid directly above makes the
  // surface a full block.
  const FluidState above = fluidStateOf(world.getBlock(pos.relative(Direction::Up)));
  if (above.getType().isSame(state.getType()))
    return 1.0f;
  return state.getOwnHeight();
}

FluidState fluidNewLiquid(World &world, Pos pos, const BlockState &blockState,
                          const Fluid &self) {
  int highestNeighbor = 0;
  int neighbourSources = 0;
  for (Direction direction : {Direction::North, Direction::South,
                              Direction::West, Direction::East}) {
    const Pos relativePos = pos.relative(direction);
    const BlockState neighborState = world.getBlock(relativePos);
    const FluidState fluidState = fluidStateOf(neighborState);
    if (!fluidState.getType().isSame(self))
      continue;
    if (!fluidCanPassThroughWall(world, direction, pos, blockState,
                                 relativePos, neighborState))
      continue;
    if (fluidState.isSource())
      ++neighbourSources;
    highestNeighbor = std::max<int>(highestNeighbor, fluidState.getAmount());
  }
  if (neighbourSources >= 2 && fluidCanConvertToSource(world, self)) {
    const BlockState belowState = world.getBlock(pos.relative(Direction::Down));
    const FluidState belowFluid = fluidStateOf(belowState);
    if (blocksMotion(belowState) ||
        isSourceBlockOfThisType(belowFluid, self))
      return getSource(self, false);
  }
  const Pos abovePos = pos.relative(Direction::Up);
  const BlockState aboveState = world.getBlock(abovePos);
  const FluidState aboveFluid = fluidStateOf(aboveState);
  if (!aboveFluid.isEmpty() && aboveFluid.getType().isSame(self) &&
      fluidCanPassThroughWall(world, Direction::Up, pos, blockState,
                              abovePos, aboveState))
    return getFlowing(self, 8, true);
  const int amount = highestNeighbor - dropOff(self);
  if (amount <= 0)
    return FluidState();
  return getFlowing(self, amount, false);
}

void fluidSpread(World &world, Pos pos, const BlockState &blockState,
                 const FluidState &state) {
  if (state.isEmpty())
    return;
  const Fluid &self = state.getType();
  const Pos belowPos = pos.relative(Direction::Down);
  const BlockState belowState = world.getBlock(belowPos);
  const FluidState belowFluid = fluidStateOf(belowState);
  if (canMaybePassThrough(world, self, pos, blockState, Direction::Down,
                          belowPos, belowState, belowFluid)) {
    const FluidState newBelowFluid = fluidNewLiquid(world, belowPos, belowState, self);
    const Fluid &newBelowFluidType = newBelowFluid.getType();
    if (canBeReplacedWith(belowFluid, world, belowPos, newBelowFluidType,
                          Direction::Down) &&
        canHoldSpecificFluid(world, belowPos, belowState, newBelowFluidType)) {
      spreadTo(world, belowPos, belowState, Direction::Down, newBelowFluid, self);
      if (sourceNeighborCount(world, self, pos) >= 3)
        spreadToSides(world, pos, state, blockState, self);
      return;
    }
  }
  if (state.isSource() ||
      !isWaterHoleImpl(world, self, pos, blockState, belowPos, belowState))
    spreadToSides(world, pos, state, blockState, self);
}

// LiquidBlock.shouldSpreadLiquid (LiquidBlock.java:213-235). Lava meeting water
// turns into obsidian when it is a source and cobblestone otherwise; lava over
// soul soil next to blue ice turns into basalt. Returns false when the lava was
// replaced, which is the caller's signal to stop spreading. The block-side
// callers (onPlace / neighborChanged) arrive with F-5; this is the rule itself.
bool liquidShouldSpreadLiquid(World &world, Pos pos, const BlockState &state) {
  const FluidState fluid = fluidStateOf(state);
  if (!fluidHasTag(fluid.getType(), FluidTag::Lava))
    return true;
  const bool overSoulSoil = world.getBlock(pos.relative(Direction::Down))
                                .is(Blocks::SOUL_SOIL);
  // LiquidBlock.POSSIBLE_FLOW_DIRECTIONS is {DOWN, SOUTH, NORTH, EAST, WEST}
  // (LiquidBlock.java:55-57) and the loop visits `pos.relative(getOpposite())`,
  // so the neighbour order is up, north, south, west, east. The N/S and E/W
  // order decides which conversion wins when both are possible.
  for (Direction direction : {Direction::Down, Direction::South, Direction::North,
                              Direction::East, Direction::West}) {
    const Pos neighborPos = pos.relative(opposite(direction));
    if (fluidHasTag(fluidStateOf(world.getBlock(neighborPos)).getType(),
                    FluidTag::Water)) {
      // LiquidBlock.java:220 reads the world's current lava state, not the state
      // captured when the tick was queued.
      const FluidState current = fluidStateOf(world.getBlock(pos));
      const BlockState convertTo = current.isSource()
                                       ? Blocks::OBSIDIAN.defaultState()
                                       : Blocks::COBBLESTONE.defaultState();
      world.setBlock(pos, convertTo, 3);
      // TODO: fizz (levelEvent 1501) needs the event/sound layer.
      return false;
    }
    if (overSoulSoil &&
        world.getBlock(neighborPos).is(Blocks::BLUE_ICE)) {
      // RotatedPillarBlock.BASALT.defaultBlockState(): axis = y.
      world.setBlock(pos,
                     Blocks::BASALT.defaultState().with(
                         PropertyKind::Axis, std::uint8_t{1}),
                     3);
      // TODO: fizz (levelEvent 1501) needs the event/sound layer.
      return false;
    }
  }
  return true;
}

void fluidTick(World &world, Pos pos, const BlockState &blockState,
               const FluidState &state) {
  FluidState current = state;
  BlockState currentBlock = blockState;
  if (!current.isSource()) {
    // FlowingFluid.java:446 deliberately re-reads level.getBlockState(pos) here
    // instead of reusing the BlockState passed to tick. Keep that read: the
    // argument is the dispatch-time snapshot, while getNewLiquid's contract uses
    // the world state at the start of the flow computation. Once spreadTo starts
    // writing, it still reads each target again before replacing it (line 161).
    const FluidState newFluidState =
        fluidNewLiquid(world, pos, world.getBlock(pos), current.getType());
    // FlowingFluid.tick computes the spread delay *before* writing the block
    // (FlowingFluid.java:444-456), so the random draw for a rising lava surface
    // happens ahead of setBlock and its callbacks.
    const int tickDelay = computeSpreadDelay(world, pos, current.getType(), state,
                                             newFluidState);
    if (newFluidState.isEmpty()) {
      current = newFluidState;
      currentBlock = Blocks::AIR.defaultState();
      world.setBlock(pos, currentBlock, 3);
    } else if (!(newFluidState == current)) {
      current = newFluidState;
      currentBlock = newFluidState.getType().createLegacyBlock(current);
      world.setBlock(pos, currentBlock, 3);
      world.scheduleFluidTick(pos, FluidRef(current.getType()), tickDelay);
    }
  }
  fluidSpread(world, pos, currentBlock, current);
}

Vec3 fluidFlow(World &world, Pos pos, const FluidState &state) {
  const Fluid &self = state.getType();
  double flowX = 0.0;
  double flowZ = 0.0;
  for (Direction direction : {Direction::North, Direction::South,
                              Direction::West, Direction::East}) {
    const Pos neighborPos = pos.relative(direction);
    const FluidState neighborFluid = fluidStateOf(world.getBlock(neighborPos));
    // FlowingFluid.affectsFlow: empty or same family.
    if (!(neighborFluid.isEmpty() || neighborFluid.getType().isSame(self)))
      continue;
    float neighborHeight = neighborFluid.getOwnHeight();
    float distance = 0.0f;
    if (neighborHeight == 0.0f) {
      if (!blocksMotion(world.getBlock(neighborPos))) {
        const Pos belowNeighbor = neighborPos.relative(Direction::Down);
        const FluidState belowNeighborState =
            fluidStateOf(world.getBlock(belowNeighbor));
        if (belowNeighborState.isEmpty() ||
            belowNeighborState.getType().isSame(self)) {
          neighborHeight = belowNeighborState.getOwnHeight();
          if (neighborHeight > 0.0f)
            distance = state.getOwnHeight() - (neighborHeight - 0.8888889f);
        }
      }
    } else {
      distance = state.getOwnHeight() - neighborHeight;
    }
    if (distance != 0.0f) {
      const int stepX = direction == Direction::West    ? -1
                        : direction == Direction::East  ? 1
                                                        : 0;
      const int stepZ = direction == Direction::North   ? -1
                        : direction == Direction::South ? 1
                                                        : 0;
      flowX += stepX * distance;
      flowZ += stepZ * distance;
    }
  }
  Vec3 flow{flowX, 0.0, flowZ};
  if (state.isFalling()) {
    // FlowingFluid.isSolidFace: a same-family fluid never counts, and the
    // direction==UP branch of the Java method is unreachable from the horizontal
    // loop that calls it.
    const auto isSolidFace = [&](Pos p, Direction direction) {
      if (fluidStateOf(world.getBlock(p)).getType().isSame(self))
        return false;
      // TODO: IceBlock is not registered; when it is, it returns false here.
      return faceSturdy(world, p, world.getBlock(p), direction);
    };
    for (Direction direction : {Direction::North, Direction::South,
                                Direction::West, Direction::East}) {
      const Pos neighborPos = pos.relative(direction);
      if (isSolidFace(neighborPos, direction) ||
          isSolidFace(neighborPos.relative(Direction::Up), direction)) {
        flow = flow.normalize() + Vec3{0.0, -6.0, 0.0};
        break;
      }
    }
  }
  return flow.normalize();
}

} // namespace redstone

namespace redstone {
// Public wrapper: the anonymous-namespace helper above plus the same rule the
// lava tick uses, so tests and F-5 callers share one implementation.
int fluidSpreadDelay(World &world, Pos pos, const Fluid &self,
                     const FluidState &oldState, const FluidState &newState) {
  return computeSpreadDelay(world, pos, self, oldState, newState);
}
} // namespace redstone

namespace redstone {
// ---------------------------------------------------------------------------
// LiquidBlock's three request paths (F-5). They are kept separate on purpose:
// vanilla reaches them from onPlace, neighborChanged and updateShape, and each
// has its own trigger condition.
// ---------------------------------------------------------------------------
namespace {
// LevelAccessor.createTick + ScheduledTickAccess.scheduleTick: the identity of
// the queued entry is the fluid the CALLBACK saw, never a fresh read of the
// world at pos. The three callers below each pass the state their vanilla
// overload was handed, so a synchronous write during the callbacks cannot
// retarget the request (LiquidBlock.java:148-151,167-179,194-198).
void requestFluidTick(World &world, Pos pos, const FluidState &fluid) {
  if (fluid.isEmpty())
    return;
  world.scheduleFluidTick(pos, FluidRef(fluid.getType()),
                          fluid.getType().tickDelay());
}
} // namespace

bool isWaterloggedHost(const BlockState &state) {
  // SimpleWaterloggedBlock declares the WATERLOGGED property; that declaration
  // is what makes the block a LiquidBlockContainer here.
  return state.hasProperty("waterlogged");
}

bool canPlaceLiquid(const BlockState &, const Fluid &type) {
  // SimpleWaterloggedBlock.canPlaceLiquid ignores the level and the current
  // waterlogged value: any water source may fill any waterlogged host, and
  // nothing else may. The dry/wet decision belongs to placeLiquid.
  return type.is(Fluids::water());
}

bool placeLiquid(World &world, Pos pos, const BlockState &state,
                 const FluidState &fluidState) {
  if (waterloggedOf(state) || !fluidState.is(Fluids::water()))
    return false;
  world.setBlock(pos,
                 state.with(PropertyKind::Waterlogged, std::uint8_t{1}), 3);
  // SimpleWaterloggedBlock.java:29 schedules through the level, with the
  // incoming fluid state's own type and tick delay.
  world.scheduleFluidTick(pos, FluidRef(fluidState.getType()),
                          fluidState.getType().tickDelay());
  return true;
}

void liquidOnPlace(World &world, Pos pos, const BlockState &state) {
  if (!isLiquidBlock(state))
    return;
  // LiquidBlock.onPlace: shouldSpreadLiquid may convert lava to stone or
  // obsidian in place, in which case no tick is requested.
  if (!liquidShouldSpreadLiquid(world, pos, state))
    return;
  requestFluidTick(world, pos, fluidStateOf(state));
}

void liquidNeighborChanged(World &world, Pos pos, const BlockState &state) {
  if (!isLiquidBlock(state))
    return;
  if (!liquidShouldSpreadLiquid(world, pos, state))
    return;
  requestFluidTick(world, pos, fluidStateOf(state));
}

void liquidUpdateShape(World &world, Pos pos, const BlockState &state,
                       const BlockState &neighbor) {
  if (!isLiquidBlock(state))
    return;
  // LiquidBlock.updateShape schedules only when one side is a source.
  const FluidState self = fluidStateOf(state);
  if (!self.isSource() && !fluidStateOf(neighbor).isSource())
    return;
  // ...and it schedules the tile's own fluid state, not the source it found.
  requestFluidTick(world, pos, self);
}
} // namespace redstone
