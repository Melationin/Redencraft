#pragma once
/// @file living_entity.hpp
/// @brief 定义生物基类 `LivingEntity`、装备槽位、伤害源/护甲减伤规则以及盔甲架 `ArmorStand`。
///
/// 在 `Entity` 之上扩展生命值、属性系统、药水效果、装备栏、受伤无敌帧、
/// 攀爬与流体/空中运动等原版 26.1 生物共有语义。

#include "redstone/entity/attributes.hpp"
#include "redstone/entity/mob_effects.hpp"
#include "redstone/hash.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace redstone {

/// 生物装备与手持槽位枚举，对应原版 `EquipmentSlot` 与盔甲架 `DisabledSlots` 位偏移。
enum class EquipmentSlot : std::uint8_t {
  MainHand = 0,
  OffHand = 1,
  Feet = 2,
  Legs = 3,
  Chest = 4,
  Head = 5,
  Body = 6,
  Saddle = 7,
  Count = 8
};

/// 伤害来源上下文：同时携带致伤类型、直接/间接施害实体、来源位置以及各类穿透标志，
/// 供 `LivingEntity::hurtServer` 与 `ArmorStand::hurtServer` 精确判定护甲、抗性与击退行为。
struct DamageSource {
  DamageKind kind = DamageKind::Generic;
  Entity *causingEntity = nullptr;
  Entity *directEntity = nullptr;
  std::optional<Vec3> sourcePosition;
  bool bypassesArmor = false;
  bool bypassesInvulnerability = false;
  bool bypassesEffects = false;
  bool bypassesResistance = false;
  bool bypassesCooldown = false;
  bool noKnockback = false;
  bool noImpact = false;
  bool damagesHelmet = false;
  bool isExplosion = false;
  bool isFire = false;
  bool isFall = false;
  bool isFreezing = false;
  bool isCreativePlayer = false;
  bool alwaysKillsArmorStands = false;
  bool canBreakArmorStand = false;

/// 按 `DamageKind` 构造默认标志完备的 `DamageSource`，避免调用方遗漏原版穿透规则（如窒息/虚空无视护甲）。
  static DamageSource fromKind(DamageKind kind, Entity *causing = nullptr,
                               Entity *direct = nullptr,
                               std::optional<Vec3> pos = std::nullopt);
};

/// 原版战斗伤害折算规则，分别计算护甲/韧性减伤与附魔保护点减伤。
struct CombatRules {
  static float getDamageAfterAbsorb(float damage, float totalArmor,
                                    float armorToughness);
  static float getDamageAfterMagicAbsorb(float damage, float enchantPoints);
};

/// 生物实体基类，对齐原版 26.1 `net.minecraft.world.entity.LivingEntity`。
class LivingEntity : public Entity {
public:
  static constexpr std::uint8_t kDataLivingFlagsId = 8;
  static constexpr std::uint8_t kDataHealthId = 9;
  static constexpr std::uint8_t kDataEffectAmbienceId = 11;
  static constexpr std::uint8_t kDataArrowCountId = 12;
  static constexpr std::uint8_t kDataStingerCountId = 13;
  static constexpr std::uint8_t kDataSleepingPosId = 14;

  static constexpr std::int8_t kLivingFlagIsUsing = 1;
  static constexpr std::int8_t kLivingFlagOffHand = 2;
  static constexpr std::int8_t kLivingFlagSpinAttack = 4;
  static constexpr int kInvulnerableDuration = 20;

  explicit LivingEntity(std::string_view typeId);

  bool isLiving() const override { return true; }
  bool isPickable() const override { return !isRemoved(); }
  bool isPushable() const override { return isAlive() && !isSpectator(); }
  bool canBeCollidedWith() const override { return false; }
  float maxUpStep() const override {
    return static_cast<float>(attributes_.getValue(AttributeId::StepHeight));
  }
  bool isAlive() const override { return !isRemoved() && health() > 0.0f; }
  bool isDeadOrDying() const { return health() <= 0.0f; }
  virtual bool isSpectator() const { return false; }

  /// 直接读取本地缓存的生命值，避免在 `isAlive()` / `isPushable()` 等高频热路径上反复检索 `SynchedEntityData`。
  float health() const noexcept { return cachedHealth_; }
  void setHealth(float value);
  float maxHealth() const {
    return static_cast<float>(attributes_.getValue(AttributeId::MaxHealth));
  }
  void heal(float amount);

  float absorptionAmount() const noexcept { return absorptionAmount_; }
  void setAbsorptionAmount(float amount);
  float maxAbsorption() const {
    return static_cast<float>(
        attributes_.getValue(AttributeId::MaxAbsorption));
  }

  AttributeMap &attributes() noexcept { return attributes_; }
  const AttributeMap &attributes() const noexcept { return attributes_; }
  double attributeValue(AttributeId id) const {
    return attributes_.getValue(id);
  }

  bool addEffect(MobEffectInstance instance, Entity *source = nullptr);
  bool removeEffect(MobEffectId id);
  bool removeAllEffects();
  bool hasEffect(MobEffectId id) const { return activeEffects_.contains(id); }
  const MobEffectInstance *getEffect(MobEffectId id) const;
  const phmap::flat_hash_map<MobEffectId, MobEffectInstance> &
  activeEffects() const noexcept {
    return activeEffects_;
  }

  const ItemStack &getItemBySlot(EquipmentSlot slot) const;
  void setItemSlot(EquipmentSlot slot, ItemStack stack);
  int armorValue() const;

  void setSpeed(float value) noexcept { speed_ = value; }
  float speed() const noexcept { return speed_; }

  bool isSleeping() const { return sleepingPos().has_value(); }
  std::optional<Pos> sleepingPos() const {
    return entityData_.get<std::optional<Pos>>(kDataSleepingPosId);
  }
  void setSleepingPos(std::optional<Pos> pos) {
    entityData_.set(kDataSleepingPosId, pos);
  }
  virtual void stopSleeping() { setSleepingPos(std::nullopt); }

  bool isAffectedByFluids() const { return true; }
  bool isSensitiveToWater() const { return false; }
  bool canBreatheUnderwater() const { return false; }
  bool isInvertedHealAndHarm() const { return false; }
  virtual float getWaterSlowDown() const { return 0.8f; }
  virtual double getFluidJumpThreshold() const {
    return static_cast<double>(eyeHeight_) < 0.4 ? 0.0 : 0.4;
  }

  void tick(World &world) override;
  void baseTick(World &world);
  virtual void aiStep(World &world);
  virtual void serverAiStep(World &world) { (void)world; }
  virtual void travel(World &world, Vec3 input);
  bool onClimbable(World &world);

  bool hurt(World &world, DamageKind kind, float amount) override;
  /// 服务端伤害入口，使用完整的 `DamageSource` 语义（与 `Entity::hurt` 区分以兼容原版 26.1）。
  virtual bool hurtServer(World &world, const DamageSource &source,
                          float amount);
  virtual void knockback(World &world, double power, double xd, double zd);
  bool causeFallDamage(World &world, double distance, float damageMultiplier,
                       DamageKind kind) override;
  void syncDataFields() override;

  int invulnerableTime = 0;
  int hurtTime = 0;
  int hurtDuration = 0;
  int deathTime = 0;
  float lastHurt = 0.0f;
  bool jumping = false;
  float xxa = 0.0f;
  float yya = 0.0f;
  float zza = 0.0f;
  int noJumpDelay = 0;

protected:
  void onSyncedDataUpdated(std::uint8_t id) override;
  virtual void tickDeath(World &world);
  virtual void die(World &world, const DamageSource &source);
  virtual void actuallyHurt(World &world, const DamageSource &source,
                            float damage);
  virtual float getDamageAfterArmorAbsorb(const DamageSource &source,
                                          float damage);
  virtual float getDamageAfterMagicAbsorb(const DamageSource &source,
                                          float damage);
  virtual void hurtHelmet(const DamageSource &source, float damage) {
    (void)source;
    (void)damage;
  }
  virtual void hurtArmor(const DamageSource &source, float damage) {
    (void)source;
    (void)damage;
  }
  virtual float applyItemBlocking(World &world, const DamageSource &source,
                                  float damage) {
    (void)world;
    (void)source;
    (void)damage;
    return 0.0f;
  }
  virtual void pushEntities(World &world);
  virtual void doPush(Entity &other);
  virtual void onAttributeUpdated(AttributeId id);

  void tickEffects(World &world);
  /// 消费 `AttributeMap` 中变脏的属性 ID 并触发生物状态联动（如最大血量下降时钳位当前生命值）。
  void refreshDirtyAttributes();
  int calculateFallDamage(double distance, float damageMultiplier) const;
  double getEffectiveGravity() const;
  Vec3 getFluidFallingAdjustedMovement(double baseGravity, bool isFalling,
                                       Vec3 movement) const;
  Vec3 handleOnClimbable(World &world, Vec3 delta);
  void moveRelative(float amount, Vec3 input);
  void travelInAir(World &world, Vec3 input);
  void travelInFluid(World &world, Vec3 input);
  void travelInWater(World &world, Vec3 input, double baseGravity,
                     bool isFalling, double oldY);
  void travelInLava(World &world, Vec3 input, double baseGravity,
                    bool isFalling, double oldY);
  void jumpOutOfFluid(World &world, double oldY);
  void jumpFromGround(World &world);
  void jumpInLiquid(bool isWater);

  AttributeMap attributes_;
  phmap::flat_hash_map<MobEffectId, MobEffectInstance> activeEffects_;
  std::array<ItemStack, static_cast<std::size_t>(EquipmentSlot::Count)>
      equipment_{};
  std::optional<Pos> lastClimbablePos_;
  std::optional<Pos> lastPos_;
  float absorptionAmount_ = 0.0f;
  /// `kDataHealthId` 的本地数值镜像：通过 `setHealth` 与 `onSyncedDataUpdated` 同步更新，
  /// 使 `health()`、`isAlive()` 与空间桶推挤判定无需线性查找 `SynchedEntityData::items_`。
  float cachedHealth_ = 1.0f;
  float speed_ = 0.1f;
  int lastHurtByPlayerMemoryTime_ = 0;
};

/// 盔甲架实体（`minecraft:armor_stand`），严格复刻原版 26.1 `ArmorStand` 的受击、标记模式与姿态语义。
class ArmorStand : public LivingEntity {
public:
  static constexpr std::uint8_t kDataClientFlagsId = 15;
  static constexpr std::uint8_t kDataHeadPoseId = 16;
  static constexpr std::uint8_t kDataBodyPoseId = 17;
  static constexpr std::uint8_t kDataLeftArmPoseId = 18;
  static constexpr std::uint8_t kDataRightArmPoseId = 19;
  static constexpr std::uint8_t kDataLeftLegPoseId = 20;
  static constexpr std::uint8_t kDataRightLegPoseId = 21;

  static constexpr std::int8_t kClientFlagSmall = 1;
  static constexpr std::int8_t kClientFlagShowArms = 4;
  static constexpr std::int8_t kClientFlagNoBasePlate = 8;
  static constexpr std::int8_t kClientFlagMarker = 16;

  ArmorStand(World &world, Vec3 pos);

  bool isPushable() const override { return false; }
  bool isIgnoringBlockTriggers() const override { return isMarker(); }
  bool ignoreExplosion(World &world) const override {
    (void)world;
    return isInvisible();
  }
  bool hasPhysics() const { return !isNoGravity() && !isMarker(); }

  bool isSmall() const {
    return (entityData_.get<std::int8_t>(kDataClientFlagsId) &
            kClientFlagSmall) != 0;
  }
  void setSmall(bool value);
  bool showArms() const {
    return (entityData_.get<std::int8_t>(kDataClientFlagsId) &
            kClientFlagShowArms) != 0;
  }
  void setShowArms(bool value);
  bool showBasePlate() const {
    return (entityData_.get<std::int8_t>(kDataClientFlagsId) &
            kClientFlagNoBasePlate) == 0;
  }
  void setNoBasePlate(bool value);
  bool isMarker() const {
    return (entityData_.get<std::int8_t>(kDataClientFlagsId) &
            kClientFlagMarker) != 0;
  }
  void setMarker(bool value);

  void setHeadPose(Rotations3f rot) {
    headPose_ = rot;
    entityData_.set(kDataHeadPoseId, rot);
  }
  void setBodyPose(Rotations3f rot) {
    bodyPose_ = rot;
    entityData_.set(kDataBodyPoseId, rot);
  }
  void setLeftArmPose(Rotations3f rot) {
    leftArmPose_ = rot;
    entityData_.set(kDataLeftArmPoseId, rot);
  }
  void setRightArmPose(Rotations3f rot) {
    rightArmPose_ = rot;
    entityData_.set(kDataRightArmPoseId, rot);
  }
  void setLeftLegPose(Rotations3f rot) {
    leftLegPose_ = rot;
    entityData_.set(kDataLeftLegPoseId, rot);
  }
  void setRightLegPose(Rotations3f rot) {
    rightLegPose_ = rot;
    entityData_.set(kDataRightLegPoseId, rot);
  }
  const Rotations3f &headPose() const noexcept { return headPose_; }
  const Rotations3f &bodyPose() const noexcept { return bodyPose_; }
  const Rotations3f &leftArmPose() const noexcept { return leftArmPose_; }
  const Rotations3f &rightArmPose() const noexcept { return rightArmPose_; }
  const Rotations3f &leftLegPose() const noexcept { return leftLegPose_; }
  const Rotations3f &rightLegPose() const noexcept { return rightLegPose_; }

  EntityDimensions getDimensions(Pose pose) const override;
  bool hurtServer(World &world, const DamageSource &source,
                  float amount) override;
  void travel(World &world, Vec3 input) override;
  void pushEntities(World &world) override;
  void syncDataFields() override;

  /// 记录上次被玩家击打的 `gameTime`，用于原版“5 刻内连击两次才会破坏盔甲架”的判定。
  std::int64_t lastHit = 0;
  /// 装备槽位禁用掩码（对应 NBT `DisabledSlots`），控制玩家可交互/替换的槽位。
  int disabledSlots = 0;
  /// 逻辑隐身标志（对应 NBT `Invisible`），用于区分标记模式与显式隐身并在 `syncDataFields` 中同步到实体共享标志位。
  bool standInvisible = false;

private:
  void setClientFlagBit(std::int8_t bit, bool value);
  void causeDamage(World &world, const DamageSource &source, float dmg);
  void brokenByPlayer(World &world);
  void brokenByAnything(World &world);

  Rotations3f headPose_{0.0f, 0.0f, 0.0f};
  Rotations3f bodyPose_{0.0f, 0.0f, 0.0f};
  Rotations3f leftArmPose_{-10.0f, 0.0f, -10.0f};
  Rotations3f rightArmPose_{-15.0f, 0.0f, 10.0f};
  Rotations3f leftLegPose_{-1.0f, 0.0f, -1.0f};
  Rotations3f rightLegPose_{1.0f, 0.0f, 1.0f};
};

} // namespace redstone
