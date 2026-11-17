#pragma once
/// @file player.hpp
/// @brief 定义服务端玩家实体 `ServerPlayer`、饥饿饱食度状态 `PlayerFoodData`、物品冷却器 `ItemCooldowns` 与全员睡眠判定。
///
/// 玩家实体虽不依赖服务端 AI，但需要处理客户端权威移动包（`applyClientMovement`）、
/// 物品/经验球拾取、旁观者空间桶位同步与床铺睡眠进度等原版 26.1 服务端玩家逻辑。

#include "redstone/entity/living_entity.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace redstone {
class ServerPlayer;

/// 对应原版 26.1 玩家饥饿与饱和度系统（`net.minecraft.world.food.FoodData`），驱动自然回血与饥饿掉血。
struct PlayerFoodData {
  int foodLevel = 20;
  float saturationLevel = 5.0f;
  float exhaustionLevel = 0.0f;
  int tickTimer = 0;

  void add(int nutrition, float saturation);
  void addExhaustion(float amount);
  bool needsFood() const noexcept { return foodLevel < 20; }
  bool hasEnoughFood() const noexcept { return foodLevel > 6; }
  void tick(World &world, ServerPlayer &player, bool naturalRegen = true,
            bool peaceful = false, bool hardDifficulty = false);
};

/// 对应原版 26.1 按物品 ID 追踪的冷却计时器（`net.minecraft.world.item.ItemCooldowns`），
/// 使用 `std::less<>` 透明比较器以支持 `std::string_view` 零分配查询。
class ItemCooldowns {
public:
  void tick();
  bool isOnCooldown(std::string_view itemId) const;
  int remainingTicks(std::string_view itemId) const;
  void addCooldown(std::string itemId, int ticks);
  void removeCooldown(std::string_view itemId);

private:
  int tickCount_ = 0;
  std::map<std::string, int, std::less<>> endTicks_;
};

/// 服务端权威玩家实体（`minecraft:player`），在构造时将 `isPlayer_` 置为 `true` 以便引擎快速识别玩家。
class ServerPlayer : public LivingEntity {
public:
  static constexpr std::uint8_t kDataPlayerMainHandId = 15;
  static constexpr std::uint8_t kDataPlayerModeCustomisationId = 16;
  static constexpr std::uint8_t kDataPlayerAbsorptionId = 17;
  static constexpr std::uint8_t kDataScoreId = 18;
  static constexpr std::uint8_t kDataShoulderParrotLeftId = 19;
  static constexpr std::uint8_t kDataShoulderParrotRightId = 20;

  static constexpr int kSleepDurationTicks = 100;

  explicit ServerPlayer(World &world, Vec3 pos = {}, std::string name = "Player");

  bool isSpectator() const override { return spectator; }
  /// 切换旁观者模式并立即同步空间桶的 `Pushable` 位平面，确保其他生物推挤遍历立即忽略旁观者。
  void setSpectator(bool value) {
    spectator = value;
    syncSectionPushable();
  }
  bool isSteppingCarefully() const { return isShiftKeyDown(); }
  bool isPickable() const override { return !isRemoved() && !spectator; }
  bool isPushable() const override { return isAlive() && !spectator; }

  EntityDimensions getDimensions(Pose pose) const override;
  void syncDataFields() override;
  void tick(World &world) override;
  bool hurtServer(World &world, const DamageSource &source,
                  float amount) override;
  void stopSleeping() override;

  /// 根据客户端权威移动数据包更新位置与朝向，并沿移动轨迹执行踩踏方块、方块内效果与实体拾取检查。
  void applyClientMovement(World &world, Vec3 pos, float newYaw, float newPitch,
                           bool onGroundNow);

  /// 触碰玩家拾取包围盒内的附近掉落物与经验球。
  void touchNearbyEntities(World &world);

  /// 尝试在 `bedPos` 处的床方块上开始入睡，成功返回 `true`。
  bool sleepInBedAt(World &world, Pos bedPos);

  std::string username;
  bool creative = true;
  bool spectator = false;
  bool mayBuild = true;
  int sleepCounter = 0;
  int totalExperience = 0;
  int experienceLevel = 0;
  int takeXpDelay = 0;
  PlayerFoodData foodData;
  ItemCooldowns cooldowns;
  /// 物品拾取回调钩子：之所以通过回调解耦，是因为实体核心库不直接持有协议玩家背包，由服务端会话层注入并返还实际收进背包的物品数量。
  std::function<int(const ItemStack &)> onItemPickup;
};

/// 当世界中至少存在一名存活的非旁观者玩家，且所有非旁观者玩家的 `sleepCounter` 均达到 `ServerPlayer::kSleepDurationTicks` 时返回 `true`。
bool allPlayersSleptThroughNight(const World &world);

} // namespace redstone
