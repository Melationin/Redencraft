#pragma once
/// @file entity.hpp
/// @brief 定义实体核心基类 `Entity`、方块内效果收集器、流体交互及基础非生物实体（掉落物、点燃的 TNT、下落方块）。
///
/// 该头文件是整个实体子系统的根基，汇总了空间分桶索引（`EntitySectionBucket` 联动）、
/// 物理碰撞、骑乘树、传送门状态机与同步元数据访问等公共契约，并在末尾统一引入各派生实体头文件。
#include "redstone/engine.hpp"
#include "redstone/entity/entity_data.hpp"
#include "redstone/nbt.hpp"
#include "redstone/rail.hpp"
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace redstone {
// 以空手（无精准采集/时运）评估方块的战利品掉落，供 `destroyBlock`、`updateOrDestroy`、活塞推碎与爆炸使用。
// 玻璃、珊瑚扇、活塞头、基岩等脆弱或不可破坏方块在没有精准采集（Silk Touch）时按原版语义返回空列表。
std::vector<ItemStack> blockDrops(const BlockState &state);
// 获取方块的物品形态 ID，用于创造模式物品栏以及矿车/活塞破坏时的掉落物生成。
std::string blockItem(const BlockState &state);
bool isReplaceable(const BlockState &state);
bool isAirState(const BlockState &state);

/// 原版伤害类型枚举，决定护甲减伤、无敌免疫、实体受击反馈及死亡消息语义。
enum class DamageKind {
  Generic,
  Fire,
  Lava,
  Explosion,
  FallingBlock,
  Anvil,
  FallingStalactite,
  Projectile,
  Magic,
  Fall,
  Attack,
  Cramming,
  Drown,
  Freeze,
  Starve,
  Cactus,
  SweetBerryBush,
  EnderPearl,
  InWall,
  OutOfBorder,
  Wither,
  GenericKill
};

/// 实体移动发起方类型，用于区分自身位移与活塞/潜影盒等外部推动，决定粘滞减速与推挤限幅逻辑。
enum class MoverType : std::uint8_t {
  Self = 0,
  Player = 1,
  Piston = 2,
  ShulkerBox = 3,
  Shulker = 4,
};

/// 实体穿过方块内部时收集的内置状态效果类型（按原版每步合并去重后统一执行）。
enum class InsideBlockEffectType : std::uint8_t {
  Freeze = 0,
  ClearFreeze = 1,
  FireIgnite = 2,
  LavaIgnite = 3,
  Extinguish = 4,
};

/// 方块内部效果应用器抽象接口，用于在沿着位移轨迹遍历方块时延迟收集并按顺序调度内置效果与自定义回调。
class InsideBlockEffectApplier {
public:
  using Callback = std::function<void(Entity &, World &)>;
  virtual ~InsideBlockEffectApplier() = default;
  virtual void apply(InsideBlockEffectType type) = 0;
  virtual void runBefore(InsideBlockEffectType type, Callback effect) = 0;
  virtual void runAfter(InsideBlockEffectType type, Callback effect) = 0;
};

/// 按移动步进（step）合并方块内效果的实现类，对齐原版 `InsideBlockEffectApplier.StepBasedCollector`。
/// 之所以需要按步分批冲刷（`advanceStep` / `flushStep`），是为了在实体高速穿过多个相邻方块时，
/// 避免同一步内重复触发同一种内置效果，并保证“熄火/点火”等前后置回调严格按遍历步序执行。
class StepBasedInsideBlockEffectApplier final : public InsideBlockEffectApplier {
public:
  void advanceStep(int step);
  void applyAndClear(Entity &entity, World &world);
  void apply(InsideBlockEffectType type) override;
  void runBefore(InsideBlockEffectType type, Callback effect) override;
  void runAfter(InsideBlockEffectType type, Callback effect) override;

private:
  void flushStep();
  static constexpr std::size_t kTypeCount = 5;
  std::array<bool, kTypeCount> effectsInStep_{};
  std::array<std::vector<Callback>, kTypeCount> beforeEffectsInStep_{};
  std::array<std::vector<Callback>, kTypeCount> afterEffectsInStep_{};
  struct PendingEffect {
    bool isBuiltin = false;
    InsideBlockEffectType builtin = InsideBlockEffectType::Freeze;
    Callback callback;
  };
  std::vector<PendingEffect> finalEffects_;
  int lastStep_ = -1;
};

/// 单个流体种类（水或熔岩）在当前刻的沉浸状态与累积流速向量。
struct EntityFluidTracker {
  double height = 0.0;
  bool eyesInside = false;
  Vec3 accumulatedCurrent{};
  int currentCount = 0;

  void reset() {
    height = 0.0;
    eyesInside = false;
    accumulatedCurrent = {};
    currentCount = 0;
  }
  void accumulateCurrent(Vec3 flow) {
    accumulatedCurrent += flow;
    ++currentCount;
  }
  void applyCurrentTo(Entity &entity, double scale) const;
};

/// 聚合实体的水/熔岩浸入高度、眼部浸没标记与流体推力状态，供物理移动、溺水与熄火逻辑复用。
class EntityFluidInteraction {
public:
  void update(World &world, const Entity &entity, bool ignoreCurrent);
  void applyCurrentTo(bool lava, Entity &entity, double scale) const;
  double getFluidHeight(bool lava) const {
    return lava ? lava_.height : water_.height;
  }
  bool isInFluid(bool lava) const { return getFluidHeight(lava) > 0.0; }
  bool isEyeInFluid(bool lava) const {
    return lava ? lava_.eyesInside : water_.eyesInside;
  }

private:
  EntityFluidTracker water_;
  EntityFluidTracker lava_;
};

/// 所有游戏实体的抽象基类，对齐原版 26.1 `net.minecraft.world.entity.Entity`。
/// 承担位置/速度积分、方块与实体碰撞、骑乘树、传送门冷却与元数据同步等基础能力。
class Entity : public std::enable_shared_from_this<Entity> {
public:
  static constexpr int kFlagOnFire = 0;
  static constexpr int kFlagShiftKeyDown = 1;
  static constexpr int kFlagSprinting = 3;
  static constexpr int kFlagSwimming = 4;
  static constexpr int kFlagInvisible = 5;
  static constexpr int kFlagGlowing = 6;
  static constexpr int kFlagFallFlying = 7;

  static constexpr std::uint8_t kDataSharedFlagsId = 0;
  static constexpr std::uint8_t kDataAirSupplyId = 1;
  static constexpr std::uint8_t kDataCustomNameId = 2;
  static constexpr std::uint8_t kDataCustomNameVisibleId = 3;
  static constexpr std::uint8_t kDataSilentId = 4;
  static constexpr std::uint8_t kDataNoGravityId = 5;
  static constexpr std::uint8_t kDataPoseId = 6;
  static constexpr std::uint8_t kDataTicksFrozenId = 7;
  static constexpr int kTotalAirSupply = 300;
  static constexpr int kBaseTicksRequiredToFreeze = 140;
  static constexpr int kBoardingCooldown = 60;

  explicit Entity(std::string type);
  virtual ~Entity() = default;
  Entity(const Entity &) = delete;
  Entity &operator=(const Entity &) = delete;

  virtual void tick(World &world) = 0;
  virtual bool fireImmune() const;
  virtual bool hurt(World &world, DamageKind kind, float damage);
  virtual bool causeFallDamage(World &world, double fallDistance,
                               float damageMultiplier, DamageKind kind);
  virtual void checkFallDamage(World &world, double ya, bool onGroundNow);
  virtual bool ignoreExplosion(World &world) const;
  virtual bool isPushable() const { return false; }
  virtual bool isPickable() const { return false; }
  virtual bool canBeCollidedWith() const { return false; }
  virtual bool canCollideWith(const Entity &other) const {
    return other.canBeCollidedWith() && !isPassengerOfSameVehicle(other);
  }
  virtual bool isAlive() const { return !isRemoved(); }
  virtual bool isLiving() const { return false; }
  bool isPlayer() const noexcept { return isPlayer_; }
  virtual bool isMinecart() const { return false; }
  virtual bool isItemEntity() const { return false; }
  // 当实体可能与自身 `canBeCollidedWith()` 为假的对象发生硬碰撞时返回 `true`（例如矿车可碰撞 `isPushable()` 的生物）。
  // 该标志用于告知 `World::collectEntityCollisions` / `hasEntityCollision` 额外扫描空间桶中的 `Pushable` 位平面，
  // 从而避免为常规实体平白增加 `Pushable` 过滤分支的开销。
  virtual bool hasCustomEntityCollision() const { return false; }
  virtual bool isIgnoringBlockTriggers() const { return false; }
  virtual float maxUpStep() const { return 0.0f; }
  virtual EntityDimensions getDimensions(Pose pose) const;
  virtual void syncDataFields() {}

  const EntityType &type() const { return *type_; }
  const std::string &typeName() const { return type_->name; }
  int id = 0;
  // 持久化 UUID；仅用于存档与命令引用，绝不作为运行时网络协议或追踪会话的实体 ID。
  std::array<std::int32_t, 4> uuid{};
  // `EntitySectionStorage` 空间索引缓存：记录当前实体所属区块段（`SectionPos.asLong`）
  // 以及在 `EntitySectionBucket` 紧凑数组中的精确槽位下标，便于 O(1) 原位更新或移除。
  static constexpr std::uint32_t kInvalidSectionIndex =
      std::numeric_limits<std::uint32_t>::max();
  std::uint64_t entitySection = 0;
  std::uint32_t entitySectionIndex = kInvalidSectionIndex;
  // 速度脉冲单调版本号；使每个网络追踪会话（`TrackedEntity`）都能独立观测速度变化，而不破坏其他客户端的同步状态。
  std::uint64_t motionRevision = 0;
  bool blocksBuilding = false;
  bool hurtMarked = false;
  Vec3 position;
  Vec3 oldPosition;
  Vec3 deltaMovement;
  float yaw = 0, pitch = 0;
  float oldYaw = 0, oldPitch = 0;
  int tickCount = 0;
  bool onGround = false;
  bool horizontalCollision = false;
  bool verticalCollision = false;
  bool verticalCollisionBelow = false;
  bool minorHorizontalCollision = false;
  bool noPhysics = false;
  // 缓存的骑乘/玩家状态标志：用于在实体推挤热循环、空间查询与存档遍历时直接进行 O(1) 无锁判断，
  // 避免频繁对 `weak_ptr` 执行 `lock()`/`expired()` 或依赖 `dynamic_cast`/虚函数分发。
  bool hasVehicle_ = false;
  bool hasPassengers_ = false;
  bool isPlayer_ = false;
  int remainingFireTicks = 0;
  int portalCooldown = 0;
  int boardingCooldown = 0;
  double fallDistance = 0.0;
  double moveDist = 0.0;
  double flyDist = 0.0;
  bool isInPowderSnow = false;
  bool wasInPowderSnow = false;
  bool wasTouchingWater = false;
  bool wasEyeInWater = false;
  std::optional<Pos> mainSupportingBlockPos;

  /// 记录实体停留在下界/末地传送门方块内的蓄力进度与入口位置，用于跨维度传送判定。
  struct PortalProcessState {
    bool active = false;
    Pos entryPosition{};
    int portalTime = 0;
    bool insidePortalThisTick = false;
  };

  SynchedEntityData &entityData() noexcept { return entityData_; }
  const SynchedEntityData &entityData() const noexcept { return entityData_; }
  bool getSharedFlag(int flag) const;
  void setSharedFlag(int flag, bool value);
  bool isOnFire() const {
    return remainingFireTicks > 0 || getSharedFlag(kFlagOnFire);
  }
  bool isShiftKeyDown() const { return getSharedFlag(kFlagShiftKeyDown); }
  void setShiftKeyDown(bool value) { setSharedFlag(kFlagShiftKeyDown, value); }
  virtual bool isCrouching() const { return pose() == Pose::Crouching; }
  virtual bool isDiscrete() const { return isCrouching(); }
  bool isSprinting() const { return getSharedFlag(kFlagSprinting); }
  virtual void setSprinting(bool value) { setSharedFlag(kFlagSprinting, value); }
  bool isSwimming() const { return getSharedFlag(kFlagSwimming); }
  void setSwimming(bool value) { setSharedFlag(kFlagSwimming, value); }
  bool isInvisible() const { return getSharedFlag(kFlagInvisible); }
  void setInvisible(bool value) { setSharedFlag(kFlagInvisible, value); }
  bool isGlowing() const {
    return hasGlowingTag_ || getSharedFlag(kFlagGlowing);
  }
  void setGlowingTag(bool value) {
    hasGlowingTag_ = value;
    setSharedFlag(kFlagGlowing, value);
  }
  bool isFallFlying() const { return getSharedFlag(kFlagFallFlying); }
  void setSharedFlagOnFire(bool value) {
    setSharedFlag(kFlagOnFire, value || hasVisualFire_);
  }
  bool isSilent() const { return entityData_.get<bool>(kDataSilentId); }
  void setSilent(bool value) { entityData_.set(kDataSilentId, value); }
  bool isNoGravity() const { return entityData_.get<bool>(kDataNoGravityId); }
  void setNoGravity(bool value) { entityData_.set(kDataNoGravityId, value); }
  int airSupply() const { return entityData_.get<int>(kDataAirSupplyId); }
  virtual int maxAirSupply() const { return kTotalAirSupply; }
  void setAirSupply(int value) { entityData_.set(kDataAirSupplyId, value); }
  int ticksFrozen() const { return entityData_.get<int>(kDataTicksFrozenId); }
  void setTicksFrozen(int value) {
    entityData_.set(kDataTicksFrozenId, std::max(0, value));
  }
  virtual int ticksRequiredToFreeze() const { return kBaseTicksRequiredToFreeze; }
  virtual bool canFreeze() const { return !isRemoved(); }
  bool isFullyFrozen() const {
    return ticksFrozen() >= ticksRequiredToFreeze();
  }
  void clearFreeze() { setTicksFrozen(0); }
  Pose pose() const { return entityData_.get<Pose>(kDataPoseId); }
  void setPose(Pose p) { entityData_.set(kDataPoseId, p); }
  const std::optional<std::string> &customName() const {
    return entityData_.get<std::optional<std::string>>(kDataCustomNameId);
  }
  void setCustomName(std::optional<std::string> name) {
    entityData_.set(kDataCustomNameId, std::move(name));
  }
  bool isCustomNameVisible() const {
    return entityData_.get<bool>(kDataCustomNameVisibleId);
  }
  void setCustomNameVisible(bool value) {
    entityData_.set(kDataCustomNameVisibleId, value);
  }

  EntityDimensions dimensions() const noexcept { return dimensions_; }
  Aabb boundingBox() const;
  Vec3 eyePosition() const { return position + Vec3{0, eyeHeight(), 0}; }
  double eyeHeight() const { return eyeHeight_; }
  void refreshDimensions();
  void refreshDimensions(World &world);
  double distanceToSqr(const Vec3 &to) const;
  void setPos(double x, double y, double z);
  void setPos(const Vec3 &pos) { setPos(pos.x, pos.y, pos.z); }
  void reapplyPosition() { setPos(position); }
  void setDeltaMovement(const Vec3 &value) { deltaMovement = value; }
  void setDeltaMovement(double x, double y, double z) {
    deltaMovement = {x, y, z};
  }
  void addDeltaMovement(const Vec3 &value) { deltaMovement += value; }
  void setOldPosAndRot() {
    oldPosition = position;
    oldYaw = yaw;
    oldPitch = pitch;
  }
  void setOnGroundWithMovement(World &world, bool ground, Vec3 movement);
  void setOnGroundWithMovement(World &world, bool ground,
                               bool horizontalCollided, Vec3 movement);
  void checkSupportingBlock(World &world, bool ground,
                            std::optional<Vec3> movement);
  bool isSupportedBy(Pos pos) const {
    return mainSupportingBlockPos.has_value() && *mainSupportingBlockPos == pos;
  }
  Pos getOnPos(World &world, float yOffset) const;
  Pos getOnPos(World &world) const { return getOnPos(world, 1.0e-5f); }
  Pos getOnPosLegacy(World &world) const { return getOnPos(world, 0.2f); }
  virtual double getBlockPosBelowOffset() const { return 0.500001; }
  Pos getBlockPosBelowThatAffectsMyMovement(World &world) const {
    return getOnPos(world, static_cast<float>(getBlockPosBelowOffset()));
  }
  virtual float getBlockSpeedFactor(World &world) const;
  BlockState getInBlockState(World &world);
  virtual void onAboveBubbleColumn(World &world, bool dragDown, Pos pos);
  virtual void onInsideBubbleColumn(World &world, bool dragDown);
  void makeStuckInBlock(const BlockState &state, Vec3 multiplier);
  void resetFallDistance() { fallDistance = 0.0; }
  bool isRemoved() const { return removed_; }
  void discard();
  void kill(World &world);

  void baseTick(World &world);
  void checkBelowWorld(World &world);
  virtual int dimensionChangingDelay() const { return 300; }
  virtual bool canUsePortal(bool allowVehicles = false) const;
  bool isOnPortalCooldown() const noexcept { return portalCooldown > 0; }
  void setPortalCooldown() { portalCooldown = dimensionChangingDelay(); }
  void setPortalCooldown(int cooldown) { portalCooldown = cooldown; }
  void processPortalCooldown();
  void setAsInsidePortal(Pos pos);
  void handlePortal(World &world);
  void applyFireBlockEffect(World &world, float fireDamage);
  void clearPortalProcess() noexcept { portalProcess_ = {}; }
  const PortalProcessState &portalProcess() const noexcept {
    return portalProcess_;
  }
  void move(World &world, MoverType moverType, Vec3 movement);
  void move(World &world, Vec3 movement, bool piston = false) {
    move(world, piston ? MoverType::Piston : MoverType::Self, movement);
  }
  void push(double x, double y, double z) {
    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
      deltaMovement.x += x;
      deltaMovement.y += y;
      deltaMovement.z += z;
      ++motionRevision;
    }
  }
  void push(const Vec3 &value);
  virtual void push(Entity &entity);
  void igniteForSeconds(float seconds);
  void igniteForTicks(int ticks);
  virtual int getFireImmuneTicks() const { return 1; }
  void clearFire();
  void lavaHurt(World &world);
  void lavaIgnite(World &world);
  bool updateFluidHeightAndDoFluidPushing(World &world, bool lava);
  bool updateFluidInteraction(World &world);
  void updateSwimming();
  void applyEffectsFromBlocks(World &world);
  void applyEffectsFromBlocks(World &world, Vec3 from, Vec3 to);
  bool isInWater(World &world) const;
  bool isInLava(World &world) const;
  double fluidHeight(World &world, bool lava) const;
  bool isInWater() const { return fluidInteraction_.isInFluid(false); }
  bool isInLava() const { return fluidInteraction_.isInFluid(true); }
  bool isEyeInWater() const { return fluidInteraction_.isEyeInFluid(false); }
  bool isUnderWater() const { return wasEyeInWater && isInWater(); }
  double fluidHeight(bool lava) const {
    return fluidInteraction_.getFluidHeight(lava);
  }
  void moveTowardsClosestSpace(World &world, double x, double y, double z);
  void spawnAtLocation(World &world, const ItemStack &stack);

  // 骑乘与载具树接口（对应原版 `Entity.java:2322-2472`）。
  void rideTick(World &world);
  void positionRider(Entity &passenger);
  virtual Vec3 getVehicleAttachmentPoint(const Entity &vehicle) const;
  virtual Vec3 getPassengerRidingPosition(const Entity &passenger) const;
  virtual Vec3 getPassengerAttachmentPoint(const Entity &passenger,
                                           EntityDimensions dims,
                                           float scale) const;
  bool startRiding(Entity &vehicle, bool force = false,
                   bool sendEvent = true);
  void stopRiding();
  void removeVehicle();
  void ejectPassengers();
  void unRide();
  virtual bool canRide(const Entity &vehicle) const {
    (void)vehicle;
    return !isShiftKeyDown() && boardingCooldown <= 0;
  }
  virtual bool canAddPassenger(const Entity &) const {
    return passengers_.empty();
  }
  virtual bool couldAcceptPassenger() const { return true; }
  bool isPassenger() const { return hasVehicle_ && !vehicle_.expired(); }
  bool isVehicle() const noexcept { return hasPassengers_; }
  std::shared_ptr<Entity> getVehicle() const { return vehicle_.lock(); }
  std::vector<std::shared_ptr<Entity>> getPassengers() const;
  std::shared_ptr<Entity> getFirstPassenger() const;
  virtual std::shared_ptr<Entity> getControllingPassenger() const {
    return {};
  }
  Entity &getRootVehicle();
  const Entity &getRootVehicle() const;
  bool hasPassenger(const Entity &e) const;
  bool hasIndirectPassenger(const Entity &e) const;
  bool isPassengerOfSameVehicle(const Entity &other) const {
    if (!isPassenger() && !other.isPassenger())
      return false;
    return &getRootVehicle() == &other.getRootVehicle();
  }

  /// 实体单次位移片段记录，供每刻结束后沿实际轨迹统一触发方块内效果（如绊线、压力板、火与水）。
  struct Movement {
    Vec3 from;
    Vec3 to;
    std::optional<Vec3> axisDependentOriginalMovement;
  };

protected:
  virtual void onSyncedDataUpdated(std::uint8_t id);
  Vec3 collide(World &world, Vec3 movement);
  Vec3 limitPistonMovement(World &world, Vec3 movement);
  /// 当姿态或尺度变化导致碰撞箱变大时，尝试将实体挤出卡住的方块空间。
  bool fudgePositionAfterSizeChange(World &world,
                                    const EntityDimensions &previous);
  bool removed_ = false;
  bool firstTick_ = true;
  const EntityType *type_ = nullptr;
  EntityDimensions dimensions_;
  float eyeHeight_ = 1.53f;
  SynchedEntityData entityData_;
  EntityFluidInteraction fluidInteraction_{};
  Vec3 stuckSpeedMultiplier_{};
  bool onGroundNoBlocks_ = false;
  bool hasGlowingTag_ = false;
  bool hasVisualFire_ = false;
  std::array<double, 3> pistonDeltas_{};
  std::int64_t pistonDeltasGameTime_ = -1;
  std::weak_ptr<Entity> vehicle_;
  std::vector<std::weak_ptr<Entity>> passengers_;
  std::vector<Movement> movementThisTick_;
  std::vector<Movement> finalMovementsThisTick_;
  StepBasedInsideBlockEffectApplier insideEffectCollector_;
  std::optional<BlockState> inBlockState_;
  // 以下三个内联同步方法用于原位更新 `EntitySectionBucket` 中的 SOA（结构体数组）缓存：
  // `World` 的碰撞检测与推挤循环直接扫描桶内的 `boxes` 和位平面（`Active`/`Pushable`）以提升缓存局部性，
  // 因此位置变更、可推挤状态切换或实体销毁时必须即时同步回桶，避免等下一刻全量重分桶。
  void syncSectionBoundingBox() noexcept {
    if (sectionBucket_ != nullptr &&
        entitySectionIndex < sectionBucket_->boxes.size()) {
      sectionBucket_->boxes[entitySectionIndex] = boundingBox();
    }
  }
  void syncSectionPushable() noexcept {
    if (sectionBucket_ != nullptr &&
        entitySectionIndex < sectionBucket_->size()) {
      sectionBucket_->setFlag(entitySectionIndex, EntitySectionFlag::Pushable,
                              isPushable());
    }
  }
  void syncSectionRemoval() noexcept {
    if (sectionBucket_ != nullptr &&
        entitySectionIndex < sectionBucket_->size()) {
      sectionBucket_->setFlag(entitySectionIndex, EntitySectionFlag::Active,
                              false);
      sectionBucket_->setFlag(entitySectionIndex, EntitySectionFlag::Pushable,
                              false);
    }
  }

private:
  friend struct EntitySectionBucket;
  friend class World;
  EntitySectionBucket *sectionBucket_ = nullptr;
  void addPassenger(Entity &passenger);
  void removePassenger(Entity &passenger);
  void applyEffectsFromBlocksList(World &world,
                                  const std::vector<Movement> &movements);
  int checkInsideBlocksSegment(World &world, Vec3 from, Vec3 to,
                               InsideBlockEffectApplier &collector,
                               std::unordered_set<std::uint64_t> &visitedBlocks,
                               int maxIterations);
  bool removalObserved_ = false;
  // 仅在无回调碰撞收集与解算阶段复用的临时缓冲区，避免每刻移动检测重复分配堆内存。
  std::vector<ShapeBox> movementColliders_;
  std::vector<ShapeBox> entityColliders_;
  PortalProcessState portalProcess_{};
};

/// 掉落物实体（`minecraft:item`），实现物品物理、合并堆叠、拾取冷却与熔岩/虚空销毁语义。
// 标记"从存档/托管存储恢复"的构造路径：不消耗 World 随机源。
struct RestoreFromStorageTag {};

class ItemEntity : public Entity {
public:
  static constexpr std::uint8_t kDataItemId = 8;

  ItemEntity(World &world, Vec3 pos, ItemStack stack);
  ItemEntity(World &world, Vec3 pos, ItemStack stack, Vec3 motion);
  // 恢复路径：yaw/pitch 随后由记录覆盖、bobOffs 仅客户端视觉，因此不掷随机数，
  // 与 PrimedTnt 的恢复构造保持同一契约——区块加载不得扰动 World 随机序列。
  ItemEntity(World &world, Vec3 pos, ItemStack stack, Vec3 motion,
             RestoreFromStorageTag restore);
  void tick(World &world) override;
  bool fireImmune() const override;
  bool hurt(World &world, DamageKind kind, float damage) override;
  bool isItemEntity() const override { return true; }
  bool isPickable() const override { return true; }
  double getBlockPosBelowOffset() const override { return 0.999999; }
  void syncDataFields() override;

  ItemStack item;
  int age = 0;
  int pickupDelay = 0;
  int health = 5;
  float bobOffs = 0;

private:
  void setFluidMovement(double multiplier);
  bool isMergable() const;
  void mergeWithNeighbours(World &world);
  void tryToMerge(World &world, ItemEntity &other);
};

/// 已点燃的 TNT 实体（`minecraft:tnt`），维护引信倒计时与爆炸触发。
class PrimedTnt : public Entity {
public:
  static constexpr std::uint8_t kDataFuseId = 8;
  static constexpr std::uint8_t kDataBlockStateId = 9;

  explicit PrimedTnt(World &world, Vec3 pos);
  // 存档/托管恢复路径专用构造：直接传入保存的速度向量，因而不会消耗 `World::random` 的点燃随机扰动序列。
  PrimedTnt(World &world, Vec3 pos, Vec3 motion);
  void tick(World &world) override;
  bool fireImmune() const override { return true; }
  bool isPickable() const override { return !isRemoved(); }
  void syncDataFields() override;

  static constexpr int defaultFuse = 80;
  int fuse = defaultFuse;
  float explosionPower = 4.0f;
  BlockState blockState = defaultState("tnt");
  bool usedPortal = false;
};

/// 下落方块实体（`minecraft:falling_block`），用于沙子、砾石、混凝土粉末、铁砧等方块的重力下落与落地放置/掉落。
class FallingBlockEntity : public Entity {
public:
  static constexpr std::uint8_t kDataStartPosId = 8;

  FallingBlockEntity(World &world, Vec3 pos, BlockState state);
  static FallingBlockEntity *fall(World &world, Pos pos,
                                  const BlockState &state);
  void tick(World &world) override;
  bool isPickable() const override { return !isRemoved(); }
  bool causeFallDamage(World &world, double fallDistance,
                       float damageMultiplier, DamageKind kind) override;
  void setHurtsEntities(float damagePerDistance, int damageMax) {
    hurtEntities = true;
    fallDamagePerDistance = damagePerDistance;
    fallDamageMax = damageMax;
  }
  void syncDataFields() override;

  BlockState blockState;
  int time = 0;
  bool dropItem = true;
  bool cancelDrop = false;
  bool hurtEntities = false;
  int fallDamageMax = 40;
  float fallDamagePerDistance = 0;
  std::optional<nbt::Tag> blockData;
  Pos startPos;
};

} // namespace redstone

#include "redstone/entity/decorative_entities.hpp"
#include "redstone/entity/projectiles.hpp"
#include "redstone/entity/vehicles.hpp"
#include "redstone/entity/living_entity.hpp"
#include "redstone/entity/player.hpp"
