#pragma once
#include "redstone/engine.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace redstone::client {
// Item represented by a block; an empty result means there is no selectable
// item.
inline std::string itemForBlock(const BlockState &state) {
  const auto &name = state.name;
  if (name == "minecraft:air" || name == "minecraft:cave_air" ||
      name == "minecraft:void_air" || name == "minecraft:moving_piston")
    return {};
  if (name == "minecraft:piston_head")
    return state.get("type") == "sticky" ? "minecraft:sticky_piston"
                                         : "minecraft:piston";
  if (isDeadCoralWallFan(state)) {
    auto item = name;
    auto pos = item.rfind("_wall_fan");
    if (pos != std::string::npos)
      item.replace(pos, 9, "_fan");
    return item;
  }
  return name;
}

inline std::vector<std::string> creativeCatalog() {
  std::vector<std::string> result;
  for (const auto &entry : blockRegistry()) {
    const auto item = itemForBlock(BlockState(entry.name));
    // The wall fan and piston head share an item with another registered block.
    if (item.empty() || item != entry.name ||
        std::find(result.begin(), result.end(), item) != result.end())
      continue;
    result.push_back(item);
  }
  return result;
}

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
  // A positive mouse wheel delta moves left, matching the vanilla hotbar.
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
