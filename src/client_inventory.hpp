#pragma once
// 轻量调试客户端本地物品栏与方块拾取目录：提供方块反查物品、创造模式物品表与快捷栏操作。
#include "redstone/entity.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace redstone::client {
// 获取方块对应的物品名称；返回空字符串表示该方块没有可直接选取的物品。
inline std::string itemForBlock(const BlockState &state) {
  if (state.is(Blocks::PISTON_HEAD))
    return state.get("type") == "sticky" ? "minecraft:sticky_piston"
                                         : "minecraft:piston";
  return blockItem(state);
}

// 从已注册方块表构建调试客户端的创造模式物品目录，去重并过滤无独立物品的方块变种。
inline std::vector<std::string> creativeCatalog() {
  std::vector<std::string> result;
  for (const auto &entry : blockRegistry()) {
    const auto item = itemForBlock(BlockState(entry.name));
    // 墙面珊瑚扇与活塞头与另一个已注册方块共用同一物品，此处跳过以免目录重复。
    if (item.empty() || item != entry.name ||
        std::find(result.begin(), result.end(), item) != result.end())
      continue;
    result.push_back(item);
  }
  return result;
}

// 调试客户端本地 9 格快捷栏与背包面板开关状态，支持数字键选槽、滚轮切换与中键拾取方块。
struct Inventory {
  std::array<std::string, 9> slots = {
      "minecraft:piston",       "minecraft:sticky_piston",
      "minecraft:observer",     "minecraft:redstone_block",
      "minecraft:slime_block",  "minecraft:honey_block",
      "minecraft:smooth_stone", "minecraft:note_block",
      "minecraft:tnt"};
  int selectedSlot = 0;
  bool inventoryOpen = false;

  bool selectSlot(int index) {
    if (index < 0 || index >= static_cast<int>(slots.size()))
      return false;
    selectedSlot = index;
    return true;
  }
  // 正向鼠标滚轮增量向左移动选中槽位，与原版快捷栏操作方向一致。
  void scroll(int signedNotches) {
    const int shift = signedNotches % static_cast<int>(slots.size());
    selectedSlot = (selectedSlot - shift + static_cast<int>(slots.size())) %
                   static_cast<int>(slots.size());
  }
  void selectItem(std::string item) { slots[selectedSlot] = std::move(item); }
  const std::string &selectedItem() const { return slots[selectedSlot]; }
  void toggleInventory() { inventoryOpen = !inventoryOpen; }
  bool pickBlock(const BlockState &state) {
    auto item = itemForBlock(state);
    if (item.empty())
      return false;
    auto existing = std::find(slots.begin(), slots.end(), item);
    if (existing != slots.end())
      selectedSlot = static_cast<int>(existing - slots.begin());
    else
      selectItem(std::move(item));
    return true;
  }
};
} // namespace redstone::client
