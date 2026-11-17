#pragma once
/// @file mob_effects.hpp
/// @brief 定义原版 26.1 状态效果（`MobEffectId`、`MobEffectInstance`）及其与属性系统的联动映射。
///
/// 状态效果（药水效果）通过 `mobEffectAttributeTable()` 向 `AttributeMap` 注入或移除属性修饰符，
/// 并在 `MobEffectInstance` 中维护嵌套的 `hiddenEffect`，以准确还原原版“高倍率覆盖低倍率、
/// 高倍率到期后恢复低倍率”的叠层语义。

#include "redstone/entity/attributes.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace redstone {

/// 原版 26.1 全部状态效果 ID（含试炼密室与鹦鹉螺之息等 26.1 效果）。
enum class MobEffectId : std::uint8_t {
  Speed = 0,
  Slowness,
  Haste,
  MiningFatigue,
  Strength,
  InstantHealth,
  InstantDamage,
  JumpBoost,
  Nausea,
  Regeneration,
  Resistance,
  FireResistance,
  WaterBreathing,
  Invisibility,
  Blindness,
  NightVision,
  Hunger,
  Weakness,
  Poison,
  Wither,
  HealthBoost,
  Absorption,
  Saturation,
  Glowing,
  Levitation,
  Luck,
  Unluck,
  SlowFalling,
  ConduitPower,
  DolphinsGrace,
  BadOmen,
  HeroOfTheVillage,
  Darkness,
  TrialOmen,
  RaidOmen,
  WindCharged,
  Weaving,
  Oozing,
  Infested,
  BreathOfTheNautilus,
  Count
};

/// 状态效果的善恶分类（有益/有害/中性），用于牛奶/图腾/信标与 HUD 颜色判定。
enum class MobEffectCategory : std::uint8_t {
  Beneficial = 0,
  Harmful = 1,
  Neutral = 2,
};

/// 静态状态效果定义，记录资源名、分类及是否为瞬发效果（`instantaneous`）。
struct MobEffectDef {
  MobEffectId id;
  std::string_view name;
  MobEffectCategory category;
  bool instantaneous;
};

/// 查询指定 `MobEffectId` 的静态元数据定义表。
inline const MobEffectDef &mobEffectDef(MobEffectId id) {
  static constexpr std::array<MobEffectDef,
                              static_cast<std::size_t>(MobEffectId::Count)>
      kDefs{{
          {MobEffectId::Speed, "minecraft:speed", MobEffectCategory::Beneficial,
           false},
          {MobEffectId::Slowness, "minecraft:slowness",
           MobEffectCategory::Harmful, false},
          {MobEffectId::Haste, "minecraft:haste", MobEffectCategory::Beneficial,
           false},
          {MobEffectId::MiningFatigue, "minecraft:mining_fatigue",
           MobEffectCategory::Harmful, false},
          {MobEffectId::Strength, "minecraft:strength",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::InstantHealth, "minecraft:instant_health",
           MobEffectCategory::Beneficial, true},
          {MobEffectId::InstantDamage, "minecraft:instant_damage",
           MobEffectCategory::Harmful, true},
          {MobEffectId::JumpBoost, "minecraft:jump_boost",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::Nausea, "minecraft:nausea", MobEffectCategory::Harmful,
           false},
          {MobEffectId::Regeneration, "minecraft:regeneration",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::Resistance, "minecraft:resistance",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::FireResistance, "minecraft:fire_resistance",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::WaterBreathing, "minecraft:water_breathing",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::Invisibility, "minecraft:invisibility",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::Blindness, "minecraft:blindness",
           MobEffectCategory::Harmful, false},
          {MobEffectId::NightVision, "minecraft:night_vision",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::Hunger, "minecraft:hunger", MobEffectCategory::Harmful,
           false},
          {MobEffectId::Weakness, "minecraft:weakness",
           MobEffectCategory::Harmful, false},
          {MobEffectId::Poison, "minecraft:poison", MobEffectCategory::Harmful,
           false},
          {MobEffectId::Wither, "minecraft:wither", MobEffectCategory::Harmful,
           false},
          {MobEffectId::HealthBoost, "minecraft:health_boost",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::Absorption, "minecraft:absorption",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::Saturation, "minecraft:saturation",
           MobEffectCategory::Beneficial, true},
          {MobEffectId::Glowing, "minecraft:glowing",
           MobEffectCategory::Neutral, false},
          {MobEffectId::Levitation, "minecraft:levitation",
           MobEffectCategory::Harmful, false},
          {MobEffectId::Luck, "minecraft:luck", MobEffectCategory::Beneficial,
           false},
          {MobEffectId::Unluck, "minecraft:unluck", MobEffectCategory::Harmful,
           false},
          {MobEffectId::SlowFalling, "minecraft:slow_falling",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::ConduitPower, "minecraft:conduit_power",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::DolphinsGrace, "minecraft:dolphins_grace",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::BadOmen, "minecraft:bad_omen",
           MobEffectCategory::Neutral, false},
          {MobEffectId::HeroOfTheVillage, "minecraft:hero_of_the_village",
           MobEffectCategory::Beneficial, false},
          {MobEffectId::Darkness, "minecraft:darkness",
           MobEffectCategory::Harmful, false},
          {MobEffectId::TrialOmen, "minecraft:trial_omen",
           MobEffectCategory::Neutral, false},
          {MobEffectId::RaidOmen, "minecraft:raid_omen",
           MobEffectCategory::Neutral, false},
          {MobEffectId::WindCharged, "minecraft:wind_charged",
           MobEffectCategory::Harmful, false},
          {MobEffectId::Weaving, "minecraft:weaving",
           MobEffectCategory::Harmful, false},
          {MobEffectId::Oozing, "minecraft:oozing", MobEffectCategory::Harmful,
           false},
          {MobEffectId::Infested, "minecraft:infested",
           MobEffectCategory::Harmful, false},
          {MobEffectId::BreathOfTheNautilus, "minecraft:breath_of_the_nautilus",
           MobEffectCategory::Beneficial, false},
      }};
  return kDefs[static_cast<std::size_t>(id)];
}

/// 将带或不带 `minecraft:` 前缀的状态效果名称解析为 `MobEffectId`。
inline std::optional<MobEffectId> mobEffectIdFromName(std::string_view name) {
  for (std::size_t i = 0; i < static_cast<std::size_t>(MobEffectId::Count);
       ++i) {
    const auto id = static_cast<MobEffectId>(i);
    const auto defName = mobEffectDef(id).name;
    if (name == defName ||
        (name.rfind("minecraft:", 0) != 0 &&
         defName.substr(sizeof("minecraft:") - 1) == name)) {
      return id;
    }
  }
  return std::nullopt;
}

/// 描述某个状态效果对实体属性的固定修饰条目，按 `(amplifier + 1) * amountPerLevel` 计算数值。
struct MobEffectAttributeEntry {
  MobEffectId effect;
  AttributeId attribute;
  std::string_view modifierId;
  double amountPerLevel;
  AttributeOp operation;
};

/// 原版中会直接修改 `AttributeMap` 的状态效果映射表；统一表驱动可避免在各处散落分支判断。
inline const std::array<MobEffectAttributeEntry, 11> &
mobEffectAttributeTable() {
  static constexpr std::array<MobEffectAttributeEntry, 11> kTable{{
      {MobEffectId::Speed, AttributeId::MovementSpeed,
       "minecraft:effect.speed", 0.2, AttributeOp::AddMultipliedTotal},
      {MobEffectId::Slowness, AttributeId::MovementSpeed,
       "minecraft:effect.slowness", -0.15, AttributeOp::AddMultipliedTotal},
      {MobEffectId::Haste, AttributeId::AttackSpeed,
       "minecraft:effect.haste", 0.1, AttributeOp::AddMultipliedTotal},
      {MobEffectId::MiningFatigue, AttributeId::AttackSpeed,
       "minecraft:effect.mining_fatigue", -0.1, AttributeOp::AddMultipliedTotal},
      {MobEffectId::Strength, AttributeId::AttackDamage,
       "minecraft:effect.strength", 3.0, AttributeOp::AddValue},
      {MobEffectId::JumpBoost, AttributeId::SafeFallDistance,
       "minecraft:effect.jump_boost", 1.0, AttributeOp::AddValue},
      {MobEffectId::Invisibility, AttributeId::WaypointTransmitRange,
       "minecraft:effect.waypoint_transmit_range_hide", -1.0,
       AttributeOp::AddMultipliedTotal},
      {MobEffectId::Weakness, AttributeId::AttackDamage,
       "minecraft:effect.weakness", -4.0, AttributeOp::AddValue},
      {MobEffectId::HealthBoost, AttributeId::MaxHealth,
       "minecraft:effect.health_boost", 4.0, AttributeOp::AddValue},
      {MobEffectId::Absorption, AttributeId::MaxAbsorption,
       "minecraft:effect.absorption", 4.0, AttributeOp::AddValue},
      {MobEffectId::Luck, AttributeId::Luck, "minecraft:effect.luck", 1.0,
       AttributeOp::AddValue},
  }};
  return kTable;
}

/// 在效果施加或倍率变更时，向实体 `AttributeMap` 写入对应的属性修饰符（`Unluck` 单独映射为负向 `Luck`）。
inline void addMobEffectAttributeModifiers(MobEffectId effect,
                                           AttributeMap &attributes,
                                           int amplifier) {
  for (const auto &entry : mobEffectAttributeTable()) {
    if (entry.effect != effect)
      continue;
    if (auto *inst = attributes.getInstance(entry.attribute)) {
      inst->removeModifier(entry.modifierId);
      inst->addPermanentModifier(AttributeModifier{
          std::string(entry.modifierId), entry.amountPerLevel * (amplifier + 1),
          entry.operation, true});
    }
  }
  if (effect == MobEffectId::Unluck) {
    if (auto *inst = attributes.getInstance(AttributeId::Luck)) {
      inst->removeModifier("minecraft:effect.unluck");
      inst->addPermanentModifier(AttributeModifier{
          "minecraft:effect.unluck", -1.0 * (amplifier + 1),
          AttributeOp::AddValue, true});
    }
  }
}

/// 在效果过期或被移除时，从实体 `AttributeMap` 清除该效果注入的属性修饰符。
inline void removeMobEffectAttributeModifiers(MobEffectId effect,
                                              AttributeMap &attributes) {
  for (const auto &entry : mobEffectAttributeTable()) {
    if (entry.effect != effect)
      continue;
    if (auto *inst = attributes.getInstance(entry.attribute))
      inst->removeModifier(entry.modifierId);
  }
  if (effect == MobEffectId::Unluck) {
    if (auto *inst = attributes.getInstance(AttributeId::Luck))
      inst->removeModifier("minecraft:effect.unluck");
  }
}

/// 运行时激活的药水状态效果实例，语义对齐原版 26.1 `MobEffectInstance`。
/// 通过 `hiddenEffect` 单链表保留被更高等级覆盖但剩余时间更长的旧效果，以便高等级到期后回退恢复。
struct MobEffectInstance {
  static constexpr int kInfiniteDuration = -1;

  MobEffectId effect = MobEffectId::Speed;
  int duration = 0;
  int amplifier = 0;
  bool ambient = false;
  bool visible = true;
  bool showIcon = true;
  /// 被当前高倍率效果暂时掩盖的较低倍率同种效果，其剩余时间仍随每刻同步递减。
  std::unique_ptr<MobEffectInstance> hiddenEffect;

  MobEffectInstance() = default;
  MobEffectInstance(MobEffectId eff, int dur, int amp = 0, bool amb = false,
                    bool vis = true, bool icon = true)
      : effect(eff), duration(dur), amplifier(amp), ambient(amb), visible(vis),
        showIcon(icon) {}

  MobEffectInstance(const MobEffectInstance &other)
      : effect(other.effect), duration(other.duration),
        amplifier(other.amplifier), ambient(other.ambient),
        visible(other.visible), showIcon(other.showIcon),
        hiddenEffect(other.hiddenEffect
                         ? std::make_unique<MobEffectInstance>(*other.hiddenEffect)
                         : nullptr) {}

  MobEffectInstance &operator=(const MobEffectInstance &other) {
    if (this == &other)
      return *this;
    effect = other.effect;
    duration = other.duration;
    amplifier = other.amplifier;
    ambient = other.ambient;
    visible = other.visible;
    showIcon = other.showIcon;
    hiddenEffect = other.hiddenEffect
                       ? std::make_unique<MobEffectInstance>(*other.hiddenEffect)
                       : nullptr;
    return *this;
  }

  MobEffectInstance(MobEffectInstance &&) noexcept = default;
  MobEffectInstance &operator=(MobEffectInstance &&) noexcept = default;

  bool isInfiniteDuration() const noexcept {
    return duration == kInfiniteDuration;
  }

  bool isShorterDurationThan(const MobEffectInstance &other) const noexcept {
    return !isInfiniteDuration() &&
           (duration < other.duration || other.isInfiniteDuration());
  }

  bool hasRemainingDuration() const noexcept {
    return isInfiniteDuration() || duration > 0;
  }

  /// 递减主效果与 `hiddenEffect` 的剩余刻数（无限时长效果 `-1` 除外），返回递减后的剩余时长。
  int tickDownDuration() {
    if (hiddenEffect)
      hiddenEffect->tickDownDuration();
    if (!isInfiniteDuration())
      --duration;
    return duration;
  }

  /// 合并同类型的最新状态效果实例（高倍率覆盖或延长时长）；若属性或外观状态发生变化则返回 `true`。
  bool update(const MobEffectInstance &other) {
    bool changed = false;
    if (other.amplifier > amplifier) {
      if (isShorterDurationThan(other)) {
        hiddenEffect.reset();
      } else {
        auto oldCopy = std::make_unique<MobEffectInstance>(*this);
        hiddenEffect = std::move(oldCopy);
      }
      amplifier = other.amplifier;
      duration = other.duration;
      changed = true;
    } else if (other.amplifier == amplifier && isShorterDurationThan(other)) {
      duration = other.duration;
      changed = true;
    } else if (other.amplifier < amplifier && !other.isInfiniteDuration()) {
      if (!hiddenEffect) {
        hiddenEffect = std::make_unique<MobEffectInstance>(other);
      } else {
        hiddenEffect->update(other);
      }
    }
    if (!other.ambient && ambient) {
      ambient = other.ambient;
      changed = true;
    }
    if (other.visible != visible) {
      visible = other.visible;
      changed = true;
    }
    if (other.showIcon != showIcon) {
      showIcon = other.showIcon;
      changed = true;
    }
    return changed;
  }
};

} // namespace redstone
