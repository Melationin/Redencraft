#include "redstone/containers.hpp"
#include "redstone/fluid_flow.hpp"
#include "redstone/redstone.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace redstone {
namespace {

inline double randomTriangle(Random &rng, double origin, double deviation) {
  return origin + deviation * (rng.nextDouble() - rng.nextDouble());
}

inline int effectiveMaxStack(const ItemStack &stack) {
  return std::min(64, stack.maxStackSize());
}

inline bool stacksCanMerge(const ItemStack &a, const ItemStack &b) {
  return !a.empty() && !b.empty() && a.id == b.id &&
         a.count < effectiveMaxStack(a);
}

inline Direction horizontalClockWise(Direction d) {
  switch (d) {
  case Direction::North:
    return Direction::East;
  case Direction::East:
    return Direction::South;
  case Direction::South:
    return Direction::West;
  case Direction::West:
    return Direction::North;
  default:
    return d;
  }
}

inline Direction horizontalCounterClockWise(Direction d) {
  switch (d) {
  case Direction::North:
    return Direction::West;
  case Direction::West:
    return Direction::South;
  case Direction::South:
    return Direction::East;
  case Direction::East:
    return Direction::North;
  default:
    return d;
  }
}

inline bool isBlockTypeInCopperChestFamily(const BlockState &state) {
  return state.is(Blocks::COPPER_CHEST) ||
         state.is(Blocks::EXPOSED_COPPER_CHEST) ||
         state.is(Blocks::WEATHERED_COPPER_CHEST) ||
         state.is(Blocks::OXIDIZED_COPPER_CHEST) ||
         state.is(Blocks::WAXED_COPPER_CHEST) ||
         state.is(Blocks::WAXED_EXPOSED_COPPER_CHEST) ||
         state.is(Blocks::WAXED_WEATHERED_COPPER_CHEST) ||
         state.is(Blocks::WAXED_OXIDIZED_COPPER_CHEST);
}

inline bool isChestBlockedByBlock(const World &world, Pos pos) {
  const Pos above = pos.relative(Direction::Up);
  return isRedstoneConductor(world.getBlock(above));
}

inline bool isChestBlockedAt(const World &, Pos) { return false; }

bool isChestBlocked(const World &world, Pos pos) {
  // TODO: Cat entity sitting on chest check once Cat entity exists.
  (void)isChestBlockedAt;
  return isChestBlockedByBlock(world, pos);
}

// SectionPos key matching World entitySectionStorage layout.
inline std::uint64_t hopperSectionPosKey(int x, int y, int z) {
  return (static_cast<std::uint64_t>(x) & 0x3FFFFFFull) << 38 |
         (static_cast<std::uint64_t>(z) & 0x3FFFFFFull) << 12 |
         (static_cast<std::uint64_t>(y) & 0xFFFull);
}

inline Aabb hopperSuckAabbWorld(Pos pos) {
  // Hopper.SUCK_AABB = Block.column(16.0, 11.0, 32.0) -> [0, 11/16, 0, 1, 2, 1].
  return {static_cast<double>(pos.x),
          static_cast<double>(pos.y) + 11.0 / 16.0,
          static_cast<double>(pos.z),
          static_cast<double>(pos.x) + 1.0,
          static_cast<double>(pos.y) + 2.0,
          static_cast<double>(pos.z) + 1.0};
}

void registerHopperSuckItemWatchers(World &world, Pos pos,
                                    const std::shared_ptr<HopperBlockEntity> &hopper) {
  const Aabb box = hopperSuckAabbWorld(pos);
  const int x0 = static_cast<int>(std::floor(box.minX - 2.0)) >> 4;
  const int y0 = static_cast<int>(std::floor(box.minY - 4.0)) >> 4;
  const int z0 = static_cast<int>(std::floor(box.minZ - 2.0)) >> 4;
  const int x1 = static_cast<int>(std::floor(box.maxX + 2.0)) >> 4;
  const int y1 = static_cast<int>(std::floor(box.maxY)) >> 4;
  const int z1 = static_cast<int>(std::floor(box.maxZ + 2.0)) >> 4;
  for (int sx = x0; sx <= x1; ++sx)
    for (int sy = y0; sy <= y1; ++sy)
      for (int sz = z0; sz <= z1; ++sz)
        world.watchHopperItemSection(hopperSectionPosKey(sx, sy, sz), hopper);
}

bool isFullCollisionShapeBlock(const BlockState &state) {
  if (state.isAir())
    return false;
  const VoxelShape &shape = collisionShape(state);
  if (shape.empty())
    return false;
  // Block.isShapeFullBlock checks full 0..16 cube coverage.
  for (const auto &b : shape) {
    if (b.x0 <= 0.0 && b.y0 <= 0.0 && b.z0 <= 0.0 && b.x1 >= 16.0 &&
        b.y1 >= 16.0 && b.z1 >= 16.0)
      return true;
  }
  return false;
}

bool doesAboveBlockBlockHopperSuck(World &world, Pos /*abovePos*/,
                                   const BlockState &aboveState) {
  if (!isFullCollisionShapeBlock(aboveState))
    return false;
  auto tagged = world.hasBlockTag(aboveState, "minecraft:does_not_block_hoppers");
  return !(tagged && *tagged);
}

// Compostables probability table (ComposterBlock.bootStrap, Java 26.1).
float lookupComposterChance(std::string_view id) {
  struct Entry {
    std::string_view id;
    float chance;
  };
  static constexpr Entry kTable[] = {
      {"minecraft:jungle_leaves", 0.3f},
      {"minecraft:oak_leaves", 0.3f},
      {"minecraft:spruce_leaves", 0.3f},
      {"minecraft:dark_oak_leaves", 0.3f},
      {"minecraft:pale_oak_leaves", 0.3f},
      {"minecraft:acacia_leaves", 0.3f},
      {"minecraft:cherry_leaves", 0.3f},
      {"minecraft:birch_leaves", 0.3f},
      {"minecraft:azalea_leaves", 0.3f},
      {"minecraft:mangrove_leaves", 0.3f},
      {"minecraft:oak_sapling", 0.3f},
      {"minecraft:spruce_sapling", 0.3f},
      {"minecraft:birch_sapling", 0.3f},
      {"minecraft:jungle_sapling", 0.3f},
      {"minecraft:acacia_sapling", 0.3f},
      {"minecraft:cherry_sapling", 0.3f},
      {"minecraft:dark_oak_sapling", 0.3f},
      {"minecraft:pale_oak_sapling", 0.3f},
      {"minecraft:mangrove_propagule", 0.3f},
      {"minecraft:beetroot_seeds", 0.3f},
      {"minecraft:dried_kelp", 0.3f},
      {"minecraft:short_grass", 0.3f},
      {"minecraft:kelp", 0.3f},
      {"minecraft:melon_seeds", 0.3f},
      {"minecraft:pumpkin_seeds", 0.3f},
      {"minecraft:seagrass", 0.3f},
      {"minecraft:sweet_berries", 0.3f},
      {"minecraft:glow_berries", 0.3f},
      {"minecraft:wheat_seeds", 0.3f},
      {"minecraft:moss_carpet", 0.3f},
      {"minecraft:pale_moss_carpet", 0.3f},
      {"minecraft:pale_hanging_moss", 0.3f},
      {"minecraft:pink_petals", 0.3f},
      {"minecraft:wildflowers", 0.3f},
      {"minecraft:leaf_litter", 0.3f},
      {"minecraft:small_dripleaf", 0.3f},
      {"minecraft:hanging_roots", 0.3f},
      {"minecraft:mangrove_roots", 0.3f},
      {"minecraft:torchflower_seeds", 0.3f},
      {"minecraft:pitcher_pod", 0.3f},
      {"minecraft:firefly_bush", 0.3f},
      {"minecraft:bush", 0.3f},
      {"minecraft:cactus_flower", 0.3f},
      {"minecraft:short_dry_grass", 0.3f},
      {"minecraft:tall_dry_grass", 0.3f},
      {"minecraft:dried_kelp_block", 0.5f},
      {"minecraft:tall_grass", 0.5f},
      {"minecraft:flowering_azalea_leaves", 0.5f},
      {"minecraft:cactus", 0.5f},
      {"minecraft:sugar_cane", 0.5f},
      {"minecraft:vine", 0.5f},
      {"minecraft:nether_sprouts", 0.5f},
      {"minecraft:weeping_vines", 0.5f},
      {"minecraft:twisting_vines", 0.5f},
      {"minecraft:melon_slice", 0.5f},
      {"minecraft:glow_lichen", 0.5f},
      {"minecraft:sea_pickle", 0.65f},
      {"minecraft:lily_pad", 0.65f},
      {"minecraft:pumpkin", 0.65f},
      {"minecraft:carved_pumpkin", 0.65f},
      {"minecraft:melon", 0.65f},
      {"minecraft:apple", 0.65f},
      {"minecraft:beetroot", 0.65f},
      {"minecraft:carrot", 0.65f},
      {"minecraft:cocoa_beans", 0.65f},
      {"minecraft:potato", 0.65f},
      {"minecraft:wheat", 0.65f},
      {"minecraft:brown_mushroom", 0.65f},
      {"minecraft:red_mushroom", 0.65f},
      {"minecraft:mushroom_stem", 0.65f},
      {"minecraft:crimson_fungus", 0.65f},
      {"minecraft:warped_fungus", 0.65f},
      {"minecraft:nether_wart", 0.65f},
      {"minecraft:crimson_roots", 0.65f},
      {"minecraft:warped_roots", 0.65f},
      {"minecraft:shroomlight", 0.65f},
      {"minecraft:dandelion", 0.65f},
      {"minecraft:poppy", 0.65f},
      {"minecraft:blue_orchid", 0.65f},
      {"minecraft:allium", 0.65f},
      {"minecraft:azure_bluet", 0.65f},
      {"minecraft:red_tulip", 0.65f},
      {"minecraft:orange_tulip", 0.65f},
      {"minecraft:white_tulip", 0.65f},
      {"minecraft:pink_tulip", 0.65f},
      {"minecraft:oxeye_daisy", 0.65f},
      {"minecraft:cornflower", 0.65f},
      {"minecraft:lily_of_the_valley", 0.65f},
      {"minecraft:wither_rose", 0.65f},
      {"minecraft:open_eyeblossom", 0.65f},
      {"minecraft:closed_eyeblossom", 0.65f},
      {"minecraft:fern", 0.65f},
      {"minecraft:sunflower", 0.65f},
      {"minecraft:lilac", 0.65f},
      {"minecraft:rose_bush", 0.65f},
      {"minecraft:peony", 0.65f},
      {"minecraft:large_fern", 0.65f},
      {"minecraft:spore_blossom", 0.65f},
      {"minecraft:azalea", 0.65f},
      {"minecraft:moss_block", 0.65f},
      {"minecraft:pale_moss_block", 0.65f},
      {"minecraft:big_dripleaf", 0.65f},
      {"minecraft:hay_block", 0.85f},
      {"minecraft:brown_mushroom_block", 0.85f},
      {"minecraft:red_mushroom_block", 0.85f},
      {"minecraft:nether_wart_block", 0.85f},
      {"minecraft:warped_wart_block", 0.85f},
      {"minecraft:flowering_azalea", 0.85f},
      {"minecraft:bread", 0.85f},
      {"minecraft:baked_potato", 0.85f},
      {"minecraft:cookie", 0.85f},
      {"minecraft:torchflower", 0.85f},
      {"minecraft:pitcher_plant", 0.85f},
      {"minecraft:cake", 1.0f},
      {"minecraft:pumpkin_pie", 1.0f},
  };
  for (const auto &entry : kTable) {
    if (entry.id == id)
      return entry.chance;
  }
  return -1.0f;
}

int composterLevelOf(const BlockState &state) {
  try {
    return std::stoi(state.get("level", "0"));
  } catch (...) {
    return 0;
  }
}

void spawnDefaultDispensedItem(World &world, Pos pos, Direction facing,
                               const ItemStack &stack) {
  if (stack.empty())
    return;
  const Vec3 step = [&]() -> Vec3 {
    switch (facing) {
    case Direction::Down:
      return {0, -1, 0};
    case Direction::Up:
      return {0, 1, 0};
    case Direction::North:
      return {0, 0, -1};
    case Direction::South:
      return {0, 0, 1};
    case Direction::West:
      return {-1, 0, 0};
    case Direction::East:
      return {1, 0, 0};
    }
    return {0, 0, 0};
  }();
  // DispenserBlock.getDispensePosition: center + 0.7 * step.
  double sx = pos.x + 0.5 + 0.7 * step.x;
  double sy = pos.y + 0.5 + 0.7 * step.y;
  double sz = pos.z + 0.5 + 0.7 * step.z;
  if (facing == Direction::Up || facing == Direction::Down)
    sy -= 0.125;
  else
    sy -= 0.15625;
  const double pow = world.random.nextDouble() * 0.1 + 0.2;
  const Vec3 motion{
      randomTriangle(world.random, step.x * pow, 0.0172275 * 6),
      randomTriangle(world.random, 0.2, 0.0172275 * 6),
      randomTriangle(world.random, step.z * pow, 0.0172275 * 6),
  };
  world.addEntity(
      std::make_shared<ItemEntity>(world, Vec3{sx, sy, sz}, stack, motion));
  world.log.push_back({"dispense_item", pos, stack.id});
}

void dropItemStackScattered(World &world, Pos pos, ItemStack stack) {
  // Containers.dropItemStack(Level, double, double, double, ItemStack).
  constexpr double kWidth = 0.25;
  constexpr double kRange = 1.0 - kWidth;
  constexpr double kHalf = kWidth / 2.0;
  const double xo = pos.x + world.random.nextDouble() * kRange + kHalf;
  const double yo = pos.y + world.random.nextDouble() * kRange;
  const double zo = pos.z + world.random.nextDouble() * kRange + kHalf;
  while (!stack.empty()) {
    const int splitCount =
        std::min(stack.count, world.random.nextInt(21) + 10);
    ItemStack piece = itemStack(stack.id, splitCount);
    stack.count -= splitCount;
    const Vec3 motion{
        randomTriangle(world.random, 0.0, 0.11485000171139836),
        randomTriangle(world.random, 0.2, 0.11485000171139836),
        randomTriangle(world.random, 0.0, 0.11485000171139836),
    };
    world.addEntity(
        std::make_shared<ItemEntity>(world, Vec3{xo, yo, zo}, piece, motion));
  }
}

bool tryPlaceFluidFromBucket(World &world, Pos target, bool isWater) {
  if (!world.isLoaded(target))
    return false;
  if (isWater && world.waterEvaporates)
    return true;
  const BlockState s = world.getBlock(target);
  if (isWater && s.block().isLiquidContainer() &&
      s.block().isWaterloggedHost() && canPlaceLiquid(s, Fluids::water())) {
    return placeLiquid(world, target, s, fluidStateOf(defaultState("water")));
  }
  const bool mayReplace = isReplaceable(s) || !blocksMotion(s);
  if (!(s.isAir() || mayReplace))
    return false;
  if (!s.isAir() && !isLiquidBlock(s))
    world.updateOrDestroy(target, s, airState(), 3, 512);
  return world.setBlock(target, defaultState(isWater ? "water" : "lava"), 3) ||
         fluidStateOf(s).isSource();
}

std::optional<std::string> tryPickupFluidWithBucket(World &world, Pos target) {
  if (!world.isLoaded(target))
    return std::nullopt;
  const BlockState state = world.getBlock(target);
  if (state.block().isLiquidContainer() && state.block().isWaterloggedHost() &&
      waterloggedOf(state)) {
    world.setBlock(target, state.with(PropertyKind::Waterlogged, std::uint8_t{0}),
                   3);
    return std::string("minecraft:water_bucket");
  }
  if (isLiquidBlock(state)) {
    const FluidState fluid = fluidStateOf(state);
    if (fluid.isSource() && !fluid.isEmpty()) {
      const bool lava = fluid.is(Fluids::lava());
      world.setBlock(target, airState(), 3);
      return std::string(lava ? "minecraft:lava_bucket"
                              : "minecraft:water_bucket");
    }
  }
  return std::nullopt;
}

ItemStack consumeDispenseRemainder(World &world, Pos pos, Direction facing,
                                   DispenserBlockEntity &entity,
                                   ItemStack dispensed,
                                   const ItemStack &remainder) {
  --dispensed.count;
  if (dispensed.empty())
    return remainder;
  ItemStack leftover = entity.insertItem(world, remainder);
  if (!leftover.empty())
    spawnDefaultDispensedItem(world, pos, facing, leftover);
  return dispensed;
}

// Hopper transfer helpers following HopperBlockEntity.java.
bool canPlaceItemInContainer(const ResolvedContainerView &container,
                             const ItemStack &stack, int slot, Direction from) {
  (void)from;
  if (slot < container.first->containerSize())
    return container.first->canPlaceItem(slot, stack);
  return container.second &&
         container.second->canPlaceItem(slot - container.first->containerSize(),
                                        stack);
}

bool canTakeItemFromContainer(const ContainerBlockEntity &dest,
                              const ContainerBlockEntity &src,
                              const ItemStack &stack, int slot, Direction dir) {
  (void)dest;
  (void)src;
  (void)stack;
  (void)slot;
  (void)dir;
  return true;
}

ItemStack tryMoveInItem(World &world, ContainerBlockEntity *source,
                        ResolvedContainerView &dest, ItemStack stack,
                        int slot, Direction fromDirection) {
  const ItemStack &existing = dest.getItem(slot);
  if (!canPlaceItemInContainer(dest, stack, slot, fromDirection))
    return stack;

  bool moved = false;
  bool wasEmptyDest = false;
  if (dest.first && !dest.second && dest.first->isEmpty()) {
    wasEmptyDest = dest.first->isEmpty();
  } else if (dest.first && dest.second && dest.first->isEmpty() &&
             dest.second->isEmpty()) {
    wasEmptyDest = true;
  }

  if (existing.empty()) {
    ItemStack copy = stack;
    dest.setItem(world, slot, copy);
    stack = {};
    moved = true;
  } else if (stacksCanMerge(existing, stack)) {
    const int room = effectiveMaxStack(existing) - existing.count;
    const int transfer = std::min(stack.count, room);
    if (transfer > 0) {
      ItemStack updated = existing;
      updated.count += transfer;
      dest.setItem(world, slot, updated);
      stack.count -= transfer;
      if (stack.count <= 0)
        stack = {};
      moved = true;
    }
  }

  if (moved) {
    if (wasEmptyDest && dest.first &&
        dest.first->kind == ContainerKind::Hopper && !dest.second) {
      auto *destHopper = static_cast<HopperBlockEntity *>(dest.first.get());
      if (!destHopper->isOnCustomCooldown()) {
        int k = 0;
        if (source && source->kind == ContainerKind::Hopper) {
          auto *srcHopper = static_cast<HopperBlockEntity *>(source);
          if (destHopper->tickedGameTime >= srcHopper->tickedGameTime)
            k = 1;
        }
        destHopper->setCooldown(&world, 8 - k);
      }
    }
    dest.setChanged(world);
  }
  return stack;
}

ItemStack addItemToResolved(World &world, ContainerBlockEntity *source,
                            ResolvedContainerView &dest, ItemStack stack,
                            Direction fromDirection) {
  if (!dest.valid() || stack.empty())
    return stack;
  const int size = dest.size();
  for (int i = 0; i < size && !stack.empty(); ++i)
    stack = tryMoveInItem(world, source, dest, std::move(stack), i,
                          fromDirection);
  return stack;
}

bool isResolvedContainerFull(World &world, const ResolvedContainerView &view) {
  if (!view.valid())
    return true;
  if (!view.second) {
    if (view.first->isInventoryFull()) {
      ++world.containerTickStats.cachedFullChecks;
      return true;
    }
    return false;
  }
  if (view.first->isInventoryFull() && view.second->isInventoryFull()) {
    ++world.containerTickStats.cachedFullChecks;
    return true;
  }
  return false;
}

bool addItemFromWorldEntity(World &world, ContainerBlockEntity &dest,
                            ItemEntity &itemEntity) {
  if (itemEntity.isRemoved() || itemEntity.item.empty())
    return false;
  ResolvedContainerView view;
  // Recover shared_ptr handle via World lookup when available.
  view.first = world.containerEntity(dest.pos);
  if (!view.first)
    return false;
  ItemStack copy = itemEntity.item;
  ItemStack remainder =
      addItemToResolved(world, nullptr, view, copy, Direction::Up);
  if (remainder.empty()) {
    itemEntity.discard();
    return true;
  }
  if (remainder.count != itemEntity.item.count) {
    itemEntity.item = remainder;
    return true;
  }
  return false;
}

bool hopperEjectItems(World &world, Pos pos, HopperBlockEntity &self) {
  const Pos targetPos = pos.relative(self.facing);
  const BlockState targetState = world.getBlock(targetPos);
  // Composter output insertion (ComposterBlock.InputContainer).
  if (targetState.is(Blocks::COMPOSTER) &&
      opposite(self.facing) == Direction::Up) {
    const int level = composterLevelOf(targetState);
    if (level < 7) {
      for (int slot = 0; slot < self.containerSize(); ++slot) {
        const ItemStack &s = self.getItem(slot);
        if (!s.empty() && isCompostableItem(s.id)) {
          ItemStack taken = self.removeItem(slot, 1);
          const float chance = composterChance(taken.id);
          if ((level == 0 && chance > 0.0f) ||
              world.random.nextDouble() < chance) {
            const int nextLevel = level + 1;
            const BlockState nextState =
                targetState.with("level", std::to_string(nextLevel));
            world.setBlock(targetPos, nextState, 3);
            world.updateNeighbourForOutputSignal(targetPos,
                                                 targetState.block());
            if (nextLevel == 7)
              world.scheduleTick(targetPos, targetState.block(), 20);
          }
          self.setChanged(world);
          return true;
        }
      }
    }
    return false;
  }

  ResolvedContainerView dest = resolveContainerView(world, targetPos, true);
  if (!dest.valid()) {
    // TODO: ContainerEntity (chest_minecart / hopper_minecart) destination.
    return false;
  }
  if (isResolvedContainerFull(world, dest))
    return false;

  const Direction fromDir = opposite(self.facing);
  for (int slot = 0; slot < self.containerSize(); ++slot) {
    const ItemStack &slotStack = self.getItem(slot);
    if (slotStack.empty())
      continue;
    const ItemStack original = slotStack;
    ItemStack single = self.removeItem(slot, 1);
    ItemStack remainder =
        addItemToResolved(world, &self, dest, single, fromDir);
    if (remainder.empty()) {
      self.setChanged(world);
      return true;
    }
    self.setItem(slot, original);
  }
  return false;
}

bool tryTakeInItemFromSlot(World &world, HopperBlockEntity &dest,
                           const ResolvedContainerView &srcView, int slot,
                           Direction direction) {
  const int firstSize = srcView.first->containerSize();
  ContainerBlockEntity &src =
      slot < firstSize ? *srcView.first : *srcView.second;
  const int localSlot = slot < firstSize ? slot : slot - firstSize;
  const ItemStack &itemStack = src.getItem(localSlot);
  if (!itemStack.empty() &&
      canTakeItemFromContainer(dest, src, itemStack, localSlot, direction)) {
    const ItemStack original = itemStack;
    ItemStack single = src.removeItem(localSlot, 1);
    ResolvedContainerView destView;
    destView.first = world.containerEntity(dest.pos);
    ItemStack remainder =
        addItemToResolved(world, nullptr, destView, single, Direction::Up);
    if (remainder.empty()) {
      src.setChanged(world);
      dest.setChanged(world);
      return true;
    }
    src.setItem(localSlot, original);
  }
  return false;
}

bool hopperSuckInItems(World &world, HopperBlockEntity &self) {
  const Pos abovePos = self.pos.relative(Direction::Up);
  const BlockState aboveState = world.getBlock(abovePos);

  // Composter extraction (ComposterBlock.OutputContainer).
  if (aboveState.is(Blocks::COMPOSTER)) {
    if (composterLevelOf(aboveState) == 8) {
      ResolvedContainerView destView;
      destView.first = world.containerEntity(self.pos);
      ItemStack remainder = addItemToResolved(
          world, nullptr, destView, itemStack("minecraft:bone_meal", 1),
          Direction::Down);
      if (remainder.empty()) {
        world.setBlock(abovePos, aboveState.with("level", "0"), 3);
        world.updateNeighbourForOutputSignal(abovePos, aboveState.block());
        self.setChanged(world);
        return true;
      }
    }
    return false;
  }

  ResolvedContainerView src = resolveContainerView(world, abovePos, true);
  if (src.valid()) {
    bool allEmpty = src.first->isEmpty() &&
                    (!src.second || src.second->isEmpty());
    if (allEmpty) {
      ++world.containerTickStats.cachedEmptyChecks;
      return false;
    }
    const int size = src.size();
    for (int i = 0; i < size; ++i) {
      if (tryTakeInItemFromSlot(world, self, src, i, Direction::Down))
        return true;
    }
    return false;
  }

  // No container block at abovePos: check if blocked, otherwise suck ItemEntities.
  if (doesAboveBlockBlockHopperSuck(world, abovePos, aboveState))
    return false;

  const Aabb suckBox = hopperSuckAabbWorld(self.pos);
  for (Entity *entity : world.entitiesIn(suckBox)) {
    auto *itemEntity = dynamic_cast<ItemEntity *>(entity);
    if (!itemEntity || itemEntity->isRemoved() || itemEntity->item.empty())
      continue;
    if (addItemFromWorldEntity(world, self, *itemEntity))
      return true;
  }
  return false;
}

template <class F>
bool hopperTryMoveItems(World &world, Pos pos, const BlockState &state,
                        HopperBlockEntity &self, F &&action) {
  if (self.isOnCooldown())
    return false;
  if (state.getView("enabled", "true") != "true")
    return false;

  bool changed = false;
  if (!self.isEmpty()) {
    changed = hopperEjectItems(world, pos, self);
  } else {
    ++world.containerTickStats.cachedEmptyChecks;
  }

  if (!self.isInventoryFull()) {
    changed |= action();
  } else {
    ++world.containerTickStats.cachedFullChecks;
  }

  if (changed) {
    self.setCooldown(&world, 8);
    self.setChanged(world);
    return true;
  }
  return false;
}

void attachSleepWatchers(World &world, Pos pos,
                         const std::shared_ptr<HopperBlockEntity> &hopper,
                         bool enabled) {
  world.watchHopperBlock(pos, hopper);
  if (!enabled)
    return;

  hopper->addWatcher(hopper);

  const Pos destPos = pos.relative(hopper->facing);
  world.watchHopperBlock(destPos, hopper);
  ResolvedContainerView dest = resolveContainerView(world, destPos, true);
  if (dest.first)
    dest.first->addWatcher(hopper);
  if (dest.second)
    dest.second->addWatcher(hopper);

  const Pos abovePos = pos.relative(Direction::Up);
  world.watchHopperBlock(abovePos, hopper);
  ResolvedContainerView src = resolveContainerView(world, abovePos, true);
  if (src.first)
    src.first->addWatcher(hopper);
  if (src.second)
    src.second->addWatcher(hopper);

  const BlockState aboveState = world.getBlock(abovePos);
  if (!src.valid() && !aboveState.is(Blocks::COMPOSTER) &&
      !doesAboveBlockBlockHopperSuck(world, abovePos, aboveState)) {
    registerHopperSuckItemWatchers(world, pos, hopper);
  }
}

void updateHopperEnabledState(World &world, Pos pos, const BlockState &state) {
  const bool shouldBeEnabled = !world.hasNeighborSignal(pos);
  const bool currentlyEnabled = state.getView("enabled", "true") == "true";
  if (shouldBeEnabled != currentlyEnabled) {
    world.setBlock(pos, state.with("enabled", shouldBeEnabled ? "true" : "false"),
                   2);
  }
  if (auto hopper = hopperEntityAt(world, pos)) {
    if (shouldBeEnabled)
      hopper->wake(world);
  }
}

} // namespace

// ---------------------------------------------------------------------------
// ContainerBlockEntity
// ---------------------------------------------------------------------------
ContainerBlockEntity::ContainerBlockEntity(ContainerKind kind, Pos pos,
                                           int slotCount)
    : kind(kind), pos(pos), items(static_cast<std::size_t>(slotCount)) {}

const ItemStack &ContainerBlockEntity::getItem(int slot) const {
  static const ItemStack kEmpty{};
  if (slot < 0 || slot >= containerSize())
    return kEmpty;
  return items[static_cast<std::size_t>(slot)];
}

void ContainerBlockEntity::setItem(int slot, const ItemStack &stack) {
  if (slot < 0 || slot >= containerSize())
    return;
  if (stack.empty()) {
    items[static_cast<std::size_t>(slot)] = {};
  } else {
    ItemStack clamped = stack;
    const int maxS = effectiveMaxStack(clamped);
    if (clamped.count > maxS)
      clamped.count = maxS;
    items[static_cast<std::size_t>(slot)] = std::move(clamped);
  }
  markMutated();
}

ItemStack ContainerBlockEntity::removeItem(int slot, int amount) {
  if (slot < 0 || slot >= containerSize() || amount <= 0)
    return {};
  ItemStack &slotStack = items[static_cast<std::size_t>(slot)];
  if (slotStack.empty())
    return {};
  const int take = std::min(amount, slotStack.count);
  ItemStack result = itemStack(slotStack.id, take);
  slotStack.count -= take;
  if (slotStack.count <= 0)
    slotStack = {};
  markMutated();
  return result;
}

ItemStack ContainerBlockEntity::removeItemNoUpdate(int slot) {
  if (slot < 0 || slot >= containerSize())
    return {};
  ItemStack &slotStack = items[static_cast<std::size_t>(slot)];
  if (slotStack.empty())
    return {};
  ItemStack out = std::move(slotStack);
  slotStack = {};
  markMutated();
  return out;
}

void ContainerBlockEntity::markMutated() {
  if (++modCount == 0)
    modCount = 1;
}

void ContainerBlockEntity::refreshSummary() const {
  if (cachedSummaryMod_ == modCount)
    return;
  int occupied = 0;
  int full = 0;
  float fillSum = 0.0f;
  for (const auto &s : items) {
    if (s.empty())
      continue;
    ++occupied;
    const int maxS = effectiveMaxStack(s);
    if (s.count >= maxS)
      ++full;
    fillSum += static_cast<float>(s.count) / static_cast<float>(maxS);
  }
  cachedOccupied_ = occupied;
  cachedFullCount_ = full;
  cachedEmpty_ = (occupied == 0);
  cachedFull_ = (full == static_cast<int>(items.size()));
  if (occupied == 0 || items.empty()) {
    cachedSignal_ = 0;
  } else {
    const float avg = fillSum / static_cast<float>(items.size());
    cachedSignal_ = static_cast<int>(std::floor(avg * 14.0f)) + 1;
  }
  cachedSummaryMod_ = modCount;
}

bool ContainerBlockEntity::isEmpty() const {
  refreshSummary();
  return cachedEmpty_;
}

bool ContainerBlockEntity::isInventoryFull() const {
  refreshSummary();
  return cachedFull_;
}

int ContainerBlockEntity::occupiedSlotCount() const {
  refreshSummary();
  return cachedOccupied_;
}

int ContainerBlockEntity::fullSlotCount() const {
  refreshSummary();
  return cachedFullCount_;
}

int ContainerBlockEntity::singleContainerComparatorSignal() const {
  refreshSummary();
  return cachedSignal_;
}

bool ContainerBlockEntity::canPlaceItem(int slot,
                                        const ItemStack &stack) const {
  (void)slot;
  (void)stack;
  return true;
}

void ContainerBlockEntity::addWatcher(
    const std::shared_ptr<HopperBlockEntity> &hopper) {
  if (!hopper)
    return;
  for (const auto &w : watchers) {
    if (w.lock().get() == hopper.get())
      return;
  }
  watchers.push_back(hopper);
}

void ContainerBlockEntity::wakeWatchers(World &world) {
  if (watchers.empty())
    return;
  auto current = std::move(watchers);
  watchers.clear();
  for (const auto &weak : current) {
    if (auto h = weak.lock()) {
      if (!h->removed)
        h->wake(world);
    }
  }
}

void ContainerBlockEntity::setChanged(World &world) {
  markMutated();
  wakeWatchers(world);
  if (world.isLoaded(pos)) {
    const BlockState state = world.getBlock(pos);
    world.updateNeighbourForOutputSignal(pos, state.block());
  }
}

// ---------------------------------------------------------------------------
// DispenserBlockEntity
// ---------------------------------------------------------------------------
int DispenserBlockEntity::getRandomSlot(World &world) const {
  int chosen = -1;
  int replaceOdds = 1;
  for (int i = 0; i < containerSize(); ++i) {
    if (!items[static_cast<std::size_t>(i)].empty() &&
        world.random.nextInt(replaceOdds++) == 0) {
      chosen = i;
    }
  }
  return chosen;
}

ItemStack DispenserBlockEntity::insertItem(World &world,
                                           const ItemStack &stack) {
  if (stack.empty())
    return {};
  ItemStack remaining = stack;
  const int maxS = effectiveMaxStack(remaining);
  for (int i = 0; i < containerSize() && !remaining.empty(); ++i) {
    ItemStack &slot = items[static_cast<std::size_t>(i)];
    if (!slot.empty() && slot.id == remaining.id && slot.count < maxS) {
      const int add = std::min(remaining.count, maxS - slot.count);
      slot.count += add;
      remaining.count -= add;
      markMutated();
    }
  }
  for (int i = 0; i < containerSize() && !remaining.empty(); ++i) {
    ItemStack &slot = items[static_cast<std::size_t>(i)];
    if (slot.empty()) {
      const int add = std::min(remaining.count, maxS);
      slot = itemStack(remaining.id, add);
      remaining.count -= add;
      markMutated();
    }
  }
  if (remaining.count != stack.count)
    setChanged(world);
  return remaining.count > 0 ? remaining : ItemStack{};
}

// ---------------------------------------------------------------------------
// HopperBlockEntity & Lithium Sleeping State Transitions
// ---------------------------------------------------------------------------
HopperBlockEntity::HopperBlockEntity(Pos pos)
    : ContainerBlockEntity(ContainerKind::Hopper, pos, 5) {}

void HopperBlockEntity::setCooldown(World *world, int time) {
  if (world && time > 0) {
    auto it = world->containerTickerSlots.find(pos);
    if (it != world->containerTickerSlots.end() && it->second &&
        !it->second->removed) {
      auto *slot = it->second;
      if (time == 7 &&
          tickedGameTime == std::numeric_limits<std::int64_t>::max()) {
        // Lithium HopperBlockEntityMixin: skip current tick so cooldown=7
        // aligns with both ticking orders.
        cooldownTime = time;
        sleepOnlyCurrentTick(*world);
        return;
      }
      if (slot->sleepMode == ContainerSleepMode::SleepIndefinite) {
        wake(*world);
      }
    }
  }
  cooldownTime = time;
}

void HopperBlockEntity::wake(World &world) {
  auto it = world.containerTickerSlots.find(pos);
  if (it == world.containerTickerSlots.end() || !it->second ||
      it->second->removed)
    return;
  ContainerTickerSlot &slot = *it->second;
  if (slot.sleepMode != ContainerSleepMode::Active) {
    slot.sleepMode = ContainerSleepMode::Active;
    ++world.containerTickStats.wakeups;
  }
}

void HopperBlockEntity::sleepIndefinite(World &world) {
  auto it = world.containerTickerSlots.find(pos);
  if (it == world.containerTickerSlots.end() || !it->second ||
      it->second->removed)
    return;
  ContainerTickerSlot &slot = *it->second;
  if (slot.sleepMode != ContainerSleepMode::SleepIndefinite) {
    slot.sleepMode = ContainerSleepMode::SleepIndefinite;
    tickedGameTime = std::numeric_limits<std::int64_t>::max();
    ++world.containerTickStats.sleepTransitions;
  }
}

void HopperBlockEntity::sleepOnlyCurrentTick(World &world) {
  auto it = world.containerTickerSlots.find(pos);
  if (it == world.containerTickerSlots.end() || !it->second ||
      it->second->removed)
    return;
  ContainerTickerSlot &slot = *it->second;
  if (slot.sleepMode == ContainerSleepMode::SleepIndefinite)
    ++world.containerTickStats.wakeups;
  slot.sleepMode = ContainerSleepMode::SleepUntil;
  slot.wakeGameTime = world.gameTime() + 1;
}

// ---------------------------------------------------------------------------
// Classification & Construction
// ---------------------------------------------------------------------------
bool isAnyChestBlock(const BlockState &state) {
  return state.block().isChest();
}

bool canChestsConnect(const BlockState &a, const BlockState &b) {
  if (a.sameBlock(b))
    return true;
  return isBlockTypeInCopperChestFamily(a) && isBlockTypeInCopperChestFamily(b);
}

std::optional<ContainerKind> containerKindForState(const BlockState &state) {
  if (state.is(Blocks::TRAPPED_CHEST))
    return ContainerKind::TrappedChest;
  if (isAnyChestBlock(state))
    return ContainerKind::Chest;
  if (state.is(Blocks::BARREL))
    return ContainerKind::Barrel;
  if (state.is(Blocks::DISPENSER))
    return ContainerKind::Dispenser;
  if (state.is(Blocks::DROPPER))
    return ContainerKind::Dropper;
  if (state.is(Blocks::HOPPER))
    return ContainerKind::Hopper;
  return std::nullopt;
}

std::string_view containerEntityTypeId(ContainerKind kind) {
  switch (kind) {
  case ContainerKind::Chest:
    return "minecraft:chest";
  case ContainerKind::TrappedChest:
    return "minecraft:trapped_chest";
  case ContainerKind::Barrel:
    return "minecraft:barrel";
  case ContainerKind::Dispenser:
    return "minecraft:dispenser";
  case ContainerKind::Dropper:
    return "minecraft:dropper";
  case ContainerKind::Hopper:
    return "minecraft:hopper";
  }
  return "minecraft:chest";
}

bool isContainerEntityTypeId(std::string_view id) {
  std::string normalized;
  if (id.find(':') == std::string_view::npos) {
    normalized = "minecraft:" + std::string(id);
    id = normalized;
  }
  return id == "minecraft:chest" || id == "minecraft:trapped_chest" ||
         id == "minecraft:barrel" || id == "minecraft:dispenser" ||
         id == "minecraft:dropper" || id == "minecraft:hopper";
}

std::shared_ptr<ContainerBlockEntity>
makeContainerEntityForState(const BlockState &state, Pos pos) {
  auto kind = containerKindForState(state);
  if (!kind)
    return nullptr;
  switch (*kind) {
  case ContainerKind::Chest:
    return std::make_shared<ChestBlockEntity>(pos, false);
  case ContainerKind::TrappedChest:
    return std::make_shared<ChestBlockEntity>(pos, true);
  case ContainerKind::Barrel:
    return std::make_shared<BarrelBlockEntity>(pos);
  case ContainerKind::Dispenser:
    return std::make_shared<DispenserBlockEntity>(pos, false);
  case ContainerKind::Dropper:
    return std::make_shared<DispenserBlockEntity>(pos, true);
  case ContainerKind::Hopper: {
    auto h = std::make_shared<HopperBlockEntity>(pos);
    h->facing = directionFromString(state.getView("facing", "down"));
    return h;
  }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// World Lifecycle & Ticking
// ---------------------------------------------------------------------------
void ensureContainerBlockEntity(World &world, Pos pos,
                                const BlockState &state) {
  const auto kind = containerKindForState(state);
  if (!kind)
    return;
  auto existing = world.containerEntity(pos);
  if (existing && existing->kind == *kind) {
    if (*kind == ContainerKind::Hopper) {
      auto *h = static_cast<HopperBlockEntity *>(existing.get());
      h->facing = directionFromString(state.getView("facing", "down"));
      world.registerContainerTicker(pos, existing);
    }
    return;
  }
  auto created = makeContainerEntityForState(state, pos);
  if (created) {
    world.setContainerEntity(pos, std::move(created),
                             *kind == ContainerKind::Hopper);
  }
}

void dropContainerContents(World &world, Pos pos,
                           ContainerBlockEntity &entity) {
  for (int i = 0; i < entity.containerSize(); ++i) {
    ItemStack stack = entity.removeItemNoUpdate(i);
    if (!stack.empty())
      dropItemStackScattered(world, pos, std::move(stack));
  }
}

void preRemoveContainerSideEffects(World &world, Pos pos,
                                   const BlockState &oldState, bool dropItems) {
  auto existing = world.containerEntity(pos);
  if (!existing)
    return;
  if (dropItems)
    dropContainerContents(world, pos, *existing);
  world.removeContainerEntity(pos, false, oldState.is(Blocks::TRAPPED_CHEST));
}

void tickContainerBlockEntities(World &world) {
  world.mergePendingContainerTickers();
  world.tickingContainerBlockEntities = true;
  const std::int64_t now = world.gameTime();
  for (std::size_t i = 0; i < world.containerTickers.size(); ++i) {
    world.currentContainerTickerIndex_ = i;
    ContainerTickerSlot &slot = *world.containerTickers[i];
    if (slot.removed || !slot.entity || slot.entity->removed)
      continue;
    if (slot.sleepMode == ContainerSleepMode::SleepIndefinite) {
      ++world.containerTickStats.sleepingSkippedTicks;
      continue;
    }
    if (slot.sleepMode == ContainerSleepMode::SleepUntil) {
      if (now < slot.wakeGameTime) {
        ++world.containerTickStats.sleepingSkippedTicks;
        continue;
      }
      slot.sleepMode = ContainerSleepMode::Active;
    }

    const Pos pos = slot.pos;
    if (!world.isLoaded(pos))
      continue;
    const BlockState state = world.getBlock(pos);
    if (!state.is(Blocks::HOPPER))
      continue;

    ++world.containerTickStats.activeTicks;
    auto hopper = std::static_pointer_cast<HopperBlockEntity>(slot.entity);
    hopper->facing = directionFromString(state.getView("facing", "down"));
    hopper->setCooldown(nullptr, hopper->cooldownTime - 1);
    hopper->tickedGameTime = now;
    if (!hopper->isOnCooldown()) {
      hopper->setCooldown(nullptr, 0);
      const bool moved = hopperTryMoveItems(world, pos, state, *hopper, [&] {
        return hopperSuckInItems(world, *hopper);
      });
      if (!moved) {
        ++world.containerTickStats.cachedNoOpTransfers;
        const bool enabled = state.getView("enabled", "true") == "true";
        if (slot.sleepMode == ContainerSleepMode::Active &&
            !hopper->isOnCooldown()) {
          attachSleepWatchers(world, pos, hopper, enabled);
          hopper->sleepIndefinite(world);
        }
      }
    }
  }
  world.tickingContainerBlockEntities = false;
  world.sweepContainerTickers();
}

// ---------------------------------------------------------------------------
// Unified Container Views & Comparator / Trapped Signals
// ---------------------------------------------------------------------------
const ItemStack &ResolvedContainerView::getItem(int index) const {
  static const ItemStack kEmpty{};
  if (!first || index < 0)
    return kEmpty;
  if (index < first->containerSize())
    return first->getItem(index);
  if (second)
    return second->getItem(index - first->containerSize());
  return kEmpty;
}

void ResolvedContainerView::setItem(World &world, int index,
                                    const ItemStack &stack) {
  (void)world;
  if (!first || index < 0)
    return;
  if (index < first->containerSize()) {
    first->setItem(index, stack);
  } else if (second) {
    second->setItem(index - first->containerSize(), stack);
  }
}

void ResolvedContainerView::setChanged(World &world) {
  if (first)
    first->setChanged(world);
  if (second)
    second->setChanged(world);
}

std::vector<Pos> ResolvedContainerView::positions() const {
  std::vector<Pos> out;
  if (first)
    out.push_back(first->pos);
  if (second)
    out.push_back(second->pos);
  return out;
}

ResolvedContainerView resolveContainerView(World &world, Pos pos,
                                           bool ignoreBeingBlocked) {
  if (!world.isLoaded(pos))
    return {};
  const BlockState state = world.getBlock(pos);
  auto entity = world.containerEntity(pos);
  if (!entity)
    return {};

  if (isAnyChestBlock(state)) {
    if (!ignoreBeingBlocked && isChestBlocked(world, pos))
      return {};
    const std::string_view type = state.getView("type", "single");
    if (type != "single") {
      const Pos partnerPos = chestConnectedBlockPos(pos, state);
      const BlockState partnerState = world.getBlock(partnerPos);
      if (canChestsConnect(state, partnerState) &&
          partnerState.getView("type", "single") != "single" &&
          partnerState.getView("facing") == state.getView("facing")) {
        if (!ignoreBeingBlocked && isChestBlocked(world, partnerPos))
          return {};
        auto partner = world.containerEntity(partnerPos);
        if (partner) {
          // ChestBlock.getBlockType: RIGHT is FIRST (0..26), LEFT is SECOND.
          if (type == "right")
            return {std::move(entity), std::move(partner)};
          return {std::move(partner), std::move(entity)};
        }
      }
    }
  }
  return {std::move(entity), nullptr};
}

int containerComparatorSignal(World &world, Pos pos, const BlockState &state) {
  ResolvedContainerView view = resolveContainerView(world, pos, false);
  if (!view.valid())
    return 0;
  (void)state;
  if (!view.second)
    return view.first->singleContainerComparatorSignal();

  float fillSum = 0.0f;
  int occupied = 0;
  const int totalSize = view.size();
  for (int i = 0; i < totalSize; ++i) {
    const ItemStack &s = view.getItem(i);
    if (!s.empty()) {
      ++occupied;
      fillSum += static_cast<float>(s.count) /
                 static_cast<float>(effectiveMaxStack(s));
    }
  }
  if (occupied == 0 || totalSize == 0)
    return 0;
  const float avg = fillSum / static_cast<float>(totalSize);
  return static_cast<int>(std::floor(avg * 14.0f)) + 1;
}

int trappedChestSignal(const World &world, Pos pos) {
  auto entity = world.containerEntity(pos);
  if (!entity || entity->kind != ContainerKind::TrappedChest)
    return 0;
  return std::clamp(entity->openCount, 0, 15);
}

void containerUpdateOpenCount(World &world, Pos pos, const BlockState &state,
                              int delta) {
  auto entity = world.containerEntity(pos);
  if (!entity)
    return;
  entity->openCount = std::max(0, entity->openCount + delta);
  if (state.is(Blocks::TRAPPED_CHEST)) {
    world.updateNeighborsAt(pos, state.block());
    world.updateNeighborsAt(pos.relative(Direction::Down), state.block());
  }
  if (state.is(Blocks::BARREL)) {
    const bool shouldBeOpen = entity->openCount > 0;
    if (openOf(state) != shouldBeOpen)
      world.setBlock(pos, state.with(PropertyKind::Open, shouldBeOpen ? 1 : 0),
                     3);
  }
}

// ---------------------------------------------------------------------------
// Double-Chest & Placement Helpers
// ---------------------------------------------------------------------------
Direction chestConnectedDirection(const BlockState &state) {
  const Direction facing =
      directionFromString(state.getView("facing", "north"));
  return state.getView("type", "single") == "left"
             ? horizontalClockWise(facing)
             : horizontalCounterClockWise(facing);
}

Pos chestConnectedBlockPos(Pos pos, const BlockState &state) {
  return pos.relative(chestConnectedDirection(state));
}

namespace {
std::optional<Direction> candidateChestPartnerFacing(const World &world,
                                                     Pos pos, Direction dir,
                                                     const BlockState &base) {
  const BlockState neighbour = world.getBlock(pos.relative(dir));
  if (canChestsConnect(base, neighbour) &&
      neighbour.getView("type", "single") == "single") {
    return directionFromString(neighbour.getView("facing", "north"));
  }
  return std::nullopt;
}
} // namespace

BlockState chestPlacementState(World &world, Pos pos, const BlockState &base,
                               Direction playerHorizontal, Direction clickedFace,
                               bool sneaking) {
  std::string_view type = "single";
  Direction facing = opposite(playerHorizontal);
  const bool horizontalFace = clickedFace == Direction::North ||
                              clickedFace == Direction::South ||
                              clickedFace == Direction::West ||
                              clickedFace == Direction::East;
  if (horizontalFace && sneaking) {
    auto nf = candidateChestPartnerFacing(world, pos, opposite(clickedFace),
                                          base);
    if (nf &&
        (*nf == Direction::North || *nf == Direction::South) !=
            (clickedFace == Direction::North ||
             clickedFace == Direction::South)) {
      facing = *nf;
      type = (horizontalCounterClockWise(facing) == opposite(clickedFace))
                 ? "right"
                 : "left";
    }
  }
  if (type == "single" && !sneaking) {
    if (candidateChestPartnerFacing(world, pos, horizontalClockWise(facing),
                                    base) == facing) {
      type = "left";
    } else if (candidateChestPartnerFacing(
                   world, pos, horizontalCounterClockWise(facing), base) ==
               facing) {
      type = "right";
    }
  }
  return base.with("facing", directionName(facing)).with("type", type);
}

BlockState barrelPlacementState(const BlockState &base,
                                Direction primaryLookDir) {
  return base.with("facing", directionName(opposite(primaryLookDir)));
}

BlockState dispenserPlacementState(const BlockState &base,
                                   Direction primaryLookDir) {
  return base.with("facing", directionName(opposite(primaryLookDir)));
}

BlockState hopperPlacementState(World &, Pos, const BlockState &base,
                                Direction clickedFace) {
  Direction placeFacing = opposite(clickedFace);
  if (placeFacing == Direction::Up)
    placeFacing = Direction::Down;
  return base.with("facing", directionName(placeFacing))
      .with("enabled", "true");
}

// ---------------------------------------------------------------------------
// BlockBehaviour Callbacks
// ---------------------------------------------------------------------------
void chestOnPlace(World &world, Pos pos, const BlockState &state,
                  const BlockState &, bool) {
  ensureContainerBlockEntity(world, pos, state);
}

void chestAffectRemoval(World &world, Pos pos, const BlockState &state,
                        bool) {
  world.updateNeighbourForOutputSignal(pos, state.block());
}

BlockState chestUpdateShape(World &world, Pos pos, const BlockState &state,
                            Direction direction, Pos,
                            const BlockState &neighbor) {
  if (waterloggedOf(state))
    world.scheduleFluidTick(pos, Fluids::water(), 5);
  const bool isHorizontal =
      direction == Direction::North || direction == Direction::South ||
      direction == Direction::West || direction == Direction::East;
  if (canChestsConnect(state, neighbor) && isHorizontal) {
    const std::string_view neighbourType = neighbor.getView("type", "single");
    if (state.getView("type", "single") == "single" &&
        neighbourType != "single" &&
        state.getView("facing") == neighbor.getView("facing") &&
        chestConnectedDirection(neighbor) == opposite(direction)) {
      return state.with("type", neighbourType == "left" ? "right" : "left");
    }
  } else if (state.getView("type", "single") != "single" &&
             chestConnectedDirection(state) == direction) {
    return state.with("type", "single");
  }
  return state;
}

void barrelOnPlace(World &world, Pos pos, const BlockState &state,
                   const BlockState &, bool) {
  ensureContainerBlockEntity(world, pos, state);
}

void barrelAffectRemoval(World &world, Pos pos, const BlockState &state,
                         bool) {
  world.updateNeighbourForOutputSignal(pos, state.block());
}

void barrelTick(World &world, Pos pos, const BlockState &state) {
  auto entity = world.containerEntity(pos);
  if (entity && state.is(Blocks::BARREL)) {
    const bool open = entity->openCount > 0;
    if (openOf(state) != open)
      world.setBlock(pos, state.with(PropertyKind::Open, open ? 1 : 0), 3);
  }
}

void dispenserOnPlace(World &world, Pos pos, const BlockState &state,
                      const BlockState &, bool) {
  ensureContainerBlockEntity(world, pos, state);
}

void dispenserAffectRemoval(World &world, Pos pos, const BlockState &state,
                            bool) {
  world.updateNeighbourForOutputSignal(pos, state.block());
}

void dispenserNeighborChanged(World &world, Pos pos, const BlockState &state,
                              const BlockRef &, bool) {
  const bool powered = world.hasNeighborSignal(pos) ||
                       world.hasNeighborSignal(pos.relative(Direction::Up));
  const bool triggered = state.getView("triggered", "false") == "true";
  if (powered && !triggered) {
    world.scheduleTick(pos, state.block(), 4);
    world.setBlock(pos, state.with("triggered", "true"), 2);
  } else if (!powered && triggered) {
    world.setBlock(pos, state.with("triggered", "false"), 2);
  }
}

namespace {
void executeDispenseItem(World &world, Pos pos, const BlockState &state,
                         DispenserBlockEntity &entity, int slot) {
  const Direction facing = directionFromString(state.getView("facing", "north"));
  const Pos target = pos.relative(facing);
  ItemStack stack = entity.getItem(slot);
  if (stack.empty())
    return;

  if (stack.id == "minecraft:tnt") {
    if (world.tntExplodes) {
      world.spawnPrimedTnt(
          {target.x + 0.5, static_cast<double>(target.y), target.z + 0.5});
      --stack.count;
      entity.setItem(slot, stack);
      entity.setChanged(world);
      world.log.push_back({"dispense_tnt", pos, "minecraft:tnt"});
    }
    return;
  }

  if (stack.id == "minecraft:water_bucket" ||
      stack.id == "minecraft:lava_bucket") {
    const bool isWater = stack.id == "minecraft:water_bucket";
    if (tryPlaceFluidFromBucket(world, target, isWater)) {
      const ItemStack updated = consumeDispenseRemainder(
          world, pos, facing, entity, stack, itemStack("minecraft:bucket", 1));
      entity.setItem(slot, updated);
      entity.setChanged(world);
      world.log.push_back({"dispense_bucket", pos, stack.id});
      return;
    }
  }

  if (stack.id == "minecraft:bucket") {
    if (auto picked = tryPickupFluidWithBucket(world, target)) {
      const ItemStack updated = consumeDispenseRemainder(
          world, pos, facing, entity, stack, itemStack(*picked, 1));
      entity.setItem(slot, updated);
      entity.setChanged(world);
      world.log.push_back({"dispense_pickup", pos, *picked});
      return;
    }
  }

  if (stack.id == "minecraft:flint_and_steel") {
    const BlockState targetState = world.getBlock(target);
    if (targetState.is(Blocks::TNT)) {
      if (world.primeTntBlock(target)) {
        // TODO: decrement ItemStack durability when item durability/components
        // exist.
        world.log.push_back({"dispense_ignite_tnt", pos, "minecraft:tnt"});
      }
    }
    return;
  }

  if (stack.id == "minecraft:minecart") {
    const BlockState front = world.getBlock(target);
    double yOffset = 0.0;
    bool placeCart = false;
    if (isRailBlock(front)) {
      yOffset = railShapeIsSlope(railShapeOf(front)) ? 0.6 : 0.1;
      placeCart = true;
    } else if (isAirState(front)) {
      const BlockState below = world.getBlock(target.relative(Direction::Down));
      if (isRailBlock(below)) {
        yOffset =
            (facing != Direction::Down && railShapeIsSlope(railShapeOf(below)))
                ? -0.4
                : -0.9;
        placeCart = true;
      }
    }
    if (placeCart) {
      const auto step = [&](int &x, int &y, int &z) {
        switch (facing) {
        case Direction::Down:
          y -= 1;
          break;
        case Direction::Up:
          y += 1;
          break;
        case Direction::North:
          z -= 1;
          break;
        case Direction::South:
          z += 1;
          break;
        case Direction::West:
          x -= 1;
          break;
        case Direction::East:
          x += 1;
          break;
        }
      };
      int sx = 0, sy = 0, sz = 0;
      step(sx, sy, sz);
      world.spawnMinecart({pos.x + 0.5 + sx * 1.125,
                           std::floor(pos.y + 0.5) + sy + yOffset,
                           pos.z + 0.5 + sz * 1.125});
      --stack.count;
      entity.setItem(slot, stack);
      entity.setChanged(world);
      world.log.push_back({"dispense_minecart", pos, "minecraft:minecart"});
      return;
    }
  }

  // TODO: remaining special DispenseItemBehavior entries (arrows, fire_charge,
  // bone_meal, shears, spawn_eggs, boats, shulker_boxes, armor, potions).
  ItemStack split = itemStack(stack.id, 1);
  --stack.count;
  entity.setItem(slot, stack);
  entity.setChanged(world);
  spawnDefaultDispensedItem(world, pos, facing, split);
}
} // namespace

void dispenserTick(World &world, Pos pos, const BlockState &state) {
  auto entity = dispenserEntityAt(world, pos);
  if (!entity)
    return;
  const int slot = entity->getRandomSlot(world);
  if (slot < 0) {
    world.log.push_back({"dispense_fail", pos, ""});
    return;
  }
  executeDispenseItem(world, pos, state, *entity, slot);
}

void dropperTick(World &world, Pos pos, const BlockState &state) {
  auto entity = dispenserEntityAt(world, pos);
  if (!entity)
    return;
  const int slot = entity->getRandomSlot(world);
  if (slot < 0) {
    world.log.push_back({"drop_fail", pos, ""});
    return;
  }
  const ItemStack &src = entity->getItem(slot);
  if (src.empty())
    return;
  const Direction facing = directionFromString(state.getView("facing", "north"));
  const Pos targetPos = pos.relative(facing);
  ResolvedContainerView into = resolveContainerView(world, targetPos, true);
  if (into.valid()) {
    ItemStack single = itemStack(src.id, 1);
    ItemStack remainder =
        addItemToResolved(world, entity.get(), into, single, opposite(facing));
    if (remainder.empty()) {
      ItemStack copy = src;
      --copy.count;
      entity->setItem(slot, copy);
      entity->setChanged(world);
      world.log.push_back({"drop_insert", pos, single.id});
    }
    return;
  }
  // No container ahead: default drop into world.
  ItemStack copy = src;
  ItemStack single = itemStack(copy.id, 1);
  --copy.count;
  entity->setItem(slot, copy);
  entity->setChanged(world);
  spawnDefaultDispensedItem(world, pos, facing, single);
}

void hopperOnPlace(World &world, Pos pos, const BlockState &state,
                   const BlockState &oldState, bool) {
  ensureContainerBlockEntity(world, pos, state);
  if (!oldState.sameBlock(state))
    updateHopperEnabledState(world, pos, state);
}

void hopperAffectRemoval(World &world, Pos pos, const BlockState &state, bool) {
  world.updateNeighbourForOutputSignal(pos, state.block());
}

void hopperNeighborChanged(World &world, Pos pos, const BlockState &state,
                           const BlockRef &, bool) {
  updateHopperEnabledState(world, pos, state);
  if (auto hopper = hopperEntityAt(world, pos))
    hopper->wake(world);
}

void hopperEntityInside(World &world, Pos pos, const BlockState &state,
                        Entity &entity) {
  auto *itemEntity = dynamic_cast<ItemEntity *>(&entity);
  if (!itemEntity || itemEntity->item.empty())
    return;
  auto hopper = hopperEntityAt(world, pos);
  if (!hopper)
    return;
  if (itemEntity->boundingBox().intersects(hopperSuckAabbWorld(pos))) {
    hopper->wake(world);
    hopperTryMoveItems(world, pos, state, *hopper, [&] {
      return addItemFromWorldEntity(world, *hopper, *itemEntity);
    });
  }
}

void composterOnPlace(World &world, Pos pos, const BlockState &state,
                      const BlockState &, bool) {
  if (composterLevelOf(state) == 7)
    world.scheduleTick(pos, state.block(), 20);
}

void composterTick(World &world, Pos pos, const BlockState &state) {
  if (composterLevelOf(state) == 7) {
    world.setBlock(pos, state.with("level", "8"), 3);
    world.updateNeighbourForOutputSignal(pos, state.block());
  }
}

bool isCompostableItem(std::string_view itemId) {
  return lookupComposterChance(itemId) > 0.0f;
}

float composterChance(std::string_view itemId) {
  return lookupComposterChance(itemId);
}

// ---------------------------------------------------------------------------
// Typed Accessors
// ---------------------------------------------------------------------------
std::shared_ptr<HopperBlockEntity> hopperEntityAt(const World &world, Pos pos) {
  auto e = world.containerEntity(pos);
  if (e && e->kind == ContainerKind::Hopper)
    return std::static_pointer_cast<HopperBlockEntity>(e);
  return nullptr;
}

std::shared_ptr<DispenserBlockEntity> dispenserEntityAt(const World &world,
                                                        Pos pos) {
  auto e = world.containerEntity(pos);
  if (e && (e->kind == ContainerKind::Dispenser ||
            e->kind == ContainerKind::Dropper))
    return std::static_pointer_cast<DispenserBlockEntity>(e);
  return nullptr;
}

std::shared_ptr<ChestBlockEntity> chestEntityAt(const World &world, Pos pos) {
  auto e = world.containerEntity(pos);
  if (e && (e->kind == ContainerKind::Chest ||
            e->kind == ContainerKind::TrappedChest))
    return std::static_pointer_cast<ChestBlockEntity>(e);
  return nullptr;
}

std::shared_ptr<BarrelBlockEntity> barrelEntityAt(const World &world, Pos pos) {
  auto e = world.containerEntity(pos);
  if (e && e->kind == ContainerKind::Barrel)
    return std::static_pointer_cast<BarrelBlockEntity>(e);
  return nullptr;
}

} // namespace redstone
