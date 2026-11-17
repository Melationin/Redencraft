#include "entity/internal.hpp"
#include "redstone/item.hpp"

namespace redstone {
std::string blockItem(const BlockState &state) {
  const std::string_view name = state.name();
  if (name == "minecraft:air" || name == "minecraft:cave_air" ||
      name == "minecraft:void_air" || name == "minecraft:water" ||
      name == "minecraft:lava" || name == "minecraft:fire" ||
      name == "minecraft:soul_fire" || name == "minecraft:moving_piston" ||
      name == "minecraft:piston_head" || name == "minecraft:nether_portal" ||
      name == "minecraft:end_portal" || name == "minecraft:end_gateway" ||
      name == "minecraft:frosted_ice" || name == "minecraft:bubble_column" ||
      name == "minecraft:powder_snow" || name == "minecraft:melon_stem" ||
      name == "minecraft:pumpkin_stem" ||
      name == "minecraft:attached_melon_stem" ||
      name == "minecraft:attached_pumpkin_stem")
    return {};
  if (const Item *item = itemForBlockState(state)) {
    return item->id() == 0 ? std::string{} : item->name();
  }
  return {};
}
std::vector<ItemStack> blockDrops(const BlockState &state) {
  const auto name = state.name();
  if (name == "minecraft:air" || name == "minecraft:cave_air" ||
      name == "minecraft:void_air" || name == "minecraft:water" ||
      name == "minecraft:lava" || name == "minecraft:moving_piston" ||
      name == "minecraft:piston_head" || name == "minecraft:bedrock")
    return {};
  if (name == "minecraft:glass" || isDeadCoralFan(state))
    return {}; // Silk Touch only; the empty tool used here never has it.
  if (name == "minecraft:tnt" &&
      unstableOf(state))
    return {};
  if (name == "minecraft:grass_block")
    return {itemStack("minecraft:dirt", 1)};
  auto item = blockItem(state);
  return item.empty() ? std::vector<ItemStack>{}
                      : std::vector<ItemStack>{itemStack(item, 1)};
}
bool isReplaceable(const BlockState &state) {
  return air(state) || state.is(Blocks::WATER) || state.is(Blocks::LAVA) ||
         state.is(Blocks::SHORT_GRASS) || state.is(Blocks::FERN) ||
         state.is(Blocks::DEAD_BUSH) || state.is(Blocks::BUSH) ||
         state.is(Blocks::SHORT_DRY_GRASS) || state.is(Blocks::TALL_DRY_GRASS) ||
         state.is(Blocks::SEAGRASS) || state.is(Blocks::TALL_SEAGRASS);
}
bool isAirState(const BlockState &state) { return air(state); }

Entity::Entity(std::string type)
    : type_(entityType(type)),
      entityData_([this](std::uint8_t id) { onSyncedDataUpdated(id); }) {
  if (!type_)
    throw std::invalid_argument("unregistered entity type: " + type);
  dimensions_ = type_->dimensions();
  eyeHeight_ = dimensions_.eyeHeight;
  entityData_.define(kDataSharedFlagsId, EntityDataSerializer::Byte,
                     std::int8_t{0});
  entityData_.define(kDataAirSupplyId, EntityDataSerializer::Int,
                     maxAirSupply());
  entityData_.define(kDataCustomNameId, EntityDataSerializer::OptionalComponent,
                     std::optional<std::string>{});
  entityData_.define(kDataCustomNameVisibleId, EntityDataSerializer::Boolean,
                     false);
  entityData_.define(kDataSilentId, EntityDataSerializer::Boolean, false);
  entityData_.define(kDataNoGravityId, EntityDataSerializer::Boolean, false);
  entityData_.define(kDataPoseId, EntityDataSerializer::Pose, Pose::Standing);
  entityData_.define(kDataTicksFrozenId, EntityDataSerializer::Int, 0);
}
EntityDimensions Entity::getDimensions(Pose) const {
  return type_->dimensions();
}
void Entity::onSyncedDataUpdated(std::uint8_t id) {
  if (id == kDataPoseId)
    refreshDimensions();
}
void Entity::refreshDimensions() {
  const Pose p = pose();
  const EntityDimensions newDim = getDimensions(p);
  dimensions_ = newDim;
  eyeHeight_ = newDim.eyeHeight;
  reapplyPosition();
}
void Entity::refreshDimensions(World &world) {
  const EntityDimensions oldDim = dimensions_;
  const Pose p = pose();
  const EntityDimensions newDim = getDimensions(p);
  dimensions_ = newDim;
  eyeHeight_ = newDim.eyeHeight;
  reapplyPosition();
  const bool isSmall = newDim.width <= 4.0f && newDim.height <= 4.0f;
  if (!firstTick_ && !noPhysics && isSmall &&
      (newDim.width > oldDim.width || newDim.height > oldDim.height) &&
      !isPlayer()) {
    fudgePositionAfterSizeChange(world, oldDim);
  }
}
bool Entity::fudgePositionAfterSizeChange(World &world,
                                          const EntityDimensions &previous) {
  const EntityDimensions newDim = getDimensions(pose());
  const Vec3 oldCenter =
      position + Vec3{0.0, static_cast<double>(previous.height) * 0.5, 0.0};
  const double widthDelta =
      std::max(0.0f, newDim.width - previous.width) + 1.0e-6;
  const double heightDelta =
      std::max(0.0f, newDim.height - previous.height) + 1.0e-6;
  const Aabb allowed =
      Aabb::ofSize(oldCenter, widthDelta, heightDelta, widthDelta);
  if (auto free = world.findFreePosition(this, allowed, oldCenter, newDim.width,
                                         newDim.height, newDim.width)) {
    setPos(*free + Vec3{0.0, -static_cast<double>(newDim.height) * 0.5, 0.0});
    return true;
  }
  if (newDim.width > previous.width && newDim.height > previous.height) {
    const Aabb allowedIgnoreY =
        Aabb::ofSize(oldCenter, widthDelta, 1.0e-6, widthDelta);
    if (auto free = world.findFreePosition(this, allowedIgnoreY, oldCenter,
                                           newDim.width, previous.height,
                                           newDim.width)) {
      setPos(*free + Vec3{0.0,
                          -static_cast<double>(previous.height) * 0.5 + 1.0e-6,
                          0.0});
      return true;
    }
  }
  return false;
}
bool Entity::getSharedFlag(int flag) const {
  return (entityData_.get<std::int8_t>(kDataSharedFlagsId) &
          (1 << flag)) != 0;
}
void Entity::setSharedFlag(int flag, bool value) {
  const std::int8_t prev = entityData_.get<std::int8_t>(kDataSharedFlagsId);
  const std::int8_t next =
      value ? static_cast<std::int8_t>(prev | (1 << flag))
            : static_cast<std::int8_t>(prev & ~(1 << flag));
  entityData_.set(kDataSharedFlagsId, next);
}
void Entity::discard() {
  removed_ = true;
  unRide();
  syncSectionRemoval();
}
bool Entity::fireImmune() const { return type_->fireImmune; }
bool Entity::hurt(World &, DamageKind, float) { return false; }
bool Entity::causeFallDamage(World &, double, float, DamageKind) {
  return false;
}
void Entity::checkFallDamage(World &world, double ya, bool onGroundNow) {
  if (onGroundNow) {
    if (fallDistance > 0.0) {
      const Pos onPos = getOnPosLegacy(world);
      const BlockState onState = world.getBlock(onPos);
      float multiplier = 1.0f;
      if (onState.is(Blocks::SLIME_BLOCK) && !isDiscrete())
        multiplier = 0.0f;
      else if (onState.is(Blocks::HAY_BLOCK))
        multiplier = 0.2f;
      else if (onState.block().isBed())
        multiplier = 0.5f;
      else if (onState.is(Blocks::POINTED_DRIPSTONE) &&
               onState.get("vertical_direction") == "up" &&
               onState.get("thickness") == "tip") {
        causeFallDamage(world, fallDistance + 2.5, 2.0f,
                        DamageKind::FallingStalactite);
        resetFallDistance();
        return;
      }
      causeFallDamage(world, fallDistance, multiplier, DamageKind::Fall);
    }
    resetFallDistance();
  } else if (ya < 0.0) {
    fallDistance -= ya;
  }
}
bool Entity::ignoreExplosion(World &) const { return false; }
Aabb Entity::boundingBox() const {
  return dimensions_.makeBoundingBox(position);
}
double Entity::distanceToSqr(const Vec3 &to) const {
  Vec3 d = position - to;
  return d.lengthSqr();
}
void Entity::setPos(double x, double y, double z) {
  position = {x, y, z};
  syncSectionBoundingBox();
}
void Entity::kill(World &) { discard(); }
void Entity::checkBelowWorld(World &world) {
  if (position.y < world.minY() - 64.0)
    discard();
}
void Entity::push(const Vec3 &value) { push(value.x, value.y, value.z); }
void Entity::push(Entity &entity) {
  if (!entity.noPhysics && !noPhysics) {
    double xa = entity.position.x - position.x;
    double za = entity.position.z - position.z;
    double dd = std::max(std::abs(xa), std::abs(za));
    if (dd >= 0.01f) {
      dd = std::sqrt(dd);
      xa /= dd;
      za /= dd;
      double pow = 1.0 / dd;
      if (pow > 1)
        pow = 1;
      xa *= pow * 0.05f;
      za *= pow * 0.05f;
      if (isPushable())
        push(-xa, 0, -za);
      if (entity.isPushable())
        entity.push(xa, 0, za);
    }
  }
}
void Entity::igniteForTicks(int ticks) {
  if (remainingFireTicks < ticks)
    remainingFireTicks = ticks;
}
void Entity::igniteForSeconds(float seconds) {
  igniteForTicks(static_cast<int>(std::floor(seconds * 20.0f)));
}
void Entity::clearFire() { remainingFireTicks = std::min(0, remainingFireTicks); }
void Entity::lavaIgnite(World &world) {
  (void)world;
  if (!fireImmune())
    igniteForSeconds(15.0f);
}
void Entity::lavaHurt(World &world) {
  if (fireImmune())
    return;
  igniteForSeconds(15.0f);
  hurt(world, DamageKind::Lava, 4.0f);
}
bool Entity::canUsePortal(bool allowVehicles) const {
  (void)allowVehicles;
  return !isRemoved();
}
void Entity::processPortalCooldown() {
  if (portalCooldown > 0)
    --portalCooldown;
}
void Entity::setAsInsidePortal(Pos pos) {
  if (isOnPortalCooldown()) {
    setPortalCooldown();
    return;
  }
  if (!portalProcess_.active) {
    portalProcess_.active = true;
    portalProcess_.entryPosition = pos;
    portalProcess_.portalTime = 0;
    portalProcess_.insidePortalThisTick = true;
  } else if (!portalProcess_.insidePortalThisTick) {
    portalProcess_.entryPosition = pos;
    portalProcess_.insidePortalThisTick = true;
  }
}
void Entity::handlePortal(World &world) {
  processPortalCooldown();
  if (!portalProcess_.active)
    return;
  const bool allowed = canUsePortal(false);
  bool ready = false;
  if (!portalProcess_.insidePortalThisTick) {
    portalProcess_.portalTime = std::max(portalProcess_.portalTime - 4, 0);
  } else {
    portalProcess_.insidePortalThisTick = false;
    // Non-player entities have transition time 0 in NetherPortalBlock.
    ready = allowed && portalProcess_.portalTime++ >= 0;
  }
  if (ready) {
    setPortalCooldown();
    world.queuePortalTransfer(*this, portalProcess_.entryPosition);
  } else if (!portalProcess_.insidePortalThisTick &&
             portalProcess_.portalTime <= 0) {
    portalProcess_ = {};
  }
}
void Entity::applyFireBlockEffect(World &world, float fireDamage) {
  if (!fireImmune()) {
    if (remainingFireTicks < 0)
      ++remainingFireTicks;
    if (remainingFireTicks >= 0)
      igniteForSeconds(8.0f);
  }
  hurt(world, DamageKind::Fire, fireDamage);
}
void Entity::baseTick(World &world) {
  inBlockState_ = std::nullopt;
  if (isPassenger()) {
    auto v = getVehicle();
    if (!v || v->isRemoved())
      stopRiding();
  }
  if (boardingCooldown > 0)
    --boardingCooldown;
  handlePortal(world);
  wasInPowderSnow = isInPowderSnow;
  isInPowderSnow = false;
  wasEyeInWater = isEyeInWater();
  updateFluidInteraction(world);
  updateSwimming();
  if (remainingFireTicks > 0) {
    if (fireImmune())
      clearFire();
    else {
      if (remainingFireTicks % 20 == 0 && !isInLava())
        hurt(world, DamageKind::Fire, 1.0f);
      --remainingFireTicks;
    }
  }
  if (isInLava())
    fallDistance *= 0.5;
  checkBelowWorld(world);
  setSharedFlagOnFire(remainingFireTicks > 0);
  firstTick_ = false;
}
Vec3 Entity::limitPistonMovement(World &world, Vec3 movement) {
  if (movement.lengthSqr() <= 1.0e-7)
    return movement;
  std::int64_t time = world.gameTime();
  if (time != pistonDeltasGameTime_) {
    pistonDeltas_.fill(0.0);
    pistonDeltasGameTime_ = time;
  }
  auto restrict = [&](int axis, double amount) {
    double next =
        std::clamp(amount + pistonDeltas_[axis], -0.51, 0.51);
    amount = next - pistonDeltas_[axis];
    pistonDeltas_[axis] = next;
    return amount;
  };
  if (movement.x != 0.0) {
    double x = restrict(0, movement.x);
    return std::abs(x) <= 1.0e-5 ? Vec3{} : Vec3{x, 0, 0};
  }
  if (movement.y != 0.0) {
    double y = restrict(1, movement.y);
    return std::abs(y) <= 1.0e-5 ? Vec3{} : Vec3{0, y, 0};
  }
  if (movement.z != 0.0) {
    double z = restrict(2, movement.z);
    return std::abs(z) <= 1.0e-5 ? Vec3{} : Vec3{0, 0, z};
  }
  return {};
}
void Entity::setOnGroundWithMovement(World &world, bool ground, Vec3 movement) {
  setOnGroundWithMovement(world, ground, horizontalCollision, movement);
}
void Entity::setOnGroundWithMovement(World &world, bool ground,
                                     bool horizontalCollided, Vec3 movement) {
  onGround = ground;
  horizontalCollision = horizontalCollided;
  checkSupportingBlock(world, ground, movement);
}
void Entity::checkSupportingBlock(World &world, bool ground,
                                  std::optional<Vec3> movement) {
  if (ground) {
    const Aabb box = boundingBox();
    const Aabb testArea{box.minX, box.minY - 1.0e-6, box.minZ,
                        box.maxX, box.minY,          box.maxZ};
    auto supporting = world.findSupportingBlock(*this, testArea);
    if (supporting.has_value() || onGroundNoBlocks_) {
      mainSupportingBlockPos = supporting;
    } else if (movement.has_value()) {
      const Aabb shifted{testArea.minX - movement->x, testArea.minY,
                         testArea.minZ - movement->z,
                         testArea.maxX - movement->x, testArea.maxY,
                         testArea.maxZ - movement->z};
      supporting = world.findSupportingBlock(*this, shifted);
      mainSupportingBlockPos = supporting;
    }
    onGroundNoBlocks_ = !supporting.has_value();
  } else {
    onGroundNoBlocks_ = false;
    if (mainSupportingBlockPos.has_value())
      mainSupportingBlockPos.reset();
  }
}
Pos Entity::getOnPos(World &world, float yOffset) const {
  if (mainSupportingBlockPos.has_value()) {
    const Pos support = *mainSupportingBlockPos;
    const BlockState state = world.getBlock(support);
    const bool tallSupport =
        (yOffset <= 0.5f &&
         world.hasBlockTag(state, "minecraft:fences").value_or(false)) ||
        world.hasBlockTag(state, "minecraft:walls").value_or(false) ||
        world.hasBlockTag(state, "minecraft:fence_gates").value_or(false);
    if (tallSupport)
      return support;
    return {support.x, static_cast<int>(std::floor(position.y - yOffset)),
            support.z};
  }
  return {static_cast<int>(std::floor(position.x)),
          static_cast<int>(std::floor(position.y - yOffset)),
          static_cast<int>(std::floor(position.z))};
}
BlockState Entity::getInBlockState(World &world) {
  if (!inBlockState_.has_value()) {
    inBlockState_ =
        world.getBlock({static_cast<int>(std::floor(position.x)),
                        static_cast<int>(std::floor(position.y)),
                        static_cast<int>(std::floor(position.z))});
  }
  return *inBlockState_;
}
float Entity::getBlockSpeedFactor(World &world) const {
  const Pos feet{static_cast<int>(std::floor(position.x)),
                 static_cast<int>(std::floor(position.y)),
                 static_cast<int>(std::floor(position.z))};
  const BlockState block = world.getBlock(feet);
  const float speedFactor = blockMetadata(block).speedFactor;
  if (!block.is(Blocks::WATER) && !block.is(Blocks::BUBBLE_COLUMN)) {
    return speedFactor == 1.0f
               ? blockMetadata(
                     world.getBlock(getBlockPosBelowThatAffectsMyMovement(world)))
                     .speedFactor
               : speedFactor;
  }
  return speedFactor;
}
void Entity::makeStuckInBlock(const BlockState &, Vec3 multiplier) {
  resetFallDistance();
  stuckSpeedMultiplier_ = multiplier;
}
void Entity::onAboveBubbleColumn(World &world, bool dragDown, Pos) {
  (void)world;
  const double yd = dragDown ? std::max(-0.9, deltaMovement.y - 0.03)
                             : std::min(1.8, deltaMovement.y + 0.1);
  deltaMovement.y = yd;
}
void Entity::onInsideBubbleColumn(World &, bool dragDown) {
  const double yd = dragDown ? std::max(-0.3, deltaMovement.y - 0.03)
                             : std::min(0.7, deltaMovement.y + 0.06);
  deltaMovement.y = yd;
  resetFallDistance();
}
namespace {
Vec3 collideWithShapes(Vec3 movement, const Aabb &box,
                       const std::vector<ShapeBox> &colliders) {
  if (colliders.empty())
    return movement;
  Vec3 resolved;
  auto step = [&](int axis, double value) {
    if (value == 0.0)
      return;
    const double result =
        collideAxis(axis, box.move(resolved.x, resolved.y, resolved.z),
                    colliders, value);
    if (axis == 0)
      resolved.x = result;
    else if (axis == 1)
      resolved.y = result;
    else
      resolved.z = result;
  };
  // Direction.axisStepOrder: Y first, then the larger horizontal component.
  step(1, movement.y);
  if (std::abs(movement.x) < std::abs(movement.z)) {
    step(2, movement.z);
    step(0, movement.x);
  } else {
    step(0, movement.x);
    step(2, movement.z);
  }
  return resolved;
}
std::vector<float> collectCandidateStepUpHeights(
    const Aabb &box, const std::vector<ShapeBox> &colliders,
    float maxStepHeight, float stepHeightToSkip) {
  std::vector<float> candidates;
  candidates.reserve(8);
  for (const auto &c : colliders) {
    for (double coord : {c.y0, c.y1}) {
      const float rel = static_cast<float>(coord - box.minY);
      if (rel >= 0.0f && rel != stepHeightToSkip && rel <= maxStepHeight) {
        if (std::find(candidates.begin(), candidates.end(), rel) ==
            candidates.end())
          candidates.push_back(rel);
      }
    }
  }
  std::sort(candidates.begin(), candidates.end());
  return candidates;
}
} // namespace
Vec3 Entity::collide(World &world, Vec3 movement) {
  const Aabb box = boundingBox();
  const double mx = movement.x;
  double my = movement.y;
  const double mz = movement.z;

  if (my < 0.0) {
    if (auto clippedY = world.tryCollideSupportingBlockY(box, my)) {
      if (*clippedY == 0.0) {
        my = 0.0;
        if (mx == 0.0 && mz == 0.0)
          return {};
      }
    }
  }

  auto &entityColliders = entityColliders_;
  entityColliders.clear();
  world.collectEntityCollisions(this, box.expandTowards(movement),
                                entityColliders);

  auto resolveAxis = [&](int axis, const Aabb &curBox, double dist) -> double {
    if (dist == 0.0)
      return 0.0;
    if (!entityColliders.empty())
      dist = collideAxis(axis, curBox, entityColliders, dist);
    if (std::abs(dist) < EPS)
      return 0.0;
    return world.collideBlocksOnAxis(axis, curBox, dist);
  };

  Vec3 movementStep;
  movementStep.y = (my == 0.0) ? 0.0 : resolveAxis(1, box, my);
  if (std::abs(mx) < std::abs(mz)) {
    movementStep.z = resolveAxis(2, box.move(0.0, movementStep.y, 0.0), mz);
    movementStep.x =
        resolveAxis(0, box.move(0.0, movementStep.y, movementStep.z), mx);
  } else {
    movementStep.x = resolveAxis(0, box.move(0.0, movementStep.y, 0.0), mx);
    movementStep.z =
        resolveAxis(2, box.move(movementStep.x, movementStep.y, 0.0), mz);
  }

  const bool xCollision = movement.x != movementStep.x;
  const bool yCollision = movement.y != movementStep.y;
  const bool zCollision = movement.z != movementStep.z;
  const bool onGroundAfterCollision = yCollision && movement.y < 0.0;
  const float maxStep = maxUpStep();
  if (maxStep > 0.0f && (onGroundAfterCollision || onGround) &&
      (xCollision || zCollision)) {
    const Aabb groundedBox =
        onGroundAfterCollision ? box.move(0.0, movementStep.y, 0.0) : box;
    if (!world.isStepUpFullyBlocked(groundedBox, maxStep, movementStep)) {
      auto &colliders = movementColliders_;
      Aabb stepUpBox =
          groundedBox.expandTowards({movement.x, maxStep, movement.z});
      if (!onGroundAfterCollision)
        stepUpBox = stepUpBox.expandTowards({0.0, -1.0e-5, 0.0});
      world.collectCollisionBoxes(stepUpBox, colliders);
      if (!entityColliders.empty()) {
        world.collectEntityCollisions(this, stepUpBox, colliders);
      }
      const auto candidates = collectCandidateStepUpHeights(
          groundedBox, colliders, maxStep, static_cast<float>(movementStep.y));
      for (float h : candidates) {
        const Vec3 stepFromGround = collideWithShapes(
            {movement.x, static_cast<double>(h), movement.z}, groundedBox,
            colliders);
        if (stepFromGround.horizontalDistanceSqr() >
            movementStep.horizontalDistanceSqr()) {
          const double distToGround = box.minY - groundedBox.minY;
          return stepFromGround - Vec3{0.0, distToGround, 0.0};
        }
      }
    }
  }
  return movementStep;
}
void Entity::move(World &world, MoverType moverType, Vec3 movement) {
  if (noPhysics) {
    setPos(position + movement);
    horizontalCollision = false;
    verticalCollision = false;
    verticalCollisionBelow = false;
    minorHorizontalCollision = false;
    return;
  }
  if (moverType == MoverType::Piston) {
    movement = limitPistonMovement(world, movement);
    if (movement.x == 0 && movement.y == 0 && movement.z == 0)
      return;
  }
  if (stuckSpeedMultiplier_.lengthSqr() > 1.0e-7) {
    if (moverType != MoverType::Piston)
      movement = movement.multiply(stuckSpeedMultiplier_.x,
                                   stuckSpeedMultiplier_.y,
                                   stuckSpeedMultiplier_.z);
    stuckSpeedMultiplier_ = {};
    setDeltaMovement(0.0, 0.0, 0.0);
  }
  const Vec3 resolved = collide(world, movement);
  const double resolvedLenSqr = resolved.lengthSqr();
  if (resolvedLenSqr > EPS || (movement - resolved).lengthSqr() < EPS) {
    if (fallDistance != 0.0 && resolvedLenSqr >= 1.0) {
      const double checkDist = std::min(std::sqrt(resolvedLenSqr), 8.0);
      const Vec3 dir = resolved * (checkDist / std::sqrt(resolvedLenSqr));
      if (world.clip(position, position + dir))
        resetFallDistance();
    }
    const Vec3 oldPos = position;
    const Vec3 nextPos = oldPos + resolved;
    if (movementThisTick_.size() >= 100) {
      const Movement first = movementThisTick_.front();
      movementThisTick_.erase(movementThisTick_.begin());
      const Movement second = movementThisTick_.front();
      movementThisTick_.front() =
          Movement{first.from, second.to, std::nullopt};
    }
    movementThisTick_.push_back(Movement{oldPos, nextPos, movement});
    setPos(nextPos);
  }
  const bool xCollision = std::abs(movement.x - resolved.x) > COLLISION_EPS;
  const bool zCollision = std::abs(movement.z - resolved.z) > COLLISION_EPS;
  horizontalCollision = xCollision || zCollision;
  verticalCollision = std::abs(movement.y - resolved.y) > COLLISION_EPS;
  verticalCollisionBelow = verticalCollision && movement.y < 0;
  setOnGroundWithMovement(world, verticalCollisionBelow, horizontalCollision,
                          resolved);
  checkFallDamage(world, resolved.y, onGround);
  minorHorizontalCollision = false;
  if (horizontalCollision) {
    if (xCollision)
      deltaMovement.x = 0;
    if (zCollision)
      deltaMovement.z = 0;
  }
  if (verticalCollision && movement.y != resolved.y) {
    const Pos effectPos = getOnPosLegacy(world);
    const BlockState effectState = world.getBlock(effectPos);
    if (effectState.is(Blocks::SLIME_BLOCK) && !isDiscrete()) {
      if (deltaMovement.y < 0)
        deltaMovement.y = -deltaMovement.y * 0.8;
    } else {
      deltaMovement.y = 0;
    }
  }
  const float speedFactor = getBlockSpeedFactor(world);
  deltaMovement.x *= speedFactor;
  deltaMovement.z *= speedFactor;
}
void Entity::moveTowardsClosestSpace(World &world, double x, double y,
                                     double z) {
  Pos pos{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
          static_cast<int>(std::floor(z))};
  Vec3 delta{x - pos.x, y - pos.y, z - pos.z};
  Direction closestDirection = Direction::Up;
  double closest = std::numeric_limits<double>::max();
  for (auto direction : {Direction::North, Direction::South, Direction::West,
                         Direction::East, Direction::Up}) {
    auto neighbor = pos.relative(direction);
    if (!world.isLoaded(neighbor) || !fullCube(world, neighbor)) {
      int axis = direction == Direction::North || direction == Direction::South
                     ? 2
                     : direction == Direction::West || direction == Direction::East
                           ? 0
                           : 1;
      double d = axis == 0 ? delta.x : axis == 1 ? delta.y : delta.z;
      bool positive = direction == Direction::South ||
                      direction == Direction::East || direction == Direction::Up;
      double oriented = positive ? 1.0 - d : d;
      if (oriented < closest) {
        closest = oriented;
        closestDirection = direction;
      }
    }
  }
  double speed = world.random.nextFloat() * 0.2 + 0.1;
  Vec3 scaled = deltaMovement * 0.75;
  if (closestDirection == Direction::West)
    deltaMovement = {-speed, scaled.y, scaled.z};
  else if (closestDirection == Direction::East)
    deltaMovement = {speed, scaled.y, scaled.z};
  else if (closestDirection == Direction::Down)
    deltaMovement = {scaled.x, -speed, scaled.z};
  else if (closestDirection == Direction::Up)
    deltaMovement = {scaled.x, speed, scaled.z};
  else if (closestDirection == Direction::North)
    deltaMovement = {scaled.x, scaled.y, -speed};
  else
    deltaMovement = {scaled.x, scaled.y, speed};
}
void EntityFluidTracker::applyCurrentTo(Entity &entity, double scale) const {
  if (currentCount == 0 || accumulatedCurrent.lengthSqr() < 1.0e-5)
    return;
  Vec3 impulse = entity.isPlayer()
                     ? accumulatedCurrent * (1.0 / currentCount)
                     : accumulatedCurrent.normalize();
  const Vec3 oldMovement = entity.deltaMovement;
  impulse = impulse * scale;
  if (std::abs(oldMovement.x) < 0.003 && std::abs(oldMovement.z) < 0.003 &&
      impulse.length() < 0.0045000000000000005) {
    impulse = impulse.normalize() * 0.0045000000000000005;
  }
  entity.addDeltaMovement(impulse);
  ++entity.motionRevision;
}
void EntityFluidInteraction::applyCurrentTo(bool lava, Entity &entity,
                                            double scale) const {
  if (lava)
    lava_.applyCurrentTo(entity, scale);
  else
    water_.applyCurrentTo(entity, scale);
}
void EntityFluidInteraction::update(World &world, const Entity &entity,
                                    bool ignoreCurrent) {
  water_.reset();
  lava_.reset();
  const Aabb box = entity.boundingBox().deflate(0.001);
  const int x0 = static_cast<int>(std::floor(box.minX));
  const int y0 = static_cast<int>(std::floor(box.minY));
  const int z0 = static_cast<int>(std::floor(box.minZ));
  const int x1 = static_cast<int>(std::ceil(box.maxX)) - 1;
  const int y1 = static_cast<int>(std::ceil(box.maxY)) - 1;
  const int z1 = static_cast<int>(std::ceil(box.maxZ)) - 1;
  if (!world.isChunkLoaded(blockToSection(x0), blockToSection(z0)) ||
      !world.isChunkLoaded(blockToSection(x1), blockToSection(z1)))
    return;
  const double entityY = entity.boundingBox().minY;
  const int eyeBlockX = static_cast<int>(std::floor(entity.position.x));
  const double eyeY = entity.position.y + entity.eyeHeight() - 0.11111111;
  const int eyeBlockZ = static_cast<int>(std::floor(entity.position.z));
  for (int x = x0; x <= x1; ++x) {
    for (int y = y0; y <= y1; ++y) {
      for (int z = z0; z <= z1; ++z) {
        const Pos pos{x, y, z};
        const BlockState state = world.getBlock(pos);
        const FluidState fs = resolveEntityFluidState(state);
        if (fs.isEmpty())
          continue;
        const bool isLava = fluidHasTag(fs.getType(), FluidTag::Lava);
        const bool isWater =
            !isLava && fluidHasTag(fs.getType(), FluidTag::Water);
        if (!isLava && !isWater)
          continue;
        const double fluidBottom = static_cast<double>(y);
        const double fluidTop =
            fluidBottom + static_cast<double>(redstone::fluidHeight(world, pos, fs));
        if (fluidTop < box.minY)
          continue;
        EntityFluidTracker &tracker = isLava ? lava_ : water_;
        if (x == eyeBlockX && z == eyeBlockZ && eyeY >= fluidBottom &&
            eyeY <= fluidTop) {
          tracker.eyesInside = true;
        }
        tracker.height = std::max(fluidTop - entityY, tracker.height);
        if (!ignoreCurrent) {
          Vec3 flow = fluidFlow(world, pos, fs);
          if (tracker.height < 0.4)
            flow = flow * tracker.height;
          tracker.accumulateCurrent(flow);
        }
      }
    }
  }
}
bool Entity::updateFluidHeightAndDoFluidPushing(World &world, bool lava) {
  fluidInteraction_.update(world, *this, false);
  const double scale =
      lava ? (world.fastLava ? 0.007 : 0.0023333333333333335) : 0.014;
  fluidInteraction_.applyCurrentTo(lava, *this, scale);
  return fluidInteraction_.isInFluid(lava);
}
bool Entity::updateFluidInteraction(World &world) {
  const bool ignoreCurrent = isPassenger();
  fluidInteraction_.update(world, *this, ignoreCurrent);
  const bool inWater = fluidInteraction_.isInFluid(false);
  const bool inLava = fluidInteraction_.isInFluid(true);
  if (inWater) {
    fallDistance = 0.0;
    wasTouchingWater = true;
    clearFire();
  } else {
    wasTouchingWater = false;
  }
  if (!ignoreCurrent) {
    if (inWater)
      fluidInteraction_.applyCurrentTo(false, *this, 0.014);
    if (inLava)
      fluidInteraction_.applyCurrentTo(
          true, *this, world.fastLava ? 0.007 : 0.0023333333333333335);
  }
  return inWater || inLava;
}
void Entity::updateSwimming() {
  if (isSwimming()) {
    setSwimming(isSprinting() && isInWater() && !isPassenger());
  } else {
    setSwimming(isSprinting() && isUnderWater() && !isPassenger());
  }
}
double Entity::fluidHeight(World &world, bool lava) const {
  if (!firstTick_)
    return fluidInteraction_.getFluidHeight(lava);
  EntityFluidInteraction probe;
  probe.update(world, *this, true);
  return probe.getFluidHeight(lava);
}
bool Entity::isInWater(World &world) const {
  return fluidHeight(world, false) > 0.0;
}
bool Entity::isInLava(World &world) const {
  return fluidHeight(world, true) > 0.0;
}
void Entity::spawnAtLocation(World &world, const ItemStack &stack) {
  if (!stack.empty())
    world.spawnItemAt(position, stack.id, stack.count);
}

// ---- Riding & Passenger Tree ----
void Entity::rideTick(World &world) {
  setDeltaMovement(Vec3{});
  tick(world);
  if (auto v = getVehicle())
    v->positionRider(*this);
}
Vec3 Entity::getVehicleAttachmentPoint(const Entity &) const { return {}; }
Vec3 Entity::getPassengerAttachmentPoint(const Entity &, EntityDimensions dims,
                                         float scale) const {
  return {0.0, static_cast<double>(dims.height) * static_cast<double>(scale),
          0.0};
}
Vec3 Entity::getPassengerRidingPosition(const Entity &passenger) const {
  return position + getPassengerAttachmentPoint(passenger, dimensions_, 1.0f);
}
void Entity::positionRider(Entity &passenger) {
  if (!hasPassenger(passenger))
    return;
  const Vec3 target = getPassengerRidingPosition(passenger);
  const Vec3 offset = passenger.getVehicleAttachmentPoint(*this);
  passenger.setPos(target.x - offset.x, target.y - offset.y,
                   target.z - offset.z);
}
bool Entity::startRiding(Entity &vehicle, bool force, bool sendEvent) {
  (void)sendEvent;
  if (&vehicle == vehicle_.lock().get())
    return false;
  if (!vehicle.couldAcceptPassenger())
    return false;
  if (!vehicle.type().canSerialize)
    return false;
  for (const Entity *cursor = &vehicle; cursor != nullptr;
       cursor = cursor->vehicle_.lock().get()) {
    if (cursor == this)
      return false;
  }
  if (!force && (!canRide(vehicle) || !vehicle.canAddPassenger(*this)))
    return false;
  if (isPassenger())
    stopRiding();
  setPose(Pose::Standing);
  vehicle_ = vehicle.weak_from_this();
  hasVehicle_ = true;
  vehicle.addPassenger(*this);
  return true;
}
void Entity::removeVehicle() {
  if (auto old = vehicle_.lock()) {
    vehicle_.reset();
    hasVehicle_ = false;
    old->removePassenger(*this);
  } else {
    hasVehicle_ = false;
  }
}
void Entity::stopRiding() { removeVehicle(); }
void Entity::ejectPassengers() {
  for (std::size_t i = passengers_.size(); i > 0; --i) {
    if (auto p = passengers_[i - 1].lock())
      p->stopRiding();
  }
  passengers_.clear();
  hasPassengers_ = false;
}
void Entity::unRide() {
  if (isVehicle())
    ejectPassengers();
  if (isPassenger())
    stopRiding();
}
void Entity::addPassenger(Entity &passenger) {
  if (passenger.vehicle_.lock().get() != this)
    throw std::logic_error("Use x.startRiding(y), not y.addPassenger(x)");
  if (passengers_.empty()) {
    passengers_.push_back(passenger.weak_from_this());
    hasPassengers_ = true;
    return;
  }
  auto first = getFirstPassenger();
  if (passenger.isPlayer() && (!first || !first->isPlayer()))
    passengers_.insert(passengers_.begin(), passenger.weak_from_this());
  else
    passengers_.push_back(passenger.weak_from_this());
  hasPassengers_ = true;
}
void Entity::removePassenger(Entity &passenger) {
  if (passenger.vehicle_.lock().get() == this)
    throw std::logic_error("Use x.stopRiding(y), not y.removePassenger(x)");
  passengers_.erase(
      std::remove_if(passengers_.begin(), passengers_.end(),
                     [&](const std::weak_ptr<Entity> &w) {
                       auto s = w.lock();
                       return !s || s.get() == &passenger;
                     }),
      passengers_.end());
  hasPassengers_ = !passengers_.empty();
  passenger.boardingCooldown = kBoardingCooldown;
}
std::vector<std::shared_ptr<Entity>> Entity::getPassengers() const {
  std::vector<std::shared_ptr<Entity>> out;
  out.reserve(passengers_.size());
  for (const auto &w : passengers_) {
    if (auto s = w.lock(); s && !s->isRemoved())
      out.push_back(std::move(s));
  }
  return out;
}
std::shared_ptr<Entity> Entity::getFirstPassenger() const {
  for (const auto &w : passengers_) {
    if (auto s = w.lock(); s && !s->isRemoved())
      return s;
  }
  return {};
}
Entity &Entity::getRootVehicle() {
  Entity *cursor = this;
  while (auto v = cursor->vehicle_.lock()) {
    cursor = v.get();
  }
  return *cursor;
}
const Entity &Entity::getRootVehicle() const {
  const Entity *cursor = this;
  while (auto v = cursor->vehicle_.lock()) {
    cursor = v.get();
  }
  return *cursor;
}
bool Entity::hasPassenger(const Entity &e) const {
  for (const auto &w : passengers_) {
    if (auto s = w.lock(); s.get() == &e)
      return true;
  }
  return false;
}
bool Entity::hasIndirectPassenger(const Entity &e) const {
  if (hasPassenger(e))
    return true;
  for (const auto &w : passengers_) {
    if (auto s = w.lock(); s && s->hasIndirectPassenger(e))
      return true;
  }
  return false;
}
} // namespace redstone
