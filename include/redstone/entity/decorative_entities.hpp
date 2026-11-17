#pragma once
/// @file decorative_entities.hpp
/// @brief 定义悬挂装饰实体（`HangingEntity`、`ItemFrame`）、末影水晶 `EndCrystal` 与经验球 `ExperienceOrb`。
///
/// 其中物品展示框（`ItemFrame`）通过 `getAnalogOutput()` 直接参与红石比较器信号输出，
/// 悬挂基类则负责依附方块存在性周期校验（`survives`）与贴墙薄碰撞箱计算。
#include "redstone/entity.hpp"

namespace redstone {

/// 悬挂类实体基类（如物品展示框、画等），依附于指定方块坐标与朝向并定期校验支撑是否仍有效。
class HangingEntity : public Entity {
public:
  static constexpr std::uint8_t kDataDirectionId = 8;

  HangingEntity(std::string typeName, Pos attachPos, Direction direction);
  void syncDataFields() override;
  /// 检查依附方块面是否仍满足支撑条件（且自身不与其他悬挂实体重叠）。
  virtual bool survives(World &world) const;
  Direction attachDirection() const noexcept { return attachDirection_; }
  Pos attachPos() const noexcept { return attachPos_; }
  void tick(World &world) override;

protected:
  virtual Aabb calculateBoundingBox(Pos pos, Direction direction) const;
  void recalculateBoundingBox();

  Pos attachPos_{};
  Direction attachDirection_ = Direction::South;
  /// 支撑性检查间隔计数器（原版每 100 刻校验一次 `survives`），避免每刻对所有悬挂装饰物做方块查询。
  int checkInterval_ = 0;
};

/// 物品展示框与荧光物品展示框（`minecraft:item_frame` / `minecraft:glow_item_frame`），
/// 支持 8 向旋转（`rotation_`）并向后方比较器提供 0~8 级模拟红石信号。
class ItemFrame final : public HangingEntity {
public:
  static constexpr std::uint8_t kDataItemId = 9;
  static constexpr std::uint8_t kDataRotationId = 10;

  ItemFrame(Pos attachPos, Direction direction, bool glow = false);
  void syncDataFields() override;
  bool isPickable() const override { return true; }
  bool hurt(World &world, DamageKind kind, float damage) override;
  void setFramedItem(World &world, ItemStack stack);
  void setRotation(World &world, int rot);
  void rotate(World &world);
  int rotation() const noexcept { return rotation_; }
  const ItemStack &framedItem() const noexcept { return framedItem_; }
  bool isGlow() const noexcept { return glow_; }
  /// 计算比较器读取的模拟红石信号强度：空框为 0，有物品时为 `1 + rotation_`（范围 1~8）。
  int getAnalogOutput() const noexcept;

  /// 为真时展示框不可被旋转、取物或破坏（对应原版 `Fixed` NBT 标签）。
  bool fixed = false;
  float dropChance = 1.0f;

protected:
  Aabb calculateBoundingBox(Pos pos, Direction direction) const override;

private:
  ItemStack framedItem_;
  int rotation_ = 0;
  bool glow_ = false;
};

/// 末影水晶实体（`minecraft:end_crystal`），受非无敌伤害或爆炸影响时会引爆自身。
class EndCrystal final : public Entity {
public:
  static constexpr std::uint8_t kDataBeamTargetId = 8;
  static constexpr std::uint8_t kDataShowBottomId = 9;

  EndCrystal(World &world, Vec3 pos);
  void tick(World &world) override;
  bool isPickable() const override { return true; }
  bool hurt(World &world, DamageKind kind, float damage) override;
  void syncDataFields() override;

  std::optional<Pos> beamTarget;
  bool showBottom = true;
  int time = 0;
};

/// 经验球实体（`minecraft:experience_orb`），支持附近玩家吸附追踪与同价值经验球合并（通过 `count` 累加）。
class ExperienceOrb final : public Entity {
public:
  static constexpr std::uint8_t kDataValueId = 8;

  ExperienceOrb(World &world, Vec3 pos, int xpValue);
  void tick(World &world) override;
  bool hurt(World &world, DamageKind kind, float damage) override;
  void syncDataFields() override;

  int age = 0;
  int health = 5;
  int xpValue = 0;
  int count = 1;

private:
  void scanForEntities(World &world);
  void mergeOrb(ExperienceOrb &other);
};

} // namespace redstone
