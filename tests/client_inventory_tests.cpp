#include "../src/client_inventory.hpp"
#include <cassert>
#include <climits>
using namespace redstone;
using namespace redstone::client;
int main() {
  Inventory inventory;
  inventory.scroll(1);
  assert(inventory.selectedSlot == 8);
  inventory.scroll(-2);
  assert(inventory.selectedSlot == 1);
  inventory.scroll(INT_MIN);
  assert(inventory.selectedSlot >= 0 && inventory.selectedSlot < 9);
  assert(!inventory.selectSlot(9));
  assert(!inventory.selectSlot(-1));
  assert(inventory.selectSlot(3));
  inventory.selectItem("minecraft:glass");
  assert(inventory.selectedItem() == "minecraft:glass");
  assert(inventory.pickBlock(BlockState("minecraft:piston")));
  assert(inventory.selectedSlot == 0);
  assert(inventory.pickBlock(BlockState("minecraft:obsidian")));
  assert(inventory.selectedItem() == "minecraft:obsidian");
  assert(!inventory.pickBlock(BlockState("minecraft:air")));
  assert(!inventory.pickBlock(BlockState("minecraft:moving_piston")));
  assert(
      itemForBlock(BlockState("minecraft:piston_head", {{"type", "sticky"}})) ==
      "minecraft:sticky_piston");
  assert(itemForBlock(BlockState("minecraft:piston_head",
                                 {{"type", "normal"}})) == "minecraft:piston");
  assert(itemForBlock(BlockState("minecraft:dead_brain_coral_wall_fan")) ==
         "minecraft:dead_brain_coral_fan");
  const auto catalog = creativeCatalog();
  assert(std::find(catalog.begin(), catalog.end(),
                   "minecraft:dead_brain_coral_fan") != catalog.end());
  assert(std::find(catalog.begin(), catalog.end(),
                   "minecraft:dead_brain_coral_wall_fan") == catalog.end());
  assert(std::find(catalog.begin(), catalog.end(), "minecraft:piston_head") ==
         catalog.end());
  assert(std::find(catalog.begin(), catalog.end(), "minecraft:air") ==
         catalog.end());
  inventory.toggleInventory();
  assert(inventory.inventoryOpen);
  inventory.toggleInventory();
  assert(!inventory.inventoryOpen);
}
