#include "entity/internal.hpp"
#include "redstone/entity/decorative_entities.hpp"
#include "redstone/redstone.hpp"

#include <algorithm>
#include <cmath>

namespace redstone {

HangingEntity::HangingEntity(std::string typeName, Pos attachPos,
                             Direction direction)
    : Entity(std::move(typeName)), attachPos_(attachPos),
      attachDirection_(direction) {
  entityData_.define(kDataDirectionId, EntityDataSerializer::Direction,
                     attachDirection_);
  recalculateBoundingBox();
}

void HangingEntity::syncDataFields() {
  entityData_.set(kDataDirectionId, attachDirection_);
}

namespace {
Vec3 dirStepVec(Direction direction) {
  switch (direction) {
  case Direction::Down:
    return {0.0, -1.0, 0.0};
  case Direction::Up:
    return {0.0, 1.0, 0.0};
  case Direction::North:
    return {0.0, 0.0, -1.0};
  case Direction::South:
    return {0.0, 0.0, 1.0};
  case Direction::West:
    return {-1.0, 0.0, 0.0};
  case Direction::East:
    return {1.0, 0.0, 0.0};
  }
  return {};
}

Aabb hangingWallBox(Pos pos, Direction direction) {
  const double offset = -0.46875;
  Vec3 center = Vec3{pos.x + 0.5, pos.y + 0.5, pos.z + 0.5} +
                dirStepVec(direction) * offset;
  const double xs =
      (direction == Direction::West || direction == Direction::East) ? 0.0625
                                                                     : 0.75;
  const double ys =
      (direction == Direction::Down || direction == Direction::Up) ? 0.0625
                                                                   : 0.75;
  const double zs =
      (direction == Direction::North || direction == Direction::South) ? 0.0625
                                                                       : 0.75;
  return Aabb::ofSize(center, xs, ys, zs);
}
} // namespace

Aabb HangingEntity::calculateBoundingBox(Pos pos, Direction direction) const {
  return hangingWallBox(pos, direction);
}

void HangingEntity::recalculateBoundingBox() {
  const Aabb box = calculateBoundingBox(attachPos_, attachDirection_);
  const Vec3 center = box.getCenter();
  setPos(center.x, center.y - dimensions().height * 0.5, center.z);
}

bool HangingEntity::survives(World &world) const {
  const Pos supportPos = attachPos_.relative(opposite(attachDirection_));
  const BlockState support = world.getBlock(supportPos);
  return blocksMotion(support) || isDiodeBlock(support);
}

void HangingEntity::tick(World &world) {
  baseTick(world);
  if (++checkInterval_ >= 100) {
    checkInterval_ = 0;
    if (!isRemoved() && !survives(world)) {
      discard();
      spawnAtLocation(world, itemStack(typeName(), 1));
    }
  }
}

ItemFrame::ItemFrame(Pos attachPos, Direction direction, bool glow)
    : HangingEntity(glow ? "minecraft:glow_item_frame" : "minecraft:item_frame",
                    attachPos, direction),
      glow_(glow) {
  entityData_.define(kDataItemId, EntityDataSerializer::ItemStack, framedItem_);
  entityData_.define(kDataRotationId, EntityDataSerializer::Int, rotation_);
  if (direction == Direction::North || direction == Direction::South ||
      direction == Direction::East || direction == Direction::West) {
    pitch = 0.0f;
    switch (direction) {
    case Direction::South:
      yaw = 0.0f;
      break;
    case Direction::West:
      yaw = 90.0f;
      break;
    case Direction::North:
      yaw = 180.0f;
      break;
    case Direction::East:
      yaw = 270.0f;
      break;
    default:
      break;
    }
  } else {
    pitch = direction == Direction::Up ? -90.0f : 90.0f;
    yaw = 0.0f;
  }
  oldYaw = yaw;
  oldPitch = pitch;
  recalculateBoundingBox();
}

Aabb ItemFrame::calculateBoundingBox(Pos pos, Direction direction) const {
  return hangingWallBox(pos, direction);
}

void ItemFrame::syncDataFields() {
  HangingEntity::syncDataFields();
  entityData_.set(kDataItemId, framedItem_);
  entityData_.set(kDataRotationId, rotation_);
}

void ItemFrame::setFramedItem(World &world, ItemStack stack) {
  if (!stack.empty())
    stack.count = 1;
  framedItem_ = std::move(stack);
  entityData_.set(kDataItemId, framedItem_);
  world.updateNeighbourForOutputSignal(attachPos_,
                                       world.getBlock(attachPos_).block());
}

void ItemFrame::setRotation(World &world, int rot) {
  rotation_ = ((rot % 8) + 8) % 8;
  entityData_.set(kDataRotationId, rotation_);
  world.updateNeighbourForOutputSignal(attachPos_,
                                       world.getBlock(attachPos_).block());
}

void ItemFrame::rotate(World &world) {
  if (!framedItem_.empty())
    setRotation(world, rotation_ + 1);
}

int ItemFrame::getAnalogOutput() const noexcept {
  return framedItem_.empty() ? 0 : (rotation_ % 8) + 1;
}

bool ItemFrame::hurt(World &world, DamageKind kind, float) {
  if (fixed)
    return false;
  if (kind != DamageKind::Explosion && !framedItem_.empty()) {
    spawnAtLocation(world, framedItem_);
    setFramedItem(world, {});
    setRotation(world, 0);
    return true;
  }
  if (!isRemoved()) {
    discard();
    if (!framedItem_.empty())
      spawnAtLocation(world, framedItem_);
    spawnAtLocation(world,
                    itemStack(glow_ ? "minecraft:glow_item_frame"
                                    : "minecraft:item_frame",
                              1));
    world.updateNeighbourForOutputSignal(attachPos_,
                                         world.getBlock(attachPos_).block());
  }
  return true;
}

EndCrystal::EndCrystal(World &world, Vec3 pos)
    : Entity("minecraft:end_crystal") {
  blocksBuilding = true;
  time = world.random.nextInt(100000);
  entityData_.define(kDataBeamTargetId, EntityDataSerializer::OptionalBlockPos,
                     beamTarget);
  entityData_.define(kDataShowBottomId, EntityDataSerializer::Boolean,
                     showBottom);
  setPos(pos);
  oldPosition = pos;
}

void EndCrystal::syncDataFields() {
  entityData_.set(kDataBeamTargetId, beamTarget);
  entityData_.set(kDataShowBottomId, showBottom);
}

void EndCrystal::tick(World &world) {
  ++time;
  applyEffectsFromBlocks(world);
  handlePortal(world);
}

bool EndCrystal::hurt(World &world, DamageKind kind, float) {
  if (isRemoved())
    return false;
  discard();
  if (kind != DamageKind::Explosion)
    world.explode(this, position, 6.0f, false);
  return true;
}

ExperienceOrb::ExperienceOrb(World &world, Vec3 pos, int xp)
    : Entity("minecraft:experience_orb"), xpValue(xp) {
  entityData_.define(kDataValueId, EntityDataSerializer::Int, xpValue);
  setPos(pos);
  oldPosition = pos;
  yaw = world.random.nextFloat() * 360.0f;
  deltaMovement = {(world.random.nextDouble() * 0.2 - 0.1) * 2.0,
                   world.random.nextDouble() * 0.2 * 2.0,
                   (world.random.nextDouble() * 0.2 - 0.1) * 2.0};
}

void ExperienceOrb::syncDataFields() {
  entityData_.set(kDataValueId, xpValue);
}

bool ExperienceOrb::hurt(World &, DamageKind kind, float amount) {
  if (kind == DamageKind::Fire || kind == DamageKind::Lava)
    return false;
  health -= static_cast<int>(amount);
  if (health <= 0)
    discard();
  return true;
}

void ExperienceOrb::mergeOrb(ExperienceOrb &other) {
  if (&other == this || other.isRemoved() || other.xpValue != xpValue)
    return;
  count += other.count;
  age = std::min(age, other.age);
  other.discard();
}

void ExperienceOrb::scanForEntities(World &world) {
  const Aabb box = boundingBox().inflate(0.5);
  for (Entity *e : world.entitiesIn(box)) {
    if (auto *orb = dynamic_cast<ExperienceOrb *>(e)) {
      if (orb != this && !orb->isRemoved() && orb->xpValue == xpValue &&
          orb->id > id) {
        mergeOrb(*orb);
      }
    }
  }
}

void ExperienceOrb::tick(World &world) {
  baseTick(world);
  updateFluidInteraction(world);
  if (isEyeInWater()) {
    deltaMovement = {deltaMovement.x * 0.99,
                     std::min(deltaMovement.y + 5.0e-4, 0.06),
                     deltaMovement.z * 0.99};
  } else if (world.noCollision(this, boundingBox())) {
    deltaMovement.y -= 0.03;
  }
  if (isInLava()) {
    deltaMovement = {
        (world.random.nextFloat() - world.random.nextFloat()) * 0.2, 0.2,
        (world.random.nextFloat() - world.random.nextFloat()) * 0.2};
  }
  Entity *nearestPlayer = nullptr;
  double nearestDistSqr = 64.0;
  for (const auto &e : world.entities) {
    if (!e || e->isRemoved() || !e->isPlayer())
      continue;
    const double d = distanceToSqr(e->position);
    if (d < nearestDistSqr) {
      nearestDistSqr = d;
      nearestPlayer = e.get();
    }
  }
  if (nearestPlayer) {
    const Vec3 diff = nearestPlayer->eyePosition() - position;
    const double d = diff.length();
    if (d > 1.0e-7) {
      const double factor = 1.0 - d / 8.0;
      deltaMovement +=
          (diff * (1.0 / d)) * (factor * factor * 0.1);
    }
  }
  move(world, MoverType::Self, deltaMovement);
  applyEffectsFromBlocks(world);
  float friction = 0.98f;
  if (onGround) {
    const Pos below = getBlockPosBelowThatAffectsMyMovement(world);
    friction = world.getBlock(below).block().metadata().friction * 0.98f;
  }
  deltaMovement =
      deltaMovement.multiply(friction, 0.98, friction);
  if (onGround)
    deltaMovement.y *= -0.9;
  if (tickCount % 20 == 0)
    scanForEntities(world);
  ++age;
  if (age >= 6000)
    discard();
}

} // namespace redstone
