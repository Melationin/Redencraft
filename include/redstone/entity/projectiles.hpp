#pragma once
/// @file projectiles.hpp
/// @brief 定义弹射物实体体系（`Projectile`、箭矢家族 `AbstractArrow`/`Arrow`/`SpectralArrow`/`ThrownTrident`
/// 与投掷物 `ThrowableItemProjectile`、风弹 `WindCharge`）。
///
/// 统一实现射线碰撞检测（方块与实体）、离开发射者判定、标靶方块红石脉冲时长差异
/// 以及末影珍珠区块加载票据续期等原版 26.1 弹射物语义。
#include "redstone/entity.hpp"
#include <cstdint>
#include <string>
#include <unordered_set>

namespace redstone {

/// 弹射物基类，维护发射者引用（运行时 ID 与持久化 UUID）、离体检测以及命中方块/实体分发。
class Projectile : public Entity {
public:
  explicit Projectile(std::string typeName);

  void setOwner(const Entity *owner);
  void setOwnerUuid(const std::array<std::int32_t, 4> &id) { ownerUuid_ = id; }
  Entity *getOwner(World &world) const;
  int ownerId() const noexcept { return ownerId_; }
  const std::array<std::int32_t, 4> &ownerUuid() const noexcept {
    return ownerUuid_;
  }
  bool hasLeftOwner() const noexcept { return leftOwner_; }
  bool hasBeenShot() const noexcept { return hasBeenShot_; }

  void shoot(World &world, double dx, double dy, double dz, float velocity,
             float inaccuracy);
  void shootFromRotation(World &world, float pitch, float yaw, float roll,
                         float velocity, float inaccuracy);
  void tick(World &world) override;

protected:
  void updateRotation();
  bool canHitEntity(const Entity &target) const;
  virtual void onHitBlock(World &world, const BlockHitResult &hit);
  virtual void onHitEntity(World &world, const EntityHitResult &hit);
  /// 命中标靶方块时输出的红石脉冲时长：普通投掷物为 8 刻，箭矢类覆写为 20 刻（对齐原版 `TargetBlock`）。
  virtual int targetBlockPulseTicks() const { return 8; }

  /// 同时保存运行时 `ownerId_` 与持久化 `ownerUuid_`，使发射者查找既能在本会话 O(1) 命中，也能跨存档恢复。
  int ownerId_ = 0;
  std::array<std::int32_t, 4> ownerUuid_{};
  /// 标记弹射物是否已完全飞离发射者碰撞箱，避免发射瞬间立刻击中自身。
  bool leftOwner_ = false;
  bool leftOwnerChecked_ = false;
  bool hasBeenShot_ = false;
};

/// 箭矢与三叉戟的抽象基类，支持插墙/插地滞留检测、1200 刻自然消失、暴击粒子标志及穿透列表。
class AbstractArrow : public Projectile {
public:
  enum class Pickup : std::uint8_t {
    Disallowed = 0,
    Allowed = 1,
    CreativeOnly = 2,
  };

  static constexpr std::uint8_t kDataFlagsId = 8;
  static constexpr std::uint8_t kDataPierceLevelId = 9;
  static constexpr std::uint8_t kDataInGroundId = 10;
  static constexpr std::uint8_t kFlagCrit = 1;
  static constexpr std::uint8_t kFlagNoPhysics = 2;

  explicit AbstractArrow(std::string typeName = "minecraft:arrow");

  void tick(World &world) override;
  void syncDataFields() override;

  bool isInGround() const noexcept { return inGround_; }
  void setInGround(bool value) {
    inGround_ = value;
    entityData_.set(kDataInGroundId, value);
  }
  bool isCritArrow() const noexcept { return crit_; }
  void setCritArrow(bool value);
  bool isNoPhysics() const noexcept { return noPhysics; }
  void setNoPhysics(bool value);
  std::uint8_t pierceLevel() const noexcept { return pierceLevel_; }
  void setPierceLevel(std::uint8_t value) {
    pierceLevel_ = value;
    entityData_.set(kDataPierceLevelId, static_cast<std::int8_t>(value));
  }
  double baseDamage() const noexcept { return baseDamage_; }
  void setBaseDamage(double damage) { baseDamage_ = damage; }
  int life() const noexcept { return life_; }
  void setLife(int value) noexcept { life_ = value; }
  std::optional<Pos> inBlockPos() const noexcept { return inBlockPos_; }

  Pickup pickup = Pickup::Disallowed;
  int shakeTime = 0;

protected:
  void onHitBlock(World &world, const BlockHitResult &hit) override;
  void onHitEntity(World &world, const EntityHitResult &hit) override;
  int targetBlockPulseTicks() const override { return 20; }
  /// 水中惯性系数：普通箭矢为 0.6，三叉戟覆写为 0.99 以保持水下高速飞行。
  virtual double waterInertia() const { return 0.6; }

private:
  /// 检查所嵌入的方块是否已被破坏或状态变化，若是则触发 `startFalling()` 重新受重力下落。
  bool shouldFall(const World &world) const;
  void startFalling();

  bool inGround_ = false;
  bool crit_ = false;
  std::uint8_t pierceLevel_ = 0;
  int inGroundTime_ = 0;
  int life_ = 0;
  double baseDamage_ = 2.0;
  std::optional<Pos> inBlockPos_;
  std::optional<BlockState> inBlockState_;
  /// 已穿透过的实体 ID 集合，防止带有穿透附魔的箭矢在连续游戏刻中重复伤害同一目标。
  std::unordered_set<int> piercingIgnoreIds_;
};

/// 普通箭与药水箭（`minecraft:arrow`），通过 `effectColor` 区分药水粒子颜色（-1 表示无药水）。
class Arrow final : public AbstractArrow {
public:
  static constexpr std::uint8_t kDataEffectColorId = 11;

  Arrow();
  void syncDataFields() override;
  int effectColor = -1;
};

/// 光灵箭（`minecraft:spectral_arrow`），命中目标后施加发光效果（默认持续 `duration` = 200 刻）。
class SpectralArrow final : public AbstractArrow {
public:
  SpectralArrow();
  int duration = 200;
};

/// 投掷出的三叉戟（`minecraft:trident`），保留忠诚等级与附魔光效标志，并覆写水中阻力以在水中畅行。
class ThrownTrident final : public AbstractArrow {
public:
  static constexpr std::uint8_t kDataLoyaltyId = 11;
  static constexpr std::uint8_t kDataFoilId = 12;

  ThrownTrident();
  void syncDataFields() override;

  std::uint8_t loyalty = 0;
  bool foil = false;

protected:
  double waterInertia() const override { return 0.99; }
  void onHitBlock(World &world, const BlockHitResult &hit) override;
};

/// 携带展示物品栈的可投掷弹射物基类（雪球、鸡蛋、末影珍珠、附魔之瓶共用）。
class ThrowableItemProjectile : public Projectile {
public:
  ThrowableItemProjectile(std::string typeName, ItemStack stack);

  static constexpr std::uint8_t kDataItemStackId = 8;

  void tick(World &world) override;
  void syncDataFields() override;

  ItemStack item;
};

/// 雪球（`minecraft:snowball`），命中实体造成 0 点投掷物击退/受击响应并销毁。
class Snowball final : public ThrowableItemProjectile {
public:
  Snowball();

protected:
  void onHitEntity(World &world, const EntityHitResult &hit) override;
};

/// 投掷出的鸡蛋（`minecraft:egg`），命中时触发 0 点投掷物受击响应并销毁。
class ThrownEgg final : public ThrowableItemProjectile {
public:
  ThrownEgg();

protected:
  void onHitEntity(World &world, const EntityHitResult &hit) override;
};

/// 投掷出的末影珍珠（`minecraft:ender_pearl`），命中时将发射者传送至落点，
/// 并通过 `ticketTimer_` / 上次区块坐标在跨越区块或每隔 40 刻时通知 `World::enderPearlTicketObserver` 续期加载票。
class ThrownEnderpearl final : public ThrowableItemProjectile {
public:
  ThrownEnderpearl();
  void tick(World &world) override;

protected:
  void onHitBlock(World &world, const BlockHitResult &hit) override;
  void onHitEntity(World &world, const EntityHitResult &hit) override;

private:
  void teleportOwnerOnHit(World &world, Vec3 destination);
  /// 距离下次续期末影珍珠区块加载票的剩余刻数，跨区块时会立即归零重发。
  std::int64_t ticketTimer_ = 0;
  int lastTicketChunkX_ = 0;
  int lastTicketChunkZ_ = 0;
  bool hasTicketChunk_ = false;
};

/// 附魔之瓶（`minecraft:experience_bottle`），碰撞方块或实体后碎裂并随机生成经验球。
class ThrownExperienceBottle final : public ThrowableItemProjectile {
public:
  ThrownExperienceBottle();

protected:
  void onHitBlock(World &world, const BlockHitResult &hit) override;
  void onHitEntity(World &world, const EntityHitResult &hit) override;

private:
  void shatter(World &world);
};

/// 风弹（`minecraft:wind_charge`），飞行无重力衰减并在命中时触发风爆以击退实体与激活红石机关方块。
class WindCharge final : public Projectile {
public:
  WindCharge();
  void tick(World &world) override;

protected:
  void onHitBlock(World &world, const BlockHitResult &hit) override;
  void onHitEntity(World &world, const EntityHitResult &hit) override;

private:
  void explodeAt(World &world, Vec3 pos);
  /// 生成后的不可偏转保护刻数（原版默认 5 刻），防止发射瞬间被玩家自身攻击判定反弹。
  int noDeflectTicks_ = 5;
};

} // namespace redstone
