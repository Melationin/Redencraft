#include "redstone/fluid_flow.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

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

// BlockBehaviour.calculateSolid: forceSolidOn wins, then the collision shape's
// bounding box decides — solid when its average extent >= 0.7291666 or its
// height >= 1.0. That makes glass solid (full box) while a 1/16 carpet is not
// ((1 + 1/16 + 1) / 3 = 0.6875).
bool isSolid(const BlockState &state) {
  if (blockMetadata(state).forceSolid)
    return true;
  // TODO: forceSolidOff has no registered user yet.
  const VoxelShape &shape = collisionShape(state);
  if (shape.empty())
    return false;
  double x0 = 16.0, y0 = 16.0, z0 = 16.0, x1 = 0.0, y1 = 0.0, z1 = 0.0;
  for (const ShapeBox &box : shape) {
    x0 = std::min(x0, box.x0);
    y0 = std::min(y0, box.y0);
    z0 = std::min(z0, box.z0);
    x1 = std::max(x1, box.x1);
    y1 = std::max(y1, box.y1);
    z1 = std::max(z1, box.z1);
  }
  const double xs = (x1 - x0) / 16.0;
  const double ys = (y1 - y0) / 16.0;
  const double zs = (z1 - z0) / 16.0;
  return (xs + ys + zs) / 3.0 >= 0.7291666666666666 || ys >= 1.0;
}

// BlockBehaviour.blocksMotion: `block != COBWEB && block != BAMBOO_SAPLING &&
// isSolid()`. Neither excluded block is registered, so this is isSolid().
bool blocksMotion(const BlockState &state) { return isSolid(state); }

// FlowingFluid.canHoldAnyFluid. A LiquidBlockContainer always holds fluid; that
// interface arrives with the waterlogged hosts in F-5.
bool canHoldAnyFluid(const BlockState &state) {
  // TODO: LiquidBlockContainer hosts (waterlogged blocks) belong to F-5.
  return !blocksMotion(state);
}

// FlowingFluid.canHoldSpecificFluid: without LiquidBlockContainer every block
// that can hold any fluid can hold this one.
bool canHoldSpecificFluid(World &, Pos, const BlockState &state, const Fluid &) {
  return canHoldAnyFluid(state);
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
  if (self.isSame(Fluids::water()))
    return direction == Direction::Down &&
           !other.isSame(Fluids::water());
  if (self.isSame(Fluids::lava()))
    return fluidHeight(world, pos, state) >= 0.44444445f &&
           other.isSame(Fluids::water());
  return false;
}

// FlowingFluid.SpreadContext: the short-keyed caches keyed on the offset from
// the spread origin. Vanilla uses a Short2ObjectMap; a small map is enough here
// because the radius is bounded by getSlopeFindDistance + 1.
class SpreadContext {
public:
  SpreadContext(World &world, Pos origin, const Fluid &self)
      : world_(world), origin_(origin), self_(self) {}

  const BlockState &getBlockState(Pos pos) {
    const auto key = keyOf(pos);
    auto found = states_.find(key);
    if (found != states_.end())
      return found->second;
    return states_.emplace(key, world_.getBlock(pos)).first->second;
  }

  bool isHole(Pos pos) {
    const auto key = keyOf(pos);
    auto found = holes_.find(key);
    if (found != holes_.end())
      return found->second;
    const BlockState &state = getBlockState(pos);
    const Pos below = pos.relative(Direction::Down);
    const BlockState &belowState = world_.getBlock(below);
    const bool hole = isWaterHole(pos, state, below, belowState);
    holes_.emplace(key, hole);
    return hole;
  }

private:
  // FlowingFluid.SpreadContext.getCacheKey packs (dx+128, dz+128) into a short.
  std::uint16_t keyOf(Pos pos) const {
    const int dx = (pos.x - origin_.x + 128) & 0xFF;
    const int dz = (pos.z - origin_.z + 128) & 0xFF;
    return static_cast<std::uint16_t>((dx << 8) | dz);
  }
  // Declared below; the context only forwards.
  bool isWaterHole(Pos topPos, const BlockState &topState, Pos bottomPos,
                   const BlockState &bottomState);

  World &world_;
  Pos origin_;
  const Fluid &self_;
  std::map<std::uint16_t, BlockState> states_;
  std::map<std::uint16_t, bool> holes_;
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
  const FluidPair pair = familyOf(self);
  return canHoldAnyFluid(bottomState) &&
         canHoldSpecificFluid(world, bottomPos, bottomState, *pair.flowing);
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
    const BlockState &testState = context.getBlockState(testPos);
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
  (void)direction;
  // TODO: LiquidBlockContainer.placeLiquid (waterlogged hosts) is task F-5.
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

// FlowingFluid.getSpread.
std::map<Direction, FluidState> getSpread(World &world, Pos pos,
                                          const BlockState &state,
                                          const Fluid &self);

void spreadToSides(World &world, Pos pos, const FluidState &state,
                   const BlockState &blockState, const Fluid &self) {
  int neighbor = state.getAmount() - dropOff(self);
  if (state.isFalling())
    neighbor = 7;
  if (neighbor <= 0)
    return;
  const auto spreads = getSpread(world, pos, blockState, self);
  for (const auto &[spread, newNeighborFluid] : spreads) {
    const Pos neighborPos = pos.relative(spread);
    spreadTo(world, neighborPos, world.getBlock(neighborPos), spread,
             newNeighborFluid, self);
  }
}

std::map<Direction, FluidState> getSpread(World &world, Pos pos,
                                          const BlockState &state,
                                          const Fluid &self) {
  int lowest = 1000;
  std::map<Direction, FluidState> result;
  SpreadContext *context = nullptr;
  std::unique_ptr<SpreadContext> owned;
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
    if (!context) {
      owned = std::make_unique<SpreadContext>(world, pos, self);
      context = owned.get();
    }
    int distance;
    if (context->isHole(testPos))
      distance = 0;
    else
      distance = getSlopeDistance(world, self, testPos, 1, opposite(direction),
                                  testState, *context);
    if (distance < lowest)
      result.clear();
    if (distance <= lowest) {
      if (canBeReplacedWith(testFluidState, world, testPos,
                            newFluid.getType(), direction))
        result.emplace(direction, newFluid);
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
  // Only a moving piston needs the position-aware query, because its shape
  // depends on the block entity's progress (MovingPistonBlock.java:105-108).
  // Every other state borrows the cached shape, so the hot path copies nothing.
  VoxelShape sourceTmp, targetTmp;
  const VoxelShape *sourceShape = &collisionShape(sourceState);
  if (sourceState.is(Blocks::MOVING_PISTON)) {
    sourceTmp = collisionShape(world, sourcePos, sourceState);
    sourceShape = &sourceTmp;
  }
  const VoxelShape *targetShape = &collisionShape(targetState);
  if (targetState.is(Blocks::MOVING_PISTON)) {
    targetTmp = collisionShape(world, targetPos, targetState);
    targetShape = &targetTmp;
  }
  if (isFullCube(*targetShape))
    return false;
  if (isFullCube(*sourceShape))
    return false;
  if (sourceShape->empty() && targetShape->empty())
    return true;
  return !mergedFaceOccludes(*sourceShape, *targetShape, direction);
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
  (void)blockState;
  int highestNeighbor = 0;
  int neighbourSources = 0;
  for (Direction direction : {Direction::North, Direction::South,
                              Direction::West, Direction::East}) {
    const Pos relativePos = pos.relative(direction);
    const BlockState neighborState = world.getBlock(relativePos);
    const FluidState fluidState = fluidStateOf(neighborState);
    if (!fluidState.getType().isSame(self))
      continue;
    if (!fluidCanPassThroughWall(world, direction, pos, world.getBlock(pos),
                                 relativePos, neighborState))
      continue;
    if (fluidState.isSource())
      ++neighbourSources;
    highestNeighbor = std::max<int>(highestNeighbor, fluidState.getAmount());
  }
  if (neighbourSources >= 2 && self.canConvertToSource()) {
    const BlockState belowState = world.getBlock(pos.relative(Direction::Down));
    const FluidState belowFluid = fluidStateOf(belowState);
    if (isSolid(belowState) || isSourceBlockOfThisType(belowFluid, self))
      return getSource(self, false);
  }
  const Pos abovePos = pos.relative(Direction::Up);
  const BlockState aboveState = world.getBlock(abovePos);
  const FluidState aboveFluid = fluidStateOf(aboveState);
  if (!aboveFluid.isEmpty() && aboveFluid.getType().isSame(self) &&
      fluidCanPassThroughWall(world, Direction::Up, pos, world.getBlock(pos),
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

void fluidTick(World &world, Pos pos, const BlockState &blockState,
               const FluidState &state) {
  FluidState current = state;
  BlockState currentBlock = blockState;
  if (!current.isSource()) {
    const FluidState newFluidState =
        fluidNewLiquid(world, pos, world.getBlock(pos), current.getType());
    if (newFluidState.isEmpty()) {
      current = newFluidState;
      currentBlock = Blocks::AIR.defaultState();
      world.setBlock(pos, currentBlock, 3);
    } else if (!(newFluidState == current)) {
      current = newFluidState;
      currentBlock = newFluidState.getType().createLegacyBlock(current);
      world.setBlock(pos, currentBlock, 3);
      world.scheduleFluidTick(pos, FluidRef(current.getType()),
                              tickDelay(current.getType()));
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
