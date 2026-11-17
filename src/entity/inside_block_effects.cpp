#include "entity/internal.hpp"
#include "redstone/fluid_flow.hpp"
#include "redstone/redstone.hpp"
#include <array>
#include <cmath>

namespace redstone {
namespace {

void applyBuiltinEffect(InsideBlockEffectType type, Entity &entity,
                        World &world) {
  switch (type) {
  case InsideBlockEffectType::Freeze:
    entity.isInPowderSnow = true;
    if (entity.canFreeze())
      entity.setTicksFrozen(
          std::min(entity.ticksRequiredToFreeze(), entity.ticksFrozen() + 1));
    break;
  case InsideBlockEffectType::ClearFreeze:
    entity.clearFreeze();
    break;
  case InsideBlockEffectType::FireIgnite:
    if (!entity.fireImmune()) {
      if (entity.remainingFireTicks < 0)
        ++entity.remainingFireTicks;
      if (entity.remainingFireTicks >= 0)
        entity.igniteForSeconds(8.0f);
    }
    break;
  case InsideBlockEffectType::LavaIgnite:
    entity.lavaIgnite(world);
    break;
  case InsideBlockEffectType::Extinguish:
    entity.clearFire();
    break;
  }
}

std::array<int, 3> axisStepOrder(Vec3 movement) {
  const double x = std::abs(movement.x);
  const double y = std::abs(movement.y);
  const double z = std::abs(movement.z);
  if (x < z) {
    if (y < x)
      return {1, 0, 2};
    if (z < y)
      return {0, 2, 1};
    return {0, 1, 2};
  }
  if (y < z)
    return {1, 2, 0};
  if (x < y)
    return {2, 0, 1};
  return {2, 1, 0};
}

template <class F>
void forEachBetweenCornersInDirection(int firstX, int firstY, int firstZ,
                                      int secondX, int secondY, int secondZ,
                                      Vec3 direction, F &&visit) {
  const int minX = std::min(firstX, secondX), minY = std::min(firstY, secondY),
            minZ = std::min(firstZ, secondZ);
  const int maxX = std::max(firstX, secondX), maxY = std::max(firstY, secondY),
            maxZ = std::max(firstZ, secondZ);
  const int diff[3] = {maxX - minX, maxY - minY, maxZ - minZ};
  const int start[3] = {direction.x >= 0.0 ? minX : maxX,
                        direction.y >= 0.0 ? minY : maxY,
                        direction.z >= 0.0 ? minZ : maxZ};
  const auto order = axisStepOrder(direction);
  const double d[3] = {direction.x, direction.y, direction.z};
  const int stepSign[3] = {d[order[0]] >= 0.0 ? 1 : -1,
                           d[order[1]] >= 0.0 ? 1 : -1,
                           d[order[2]] >= 0.0 ? 1 : -1};
  const int maxI0 = diff[order[0]], maxI1 = diff[order[1]],
            maxI2 = diff[order[2]];
  for (int i0 = 0; i0 <= maxI0; ++i0) {
    for (int i1 = 0; i1 <= maxI1; ++i1) {
      for (int i2 = 0; i2 <= maxI2; ++i2) {
        int coord[3] = {start[0], start[1], start[2]};
        coord[order[0]] += i0 * stepSign[0];
        coord[order[1]] += i1 * stepSign[1];
        coord[order[2]] += i2 * stepSign[2];
        if (!visit(Pos{coord[0], coord[1], coord[2]}))
          return;
      }
    }
  }
}

template <class F>
void forEachBetweenCornersInDirection(const Aabb &aabb, Vec3 direction,
                                      F &&visit) {
  forEachBetweenCornersInDirection(
      floorToInt(aabb.minX), floorToInt(aabb.minY), floorToInt(aabb.minZ),
      floorToInt(aabb.maxX), floorToInt(aabb.maxY), floorToInt(aabb.maxZ),
      direction, std::forward<F>(visit));
}

bool clipUnitBox(int bx, int by, int bz, Vec3 from, Vec3 to, Vec3 &hitOut) {
  double tmin = 0.0, tmax = 1.0;
  const double lo[3] = {double(bx), double(by), double(bz)};
  const double hi[3] = {double(bx + 1), double(by + 1), double(bz + 1)};
  const double s[3] = {from.x, from.y, from.z};
  const double d[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
  for (int axis = 0; axis < 3; ++axis) {
    if (std::abs(d[axis]) < 1.0e-7) {
      if (s[axis] < lo[axis] || s[axis] > hi[axis])
        return false;
      continue;
    }
    double a = (lo[axis] - s[axis]) / d[axis];
    double b = (hi[axis] - s[axis]) / d[axis];
    if (a > b)
      std::swap(a, b);
    tmin = std::max(tmin, a);
    tmax = std::min(tmax, b);
    if (tmin > tmax)
      return false;
  }
  hitOut = from + (to - from) * tmin;
  return true;
}

std::array<int, 3> furthestCorner(Vec3 direction) {
  const double xDot = std::abs(direction.x);
  const double yDot = std::abs(direction.y);
  const double zDot = std::abs(direction.z);
  const int xSign = direction.x >= 0.0 ? 1 : -1;
  const int ySign = direction.y >= 0.0 ? 1 : -1;
  const int zSign = direction.z >= 0.0 ? 1 : -1;
  if (xDot <= yDot && xDot <= zDot)
    return {-xSign, -zSign, ySign};
  return yDot <= zDot ? std::array<int, 3>{zSign, -ySign, -xSign}
                      : std::array<int, 3>{-ySign, xSign, -zSign};
}

double frac(double v) { return v - std::floor(v); }
int signOf(double v) { return v == 0.0 ? 0 : (v > 0.0 ? 1 : -1); }

std::uint64_t packBlockPos(Pos p) {
  return (static_cast<std::uint64_t>(p.x) & 0x3FFFFFFull) << 38 |
         (static_cast<std::uint64_t>(p.z) & 0x3FFFFFFull) << 12 |
         (static_cast<std::uint64_t>(p.y) & 0xFFFull);
}

template <class Visitor>
bool forEachBlockIntersectedBetween(Vec3 from, Vec3 to, const Aabb &aabbAtTarget,
                                    Visitor &&visitor) {
  const Vec3 travel = to - from;
  if (travel.lengthSqr() < 1.0e-10) {
    const int x0 = floorToInt(aabbAtTarget.minX),
              y0 = floorToInt(aabbAtTarget.minY),
              z0 = floorToInt(aabbAtTarget.minZ);
    const int x1 = floorToInt(aabbAtTarget.maxX),
              y1 = floorToInt(aabbAtTarget.maxY),
              z1 = floorToInt(aabbAtTarget.maxZ);
    for (int x = x0; x <= x1; ++x)
      for (int y = y0; y <= y1; ++y)
        for (int z = z0; z <= z1; ++z)
          if (!visitor(Pos{x, y, z}, 0))
            return false;
    return true;
  }

  std::unordered_set<std::uint64_t> sweptVisited;
  bool stopped = false;
  forEachBetweenCornersInDirection(
      aabbAtTarget.move(-travel.x, -travel.y, -travel.z), travel,
      [&](Pos p) {
        if (!visitor(p, 0)) {
          stopped = true;
          return false;
        }
        sweptVisited.insert(packBlockPos(p));
        return true;
      });
  if (stopped)
    return false;

  const double sizeX = aabbAtTarget.getXsize();
  const double sizeY = aabbAtTarget.getYsize();
  const double sizeZ = aabbAtTarget.getZsize();
  const auto cornerDir = furthestCorner(travel);
  const Vec3 toCenter = aabbAtTarget.getCenter();
  const Vec3 toCorner{toCenter.x + sizeX * 0.5 * cornerDir[0],
                      toCenter.y + sizeY * 0.5 * cornerDir[1],
                      toCenter.z + sizeZ * 0.5 * cornerDir[2]};
  const Vec3 fromCorner = toCorner - travel;
  int cx = floorToInt(fromCorner.x), cy = floorToInt(fromCorner.y),
      cz = floorToInt(fromCorner.z);
  const int sx = signOf(travel.x), sy = signOf(travel.y),
            sz = signOf(travel.z);
  const double inf = std::numeric_limits<double>::max();
  const double tdX = sx == 0 ? inf : sx / travel.x;
  const double tdY = sy == 0 ? inf : sy / travel.y;
  const double tdZ = sz == 0 ? inf : sz / travel.z;
  double tX = tdX * (sx > 0 ? 1.0 - frac(fromCorner.x) : frac(fromCorner.x));
  double tY = tdY * (sy > 0 ? 1.0 - frac(fromCorner.y) : frac(fromCorner.y));
  double tZ = tdZ * (sz > 0 ? 1.0 - frac(fromCorner.z) : frac(fromCorner.z));

  int iterations = 0;
  while (tX <= 1.0 || tY <= 1.0 || tZ <= 1.0) {
    if (tX < tY) {
      if (tX < tZ) {
        cx += sx;
        tX += tdX;
      } else {
        cz += sz;
        tZ += tdZ;
      }
    } else if (tY < tZ) {
      cy += sy;
      tY += tdY;
    } else {
      cz += sz;
      tZ += tdZ;
    }

    Vec3 hitPoint;
    if (clipUnitBox(cx, cy, cz, fromCorner, toCorner, hitPoint)) {
      ++iterations;
      const double chx = std::clamp(hitPoint.x, cx + 1.0e-5, cx + 1.0 - 1.0e-5);
      const double chy = std::clamp(hitPoint.y, cy + 1.0e-5, cy + 1.0 - 1.0e-5);
      const double chz = std::clamp(hitPoint.z, cz + 1.0e-5, cz + 1.0 - 1.0e-5);
      const int ox = floorToInt(chx - sizeX * cornerDir[0]);
      const int oy = floorToInt(chy - sizeY * cornerDir[1]);
      const int oz = floorToInt(chz - sizeZ * cornerDir[2]);
      const int curIter = iterations;
      forEachBetweenCornersInDirection(
          cx, cy, cz, ox, oy, oz, travel, [&](Pos p) {
            if (sweptVisited.insert(packBlockPos(p)).second) {
              if (!visitor(p, curIter)) {
                stopped = true;
                return false;
              }
            }
            return true;
          });
      if (stopped)
        return false;
    }
  }

  forEachBetweenCornersInDirection(aabbAtTarget, travel, [&](Pos p) {
    if (sweptVisited.insert(packBlockPos(p)).second) {
      if (!visitor(p, iterations + 1)) {
        stopped = true;
        return false;
      }
    }
    return true;
  });
  return !stopped;
}

bool collidedAlongVector(const Aabb &fromBox, Vec3 vector,
                         const Aabb &shapePart) {
  const Vec3 from = fromBox.getCenter();
  const Vec3 to = from + vector;
  const Aabb inflated = shapePart.inflate(fromBox.getXsize() * 0.5 - 1.0e-7,
                                          fromBox.getYsize() * 0.5 - 1.0e-7,
                                          fromBox.getZsize() * 0.5 - 1.0e-7);
  if (inflated.contains(to) || inflated.contains(from))
    return true;
  return rayBox(from, to,
                {inflated.minX, inflated.minY, inflated.minZ, inflated.maxX,
                 inflated.maxY, inflated.maxZ});
}

void applyBlockEntityInside(World &world, Pos pos, const BlockState &state,
                            Entity &entity,
                            InsideBlockEffectApplier &collector,
                            bool isPrecise) {
  if (state.is(Blocks::LAVA)) {
    collector.apply(InsideBlockEffectType::LavaIgnite);
    entity.lavaHurt(world);
    return;
  }
  if (state.is(Blocks::FIRE)) {
    collector.apply(InsideBlockEffectType::FireIgnite);
    entity.applyFireBlockEffect(world, 1.0f);
    return;
  }
  if (state.is(Blocks::SOUL_FIRE)) {
    collector.apply(InsideBlockEffectType::FireIgnite);
    entity.applyFireBlockEffect(world, 2.0f);
    return;
  }
  if ((state.is(Blocks::CAMPFIRE) || state.is(Blocks::SOUL_CAMPFIRE)) &&
      state.getView("lit") == "true") {
    entity.hurt(world, DamageKind::Fire,
                state.is(Blocks::SOUL_CAMPFIRE) ? 2.0f : 1.0f);
    return;
  }
  if (state.is(Blocks::WATER_CAULDRON)) {
    int level = 0;
    try {
      level = std::stoi(state.get("level", "0"));
    } catch (...) {
      level = 0;
    }
    if (level > 0 && isPrecise) {
      collector.apply(InsideBlockEffectType::Extinguish);
    }
    return;
  }
  if (state.is(Blocks::POWDER_SNOW)) {
    if (!entity.isLiving() || entity.getInBlockState(world).is(Blocks::POWDER_SNOW)) {
      entity.makeStuckInBlock(state, {0.9, 1.5, 0.9});
    }
    collector.apply(InsideBlockEffectType::Freeze);
    collector.apply(InsideBlockEffectType::Extinguish);
    return;
  }
  if (state.is(Blocks::COBWEB)) {
    entity.makeStuckInBlock(state, {0.25, 0.05, 0.25});
    return;
  }
  if (state.is(Blocks::SWEET_BERRY_BUSH)) {
    if (entity.isLiving())
      entity.makeStuckInBlock(state, {0.8, 0.75, 0.8});
    return;
  }
  if (state.is(Blocks::CACTUS)) {
    entity.hurt(world, DamageKind::Generic, 1.0f);
    return;
  }
  if (state.is(Blocks::BUBBLE_COLUMN) && isPrecise) {
    const BlockState above = world.getBlock(pos.relative(Direction::Up));
    const bool nothingAbove =
        collisionShape(above).empty() && fluidStateOf(above).isEmpty() &&
        !isWaterloggedHost(above);
    const bool dragDown = state.getView("drag") != "false";
    if (nothingAbove)
      entity.onAboveBubbleColumn(world, dragDown, pos);
    else
      entity.onInsideBubbleColumn(world, dragDown);
    return;
  }
  if (state.is(Blocks::NETHER_PORTAL) && entity.canUsePortal(false)) {
    entity.setAsInsidePortal(pos);
    return;
  }
  if (state.is(Blocks::END_PORTAL) && entity.canUsePortal(false)) {
    if (!entity.isOnPortalCooldown()) {
      entity.setPortalCooldown();
      world.queuePortalTransfer(entity, pos);
    }
    return;
  }
  if (state.is(Blocks::DETECTOR_RAIL)) {
    railEntityInside(world, pos, state, entity);
    return;
  }
  if (state.is(Blocks::HOPPER)) {
    hopperEntityInside(world, pos, state, entity);
    return;
  }
  if (state.block().isPressurePlate()) {
    pressurePlateEntityInside(world, pos, state, entity);
    return;
  }
  if (state.is(Blocks::TRIPWIRE)) {
    tripWireEntityInside(world, pos, state, entity);
    return;
  }
}

void applyFluidEntityInside(const FluidState &fs, Entity &entity,
                            InsideBlockEffectApplier &collector) {
  if (fs.isEmpty())
    return;
  if (fluidHasTag(fs.getType(), FluidTag::Lava)) {
    collector.apply(InsideBlockEffectType::LavaIgnite);
  } else if (fluidHasTag(fs.getType(), FluidTag::Water)) {
    entity.resetFallDistance();
    collector.apply(InsideBlockEffectType::Extinguish);
  }
}

} // namespace

void StepBasedInsideBlockEffectApplier::advanceStep(int step) {
  if (lastStep_ != step) {
    lastStep_ = step;
    flushStep();
  }
}

void StepBasedInsideBlockEffectApplier::applyAndClear(Entity &entity,
                                                      World &world) {
  flushStep();
  for (const auto &pending : finalEffects_) {
    if (!entity.isAlive())
      break;
    if (pending.isBuiltin)
      applyBuiltinEffect(pending.builtin, entity, world);
    else if (pending.callback)
      pending.callback(entity, world);
  }
  finalEffects_.clear();
  lastStep_ = -1;
}

void StepBasedInsideBlockEffectApplier::apply(InsideBlockEffectType type) {
  effectsInStep_[static_cast<std::size_t>(type)] = true;
}

void StepBasedInsideBlockEffectApplier::runBefore(InsideBlockEffectType type,
                                                  Callback effect) {
  beforeEffectsInStep_[static_cast<std::size_t>(type)].push_back(
      std::move(effect));
}

void StepBasedInsideBlockEffectApplier::runAfter(InsideBlockEffectType type,
                                                 Callback effect) {
  afterEffectsInStep_[static_cast<std::size_t>(type)].push_back(
      std::move(effect));
}

void StepBasedInsideBlockEffectApplier::flushStep() {
  for (std::size_t i = 0; i < kTypeCount; ++i) {
    const auto type = static_cast<InsideBlockEffectType>(i);
    for (auto &cb : beforeEffectsInStep_[i])
      finalEffects_.push_back({false, type, std::move(cb)});
    beforeEffectsInStep_[i].clear();
    if (effectsInStep_[i]) {
      effectsInStep_[i] = false;
      finalEffects_.push_back({true, type, {}});
    }
    for (auto &cb : afterEffectsInStep_[i])
      finalEffects_.push_back({false, type, std::move(cb)});
    afterEffectsInStep_[i].clear();
  }
}

int Entity::checkInsideBlocksSegment(
    World &world, Vec3 from, Vec3 to, InsideBlockEffectApplier &collector,
    std::unordered_set<std::uint64_t> &visitedBlocks, int maxIterations) {
  const Aabb deflated = dimensions_.makeBoundingBox(to).deflate(1.0e-5);
  const bool movedFar =
      (to - from).lengthSqr() > 0.9999900000002526 * 0.9999900000002526;
  int maxSeenIteration = 0;
  forEachBlockIntersectedBetween(
      from, to, deflated, [&](Pos pos, int iteration) -> bool {
        if (!isAlive() || iteration >= maxIterations)
          return false;
        maxSeenIteration = iteration;
        if (!world.isChunkLoaded(blockToSection(pos.x), blockToSection(pos.z)))
          return true;
        const BlockState state = world.getBlock(pos);
        const FluidState fs = resolveEntityFluidState(state);
        if (state.isAir() && fs.isEmpty())
          return true;

        const Aabb cellBox{double(pos.x),     double(pos.y),     double(pos.z),
                           double(pos.x + 1), double(pos.y + 1), double(pos.z + 1)};
        const Aabb fromBox = dimensions_.makeBoundingBox(from);
        const Vec3 travel = to - from;

        bool insideBlock = !state.isAir() && collidedAlongVector(fromBox, travel, cellBox);
        bool insideFluid = false;
        if (!fs.isEmpty()) {
          const float fh = redstone::fluidHeight(world, pos, fs);
          if (fh > 0.0f) {
            const Aabb fluidBox{double(pos.x),     double(pos.y),
                                double(pos.z),     double(pos.x + 1),
                                double(pos.y) + fh, double(pos.z + 1)};
            insideFluid = collidedAlongVector(fromBox, travel, fluidBox);
          }
        }
        if ((insideBlock || insideFluid) &&
            visitedBlocks.insert(packBlockPos(pos)).second) {
          auto *stepCollector =
              dynamic_cast<StepBasedInsideBlockEffectApplier *>(&collector);
          if (insideBlock) {
            const bool isPrecise = movedFar || deflated.intersects(pos);
            if (stepCollector)
              stepCollector->advanceStep(iteration);
            applyBlockEntityInside(world, pos, state, *this, collector,
                                   isPrecise);
          }
          if (insideFluid) {
            if (stepCollector)
              stepCollector->advanceStep(iteration);
            applyFluidEntityInside(fs, *this, collector);
          }
        }
        return true;
      });
  return maxSeenIteration + 1;
}

void Entity::applyEffectsFromBlocksList(
    World &world, const std::vector<Movement> &movements) {
  if (isRemoved() || noPhysics ||
      !world.isChunkLoaded(blockToSection(position.x), blockToSection(position.z)))
    return;
  if (onGround) {
    const Pos effectPos = getOnPosLegacy(world);
    const BlockState effectState = world.getBlock(effectPos);
    if (effectState.is(Blocks::SLIME_BLOCK) && !isDiscrete() &&
        std::abs(deltaMovement.y) < 0.1) {
      const double scale = 0.4 + std::abs(deltaMovement.y) * 0.2;
      deltaMovement.x *= scale;
      deltaMovement.z *= scale;
    }
  }
  const int prevFireTicks = remainingFireTicks;
  std::unordered_set<std::uint64_t> visitedBlocks;
  for (const auto &mv : movements) {
    Vec3 cur = mv.from;
    const Vec3 delta = mv.to - mv.from;
    int maxIter = 16;
    if (mv.axisDependentOriginalMovement.has_value() &&
        delta.lengthSqr() > 0.0) {
      const double dc[3] = {delta.x, delta.y, delta.z};
      for (int axis : axisStepOrder(*mv.axisDependentOriginalMovement)) {
        if (dc[axis] != 0.0) {
          Vec3 next = cur;
          if (axis == 0)
            next.x += dc[0];
          else if (axis == 1)
            next.y += dc[1];
          else
            next.z += dc[2];
          maxIter -= checkInsideBlocksSegment(world, cur, next,
                                              insideEffectCollector_,
                                              visitedBlocks, maxIter);
          cur = next;
        }
      }
    } else {
      maxIter -= checkInsideBlocksSegment(world, mv.from, mv.to,
                                          insideEffectCollector_,
                                          visitedBlocks, 16);
    }
    if (maxIter <= 0) {
      checkInsideBlocksSegment(world, mv.to, mv.to, insideEffectCollector_,
                               visitedBlocks, 1);
    }
  }
  insideEffectCollector_.applyAndClear(*this, world);
  const bool wasIgnitedThisTick = remainingFireTicks > prevFireTicks;
  if (!isOnFire() && !wasIgnitedThisTick)
    remainingFireTicks = -getFireImmuneTicks();
}

void Entity::applyEffectsFromBlocks(World &world) {
  finalMovementsThisTick_.clear();
  finalMovementsThisTick_ = std::move(movementThisTick_);
  movementThisTick_.clear();
  if (finalMovementsThisTick_.empty()) {
    finalMovementsThisTick_.push_back(Movement{oldPosition, position, std::nullopt});
  } else if ((finalMovementsThisTick_.back().to - position).lengthSqr() >
             9.9999994e-11) {
    finalMovementsThisTick_.push_back(
        Movement{finalMovementsThisTick_.back().to, position, std::nullopt});
  }
  applyEffectsFromBlocksList(world, finalMovementsThisTick_);
}

void Entity::applyEffectsFromBlocks(World &world, Vec3 from, Vec3 to) {
  applyEffectsFromBlocksList(world, {Movement{from, to, std::nullopt}});
}

} // namespace redstone
