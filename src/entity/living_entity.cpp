#include "redstone/entity.hpp"

#include "redstone/shapes.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace redstone {
namespace {

constexpr double kDrowningDamageIntervalStop = -20.0;
constexpr double kClimbingMaxHoriz = 0.15;
constexpr double kClimbingMinVert = -0.15;
constexpr double kPi = std::numbers::pi;

bool isClimbableBlock(const BlockState &state) {
  return state.is(Blocks::LADDER) || state.is(Blocks::VINE) ||
         state.is(Blocks::SCAFFOLDING) || state.is(Blocks::WEEPING_VINES) ||
         state.is(Blocks::WEEPING_VINES_PLANT) ||
         state.is(Blocks::TWISTING_VINES) ||
         state.is(Blocks::TWISTING_VINES_PLANT) ||
         state.is(Blocks::CAVE_VINES) || state.is(Blocks::CAVE_VINES_PLANT);
}

bool trapdoorUsableAsLadder(World &world, Pos pos, const BlockState &state) {
  if (!state.block().isTrapdoor() || state.getView("open") != "true")
    return false;
  const BlockState below = world.getBlock(pos.relative(Direction::Down));
  return below.is(Blocks::LADDER) &&
         below.getView("facing") == state.getView("facing");
}

int armorSlotProtection(const ItemStack &stack) {
  if (stack.empty())
    return 0;
  std::string_view id = stack.id;
  if (id.rfind("minecraft:", 0) == 0)
    id.remove_prefix(10);
  if (id == "netherite_helmet" || id == "diamond_helmet")
    return 3;
  if (id == "iron_helmet" || id == "chainmail_helmet" || id == "turtle_helmet")
    return 2;
  if (id == "golden_helmet" || id == "copper_helmet")
    return 2;
  if (id == "leather_helmet")
    return 1;
  if (id == "netherite_chestplate" || id == "diamond_chestplate")
    return 8;
  if (id == "iron_chestplate")
    return 6;
  if (id == "chainmail_chestplate" || id == "golden_chestplate" ||
      id == "copper_chestplate")
    return 5;
  if (id == "leather_chestplate")
    return 3;
  if (id == "netherite_leggings" || id == "diamond_leggings")
    return 6;
  if (id == "iron_leggings")
    return 5;
  if (id == "chainmail_leggings")
    return 4;
  if (id == "golden_leggings" || id == "copper_leggings")
    return 3;
  if (id == "leather_leggings")
    return 2;
  if (id == "netherite_boots" || id == "diamond_boots")
    return 3;
  if (id == "iron_boots")
    return 2;
  if (id == "chainmail_boots" || id == "golden_boots" || id == "copper_boots" ||
      id == "leather_boots")
    return 1;
  return 0;
}

bool boxCollidesWithBlocks(World &world, const Aabb &box) {
  std::vector<ShapeBox> boxes;
  world.collectCollisionBoxes(box, boxes);
  return !boxes.empty();
}

bool shouldApplyPeriodicEffect(MobEffectId id, int duration, int amplifier) {
  int baseInterval = 0;
  switch (id) {
  case MobEffectId::Regeneration:
    baseInterval = 50;
    break;
  case MobEffectId::Poison:
    baseInterval = 25;
    break;
  case MobEffectId::Wither:
    baseInterval = 40;
    break;
  default:
    return false;
  }
  const int interval = baseInterval >> std::max(0, amplifier);
  return interval <= 0 || (duration % interval) == 0;
}

} // namespace

DamageSource DamageSource::fromKind(DamageKind kind, Entity *causing,
                                    Entity *direct, std::optional<Vec3> pos) {
  DamageSource src;
  src.kind = kind;
  src.causingEntity = causing;
  src.directEntity = direct ? direct : causing;
  src.sourcePosition = pos;
  switch (kind) {
  case DamageKind::Fire:
    src.isFire = true;
    src.bypassesArmor = true;
    break;
  case DamageKind::Lava:
    src.isFire = true;
    break;
  case DamageKind::Explosion:
    src.isExplosion = true;
    src.alwaysKillsArmorStands = true;
    break;
  case DamageKind::FallingBlock:
  case DamageKind::Anvil:
  case DamageKind::FallingStalactite:
    src.damagesHelmet = true;
    break;
  case DamageKind::Projectile:
    src.canBreakArmorStand = true;
    break;
  case DamageKind::Magic:
    src.bypassesArmor = true;
    src.bypassesResistance = true;
    break;
  case DamageKind::Fall:
  case DamageKind::EnderPearl:
    src.isFall = true;
    src.bypassesArmor = true;
    src.noKnockback = true;
    break;
  case DamageKind::Attack:
    src.canBreakArmorStand = true;
    break;
  case DamageKind::Cramming:
  case DamageKind::Drown:
  case DamageKind::InWall:
    src.bypassesArmor = true;
    src.noKnockback = true;
    break;
  case DamageKind::Freeze:
    src.isFreezing = true;
    src.bypassesArmor = true;
    src.noKnockback = true;
    break;
  case DamageKind::Starve:
    src.bypassesArmor = true;
    src.noKnockback = true;
    break;
  case DamageKind::Wither:
    src.bypassesArmor = true;
    src.noKnockback = true;
    break;
  case DamageKind::OutOfBorder:
  case DamageKind::GenericKill:
    src.bypassesArmor = true;
    src.bypassesInvulnerability = true;
    src.bypassesEffects = true;
    src.alwaysKillsArmorStands = true;
    break;
  case DamageKind::Cactus:
  case DamageKind::SweetBerryBush:
  case DamageKind::Generic:
    break;
  }
  return src;
}

float CombatRules::getDamageAfterAbsorb(float damage, float totalArmor,
                                        float armorToughness) {
  const float toughnessTerm = 2.0f + armorToughness / 4.0f;
  const float effectiveArmor =
      std::clamp(totalArmor - damage / toughnessTerm, totalArmor * 0.2f, 20.0f);
  return damage * (1.0f - effectiveArmor / 25.0f);
}

float CombatRules::getDamageAfterMagicAbsorb(float damage,
                                             float enchantPoints) {
  const float clamped = std::clamp(enchantPoints, 0.0f, 20.0f);
  return damage * (1.0f - clamped / 25.0f);
}

LivingEntity::LivingEntity(std::string_view typeId) : Entity(std::string(typeId)) {
  attributes_.registerLivingDefaults();
  entityData_.define(kDataLivingFlagsId, EntityDataSerializer::Byte,
                     std::int8_t{0});
  entityData_.define(kDataHealthId, EntityDataSerializer::Float, 1.0f);
  entityData_.define(kDataEffectAmbienceId, EntityDataSerializer::Boolean,
                     false);
  entityData_.define(kDataArrowCountId, EntityDataSerializer::Int, 0);
  entityData_.define(kDataStingerCountId, EntityDataSerializer::Int, 0);
  entityData_.define(kDataSleepingPosId, EntityDataSerializer::OptionalBlockPos,
                     std::optional<Pos>{});
  setHealth(maxHealth());
}

void LivingEntity::onSyncedDataUpdated(std::uint8_t id) {
  Entity::onSyncedDataUpdated(id);
  if (id == kDataHealthId) {
    cachedHealth_ = entityData_.get<float>(kDataHealthId);
    syncSectionPushable();
  }
}

void LivingEntity::setHealth(float value) {
  const float clamped = std::clamp(value, 0.0f, maxHealth());
  entityData_.set(kDataHealthId, clamped);
  cachedHealth_ = clamped;
  syncSectionPushable();
}

void LivingEntity::heal(float amount) {
  const float current = health();
  if (current > 0.0f && amount > 0.0f)
    setHealth(current + amount);
}

void LivingEntity::setAbsorptionAmount(float amount) {
  absorptionAmount_ = std::clamp(amount, 0.0f, maxAbsorption());
}

bool LivingEntity::addEffect(MobEffectInstance instance, Entity *source) {
  (void)source;
  const MobEffectDef &def = mobEffectDef(instance.effect);
  if (def.instantaneous) {
    if (instance.effect == MobEffectId::InstantHealth) {
      if (!isInvertedHealAndHarm())
        heal(static_cast<float>(std::max(4 << instance.amplifier, 0)));
    }
    return true;
  }
  auto it = activeEffects_.find(instance.effect);
  if (it == activeEffects_.end()) {
    const MobEffectId id = instance.effect;
    const int amp = instance.amplifier;
    activeEffects_.emplace(id, std::move(instance));
    addMobEffectAttributeModifiers(id, attributes_, amp);
    refreshDirtyAttributes();
    return true;
  }
  if (it->second.update(instance)) {
    removeMobEffectAttributeModifiers(it->second.effect, attributes_);
    addMobEffectAttributeModifiers(it->second.effect, attributes_,
                                   it->second.amplifier);
    refreshDirtyAttributes();
    return true;
  }
  return false;
}

bool LivingEntity::removeEffect(MobEffectId id) {
  auto it = activeEffects_.find(id);
  if (it == activeEffects_.end())
    return false;
  activeEffects_.erase(it);
  removeMobEffectAttributeModifiers(id, attributes_);
  refreshDirtyAttributes();
  return true;
}

bool LivingEntity::removeAllEffects() {
  if (activeEffects_.empty())
    return false;
  for (const auto &[id, _] : activeEffects_)
    removeMobEffectAttributeModifiers(id, attributes_);
  activeEffects_.clear();
  refreshDirtyAttributes();
  return true;
}

const MobEffectInstance *LivingEntity::getEffect(MobEffectId id) const {
  auto it = activeEffects_.find(id);
  return it == activeEffects_.end() ? nullptr : &it->second;
}

const ItemStack &LivingEntity::getItemBySlot(EquipmentSlot slot) const {
  return equipment_[static_cast<std::size_t>(slot)];
}

void LivingEntity::setItemSlot(EquipmentSlot slot, ItemStack stack) {
  equipment_[static_cast<std::size_t>(slot)] = std::move(stack);
}

int LivingEntity::armorValue() const {
  int armor =
      static_cast<int>(std::floor(attributes_.getValue(AttributeId::Armor)));
  if (armor > 0)
    return armor;
  for (EquipmentSlot slot : {EquipmentSlot::Feet, EquipmentSlot::Legs,
                             EquipmentSlot::Chest, EquipmentSlot::Head}) {
    armor += armorSlotProtection(getItemBySlot(slot));
  }
  return armor;
}

void LivingEntity::syncDataFields() { Entity::syncDataFields(); }

void LivingEntity::tick(World &world) {
  baseTick(world);
  if (!isRemoved())
    aiStep(world);
}

void LivingEntity::baseTick(World &world) {
  Entity::baseTick(world);

  if (isAlive()) {
    const Vec3 eye = eyePosition();
    const double testWidth = static_cast<double>(dimensions_.width) * 0.8;
    const Aabb eyeBox{eye.x - testWidth * 0.5, eye.y - 1.0e-6,
                      eye.z - testWidth * 0.5, eye.x + testWidth * 0.5,
                      eye.y + 1.0e-6,          eye.z + testWidth * 0.5};
    if (!noPhysics && boxCollidesWithBlocks(world, eyeBox)) {
      hurtServer(world, DamageSource::fromKind(DamageKind::InWall), 1.0f);
    }

    const Pos eyeBlock{static_cast<int>(std::floor(eye.x)),
                       static_cast<int>(std::floor(eye.y)),
                       static_cast<int>(std::floor(eye.z))};
    const bool inBubbleColumn =
        world.getBlock(eyeBlock).is(Blocks::BUBBLE_COLUMN);
    if (isEyeInWater() && !inBubbleColumn) {
      const bool canDrown =
          !canBreatheUnderwater() && !hasEffect(MobEffectId::WaterBreathing) &&
          !hasEffect(MobEffectId::ConduitPower) &&
          !hasEffect(MobEffectId::BreathOfTheNautilus);
      if (canDrown) {
        const double oxygenBonus = attributes_.getValue(AttributeId::OxygenBonus);
        int supply = airSupply();
        if (oxygenBonus <= 0.0 ||
            world.random.nextDouble() < 1.0 / (oxygenBonus + 1.0)) {
          --supply;
        }
        setAirSupply(supply);
        if (supply <= kDrowningDamageIntervalStop) {
          setAirSupply(0);
          hurtServer(world, DamageSource::fromKind(DamageKind::Drown), 2.0f);
        }
      }
    } else if (airSupply() < maxAirSupply()) {
      setAirSupply(std::min(airSupply() + 4, maxAirSupply()));
    }
  }

  if (hurtTime > 0)
    --hurtTime;
  if (invulnerableTime > 0 && !isPlayer())
    --invulnerableTime;
  if (isDeadOrDying())
    tickDeath(world);
  if (lastHurtByPlayerMemoryTime_ > 0)
    --lastHurtByPlayerMemoryTime_;

  tickEffects(world);
}

void LivingEntity::tickDeath(World &world) {
  (void)world;
  ++deathTime;
  if (deathTime >= 20 && !isRemoved())
    discard();
}

void LivingEntity::die(World &world, const DamageSource &source) {
  (void)world;
  (void)source;
}

void LivingEntity::tickEffects(World &world) {
  std::vector<MobEffectId> expired;
  for (auto &[id, inst] : activeEffects_) {
    if (shouldApplyPeriodicEffect(id, inst.duration, inst.amplifier)) {
      if (id == MobEffectId::Regeneration) {
        if (health() < maxHealth())
          heal(1.0f);
      } else if (id == MobEffectId::Poison) {
        if (health() > 1.0f)
          hurtServer(world, DamageSource::fromKind(DamageKind::Magic), 1.0f);
      } else if (id == MobEffectId::Wither) {
        hurtServer(world, DamageSource::fromKind(DamageKind::Wither), 1.0f);
      }
    }
    const int remaining = inst.tickDownDuration();
    if (!inst.isInfiniteDuration() && remaining <= 0)
      expired.push_back(id);
  }
  for (MobEffectId id : expired) {
    auto it = activeEffects_.find(id);
    if (it == activeEffects_.end())
      continue;
    if (it->second.hiddenEffect && it->second.hiddenEffect->hasRemainingDuration()) {
      auto next = std::move(*it->second.hiddenEffect);
      it->second = std::move(next);
      removeMobEffectAttributeModifiers(id, attributes_);
      addMobEffectAttributeModifiers(id, attributes_, it->second.amplifier);
    } else {
      activeEffects_.erase(it);
      removeMobEffectAttributeModifiers(id, attributes_);
    }
  }
  if (!expired.empty())
    refreshDirtyAttributes();
}

void LivingEntity::refreshDirtyAttributes() {
  for (AttributeId id : attributes_.takeUpdateDirtyIds())
    onAttributeUpdated(id);
}

void LivingEntity::onAttributeUpdated(AttributeId id) {
  if (id == AttributeId::MaxHealth && health() > maxHealth())
    setHealth(maxHealth());
  else if (id == AttributeId::MaxAbsorption &&
           absorptionAmount() > maxAbsorption())
    setAbsorptionAmount(maxAbsorption());
  else if (id == AttributeId::Scale)
    refreshDimensions();
}

void LivingEntity::aiStep(World &world) {
  if (noJumpDelay > 0)
    --noJumpDelay;

  Vec3 m = deltaMovement;
  if (isPlayer()) {
    if (m.x * m.x + m.z * m.z < 9.0e-6) {
      m.x = 0.0;
      m.z = 0.0;
    }
  } else {
    if (std::abs(m.x) < 0.003)
      m.x = 0.0;
    if (std::abs(m.z) < 0.003)
      m.z = 0.0;
  }
  if (std::abs(m.y) < 0.003)
    m.y = 0.0;
  deltaMovement = m;

  // TODO: Stage E-5 Mob::serverAiStep (GoalSelector / Brain / PathNavigation).
  if (!isAlive()) {
    jumping = false;
    xxa = 0.0f;
    zza = 0.0f;
  } else {
    serverAiStep(world);
  }

  if (jumping && isAffectedByFluids()) {
    const double fluidH = isInLava() ? fluidHeight(true) : fluidHeight(false);
    const bool inWaterWithH = isInWater() && fluidH > 0.0;
    const double threshold = getFluidJumpThreshold();
    if (inWaterWithH && (!onGround || fluidH > threshold)) {
      jumpInLiquid(true);
    } else if (isInLava() && (!onGround || fluidH > threshold)) {
      jumpInLiquid(false);
    } else if ((onGround || (inWaterWithH && fluidH <= threshold)) &&
               noJumpDelay == 0) {
      jumpFromGround(world);
      noJumpDelay = 10;
    }
  } else {
    noJumpDelay = 0;
  }

  xxa *= 0.98f;
  zza *= 0.98f;
  const Vec3 travelInput{static_cast<double>(xxa), static_cast<double>(yya),
                         static_cast<double>(zza)};
  if (isPassenger()) {
    // Wait for vehicle positioning in World::tick.
  } else {
    travel(world, travelInput);
  }

  applyEffectsFromBlocks(world);
  pushEntities(world);
}

bool LivingEntity::onClimbable(World &world) {
  if (isSpectator())
    return false;
  const Pos blockPos{static_cast<int>(std::floor(position.x)),
                     static_cast<int>(std::floor(position.y)),
                     static_cast<int>(std::floor(position.z))};
  const BlockState state = world.getBlock(blockPos);
  if (isClimbableBlock(state) || trapdoorUsableAsLadder(world, blockPos, state)) {
    lastClimbablePos_ = blockPos;
    return true;
  }
  return false;
}

double LivingEntity::getEffectiveGravity() const {
  if (isNoGravity())
    return 0.0;
  double gravity = attributes_.getValue(AttributeId::Gravity);
  if (deltaMovement.y <= 0.0 && hasEffect(MobEffectId::SlowFalling))
    gravity = std::min(gravity, 0.01);
  return gravity;
}

void LivingEntity::moveRelative(float amount, Vec3 input) {
  const double lenSqr = input.lengthSqr();
  if (lenSqr < 1.0e-7)
    return;
  const Vec3 normalized =
      lenSqr > 1.0 ? input * (1.0 / std::sqrt(lenSqr)) : input;
  const Vec3 scaled = normalized * static_cast<double>(amount);
  const double rad = static_cast<double>(yaw) * (kPi / 180.0);
  const double sinYaw = std::sin(rad);
  const double cosYaw = std::cos(rad);
  deltaMovement.x += scaled.x * cosYaw - scaled.z * sinYaw;
  deltaMovement.y += scaled.y;
  deltaMovement.z += scaled.z * cosYaw + scaled.x * sinYaw;
}

Vec3 LivingEntity::handleOnClimbable(World &world, Vec3 delta) {
  if (!onClimbable(world))
    return delta;
  resetFallDistance();
  const double x = std::clamp(delta.x, -kClimbingMaxHoriz, kClimbingMaxHoriz);
  const double z = std::clamp(delta.z, -kClimbingMaxHoriz, kClimbingMaxHoriz);
  double y = std::max(delta.y, kClimbingMinVert);
  const Pos blockPos{static_cast<int>(std::floor(position.x)),
                     static_cast<int>(std::floor(position.y)),
                     static_cast<int>(std::floor(position.z))};
  if (y < 0.0 && !world.getBlock(blockPos).is(Blocks::SCAFFOLDING) &&
      isShiftKeyDown() && isPlayer()) {
    y = 0.0;
  }
  return {x, y, z};
}

Vec3 LivingEntity::getFluidFallingAdjustedMovement(double baseGravity,
                                                   bool isFalling,
                                                   Vec3 movement) const {
  if (baseGravity != 0.0 && !isSprinting()) {
    double yd = movement.y;
    if (isFalling && std::abs(movement.y - 0.005) >= 0.003 &&
        std::abs(movement.y - baseGravity / 16.0) < 0.003) {
      yd = -0.003;
    } else {
      yd = movement.y - baseGravity / 16.0;
    }
    return {movement.x, yd, movement.z};
  }
  return movement;
}

void LivingEntity::jumpFromGround(World &world) {
  (void)world;
  double jumpPower = attributes_.getValue(AttributeId::JumpStrength);
  if (const auto *jb = getEffect(MobEffectId::JumpBoost))
    jumpPower += 0.1 * static_cast<double>(jb->amplifier + 1);
  if (jumpPower <= 1.0e-5)
    return;
  deltaMovement = {deltaMovement.x, std::max(jumpPower, deltaMovement.y),
                   deltaMovement.z};
  if (isSprinting()) {
    const double rad = static_cast<double>(yaw) * (kPi / 180.0);
    deltaMovement.x += -std::sin(rad) * 0.2;
    deltaMovement.z += std::cos(rad) * 0.2;
  }
}

void LivingEntity::jumpInLiquid(bool isWater) {
  (void)isWater;
  deltaMovement.y += 0.03999999910593033;
}

void LivingEntity::jumpOutOfFluid(World &world, double oldY) {
  const Vec3 m = deltaMovement;
  if (!horizontalCollision)
    return;
  const Aabb testBox =
      boundingBox().move({m.x, m.y + 0.6 - position.y + oldY, m.z});
  if (!boxCollidesWithBlocks(world, testBox))
    deltaMovement = {m.x, 0.3, m.z};
}

void LivingEntity::travel(World &world, Vec3 input) {
  if (isInWater() || isInLava())
    travelInFluid(world, input);
  else
    travelInAir(world, input);
}

void LivingEntity::travelInAir(World &world, Vec3 input) {
  const Pos posBelow = getBlockPosBelowThatAffectsMyMovement(world);
  const float blockFriction =
      onGround ? world.getBlock(posBelow).block().metadata().friction : 1.0f;
  const float friction = blockFriction * 0.91f;

  const float speed =
      onGround ? speed_ * (0.21600002f /
                           (blockFriction * blockFriction * blockFriction))
               : 0.02f;
  moveRelative(speed, input);
  deltaMovement = handleOnClimbable(world, deltaMovement);
  move(world, MoverType::Self, deltaMovement);

  Vec3 moved = deltaMovement;
  if ((horizontalCollision || jumping) &&
      (onClimbable(world) || isInPowderSnow || wasInPowderSnow)) {
    moved = {moved.x, 0.2, moved.z};
  }

  double my = moved.y;
  if (const auto *levitation = getEffect(MobEffectId::Levitation)) {
    my += (0.05 * static_cast<double>(levitation->amplifier + 1) - my) * 0.2;
  } else {
    my -= getEffectiveGravity();
  }
  deltaMovement = {moved.x * friction, my * 0.98, moved.z * friction};
}

void LivingEntity::travelInFluid(World &world, Vec3 input) {
  const bool isFalling = deltaMovement.y <= 0.0;
  const double oldY = position.y;
  const double baseGravity = getEffectiveGravity();
  if (isInWater())
    travelInWater(world, input, baseGravity, isFalling, oldY);
  else
    travelInLava(world, input, baseGravity, isFalling, oldY);
}

void LivingEntity::travelInWater(World &world, Vec3 input, double baseGravity,
                                 bool isFalling, double oldY) {
  float slowDown = isSprinting() ? 0.9f : getWaterSlowDown();
  float waterSpeed = 0.02f;
  float waterEfficiency =
      static_cast<float>(attributes_.getValue(AttributeId::WaterMovementEfficiency));
  if (!onGround)
    waterEfficiency *= 0.5f;
  if (waterEfficiency > 0.0f) {
    slowDown += (0.54600006f - slowDown) * waterEfficiency;
    waterSpeed += (speed_ - waterSpeed) * waterEfficiency;
  }
  if (hasEffect(MobEffectId::DolphinsGrace))
    slowDown = 0.96f;

  moveRelative(waterSpeed, input);
  move(world, MoverType::Self, deltaMovement);
  Vec3 moved = deltaMovement;
  if (horizontalCollision && onClimbable(world))
    moved = {moved.x, 0.2, moved.z};
  moved = {moved.x * slowDown, moved.y * 0.8, moved.z * slowDown};
  deltaMovement = getFluidFallingAdjustedMovement(baseGravity, isFalling, moved);
  jumpOutOfFluid(world, oldY);
}

void LivingEntity::travelInLava(World &world, Vec3 input, double baseGravity,
                                bool isFalling, double oldY) {
  moveRelative(0.02f, input);
  move(world, MoverType::Self, deltaMovement);
  if (fluidHeight(true) <= getFluidJumpThreshold()) {
    const Vec3 scaled{deltaMovement.x * 0.5, deltaMovement.y * 0.8,
                      deltaMovement.z * 0.5};
    deltaMovement =
        getFluidFallingAdjustedMovement(baseGravity, isFalling, scaled);
  } else {
    deltaMovement = deltaMovement * 0.5;
  }
  if (baseGravity != 0.0)
    deltaMovement.y -= baseGravity / 4.0;
  jumpOutOfFluid(world, oldY);
}

void LivingEntity::pushEntities(World &world) {
  if (!isPushable())
    return;
  const bool applyPush = !noPhysics;
  const bool selfCanReceivePush = applyPush && !isVehicle();
  const bool selfPassenger = applyPush && isPassenger();
  const Entity *const selfRoot = selfPassenger ? &getRootVehicle() : this;
  double selfAccX = 0.0;
  double selfAccZ = 0.0;
  bool anySelfPush = false;
  const int crammingCount = world.pushLivingEntities(
      *this, boundingBox(), applyPush, selfCanReceivePush, selfPassenger,
      selfRoot, position.x, position.z, selfAccX, selfAccZ, anySelfPush);
  constexpr int kDefaultMaxEntityCramming = 24;
  if (crammingCount > kDefaultMaxEntityCramming - 1 &&
      world.random.nextInt(4) == 0) {
    hurtServer(world, DamageSource::fromKind(DamageKind::Cramming), 6.0f);
  }
  if (anySelfPush && !isVehicle() && isPushable())
    push(selfAccX, 0.0, selfAccZ);
}

void LivingEntity::doPush(Entity &other) {
  if (isPassengerOfSameVehicle(other) || noPhysics || other.noPhysics)
    return;
  double dx = other.position.x - position.x;
  double dz = other.position.z - position.z;
  double distMax = std::max(std::abs(dx), std::abs(dz));
  if (distMax < 0.01)
    return;
  distMax = std::sqrt(distMax);
  dx /= distMax;
  dz /= distMax;
  double inv = 1.0 / distMax;
  if (inv > 1.0)
    inv = 1.0;
  dx *= inv * 0.05;
  dz *= inv * 0.05;
  if (!isVehicle() && isPushable())
    push(-dx, 0.0, -dz);
  if (!other.isVehicle() && other.isPushable())
    other.push(dx, 0.0, dz);
}

bool LivingEntity::hurt(World &world, DamageKind kind, float amount) {
  return hurtServer(world, DamageSource::fromKind(kind), amount);
}

bool LivingEntity::hurtServer(World &world, const DamageSource &source,
                              float damage) {
  if (isDeadOrDying())
    return false;
  if (source.isFire && hasEffect(MobEffectId::FireResistance))
    return false;
  if (source.isFire && fireImmune())
    return false;

  if (isSleeping())
    stopSleeping();

  if (damage < 0.0f)
    damage = 0.0f;

  const float blocked = applyItemBlocking(world, source, damage);
  damage -= blocked;
  const bool wasBlocked = blocked > 0.0f;

  if (source.damagesHelmet && !getItemBySlot(EquipmentSlot::Head).empty()) {
    hurtHelmet(source, damage);
    damage *= 0.75f;
  }

  bool tookFullDamage = true;
  if (static_cast<float>(invulnerableTime) > 10.0f && !source.bypassesCooldown) {
    if (damage <= lastHurt)
      return false;
    actuallyHurt(world, source, damage - lastHurt);
    lastHurt = damage;
    tookFullDamage = false;
  } else {
    lastHurt = damage;
    invulnerableTime = kInvulnerableDuration;
    actuallyHurt(world, source, damage);
    hurtDuration = 10;
    hurtTime = hurtDuration;
  }

  if (source.causingEntity && source.causingEntity->isPlayer())
    lastHurtByPlayerMemoryTime_ = 100;

  if (tookFullDamage) {
    if (!source.noImpact && (!wasBlocked || damage > 0.0f))
      hurtMarked = true;

    if (!source.noKnockback) {
      double xd = 0.0;
      double zd = 0.0;
      if (source.sourcePosition.has_value()) {
        xd = source.sourcePosition->x - position.x;
        zd = source.sourcePosition->z - position.z;
      } else if (source.directEntity) {
        xd = source.directEntity->position.x - position.x;
        zd = source.directEntity->position.z - position.z;
      } else if (source.causingEntity) {
        xd = source.causingEntity->position.x - position.x;
        zd = source.causingEntity->position.z - position.z;
      }
      knockback(world, 0.4, xd, zd);
    }
  }

  if (isDeadOrDying())
    die(world, source);

  return !wasBlocked || damage > 0.0f;
}

float LivingEntity::getDamageAfterArmorAbsorb(const DamageSource &source,
                                              float damage) {
  if (!source.bypassesArmor) {
    hurtArmor(source, damage);
    damage = CombatRules::getDamageAfterAbsorb(
        damage, static_cast<float>(armorValue()),
        static_cast<float>(attributes_.getValue(AttributeId::ArmorToughness)));
  }
  return damage;
}

float LivingEntity::getDamageAfterMagicAbsorb(const DamageSource &source,
                                              float damage) {
  if (source.bypassesEffects)
    return damage;
  if (const auto *resistance = getEffect(MobEffectId::Resistance)) {
    if (!source.bypassesResistance) {
      const int absorb = std::max(0, 25 - (resistance->amplifier + 1) * 5);
      damage = std::max(damage * static_cast<float>(absorb) / 25.0f, 0.0f);
    }
  }
  return damage;
}

void LivingEntity::actuallyHurt(World &world, const DamageSource &source,
                                float damage) {
  (void)world;
  damage = getDamageAfterArmorAbsorb(source, damage);
  damage = getDamageAfterMagicAbsorb(source, damage);
  const float beforeAbsorb = damage;
  damage = std::max(damage - absorptionAmount_, 0.0f);
  setAbsorptionAmount(absorptionAmount_ - (beforeAbsorb - damage));
  if (damage > 0.0f)
    setHealth(health() - damage);
}

void LivingEntity::knockback(World &world, double power, double xd, double zd) {
  power *= 1.0 - attributes_.getValue(AttributeId::KnockbackResistance);
  if (power <= 0.0)
    return;
  hurtMarked = true;
  ++motionRevision;
  while (xd * xd + zd * zd < 1.0e-5) {
    xd = (world.random.nextDouble() - world.random.nextDouble()) * 0.01;
    zd = (world.random.nextDouble() - world.random.nextDouble()) * 0.01;
  }
  const double len = std::sqrt(xd * xd + zd * zd);
  const double nx = (xd / len) * power;
  const double nz = (zd / len) * power;
  deltaMovement = {
      deltaMovement.x / 2.0 - nx,
      onGround ? std::min(0.4, deltaMovement.y / 2.0 + power) : deltaMovement.y,
      deltaMovement.z / 2.0 - nz};
}

int LivingEntity::calculateFallDamage(double distance,
                                      float damageMultiplier) const {
  const double safeFall = attributes_.getValue(AttributeId::SafeFallDistance);
  const double fallPower = distance + 1.0e-6 - safeFall;
  const double mult = attributes_.getValue(AttributeId::FallDamageMultiplier);
  return static_cast<int>(std::floor(
      fallPower * static_cast<double>(damageMultiplier) * mult));
}

bool LivingEntity::causeFallDamage(World &world, double distance,
                                   float damageMultiplier, DamageKind kind) {
  const bool damaged =
      Entity::causeFallDamage(world, distance, damageMultiplier, kind);
  const int dmg = calculateFallDamage(distance, damageMultiplier);
  if (dmg > 0) {
    hurtServer(world, DamageSource::fromKind(kind), static_cast<float>(dmg));
    return true;
  }
  return damaged;
}

} // namespace redstone
