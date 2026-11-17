#include "redstone/entity.hpp"
#include "redstone/item.hpp"
#include "redstone/redstone.hpp"

#include <algorithm>
#include <cmath>

namespace redstone {
namespace {

bool isBedBlock(const BlockState &state) {
  const std::string_view name = state.name();
  return name.size() > 4 && name.substr(name.size() - 4) == "_bed";
}

} // namespace

void PlayerFoodData::add(int nutrition, float saturation) {
  foodLevel = std::clamp(foodLevel + nutrition, 0, 20);
  saturationLevel =
      std::clamp(saturationLevel + saturation, 0.0f, static_cast<float>(foodLevel));
}

void PlayerFoodData::addExhaustion(float amount) {
  exhaustionLevel = std::min(exhaustionLevel + amount, 40.0f);
}

void PlayerFoodData::tick(World &world, ServerPlayer &player, bool naturalRegen,
                          bool peaceful, bool hardDifficulty) {
  if (exhaustionLevel > 4.0f) {
    exhaustionLevel -= 4.0f;
    if (saturationLevel > 0.0f) {
      saturationLevel = std::max(saturationLevel - 1.0f, 0.0f);
    } else if (!peaceful) {
      foodLevel = std::max(foodLevel - 1, 0);
    }
  }
  const bool isHurt =
      player.health() > 0.0f && player.health() < player.maxHealth();
  if (naturalRegen && saturationLevel > 0.0f && isHurt && foodLevel >= 20) {
    ++tickTimer;
    if (tickTimer >= 10) {
      const float spent = std::min(saturationLevel, 6.0f);
      player.heal(spent / 6.0f);
      addExhaustion(spent);
      tickTimer = 0;
    }
  } else if (naturalRegen && foodLevel >= 18 && isHurt) {
    ++tickTimer;
    if (tickTimer >= 80) {
      player.heal(1.0f);
      addExhaustion(6.0f);
      tickTimer = 0;
    }
  } else if (foodLevel <= 0) {
    ++tickTimer;
    if (tickTimer >= 80) {
      if (player.health() > 10.0f || hardDifficulty ||
          (player.health() > 1.0f && !peaceful)) {
        DamageSource src;
        src.kind = DamageKind::Starve;
        src.bypassesArmor = true;
        player.hurtServer(world, src, 1.0f);
      }
      tickTimer = 0;
    }
  } else {
    tickTimer = 0;
  }
}

void ItemCooldowns::tick() {
  ++tickCount_;
  for (auto it = endTicks_.begin(); it != endTicks_.end();) {
    if (it->second <= tickCount_)
      it = endTicks_.erase(it);
    else
      ++it;
  }
}

bool ItemCooldowns::isOnCooldown(std::string_view itemId) const {
  return remainingTicks(itemId) > 0;
}

int ItemCooldowns::remainingTicks(std::string_view itemId) const {
  const auto it = endTicks_.find(itemId);
  if (it == endTicks_.end() || it->second <= tickCount_)
    return 0;
  return it->second - tickCount_;
}

void ItemCooldowns::addCooldown(std::string itemId, int ticks) {
  if (itemId.empty() || ticks <= 0)
    return;
  endTicks_[std::move(itemId)] = tickCount_ + ticks;
}

void ItemCooldowns::removeCooldown(std::string_view itemId) {
  const auto it = endTicks_.find(itemId);
  if (it != endTicks_.end())
    endTicks_.erase(it);
}

ServerPlayer::ServerPlayer(World &world, Vec3 pos, std::string name)
    : LivingEntity("minecraft:player"), username(std::move(name)) {
  (void)world;
  isPlayer_ = true;
  entityData_.define(kDataPlayerMainHandId, EntityDataSerializer::HumanoidArm,
                     1);
  entityData_.define(kDataPlayerModeCustomisationId, EntityDataSerializer::Byte,
                     std::int8_t{0});
  entityData_.define(kDataPlayerAbsorptionId, EntityDataSerializer::Float,
                     0.0f);
  entityData_.define(kDataScoreId, EntityDataSerializer::Int, 0);
  entityData_.define(kDataShoulderParrotLeftId,
                     EntityDataSerializer::OptionalUnsignedInt,
                     std::optional<std::uint32_t>{});
  entityData_.define(kDataShoulderParrotRightId,
                     EntityDataSerializer::OptionalUnsignedInt,
                     std::optional<std::uint32_t>{});

  attributes_.ensure(AttributeId::AttackDamage).setBaseValue(1.0);
  attributes_.ensure(AttributeId::AttackSpeed).setBaseValue(4.0);
  attributes_.ensure(AttributeId::MovementSpeed).setBaseValue(0.1);
  setSpeed(0.1f);
  refreshDimensions();
  setPos(pos);
}

EntityDimensions ServerPlayer::getDimensions(Pose p) const {
  switch (p) {
  case Pose::Sleeping:
    return {0.2f, 0.2f, 0.2f, true};
  case Pose::Swimming:
  case Pose::FallFlying:
  case Pose::SpinAttack:
    return {0.6f, 0.6f, 0.4f, false};
  case Pose::Crouching:
    return {0.6f, 1.5f, 1.27f, false};
  default:
    return {0.6f, 1.8f, 1.62f, false};
  }
}

void ServerPlayer::syncDataFields() {
  LivingEntity::syncDataFields();
  entityData_.set(kDataPlayerAbsorptionId, absorptionAmount_);
}

void ServerPlayer::stopSleeping() {
  sleepCounter = 0;
  if (auto pos = sleepingPos()) {
    const BlockState bed = {};
    (void)bed;
    setSleepingPos(std::nullopt);
  }
  setPose(isShiftKeyDown() ? Pose::Crouching : Pose::Standing);
  refreshDimensions();
}

bool ServerPlayer::sleepInBedAt(World &world, Pos bedPos) {
  if (!isAlive() || isSpectator())
    return false;
  const BlockState state = world.getBlock(bedPos);
  if (!isBedBlock(state))
    return false;
  setSleepingPos(bedPos);
  setPose(Pose::Sleeping);
  refreshDimensions();
  setPos({bedPos.x + 0.5, bedPos.y + 0.6875, bedPos.z + 0.5});
  deltaMovement = {};
  sleepCounter = 0;
  return true;
}

bool ServerPlayer::hurtServer(World &world, const DamageSource &source,
                              float amount) {
  if (creative && !source.bypassesInvulnerability)
    return false;
  if (spectator && !source.bypassesInvulnerability)
    return false;
  return LivingEntity::hurtServer(world, source, amount);
}

void ServerPlayer::touchNearbyEntities(World &world) {
  if (!isAlive() || isSpectator() || isRemoved())
    return;
  const Aabb pickupBox = [&]() {
    if (isPassenger() && getVehicle()) {
      const Aabb a = boundingBox();
      const Aabb b = getVehicle()->boundingBox();
      return Aabb{std::min(a.minX, b.minX), std::min(a.minY, b.minY),
                  std::min(a.minZ, b.minZ), std::max(a.maxX, b.maxX),
                  std::max(a.maxY, b.maxY), std::max(a.maxZ, b.maxZ)};
    }
    return boundingBox().inflate(1.0, 0.5, 1.0);
  }();
  const auto nearby = world.entitiesIn(pickupBox);
  for (Entity *e : nearby) {
    if (!e || e->isRemoved())
      continue;
    if (auto *item = dynamic_cast<ItemEntity *>(e)) {
      if (item->pickupDelay != 0 || item->item.empty())
        continue;
      const int originalCount = item->item.count;
      int accepted = originalCount;
      if (onItemPickup)
        accepted = std::clamp(onItemPickup(item->item), 0, originalCount);
      if (accepted <= 0)
        continue;
      if (world.entityTakenObserver)
        world.entityTakenObserver(id, item->id, accepted);
      item->item.count -= accepted;
      if (item->item.count <= 0)
        item->discard();
    } else if (auto *xp = dynamic_cast<ExperienceOrb *>(e)) {
      if (takeXpDelay > 0 || xp->xpValue <= 0)
        continue;
      takeXpDelay = 2;
      totalExperience += xp->xpValue;
      if (world.entityTakenObserver)
        world.entityTakenObserver(id, xp->id, 1);
      xp->discard();
    }
  }
}

void ServerPlayer::applyClientMovement(World &world, Vec3 pos, float newYaw,
                                       float newPitch, bool onGroundNow) {
  const Vec3 prev = position;
  oldPosition = prev;
  yaw = newYaw;
  pitch = newPitch;
  onGround = onGroundNow;
  setPos(pos);
  world.synchronizeEntityAfterMove(*this, prev);
  if (isShiftKeyDown() && !isSleeping()) {
    setPose(Pose::Crouching);
    refreshDimensions();
  } else if (!isSleeping()) {
    setPose(Pose::Standing);
    refreshDimensions();
  }
  if (onGroundNow && !isSteppingCarefully()) {
    const Pos below{static_cast<int>(std::floor(pos.x)),
                    static_cast<int>(std::floor(pos.y - 0.2)),
                    static_cast<int>(std::floor(pos.z))};
    const BlockState state = world.getBlock(below);
    if (state.is(Blocks::REDSTONE_ORE) ||
        state.is(Blocks::DEEPSLATE_REDSTONE_ORE)) {
      redstoneOreStepOn(world, below, state, *this);
    }
  }
  applyEffectsFromBlocks(world, prev, pos);
  touchNearbyEntities(world);
}

void ServerPlayer::tick(World &world) {
  if (invulnerableTime > 0)
    --invulnerableTime;
  if (takeXpDelay > 0)
    --takeXpDelay;
  cooldowns.tick();

  baseTick(world);
  if (!creative && !spectator && isAlive())
    foodData.tick(world, *this);

  if (isSleeping()) {
    ++sleepCounter;
    if (sleepCounter > kSleepDurationTicks)
      sleepCounter = kSleepDurationTicks;
    if (world.nightSkippedObserver && allPlayersSleptThroughNight(world)) {
      world.nightSkippedObserver();
      for (const auto &e : world.entities) {
        if (auto *p = dynamic_cast<ServerPlayer *>(e.get())) {
          if (p->isSleeping())
            p->stopSleeping();
        }
      }
    }
  }

  if (onGround && !isSteppingCarefully()) {
    const Pos below = getOnPosLegacy(world);
    const BlockState state = world.getBlock(below);
    if (state.is(Blocks::REDSTONE_ORE) ||
        state.is(Blocks::DEEPSLATE_REDSTONE_ORE)) {
      redstoneOreStepOn(world, below, state, *this);
    }
  }

  // ServerPlayer driven externally by client packets skips passive wander
  // unless non-zero impulse/velocity is active (e.g. knockback/riding).
  if (hurtMarked || !isPassenger()) {
    if (deltaMovement.lengthSqr() > 1.0e-7) {
      travel(world, {static_cast<double>(xxa), static_cast<double>(yya),
                     static_cast<double>(zza)});
    } else {
      applyEffectsFromBlocks(world);
    }
  }
  if (!isSpectator())
    pushEntities(world);
  touchNearbyEntities(world);
}

bool allPlayersSleptThroughNight(const World &world) {
  bool anyNonSpectator = false;
  for (const auto &e : world.entities) {
    const auto *p = dynamic_cast<const ServerPlayer *>(e.get());
    if (!p || p->isRemoved() || p->isSpectator())
      continue;
    anyNonSpectator = true;
    if (!p->isSleeping() ||
        p->sleepCounter < ServerPlayer::kSleepDurationTicks) {
      return false;
    }
  }
  return anyNonSpectator;
}

} // namespace redstone
