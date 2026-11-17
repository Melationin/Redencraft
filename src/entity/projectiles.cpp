#include "redstone/entity/projectiles.hpp"
#include "redstone/entity/decorative_entities.hpp"
#include "redstone/redstone.hpp"
#include "redstone/shapes.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace redstone {
namespace {
float wrapDeg(float deg) {
  float r = std::fmod(deg, 360.0f);
  if (r >= 180.0f)
    r -= 360.0f;
  if (r < -180.0f)
    r += 360.0f;
  return r;
}

float rotlerp(float current, float target, float factor) {
  return current + factor * wrapDeg(target - current);
}

int computeTargetRedstoneStrength(Direction face, Vec3 hitLocation) {
  const auto frac = [](double v) { return v - std::floor(v); };
  const double dx = std::abs(frac(hitLocation.x) - 0.5);
  const double dy = std::abs(frac(hitLocation.y) - 0.5);
  const double dz = std::abs(frac(hitLocation.z) - 0.5);
  double dist = 0.0;
  if (face == Direction::Up || face == Direction::Down)
    dist = std::max(dx, dz);
  else if (face == Direction::North || face == Direction::South)
    dist = std::max(dx, dy);
  else
    dist = std::max(dy, dz);
  const double scaled = std::clamp((0.5 - dist) / 0.5, 0.0, 1.0);
  return std::max(1, static_cast<int>(std::ceil(15.0 * scaled)));
}

void triggerWoodenButtonFromArrow(World &world, Pos pos,
                                  const BlockState &state) {
  if (!state.block().isButton())
    return;
  if (state.is(Blocks::STONE_BUTTON) ||
      state.is(Blocks::POLISHED_BLACKSTONE_BUTTON)) {
    return;
  }
  const int duration =
      (state.is(Blocks::STONE_BUTTON) ||
       state.is(Blocks::POLISHED_BLACKSTONE_BUTTON))
          ? 20
          : 30;
  if (!poweredOf(state)) {
    const BlockState next = state.with(PropertyKind::Powered, std::uint8_t{1});
    world.setBlock(pos, next, 3);
    world.updateNeighborsAt(pos, next.name());
  }
  world.scheduleTick(pos, state.block(), duration);
}
} // namespace

Projectile::Projectile(std::string typeName) : Entity(std::move(typeName)) {}

void Projectile::setOwner(const Entity *owner) {
  if (owner) {
    ownerId_ = owner->id;
    ownerUuid_ = owner->uuid;
  } else {
    ownerId_ = 0;
    ownerUuid_ = {};
  }
}

Entity *Projectile::getOwner(World &world) const {
  if (ownerId_ == 0 && ownerUuid_ == std::array<std::int32_t, 4>{})
    return nullptr;
  for (const auto &e : world.entities) {
    if (!e || e->isRemoved())
      continue;
    if ((ownerId_ != 0 && e->id == ownerId_) ||
        (ownerUuid_ != std::array<std::int32_t, 4>{} && e->uuid == ownerUuid_))
      return e.get();
  }
  return nullptr;
}

void Projectile::shoot(World &world, double dx, double dy, double dz,
                       float velocity, float inaccuracy) {
  const auto triangleNoise = [&]() {
    return (world.random.nextDouble() - world.random.nextDouble()) *
           0.007499999832361937 * static_cast<double>(inaccuracy);
  };
  const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
  Vec3 dir = len > 1.0e-7 ? Vec3{dx / len, dy / len, dz / len} : Vec3{};
  dir = dir + Vec3{triangleNoise(), triangleNoise(), triangleNoise()};
  dir = dir * static_cast<double>(velocity);
  setDeltaMovement(dir);
  const double horiz = std::sqrt(dir.x * dir.x + dir.z * dir.z);
  yaw = static_cast<float>(std::atan2(dir.x, dir.z) * 180.0 /
                           std::numbers::pi);
  pitch = static_cast<float>(std::atan2(dir.y, horiz) * 180.0 /
                             std::numbers::pi);
  oldYaw = yaw;
  oldPitch = pitch;
  hasBeenShot_ = true;
}

void Projectile::shootFromRotation(World &world, float p, float y, float roll,
                                   float velocity, float inaccuracy) {
  constexpr double degToRad = std::numbers::pi / 180.0;
  const double x = -std::sin(y * degToRad) * std::cos(p * degToRad);
  const double yy = -std::sin((p + roll) * degToRad);
  const double z = std::cos(y * degToRad) * std::cos(p * degToRad);
  shoot(world, x, yy, z, velocity, inaccuracy);
}

void Projectile::updateRotation() {
  const double horiz = std::sqrt(deltaMovement.x * deltaMovement.x +
                                 deltaMovement.z * deltaMovement.z);
  if (horiz > 1.0e-7 || std::abs(deltaMovement.y) > 1.0e-7) {
    pitch = rotlerp(
        oldPitch,
        static_cast<float>(std::atan2(deltaMovement.y, horiz) * 180.0 /
                           std::numbers::pi),
        0.2f);
    yaw = rotlerp(oldYaw,
                  static_cast<float>(std::atan2(deltaMovement.x,
                                                deltaMovement.z) *
                                     180.0 / std::numbers::pi),
                  0.2f);
  }
}

bool Projectile::canHitEntity(const Entity &target) const {
  if (target.isRemoved() || !target.isPickable())
    return false;
  if (!leftOwner_ && ownerId_ != 0 &&
      (target.id == ownerId_ || target.hasIndirectPassenger(*this)))
    return false;
  return true;
}

void Projectile::tick(World &world) {
  if (!leftOwner_) {
    Entity *owner = getOwner(world);
    if (!owner) {
      leftOwner_ = true;
    } else {
      const Aabb check = boundingBox().inflate(1.0);
      leftOwner_ = !owner->boundingBox().intersects(check);
    }
    leftOwnerChecked_ = true;
  }
  baseTick(world);
}

void Projectile::onHitBlock(World &world, const BlockHitResult &hit) {
  const BlockState state = world.getBlock(hit.pos);
  if (state.is(Blocks::TARGET)) {
    const int power = computeTargetRedstoneStrength(hit.direction, hit.location);
    if (!world.hasScheduledTick(hit.pos, state.block()))
      targetHit(world, hit.pos, state, power, targetBlockPulseTicks());
  } else if (state.is(Blocks::BELL)) {
    world.log.push_back({"bell_ring", hit.pos, typeName()});
  } else if (state.is(Blocks::CHORUS_FLOWER)) {
    world.setBlock(hit.pos, airState(), 3);
    world.log.push_back({"chorus_flower_broken", hit.pos, typeName()});
  } else if (state.is(Blocks::DECORATED_POT)) {
    world.setBlock(hit.pos, airState(), 3);
    for (int i = 0; i < 4; ++i)
      world.spawnItem(hit.pos, "minecraft:brick", 1);
    world.log.push_back({"decorated_pot_shattered", hit.pos, typeName()});
  } else if (isOnFire()) {
    if (state.is(Blocks::TNT)) {
      world.primeTntBlock(hit.pos);
    } else if ((state.is(Blocks::CAMPFIRE) ||
                state.is(Blocks::SOUL_CAMPFIRE) || state.block().isCandle()) &&
               !litOf(state) &&
               state.getView("waterlogged", "false") != "true") {
      world.setBlock(hit.pos, state.with(PropertyKind::Lit, std::uint8_t{1}), 3);
    }
  }
}

void Projectile::onHitEntity(World &, const EntityHitResult &) {}

AbstractArrow::AbstractArrow(std::string typeName)
    : Projectile(std::move(typeName)) {
  entityData_.define(kDataFlagsId, EntityDataSerializer::Byte,
                     std::int8_t{0});
  entityData_.define(kDataPierceLevelId, EntityDataSerializer::Byte,
                     std::int8_t{0});
  entityData_.define(kDataInGroundId, EntityDataSerializer::Boolean, false);
}

void AbstractArrow::setCritArrow(bool value) {
  crit_ = value;
  syncDataFields();
}

void AbstractArrow::setNoPhysics(bool value) {
  noPhysics = value;
  syncDataFields();
}

void AbstractArrow::syncDataFields() {
  std::uint8_t flags = 0;
  if (crit_)
    flags |= kFlagCrit;
  if (noPhysics)
    flags |= kFlagNoPhysics;
  entityData_.set(kDataFlagsId, static_cast<std::int8_t>(flags));
  entityData_.set(kDataPierceLevelId, static_cast<std::int8_t>(pierceLevel_));
  entityData_.set(kDataInGroundId, inGround_);
}

bool AbstractArrow::shouldFall(const World &world) const {
  if (!inGround_ || !inBlockPos_.has_value() || !inBlockState_.has_value())
    return false;
  const BlockState current = world.getBlock(*inBlockPos_);
  return current.id() != inBlockState_->id();
}

void AbstractArrow::startFalling() {
  setInGround(false);
  inBlockPos_.reset();
  inBlockState_.reset();
  inGroundTime_ = 0;
  life_ = 0;
}

void AbstractArrow::onHitBlock(World &world, const BlockHitResult &hit) {
  Projectile::onHitBlock(world, hit);
  const BlockState state = world.getBlock(hit.pos);
  inBlockPos_ = hit.pos;
  inBlockState_ = state;
  setInGround(true);
  shakeTime = 7;
  setCritArrow(false);
  setPierceLevel(0);
  piercingIgnoreIds_.clear();
  setPos(hit.location);
  setDeltaMovement(0.0, 0.0, 0.0);
  if (state.block().isButton())
    triggerWoodenButtonFromArrow(world, hit.pos, state);
}

void AbstractArrow::onHitEntity(World &world, const EntityHitResult &hit) {
  if (!hit.entity || hit.entity->isRemoved())
    return;
  if (piercingIgnoreIds_.contains(hit.entity->id))
    return;
  const double speed = std::sqrt(deltaMovement.lengthSqr());
  int dmg = static_cast<int>(
      std::ceil(std::clamp(speed * baseDamage_, 0.0, 2.147483647e9)));
  if (crit_)
    dmg += world.random.nextInt(dmg / 2 + 2);
  hit.entity->hurt(world, DamageKind::Projectile, static_cast<float>(dmg));
  if (isOnFire())
    hit.entity->igniteForSeconds(5.0f);
  if (pierceLevel_ > 0) {
    piercingIgnoreIds_.insert(hit.entity->id);
    if (piercingIgnoreIds_.size() > pierceLevel_)
      discard();
  } else {
    discard();
  }
}

void AbstractArrow::tick(World &world) {
  Projectile::tick(world);
  if (shakeTime > 0)
    --shakeTime;
  if (inGround_ && !noPhysics) {
    if (shouldFall(world)) {
      startFalling();
    } else {
      ++inGroundTime_;
      ++life_;
      if (life_ >= 1200)
        discard();
    }
    return;
  }

  inGroundTime_ = 0;
  Vec3 currentPos = position;
  Vec3 velocity = deltaMovement;
  Vec3 nextPos = currentPos + velocity;
  BlockHitResult blockHit;
  if (!noPhysics) {
    blockHit = world.clipBlock(currentPos, nextPos, false);
    if (blockHit.hit)
      nextPos = blockHit.location;
  }

  while (!isRemoved()) {
    const Aabb searchBox =
        boundingBox().expandTowards(nextPos - currentPos).inflate(1.0);
    auto entityHit = world.clipEntity(
        this, currentPos, nextPos, searchBox,
        [&](const Entity &e) {
          return canHitEntity(e) && !piercingIgnoreIds_.contains(e.id);
        });
    if (entityHit.has_value()) {
      onHitEntity(world, *entityHit);
      if (pierceLevel_ <= 0 || isRemoved())
        break;
      continue;
    }
    if (blockHit.hit) {
      onHitBlock(world, blockHit);
    }
    break;
  }

  if (isRemoved())
    return;
  if (!inGround_) {
    setPos(nextPos);
    updateRotation();
    applyEffectsFromBlocks(world, currentPos, position);
    const bool inWater = isInWater(world);
    const double inertia = inWater ? waterInertia() : 0.99;
    deltaMovement = deltaMovement * inertia;
    if (!isNoGravity() && !noPhysics)
      deltaMovement.y -= 0.05;
    setPos(position);
  }
}

Arrow::Arrow() : AbstractArrow("minecraft:arrow") {
  entityData_.define(kDataEffectColorId, EntityDataSerializer::Int, -1);
}

void Arrow::syncDataFields() {
  AbstractArrow::syncDataFields();
  entityData_.set(kDataEffectColorId, effectColor);
}

SpectralArrow::SpectralArrow() : AbstractArrow("minecraft:spectral_arrow") {}

ThrownTrident::ThrownTrident() : AbstractArrow("minecraft:trident") {
  entityData_.define(kDataLoyaltyId, EntityDataSerializer::Byte,
                     std::int8_t{0});
  entityData_.define(kDataFoilId, EntityDataSerializer::Boolean, false);
  setBaseDamage(8.0);
}

void ThrownTrident::syncDataFields() {
  AbstractArrow::syncDataFields();
  entityData_.set(kDataLoyaltyId, static_cast<std::int8_t>(loyalty));
  entityData_.set(kDataFoilId, foil);
}

void ThrownTrident::onHitBlock(World &world, const BlockHitResult &hit) {
  AbstractArrow::onHitBlock(world, hit);
  const BlockState hitState = world.getBlock(hit.pos);
  if (hitState.is(Blocks::POINTED_DRIPSTONE) &&
      deltaMovement.length() > 0.6) {
    for (const auto &drop : blockDrops(hitState))
      world.spawnItem(hit.pos, drop.id, drop.count);
    world.setBlock(hit.pos, airState(), 3);
  }
}

ThrowableItemProjectile::ThrowableItemProjectile(std::string typeName,
                                                 ItemStack stack)
    : Projectile(std::move(typeName)), item(std::move(stack)) {
  entityData_.define(kDataItemStackId, EntityDataSerializer::ItemStack, item);
}

void ThrowableItemProjectile::syncDataFields() {
  entityData_.set(kDataItemStackId, item);
}

void ThrowableItemProjectile::tick(World &world) {
  Projectile::tick(world);
  const Vec3 currentPos = position;
  Vec3 nextPos = currentPos + deltaMovement;
  const BlockHitResult blockHit = world.clipBlock(currentPos, nextPos, false);
  if (blockHit.hit)
    nextPos = blockHit.location;
  const Aabb searchBox =
      boundingBox().expandTowards(nextPos - currentPos).inflate(1.0);
  const auto entityHit =
      world.clipEntity(this, currentPos, nextPos, searchBox,
                       [&](const Entity &e) { return canHitEntity(e); });
  if (entityHit.has_value()) {
    onHitEntity(world, *entityHit);
  } else if (blockHit.hit) {
    onHitBlock(world, blockHit);
    discard();
  }
  if (isRemoved())
    return;
  setPos(nextPos);
  updateRotation();
  applyEffectsFromBlocks(world, currentPos, position);
  const float drag = isInWater(world) ? 0.8f : 0.99f;
  deltaMovement = deltaMovement * static_cast<double>(drag);
  if (!isNoGravity())
    deltaMovement.y -= 0.03;
}

Snowball::Snowball()
    : ThrowableItemProjectile("minecraft:snowball",
                              itemStack("minecraft:snowball", 1)) {}

void Snowball::onHitEntity(World &world, const EntityHitResult &hit) {
  if (hit.entity)
    hit.entity->hurt(world, DamageKind::Projectile, 0.0f);
  discard();
}

ThrownEgg::ThrownEgg()
    : ThrowableItemProjectile("minecraft:egg", itemStack("minecraft:egg", 1)) {}

void ThrownEgg::onHitEntity(World &world, const EntityHitResult &hit) {
  if (hit.entity)
    hit.entity->hurt(world, DamageKind::Projectile, 0.0f);
  discard();
}

ThrownEnderpearl::ThrownEnderpearl()
    : ThrowableItemProjectile("minecraft:ender_pearl",
                              itemStack("minecraft:ender_pearl", 1)) {}

void ThrownEnderpearl::teleportOwnerOnHit(World &world, Vec3 destination) {
  if (Entity *owner = getOwner(world); owner && !owner->isRemoved()) {
    const Vec3 prev = owner->position;
    owner->setPos(destination);
    owner->resetFallDistance();
    owner->hurt(world, DamageKind::EnderPearl, 5.0f);
    world.synchronizeEntityAfterMove(*owner, prev);
  }
  discard();
}

void ThrownEnderpearl::onHitBlock(World &world, const BlockHitResult &hit) {
  ThrowableItemProjectile::onHitBlock(world, hit);
  teleportOwnerOnHit(world, hit.location);
}

void ThrownEnderpearl::onHitEntity(World &world, const EntityHitResult &hit) {
  if (hit.entity)
    hit.entity->hurt(world, DamageKind::Projectile, 0.0f);
  teleportOwnerOnHit(world, position);
}

void ThrownEnderpearl::tick(World &world) {
  ThrowableItemProjectile::tick(world);
  if (!isAlive())
    return;
  const int cx = static_cast<int>(std::floor(position.x)) >> 4;
  const int cz = static_cast<int>(std::floor(position.z)) >> 4;
  const bool chunkChanged =
      !hasTicketChunk_ || cx != lastTicketChunkX_ || cz != lastTicketChunkZ_;
  if (--ticketTimer_ <= 0 || chunkChanged) {
    lastTicketChunkX_ = cx;
    lastTicketChunkZ_ = cz;
    hasTicketChunk_ = true;
    if (world.enderPearlTicketObserver)
      ticketTimer_ = world.enderPearlTicketObserver(cx, cz);
    else
      ticketTimer_ = 40;
  }
}

ThrownExperienceBottle::ThrownExperienceBottle()
    : ThrowableItemProjectile("minecraft:experience_bottle",
                              itemStack("minecraft:experience_bottle", 1)) {}

void ThrownExperienceBottle::shatter(World &world) {
  int amount = 3 + world.random.nextInt(5) + world.random.nextInt(5);
  world.addEntity(std::make_shared<ExperienceOrb>(world, position, amount));
  world.log.push_back(
      {"experience_bottle_shatter",
       {static_cast<int>(std::floor(position.x)),
        static_cast<int>(std::floor(position.y)),
        static_cast<int>(std::floor(position.z))},
       std::to_string(amount)});
  discard();
}

void ThrownExperienceBottle::onHitBlock(World &world,
                                        const BlockHitResult &hit) {
  ThrowableItemProjectile::onHitBlock(world, hit);
  shatter(world);
}

void ThrownExperienceBottle::onHitEntity(World &world,
                                         const EntityHitResult &hit) {
  (void)hit;
  shatter(world);
}

WindCharge::WindCharge() : Projectile("minecraft:wind_charge") {}

void WindCharge::explodeAt(World &world, Vec3 pos) {
  world.explodeTriggerBlock(this, pos, 1.2f);
  discard();
}

void WindCharge::onHitBlock(World &world, const BlockHitResult &hit) {
  Projectile::onHitBlock(world, hit);
  explodeAt(world, hit.location);
}

void WindCharge::onHitEntity(World &world, const EntityHitResult &hit) {
  if (hit.entity)
    hit.entity->hurt(world, DamageKind::Projectile, 1.0f);
  explodeAt(world, position);
}

void WindCharge::tick(World &world) {
  Projectile::tick(world);
  if (noDeflectTicks_ > 0)
    --noDeflectTicks_;
  const Vec3 currentPos = position;
  Vec3 nextPos = currentPos + deltaMovement;
  const BlockHitResult blockHit = world.clipBlock(currentPos, nextPos, false);
  if (blockHit.hit)
    nextPos = blockHit.location;
  const Aabb searchBox =
      boundingBox().expandTowards(nextPos - currentPos).inflate(1.0);
  const auto entityHit =
      world.clipEntity(this, currentPos, nextPos, searchBox,
                       [&](const Entity &e) { return canHitEntity(e); });
  if (entityHit.has_value()) {
    onHitEntity(world, *entityHit);
  } else if (blockHit.hit) {
    onHitBlock(world, blockHit);
  } else {
    setPos(nextPos);
    updateRotation();
  }
}

} // namespace redstone
