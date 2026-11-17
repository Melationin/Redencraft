#pragma once
// 物品交互分发接口：定义右键方块交互（useItemOnBlock）、手持右键使用（useItemInAir）、
// 进食消耗（consumeFoodItem）及对齐原版 UseOnContext 的参数与结果类型。
#include "redstone/entity.hpp"
#include "redstone/item.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace redstone {

// 物品交互结果枚举（对应原版 InteractionResult）：用于决定服务端是否拦截后续放置/副手逻辑，
// 以及是否向客户端触发挥手动作。
enum class InteractionOutcome : std::uint8_t {
  Pass = 0,
  Consume = 1,
  Success = 2,
  Fail = 3,
};

// 对齐原版 UseOnContext：封装玩家对着方块表面右键使用物品时的完整上下文。
struct ItemUseOnParams {
  Pos clickedPos{};
  Direction clickedFace = Direction::Up;
  Vec3 clickLocation{};
  bool inside = false;
  Direction horizontalFacing = Direction::North;
  float playerYaw = 0.0f;
  float playerPitch = 0.0f;
  bool creative = false;
  bool secondaryActive = false;
  bool mainHand = true;
  bool offhandBlocksAttacks = false;
  ServerPlayer *player = nullptr;
};

// 封装手持物品在空中/非指定方块上右键使用的玩家状态与射线参数。
struct ItemUseParams {
  Vec3 eyePos{};
  float yaw = 0.0f;
  float pitch = 0.0f;
  bool creative = false;
  bool sneaking = false;
  double reach = 5.0;
  ServerPlayer *player = nullptr;
};

// 物品交互执行结果：携带交互结果码、需通知客户端刷新的方块位置及新放置方块坐标。
struct ItemUseResult {
  InteractionOutcome outcome = InteractionOutcome::Pass;
  std::vector<Pos> updatedBlocks;
  std::optional<Pos> placedPos;

  // 对应原版 InteractionResult.consumesAction()：Success 与 Consume 均视为已消费本次输入。
  bool consumesAction() const noexcept {
    return outcome == InteractionOutcome::Success ||
           outcome == InteractionOutcome::Consume;
  }
};

/// 原版方块状态转换查找表，供 `AxeItem`（去皮/除锈）和 `HoneycombItem`（涂蜡）使用。
std::optional<BlockState> blockStateForAxeStrip(const BlockState &oldState);
std::optional<BlockState> blockStateForCopperScrape(const BlockState &oldState);
std::optional<BlockState> blockStateForCopperWaxOn(const BlockState &oldState);
std::optional<BlockState> blockStateForCopperWaxOff(const BlockState &oldState);

/// 将 `ItemStack` 上的 `DataComponents.BLOCK_STATE` 属性覆写到刚放置的 `BlockState` 上
///（对应原版 `BlockItem.updateBlockStateFromTag`）。
BlockState applyItemBlockStateComponent(const ItemStack &stack,
                                          BlockState placedState);

/// 若目标位置为受支持的铁轨方块，则在该铁轨上生成对应变种的矿车实体。
bool placeMinecartOnRail(World &world, Pos railPos, std::string_view itemId);

/// 评估对齐原版 26.1 的方块右键物品交互（涵盖 `AxeItem`、`ShovelItem`、`HoeItem`、
/// `HoneycombItem`、`FlintAndSteelItem`、`FireChargeItem`、`BoneMealItem`、
/// `SolidBucketItem`、`MinecartItem`、`ArmorStandItem`、`ItemFrameItem`、`EndCrystalItem`）。
/// 通用 `BlockItem` 放置（如 `SolidBucket` 以外的方块）在返回 `outcome == Pass` 时
/// 仍由调用方（如协议会话层）协同完成放置状态计算与方块设置。
ItemUseResult useItemOnBlock(World &world, ItemStack &stack,
                             const ItemUseOnParams &params);

/// 评估对齐原版 26.1 的手持右键物品使用（`use_item`）：涵盖 `BoatItem`、
/// `PlaceOnWaterBlockItem`、`BucketItem`、`BottleItem`、可投掷投射物
///（`snowball`、`egg`、`ender_pearl`、`experience_bottle`、`wind_charge`），
/// 以及 `Food`/`Consumable` 的进食与饮用逻辑。
ItemUseResult useItemInAir(World &world, ItemStack &stack,
                           const ItemUseParams &params);

/// 应用即时或蓄力完成后的食物消耗效果（更新 `FoodData` 并处理 `use_remainder` 返还容器）。
InteractionOutcome consumeFoodItem(World &world, ItemStack &stack,
                                   ServerPlayer *player,
                                   bool creative = false);

} // namespace redstone
