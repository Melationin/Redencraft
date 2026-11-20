#pragma once
/// @file attributes.hpp
/// @brief 定义实体属性系统（`AttributeId`、`AttributeModifier`、`AttributeInstance`、`AttributeMap`）。
///
/// 对齐原版 26.1 生物属性框架：支持基值 + 三阶段修饰符叠加（加值、乘基值、乘总值），
/// 并通过独立的脏标记区分“逻辑刷新脏”与“网络同步脏”。

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redstone/hash.hpp"

namespace redstone {

/// 原版 26.1 `LivingEntity` / `Mob` / `Player` 使用的属性标识符。
enum class AttributeId : std::uint8_t {
  Armor = 0,
  ArmorToughness,
  AttackDamage,
  AttackKnockback,
  AttackSpeed,
  BurningTime,
  ExplosionKnockbackResistance,
  FallDamageMultiplier,
  FlyingSpeed,
  FollowRange,
  Gravity,
  JumpStrength,
  KnockbackResistance,
  Luck,
  MaxAbsorption,
  MaxHealth,
  MovementEfficiency,
  MovementSpeed,
  OxygenBonus,
  SafeFallDistance,
  Scale,
  SneakingSpeed,
  StepHeight,
  WaterMovementEfficiency,
  WaypointTransmitRange,
  Count
};

/// 静态属性定义，包含默认值、上下界及是否需要通过网络包同步给客户端（`syncable`）。
struct AttributeDef {
  AttributeId id;
  std::string_view name;
  double defaultValue;
  double minValue;
  double maxValue;
  bool syncable;
};

/// 查询属性 ID 对应的静态元数据定义表，避免每次查找都构造字符串或查询哈希表。
inline const AttributeDef &attributeDef(AttributeId id) {
  static constexpr std::array<AttributeDef,
                              static_cast<std::size_t>(AttributeId::Count)>
      kDefs{{
          {AttributeId::Armor, "minecraft:armor", 0.0, 0.0, 30.0, true},
          {AttributeId::ArmorToughness, "minecraft:armor_toughness", 0.0, 0.0,
           20.0, true},
          {AttributeId::AttackDamage, "minecraft:attack_damage", 2.0, 0.0,
           2048.0, false},
          {AttributeId::AttackKnockback, "minecraft:attack_knockback", 0.0, 0.0,
           5.0, false},
          {AttributeId::AttackSpeed, "minecraft:attack_speed", 4.0, 0.0, 1024.0,
           true},
          {AttributeId::BurningTime, "minecraft:burning_time", 1.0, 0.0, 1024.0,
           false},
          {AttributeId::ExplosionKnockbackResistance,
           "minecraft:explosion_knockback_resistance", 0.0, 0.0, 1.0, false},
          {AttributeId::FallDamageMultiplier,
           "minecraft:fall_damage_multiplier", 1.0, 0.0, 100.0, false},
          {AttributeId::FlyingSpeed, "minecraft:flying_speed", 0.4, 0.0, 1024.0,
           true},
          {AttributeId::FollowRange, "minecraft:follow_range", 32.0, 0.0,
           2048.0, false},
          {AttributeId::Gravity, "minecraft:gravity", 0.08, -1.0, 1.0, false},
          {AttributeId::JumpStrength, "minecraft:jump_strength", 0.42, 0.0,
           32.0, true},
          {AttributeId::KnockbackResistance, "minecraft:knockback_resistance",
           0.0, 0.0, 1.0, true},
          {AttributeId::Luck, "minecraft:luck", 0.0, -1024.0, 1024.0, true},
          {AttributeId::MaxAbsorption, "minecraft:max_absorption", 0.0, 0.0,
           2048.0, true},
          {AttributeId::MaxHealth, "minecraft:max_health", 20.0, 1.0, 1024.0,
           true},
          {AttributeId::MovementEfficiency, "minecraft:movement_efficiency",
           0.0, 0.0, 1.0, true},
          {AttributeId::MovementSpeed, "minecraft:movement_speed", 0.7, 0.0,
           1024.0, true},
          {AttributeId::OxygenBonus, "minecraft:oxygen_bonus", 0.0, 0.0, 1024.0,
           false},
          {AttributeId::SafeFallDistance, "minecraft:safe_fall_distance", 3.0,
           -1024.0, 1024.0, false},
          {AttributeId::Scale, "minecraft:scale", 1.0, 0.0625, 16.0, true},
          {AttributeId::SneakingSpeed, "minecraft:sneaking_speed", 0.3, 0.0,
           1.0, true},
          {AttributeId::StepHeight, "minecraft:step_height", 0.6, 0.0, 10.0,
           true},
          {AttributeId::WaterMovementEfficiency,
           "minecraft:water_movement_efficiency", 0.0, 0.0, 1.0, true},
          {AttributeId::WaypointTransmitRange,
           "minecraft:waypoint_transmit_range", 0.0, 0.0, 60000000.0, true},
      }};
  return kDefs[static_cast<std::size_t>(id)];
}

/// 将带或不带 `minecraft:` 命名空间前缀的属性名称解析为 `AttributeId`，兼容命令与 NBT 读入。
inline std::optional<AttributeId> attributeIdFromName(std::string_view name) {
  for (std::size_t i = 0; i < static_cast<std::size_t>(AttributeId::Count);
       ++i) {
    const auto id = static_cast<AttributeId>(i);
    const auto defName = attributeDef(id).name;
    if (name == defName ||
        (name.rfind("minecraft:", 0) != 0 &&
         defName.substr(sizeof("minecraft:") - 1) == name)) {
      return id;
    }
  }
  return std::nullopt;
}

/// 原版属性修饰符运算阶段，严格按 `AddValue` -> `AddMultipliedBase` -> `AddMultipliedTotal` 顺序求值。
enum class AttributeOp : std::uint8_t {
  AddValue = 0,
  AddMultipliedBase = 1,
  AddMultipliedTotal = 2,
};

/// 作用于单个属性实例的修饰符；`persistent` 为真时随实体 NBT 存档保存（如装备或永久效果）。
struct AttributeModifier {
  std::string id;
  double amount = 0.0;
  AttributeOp operation = AttributeOp::AddValue;
  bool persistent = false;
  bool operator==(const AttributeModifier &) const = default;
};

/// 单个实体属性的运行时槽位，语义对齐原版 26.1 `AttributeInstance`。
/// 内部使用 `dirty_` 惰性缓存求值结果，同时分别维护 `syncDirty_`（网络同步）与
/// `updateDirty_`（实体逻辑回调，如最大生命值下降时裁剪当前血量、尺度变化时刷新碰撞箱）。
class AttributeInstance {
public:
  explicit AttributeInstance(AttributeId id)
      : id_(id), baseValue_(attributeDef(id).defaultValue) {}

  AttributeId id() const noexcept { return id_; }
  double baseValue() const noexcept { return baseValue_; }
  void setBaseValue(double value) {
    baseValue_ = value;
    markDirty();
  }

  double getValue() const {
    if (dirty_) {
      cachedValue_ = calculateValue();
      dirty_ = false;
    }
    return cachedValue_;
  }

  bool hasModifier(std::string_view modifierId) const {
    return modifiers_.contains(modifierId);
  }

  std::optional<AttributeModifier>
  getModifier(std::string_view modifierId) const {
    auto it = modifiers_.find(modifierId);
    if (it == modifiers_.end())
      return std::nullopt;
    return it->second;
  }

  void addTransientModifier(AttributeModifier mod) {
    mod.persistent = false;
    addUnique(std::move(mod));
  }

  void addOrUpdateTransientModifier(AttributeModifier mod) {
    mod.persistent = false;
    auto it = modifiers_.find(mod.id);
    if (it != modifiers_.end()) {
      it->second = std::move(mod);
      markDirty();
      return;
    }
    addUnique(std::move(mod));
  }

  void addPermanentModifier(AttributeModifier mod) {
    mod.persistent = true;
    addUnique(std::move(mod));
  }

  void addOrReplacePermanentModifier(AttributeModifier mod) {
    mod.persistent = true;
    modifiers_[mod.id] = std::move(mod);
    markDirty();
  }

  bool removeModifier(std::string_view modifierId) {
    const auto count = modifiers_.erase(modifierId);
    if (count > 0) {
      markDirty();
      return true;
    }
    return false;
  }

  std::vector<AttributeModifier> getModifiers() const {
    std::vector<AttributeModifier> out;
    out.reserve(modifiers_.size());
    for (const auto &[_, mod] : modifiers_)
      out.push_back(mod);
    return out;
  }

  bool isSyncDirty() const noexcept { return syncDirty_; }
  bool isUpdateDirty() const noexcept { return updateDirty_; }
  void clearSyncDirty() noexcept { syncDirty_ = false; }
  void clearUpdateDirty() noexcept { updateDirty_ = false; }

private:
  void addUnique(AttributeModifier mod) {
    if (!modifiers_.contains(mod.id)) {
      auto key = mod.id;
      modifiers_.emplace(std::move(key), std::move(mod));
      markDirty();
    }
  }

  void markDirty() noexcept {
    dirty_ = true;
    syncDirty_ = true;
    updateDirty_ = true;
  }

  /// 按原版三段公式计算终值并钳位到 `[minValue, maxValue]`。
  double calculateValue() const {
    double base = baseValue_;
    double multBase = 0.0;
    double multTotal = 1.0;
    for (const auto &[_, mod] : modifiers_) {
      switch (mod.operation) {
      case AttributeOp::AddValue:
        base += mod.amount;
        break;
      case AttributeOp::AddMultipliedBase:
        multBase += mod.amount;
        break;
      case AttributeOp::AddMultipliedTotal:
        multTotal *= (1.0 + mod.amount);
        break;
      }
    }
    double result = (base + base * multBase) * multTotal;
    const auto &def = attributeDef(id_);
    return std::clamp(result, def.minValue, def.maxValue);
  }

  AttributeId id_;
  double baseValue_;
  phmap::flat_hash_map<std::string, AttributeModifier, AnkerlStringHash,
                       std::equal_to<>>
      modifiers_;
  mutable double cachedValue_ = 0.0;
  mutable bool dirty_ = true;
  /// 标记需要向客户端发送属性更新包。
  bool syncDirty_ = false;
  /// 标记需要触发实体本地属性变更钩子（如 `LivingEntity::onAttributeUpdated`）。
  bool updateDirty_ = false;
};

/// 管理单个实体所有已注册属性实例的容器，等价于原版 `AttributeMap`。
class AttributeMap {
public:
  AttributeMap() = default;

  /// 为 `LivingEntity` 预先注册原版默认属性集合，确保未显式修改的属性也具备完整实例。
  void registerLivingDefaults() {
    static constexpr AttributeId kDefaults[] = {
        AttributeId::MaxHealth,
        AttributeId::KnockbackResistance,
        AttributeId::MovementSpeed,
        AttributeId::Armor,
        AttributeId::ArmorToughness,
        AttributeId::MaxAbsorption,
        AttributeId::StepHeight,
        AttributeId::Scale,
        AttributeId::Gravity,
        AttributeId::SafeFallDistance,
        AttributeId::FallDamageMultiplier,
        AttributeId::JumpStrength,
        AttributeId::OxygenBonus,
        AttributeId::BurningTime,
        AttributeId::ExplosionKnockbackResistance,
        AttributeId::WaterMovementEfficiency,
        AttributeId::MovementEfficiency,
        AttributeId::AttackKnockback,
        AttributeId::WaypointTransmitRange,
    };
    for (AttributeId id : kDefaults)
      ensure(id);
  }

  AttributeInstance &ensure(AttributeId id) {
    auto [it, _] = instances_.try_emplace(id, id);
    return it->second;
  }

  bool hasAttribute(AttributeId id) const { return instances_.contains(id); }

  AttributeInstance *getInstance(AttributeId id) {
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : &it->second;
  }

  const AttributeInstance *getInstance(AttributeId id) const {
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : &it->second;
  }

  double getValue(AttributeId id) const {
    if (const auto *inst = getInstance(id))
      return inst->getValue();
    return attributeDef(id).defaultValue;
  }

  double getBaseValue(AttributeId id) const {
    if (const auto *inst = getInstance(id))
      return inst->baseValue();
    return attributeDef(id).defaultValue;
  }

  void setBaseValue(AttributeId id, double value) {
    ensure(id).setBaseValue(value);
  }

  /// 收集并清除本地逻辑更新脏标志（每刻由 `LivingEntity::refreshDirtyAttributes` 消费）。
  std::vector<AttributeId> takeUpdateDirtyIds() {
    std::vector<AttributeId> out;
    for (auto &[id, inst] : instances_) {
      if (inst.isUpdateDirty()) {
        out.push_back(id);
        inst.clearUpdateDirty();
      }
    }
    return out;
  }

  /// 收集并清除需要网络同步的属性 ID（仅包含 `syncable == true` 的属性）。
  std::vector<AttributeId> takeSyncDirtyIds() {
    std::vector<AttributeId> out;
    for (auto &[id, inst] : instances_) {
      if (attributeDef(id).syncable && inst.isSyncDirty()) {
        out.push_back(id);
        inst.clearSyncDirty();
      }
    }
    return out;
  }

private:
  phmap::flat_hash_map<AttributeId, AttributeInstance> instances_;
};

} // namespace redstone
