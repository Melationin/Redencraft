#include "redstone/containers.hpp"
#include "redstone/item.hpp"
#include "redstone/item_tags.hpp"
#include "redstone/item_use.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>
#include <string>

using namespace redstone;

namespace {

void testFullRegistryAndMetadata() {
  const auto &items = allItems();
  assert(items.size() == 1506);
  assert(Items::AIR.id() == 0);
  assert(Items::AIR.name() == "minecraft:air");
  assert(Items::STONE.id() == 1);
  assert(itemById(0) == &Items::AIR);
  assert(itemByName("stone") == &Items::STONE);
  assert(itemByName("minecraft:redstone") == &Items::REDSTONE);
  assert(itemRegistry().size() == 1505); // excludes air in ItemType table

  // Max stack sizes aligned with 26.1
  assert(Items::ENDER_PEARL.maxStackSize() == 16);
  assert(Items::SNOWBALL.maxStackSize() == 16);
  assert(Items::EGG.maxStackSize() == 16);
  assert(Items::BUCKET.maxStackSize() == 16);
  assert(Items::DIAMOND_PICKAXE.maxStackSize() == 1);
  assert(Items::DIAMOND_PICKAXE.maxDamage() == 1561);
  assert(Items::ELYTRA.maxDamage() == 432);
  assert(Items::ELYTRA.glider());
  assert(Items::ELYTRA.rarity() == ItemRarity::Epic);
  assert(Items::NETHERITE_SWORD.fireResistant());
  assert(!Items::DIAMOND_SWORD.fireResistant());

  // 26.1 Item.BY_BLOCK coverage
  assert(itemForBlock(Blocks::REDSTONE_WIRE) == &Items::REDSTONE);
  assert(itemForBlock(Blocks::WALL_TORCH) == &Items::TORCH);
  assert(itemForBlock(Blocks::OAK_WALL_SIGN) == &Items::OAK_SIGN);
  assert(itemForBlock(Blocks::WATER_CAULDRON) == &Items::CAULDRON);
  assert(itemForBlock(Blocks::POWDER_SNOW) == &Items::POWDER_SNOW_BUCKET);
  assert(itemForBlock(Blocks::AIR) == &Items::AIR);

  assert(blockItem(Blocks::WALL_TORCH.defaultState()) == "minecraft:torch");
  assert(blockItem(Blocks::POWDER_SNOW.defaultState()).empty());
  assert(Items::TORCH.primaryBlock() == &Blocks::TORCH);
  assert(Items::TORCH.wallBlock() == &Blocks::WALL_TORCH);
  assert(Items::REDSTONE.primaryBlock() == &Blocks::REDSTONE_WIRE);
}

void testItemTagsCompostablesAndFuels() {
  ItemTagRegistry reg;
  assert(reg.entries().size() == 207);

  assert(Items::OAK_LOG.hasTag("minecraft:logs"));
  assert(Items::OAK_LOG.hasTag("minecraft:logs_that_burn"));
  assert(Items::CRIMSON_STEM.hasTag("minecraft:logs"));
  assert(Items::CRIMSON_STEM.hasTag("minecraft:non_flammable_wood"));
  assert(itemHasTag("diamond_axe", "minecraft:axes") == true);
  assert(!itemHasTag("diamond_axe", "minecraft:nonexistent_tag").has_value());

  // Fuel durations (FuelValues.vanillaBurnTimes, baseUnit = 200)
  assert(Items::LAVA_BUCKET.fuelBurnTime() == 20000);
  assert(Items::COAL_BLOCK.fuelBurnTime() == 16000);
  assert(Items::DRIED_KELP_BLOCK.fuelBurnTime() == 4001);
  assert(Items::BLAZE_ROD.fuelBurnTime() == 2400);
  assert(Items::COAL.fuelBurnTime() == 1600);
  assert(Items::OAK_BOAT.fuelBurnTime() == 1200);
  assert(Items::OAK_PLANKS.fuelBurnTime() == 300);
  assert(Items::OAK_SLAB.fuelBurnTime() == 150);
  assert(Items::STICK.fuelBurnTime() == 100);
  assert(Items::CRIMSON_PLANKS.fuelBurnTime() == 0); // NON_FLAMMABLE_WOOD removed
  assert(Items::CRIMSON_SIGN.fuelBurnTime() == 0);
  assert(isFuelItem(itemStack("coal", 1)));

  // Compostables (115 items matching ComposterBlock)
  int compostCount = 0;
  for (const Item *it : allItems()) {
    if (it->compostChance() > 0.0f)
      ++compostCount;
  }
  assert(compostCount == 115);
  assert(isCompostableItem("minecraft:cake"));
  assert(composterChance("minecraft:cake") == 1.0f);
  assert(composterChance("minecraft:wheat_seeds") > 0.29f &&
         composterChance("minecraft:wheat_seeds") < 0.31f);
}

void testItemStackComponentsAndDrops() {
  ItemStack pick = itemStack("diamond_pickaxe", 1);
  assert(pick.isDamageableItem());
  assert(!pick.hurtAndBreak(100, false));
  assert(pick.damageValue() == 100);
  assert(pick.isDamaged());

  // Unbreakable preserves durability; hurtAndBreak in creative does not mutate.
  ItemStack creativePick = pick;
  assert(!creativePick.hurtAndBreak(2000, true));
  assert(creativePick.damageValue() == 100);
  ItemStack unbr = withItemUnbreakable(pick, true);
  assert(unbr.isUnbreakable());
  assert(!unbr.hurtAndBreak(2000, false));
  assert(unbr.count == 1);

  // Breaking at max damage removes stack.
  ItemStack flint = itemStack("flint_and_steel", 1);
  assert(flint.hurtAndBreak(64, false));
  assert(flint.empty());

  // Merging checks components.
  ItemStack plainA = itemStack("stone", 1);
  ItemStack plainB = itemStack("stone", 1);
  ItemStack named = withItemCustomName(plainA, "Special Stone");
  assert(plainA.isSameItemSameComponents(plainB));
  assert(plainA.isSameItem(named));
  assert(!plainA.isSameItemSameComponents(named));

  // Tool speed & drops (Iron Ore needs stone tool, Obsidian needs diamond tool).
  ItemStack woodPick = itemStack("wooden_pickaxe", 1);
  ItemStack stonePick = itemStack("stone_pickaxe", 1);
  ItemStack diamPick = itemStack("diamond_pickaxe", 1);
  assert(diamPick.destroySpeed(Blocks::STONE.defaultState()) == 8.0f);
  assert(woodPick.isCorrectToolForDrops(Blocks::STONE.defaultState()));
  assert(!woodPick.isCorrectToolForDrops(Blocks::IRON_ORE.defaultState()));
  assert(stonePick.isCorrectToolForDrops(Blocks::IRON_ORE.defaultState()));
  assert(!stonePick.isCorrectToolForDrops(Blocks::OBSIDIAN.defaultState()));
  assert(diamPick.isCorrectToolForDrops(Blocks::OBSIDIAN.defaultState()));

  assert(blockDropsForTool(Blocks::OBSIDIAN.defaultState(), stonePick).empty());
  auto obDrop = blockDropsForTool(Blocks::OBSIDIAN.defaultState(), diamPick);
  assert(obDrop.size() == 1 && obDrop[0].id == "minecraft:obsidian");

  ItemStack silkGlass = withItemEnchantment(diamPick, "minecraft:silk_touch", 1);
  auto glassDrop = blockDropsForTool(Blocks::GLASS.defaultState(), silkGlass);
  assert(glassDrop.size() == 1 && glassDrop[0].id == "minecraft:glass");
  assert(blockDropsForTool(Blocks::GLASS.defaultState(), diamPick).empty());

  // NBT round-trip with components
  ItemStack enchanted = withItemEnchantment(
      withItemCustomName(withItemDamage(itemStack("diamond_pickaxe", 1), 42),
                         "Miner"),
      "minecraft:efficiency", 5);
  nbt::Tag nbtTag = encodeItemStackNbt(enchanted, true);
  auto decoded = decodeItemStackNbt(nbtTag);
  assert(decoded.has_value());
  assert(*decoded == enchanted);
  assert(decoded->damageValue() == 42);
  assert(decoded->enchantmentLevel("minecraft:efficiency") == 5);
}

void testItemUseOnAndInAir() {
  World world;
  world.loadChunk(0, 0);
  world.loadChunk(-1, 0);

  // Axe strip + copper scrape + honeycomb wax
  world.setBlock({2, 64, 2},
                 Blocks::OAK_LOG.defaultState().with("axis", "x"), 3);
  ItemStack axe = itemStack("iron_axe", 1);
  ItemUseOnParams p;
  p.clickedPos = {2, 64, 2};
  p.clickedFace = Direction::Up;
  p.creative = false;
  auto res = useItemOnBlock(world, axe, p);
  assert(res.outcome == InteractionOutcome::Success);
  assert(world.getBlock({2, 64, 2}).is(Blocks::STRIPPED_OAK_LOG));
  assert(world.getBlock({2, 64, 2}).getView("axis") == "x");
  assert(axe.damageValue() == 1);

  world.setBlock({3, 64, 2}, Blocks::COPPER_BLOCK.defaultState(), 3);
  ItemStack comb = itemStack("honeycomb", 2);
  p.clickedPos = {3, 64, 2};
  res = useItemOnBlock(world, comb, p);
  assert(res.outcome == InteractionOutcome::Success);
  assert(world.getBlock({3, 64, 2}).is(Blocks::WAXED_COPPER_BLOCK));
  assert(comb.count == 1);

  res = useItemOnBlock(world, axe, p);
  assert(res.outcome == InteractionOutcome::Success);
  assert(world.getBlock({3, 64, 2}).is(Blocks::COPPER_BLOCK));
  assert(axe.damageValue() == 2);

  // Shovel flatten + Hoe till
  world.setBlock({4, 64, 2}, Blocks::GRASS_BLOCK.defaultState(), 3);
  ItemStack shovel = itemStack("iron_shovel", 1);
  p.clickedPos = {4, 64, 2};
  res = useItemOnBlock(world, shovel, p);
  assert(res.outcome == InteractionOutcome::Success);
  assert(world.getBlock({4, 64, 2}).is(Blocks::DIRT_PATH));
  assert(shovel.damageValue() == 1);

  ItemStack hoe = itemStack("iron_hoe", 1);
  res = useItemOnBlock(world, hoe, p);
  assert(res.outcome == InteractionOutcome::Success);
  assert(world.getBlock({4, 64, 2}).is(Blocks::FARMLAND));
  assert(hoe.damageValue() == 1);

  // EndCrystal + ArmorStand + ChestMinecart
  world.setBlock({6, 64, 2}, Blocks::OBSIDIAN.defaultState(), 3);
  ItemStack crystal = itemStack("end_crystal", 1);
  p.clickedPos = {6, 64, 2};
  res = useItemOnBlock(world, crystal, p);
  assert(res.outcome == InteractionOutcome::Success);
  assert(crystal.empty());

  world.setBlock({8, 64, 2}, Blocks::RAIL.defaultState(), 3);
  ItemStack chestCart = itemStack("chest_minecart", 1);
  p.clickedPos = {8, 64, 2};
  res = useItemOnBlock(world, chestCart, p);
  assert(res.outcome == InteractionOutcome::Success);
  assert(chestCart.empty());

  // Boat placement in water & EnderPearl throw with cooldown & FoodData
  world.setBlock({5, 63, 5}, Blocks::WATER.defaultState(), 3);
  auto player = std::make_shared<ServerPlayer>(world, Vec3{5.5, 64.0, 5.5},
                                               "Tester");
  player->creative = false;
  player->foodData.foodLevel = 10;
  player->foodData.saturationLevel = 0.0f;
  world.addEntity(player);

  ItemStack apple = itemStack("golden_apple", 2);
  ItemUseParams up;
  up.eyePos = {5.5, 65.62, 5.5};
  up.yaw = 0.0f;
  up.pitch = 60.0f;
  up.creative = false;
  up.player = player.get();
  auto eatRes = useItemInAir(world, apple, up);
  assert(eatRes.outcome == InteractionOutcome::Consume);
  assert(apple.count == 1);
  assert(player->foodData.foodLevel == 14);
  assert(player->foodData.saturationLevel > 9.0f);

  ItemStack pearl = itemStack("ender_pearl", 2);
  auto pearlRes = useItemInAir(world, pearl, up);
  assert(pearlRes.outcome == InteractionOutcome::Success);
  assert(pearl.count == 1);
  assert(player->cooldowns.isOnCooldown("minecraft:ender_pearl"));
  // Second immediate throw blocked by cooldown
  auto pearlBlocked = useItemInAir(world, pearl, up);
  assert(pearlBlocked.outcome == InteractionOutcome::Fail);
  assert(pearl.count == 1);

  ItemStack boat = itemStack("oak_boat", 1);
  up.pitch = 90.0f;
  auto boatRes = useItemInAir(world, boat, up);
  assert(boatRes.outcome == InteractionOutcome::Success);
  assert(boat.empty());

  // BottleItem filling water potion component (on loaded water cell {5, 63, 5})
  ItemStack bottle = itemStack("glass_bottle", 1);
  auto bottleRes = useItemInAir(world, bottle, up);
  assert(bottleRes.outcome == InteractionOutcome::Success);
  assert(bottle.id == "minecraft:potion");
  assert(bottle.components && bottle.components->potionId == "minecraft:water");

  // Cooldown expiry tick (ender_pearl had 20 ticks = 1.0s cooldown)
  for (int t = 0; t < 20; ++t)
    player->cooldowns.tick();
  assert(!player->cooldowns.isOnCooldown("minecraft:ender_pearl"));

  // SolidBucketItem placing powder_snow and returning empty bucket
  world.setBlock({10, 64, 2}, Blocks::STONE.defaultState(), 3);
  ItemStack snowBucket = itemStack("powder_snow_bucket", 1);
  p.clickedPos = {10, 64, 2};
  p.clickedFace = Direction::Up;
  res = useItemOnBlock(world, snowBucket, p);
  assert(res.outcome == InteractionOutcome::Success);
  assert(world.getBlock({10, 65, 2}).is(Blocks::POWDER_SNOW));
  assert(snowBucket.id == "minecraft:bucket");

  // FlintAndSteel lighting TNT + hurting durability
  world.setBlock({12, 64, 2}, Blocks::TNT.defaultState(), 3);
  ItemStack flint = itemStack("flint_and_steel", 1);
  p.clickedPos = {12, 64, 2};
  res = useItemOnBlock(world, flint, p);
  assert(res.outcome == InteractionOutcome::Success);
  assert(flint.damageValue() == 1);
  assert(world.getBlock({12, 64, 2}).isAir());
}

} // namespace

int main() {
  testFullRegistryAndMetadata();
  testItemTagsCompostablesAndFuels();
  testItemStackComponentsAndDrops();
  testItemUseOnAndInAir();
  std::cout << "item_tests passed\n";
  return 0;
}
