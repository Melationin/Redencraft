#pragma once
/// @file vehicles.hpp
/// @brief 定义载具实体体系（`VehicleEntity`、各类矿车 `Minecart*`、船 `Boat`/`ChestBoat`）与容器实体接口 `ContainerEntity`。
///
/// 该头文件复刻了原版 26.1 矿车沿铁轨子步推进、探测铁轨/漏斗矿车容器交互、
/// 激活铁轨点燃 TNT 矿车，以及船只水陆浮力与划桨控制等物理与红石联动语义。
#include "redstone/entity.hpp"
#include <optional>
#include <string>
#include <vector>

namespace redstone {

// 为箱子矿车（`MinecartChest`）、漏斗矿车（`MinecartHopper`）和运输船（`ChestBoat`）提供统一的容器混入接口，
// 使漏斗方块与探测铁轨比较器能够以统一方式读写所有容器载具；将其独立为无 `Entity` 基类的纯接口，
// 是为了同时被 `Minecart` 分支与 `AbstractBoat` 分支安全多继承而避免菱形继承问题。
class ContainerEntity {
public:
  virtual ~ContainerEntity() = default;
  virtual int containerSize() const = 0;
  virtual const ItemStack &getContainerItem(int slot) const = 0;
  virtual void setContainerItem(int slot, ItemStack stack) = 0;
  virtual ItemStack removeContainerItem(int slot, int count) = 0;
  virtual void setContainerChanged(World &world) = 0;
  virtual int comparatorSignal() const;
  virtual bool isContainerEmpty() const;
  virtual bool isContainerFull() const;
  virtual ItemStack addItemToContainer(World &world, ItemStack stack);
};

/// 矿车与船只的共同载具基类，统一处理受击摇晃计时、累计伤害衰减以及破坏时的物品掉落（`dropItemId`）。
class VehicleEntity : public Entity {
public:
  static constexpr std::uint8_t kDataHurtId = 8;
  static constexpr std::uint8_t kDataHurtDirId = 9;
  static constexpr std::uint8_t kDataDamageId = 10;

  explicit VehicleEntity(std::string typeName);
  void syncDataFields() override;
  bool hurt(World &world, DamageKind kind, float amount) override;

  int hurtTime = 0;
  int hurtDir = 1;
  float damage = 0.0f;

protected:
  void tickHurtDecay();
  virtual std::string dropItemId() const = 0;
  virtual void onDestroyDrop(World &world);
};

/// 矿车基类（`minecraft:minecart`），实现原版铁轨吸附、沿轨分段步进（含新/旧矿车行为）及对生物的拾取/碰撞。
class Minecart : public VehicleEntity {
public:
  static constexpr std::uint8_t kDataCustomDisplayBlockId = 11;
  static constexpr std::uint8_t kDataDisplayOffsetId = 12;

  explicit Minecart(World &world, Vec3 pos,
                    std::string typeName = "minecraft:minecart");
  void tick(World &world) override;
  bool ignoreExplosion(World &world) const override;
  bool isPushable() const override { return true; }
  bool isPickable() const override { return !isRemoved(); }
  bool canBeCollidedWith() const override { return true; }
  bool isMinecart() const override { return true; }
  // 矿车不仅碰撞硬碰撞体，还能碰撞 `isPushable()` 生物；返回 `true` 以告知 `World` 空间查询额外并入 `Pushable` 位平面。
  bool hasCustomEntityCollision() const override { return true; }
  bool canCollideWith(const Entity &other) const override {
    return (other.canBeCollidedWith() || other.isPushable()) &&
           !isPassengerOfSameVehicle(other);
  }
  using Entity::push;
  void push(Entity &entity) override;
  bool isRideable() const { return true; }
  float getBlockSpeedFactor(World &world) const override;
  Vec3 getPassengerAttachmentPoint(const Entity &passenger,
                                   EntityDimensions dims,
                                   float scale) const override;
  void syncDataFields() override;

  bool onRails = false;
  bool flipped = false;
  bool firstTick = true;
  double rotationOffset = 0;
  std::optional<BlockState> customDisplayBlock;
  int displayOffset = 6;

protected:
  virtual double maxSpeed(World &world) const;
  virtual Vec3 naturalSlowdown(World &world, Vec3 movement) const;
  virtual void activateMinecart(World &world, Pos pos, bool powered);
  virtual void makeStepAlongTrack(World &world, Pos pos, RailShape shape,
                                  double &movementLeft);
  virtual BlockState defaultDisplayBlock() const { return airState(); }
  std::string dropItemId() const override { return "minecraft:minecart"; }

private:
  void tickOldBehavior(World &world);
  void tickNewBehavior(World &world);
  void moveAlongTrack(World &world);
  void comeOffTrack(World &world);
  void pushAndPickupEntities(World &world);
  Pos currentBlockPosOrRailBelow(World &world) const;
  std::optional<Vec3> railPos(World &world, double x, double y, double z) const;
};

/// 带物品栏的矿车抽象基类，负责根据容器填充度计算阻力并实现破坏时清空掉落物。
class AbstractMinecartContainer : public Minecart, public ContainerEntity {
public:
  AbstractMinecartContainer(World &world, Vec3 pos, std::string typeName,
                            int size);
  int containerSize() const override { return static_cast<int>(items_.size()); }
  const ItemStack &getContainerItem(int slot) const override;
  void setContainerItem(int slot, ItemStack stack) override;
  ItemStack removeContainerItem(int slot, int count) override;
  void setContainerChanged(World &world) override;

protected:
  Vec3 naturalSlowdown(World &world, Vec3 movement) const override;
  void onDestroyDrop(World &world) override;
  std::vector<ItemStack> items_;
};

/// 箱子矿车（`minecraft:chest_minecart`），提供 27 格存储空间并可触发探测铁轨比较器输出。
class MinecartChest final : public AbstractMinecartContainer {
public:
  MinecartChest(World &world, Vec3 pos);

protected:
  BlockState defaultDisplayBlock() const override {
    return defaultState("chest");
  }
  std::string dropItemId() const override { return "minecraft:chest_minecart"; }
};

/// 漏斗矿车（`minecraft:hopper_minecart`），受激活铁轨开关控制（`enabled`），
/// 在沿铁轨推进子步与每刻结束时均会尝试从上方容器或附近掉落物吸取物品。
class MinecartHopper final : public AbstractMinecartContainer {
public:
  MinecartHopper(World &world, Vec3 pos);
  void tick(World &world) override;

  bool enabled = true;

protected:
  void activateMinecart(World &world, Pos pos, bool powered) override;
  void makeStepAlongTrack(World &world, Pos pos, RailShape shape,
                          double &movementLeft) override;
  BlockState defaultDisplayBlock() const override {
    return defaultState("hopper");
  }
  std::string dropItemId() const override {
    return "minecraft:hopper_minecart";
  }

private:
  void tryConsumeItems(World &world);
  bool suckInItems(World &world);
  /// 本刻内是否已成功吸入过物品：由于 `makeStepAlongTrack` 每次铁轨子步与 `tick` 末尾都会调用
  /// `tryConsumeItems`，该标志确保漏斗矿车在同一游戏刻内最多完成一次吸取，防止高速多子步重复吞物。
  bool consumedItemThisFrame_ = false;
};

/// TNT 矿车（`minecraft:tnt_minecart`），支持激活铁轨引爆（`fuse` 80 刻）以及高速撞击/摔落瞬爆。
class MinecartTNT final : public Minecart {
public:
  MinecartTNT(World &world, Vec3 pos);
  void tick(World &world) override;
  bool hurt(World &world, DamageKind kind, float amount) override;
  bool causeFallDamage(World &world, double fallDistance,
                       float damageMultiplier, DamageKind kind) override;
  void primeFuse();
  bool isPrimed() const noexcept { return fuse > -1; }

  int fuse = -1;
  float explosionPowerBase = 4.0f;
  float explosionSpeedFactor = 1.0f;

protected:
  void activateMinecart(World &world, Pos pos, bool powered) override;
  BlockState defaultDisplayBlock() const override {
    return defaultState("tnt");
  }
  std::string dropItemId() const override { return "minecraft:tnt_minecart"; }

private:
  void explode(World &world, double speedSqr);
};

/// 动力矿车（`minecraft:furnace_minecart`），消耗燃料（`fuel`）并沿 `pushForce` 方向持续推动列车。
class MinecartFurnace final : public Minecart {
public:
  static constexpr std::uint8_t kDataFuelId = 13;

  MinecartFurnace(World &world, Vec3 pos);
  void tick(World &world) override;
  void syncDataFields() override;
  bool addFuel(Vec3 interactingPos, const std::string &itemId);

  int fuel = 0;
  Vec3 pushForce{};

protected:
  double maxSpeed(World &world) const override;
  Vec3 naturalSlowdown(World &world, Vec3 movement) const override;
  BlockState defaultDisplayBlock() const override {
    return defaultState("furnace");
  }
  std::string dropItemId() const override {
    return "minecraft:furnace_minecart";
  }
};

/// 船只抽象基类，管理水上漂浮、水下进水驱离乘客（`bubbleTime` / `outOfControlTicks_`）、冰面滑行与双桨状态。
class AbstractBoat : public VehicleEntity {
public:
  enum class Status {
    InWater,
    UnderWater,
    UnderFlowingWater,
    OnLand,
    InAir
  };

  static constexpr std::uint8_t kDataPaddleLeftId = 11;
  static constexpr std::uint8_t kDataPaddleRightId = 12;
  static constexpr std::uint8_t kDataBubbleTimeId = 13;

  AbstractBoat(World &world, Vec3 pos, std::string typeName,
               std::string dropItem);

  void tick(World &world) override;
  void syncDataFields() override;
  bool isPickable() const override { return !isRemoved(); }
  bool isPushable() const override { return true; }
  bool canBeCollidedWith() const override { return true; }
  bool canAddPassenger(const Entity &) const override {
    return passengers_.size() < static_cast<std::size_t>(maxPassengers());
  }
  Vec3 getPassengerAttachmentPoint(const Entity &passenger,
                                   EntityDimensions dims,
                                   float scale) const override;
  void setPaddleState(bool left, bool right);
  Status status() const noexcept { return status_; }

  bool inputLeft = false;
  bool inputRight = false;
  bool inputUp = false;
  bool inputDown = false;
  bool paddleLeft = false;
  bool paddleRight = false;
  int bubbleTime = 0;

protected:
  virtual int maxPassengers() const { return 2; }
  std::string dropItemId() const override { return dropItemId_; }

private:
  Status computeStatus(World &world) const;
  double getWaterLevelAbove(World &world) const;
  float getGroundFriction(World &world) const;
  void floatBoat(World &world);
  void controlBoat();

  std::string dropItemId_;
  Status status_ = Status::InAir;
  Status oldStatus_ = Status::InAir;
  double waterLevel_ = 0.0;
  float landFriction_ = 0.0f;
  float deltaRotation_ = 0.0f;
  float outOfControlTicks_ = 0.0f;
};

/// 普通木船（`minecraft:oak_boat` 等木质变体），最多容纳 2 名乘客。
class Boat final : public AbstractBoat {
public:
  Boat(World &world, Vec3 pos, std::string typeName = "minecraft:oak_boat",
       std::string dropItem = "minecraft:oak_boat");
};

/// 运输船（`minecraft:oak_chest_boat` 等），兼具 27 格容器与载人能力，因此最大乘客数限制为 1。
class ChestBoat final : public AbstractBoat, public ContainerEntity {
public:
  ChestBoat(World &world, Vec3 pos,
            std::string typeName = "minecraft:oak_chest_boat",
            std::string dropItem = "minecraft:oak_chest_boat");
  int containerSize() const override { return 27; }
  const ItemStack &getContainerItem(int slot) const override;
  void setContainerItem(int slot, ItemStack stack) override;
  ItemStack removeContainerItem(int slot, int count) override;
  void setContainerChanged(World &world) override;

protected:
  int maxPassengers() const override { return 1; }
  void onDestroyDrop(World &world) override;

private:
  std::vector<ItemStack> items_;
};

} // namespace redstone
