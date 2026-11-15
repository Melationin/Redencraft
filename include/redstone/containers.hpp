#pragma once

#include "redstone/entity.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {

enum class ContainerKind : std::uint8_t {
  Chest = 0,
  TrappedChest = 1,
  Barrel = 2,
  Dispenser = 3,
  Dropper = 4,
  Hopper = 5,
};

struct HopperBlockEntity;

// Base state for container block entities (RandomizableContainerBlockEntity
// subset). Maintains a Lithium-style modCount and lazy slot summary so
// empty/full/signal checks avoid linear scans on unchanged inventories, and
// notifies registered sleeping hopper watchers whenever slots mutate or break.
struct ContainerBlockEntity {
  explicit ContainerBlockEntity(ContainerKind kind, Pos pos, int slotCount);
  virtual ~ContainerBlockEntity() = default;

  ContainerKind kind;
  Pos pos;
  std::vector<ItemStack> items;
  std::uint64_t modCount = 1;
  int openCount = 0;
  bool removed = false;
  std::vector<std::weak_ptr<HopperBlockEntity>> watchers;

  int containerSize() const { return static_cast<int>(items.size()); }
  const ItemStack &getItem(int slot) const;
  void setItem(int slot, const ItemStack &stack);
  ItemStack removeItem(int slot, int amount);
  ItemStack removeItemNoUpdate(int slot);
  bool isEmpty() const;
  bool isInventoryFull() const;
  int occupiedSlotCount() const;
  int fullSlotCount() const;
  int singleContainerComparatorSignal() const;
  bool canPlaceItem(int slot, const ItemStack &stack) const;
  void setChanged(World &world);
  void addWatcher(const std::shared_ptr<HopperBlockEntity> &hopper);
  void wakeWatchers(World &world);

protected:
  void markMutated();
  void refreshSummary() const;

private:
  mutable std::uint64_t cachedSummaryMod_ = 0;
  mutable bool cachedEmpty_ = true;
  mutable bool cachedFull_ = false;
  mutable int cachedOccupied_ = 0;
  mutable int cachedFullCount_ = 0;
  mutable int cachedSignal_ = 0;
};

struct ChestBlockEntity : ContainerBlockEntity {
  explicit ChestBlockEntity(Pos pos, bool trapped = false)
      : ContainerBlockEntity(trapped ? ContainerKind::TrappedChest
                                     : ContainerKind::Chest,
                             pos, 27) {}
};

struct BarrelBlockEntity : ContainerBlockEntity {
  explicit BarrelBlockEntity(Pos pos)
      : ContainerBlockEntity(ContainerKind::Barrel, pos, 27) {}
};

struct DispenserBlockEntity : ContainerBlockEntity {
  explicit DispenserBlockEntity(Pos pos, bool dropper = false)
      : ContainerBlockEntity(dropper ? ContainerKind::Dropper
                                     : ContainerKind::Dispenser,
                             pos, 9) {}
  // DispenserBlockEntity.getRandomSlot (reservoir sampling with World::random).
  int getRandomSlot(World &world) const;
  // DispenserBlockEntity.insertItem: merges into existing stacks first, then
  // first empty slot; returns leftover stack.
  ItemStack insertItem(World &world, const ItemStack &stack);
};

struct HopperBlockEntity : ContainerBlockEntity,
                           std::enable_shared_from_this<HopperBlockEntity> {
  explicit HopperBlockEntity(Pos pos);

  int cooldownTime = -1;
  std::int64_t tickedGameTime = 0;
  Direction facing = Direction::Down;

  void setCooldown(World *world, int time);
  bool isOnCooldown() const { return cooldownTime > 0; }
  bool isOnCustomCooldown() const { return cooldownTime > 8; }
  void wake(World &world);
  void sleepIndefinite(World &world);
  void sleepOnlyCurrentTick(World &world);
};

// Container block classification & NBT identity helpers.
bool isAnyChestBlock(const BlockState &state);
bool canChestsConnect(const BlockState &a, const BlockState &b);
std::optional<ContainerKind> containerKindForState(const BlockState &state);
std::string_view containerEntityTypeId(ContainerKind kind);
bool isContainerEntityTypeId(std::string_view id);
std::shared_ptr<ContainerBlockEntity>
makeContainerEntityForState(const BlockState &state, Pos pos);

// World lifecycle integration called from World::setBlock, World::tick, and
// World::unloadChunk.
void ensureContainerBlockEntity(World &world, Pos pos, const BlockState &state);
void preRemoveContainerSideEffects(World &world, Pos pos,
                                   const BlockState &oldState, bool dropItems);
void dropContainerContents(World &world, Pos pos, ContainerBlockEntity &entity);
void tickContainerBlockEntities(World &world);

// Container block query & UI helpers (chest double-block combiner aware).
int containerComparatorSignal(World &world, Pos pos, const BlockState &state);
int trappedChestSignal(const World &world, Pos pos);
void containerUpdateOpenCount(World &world, Pos pos, const BlockState &state,
                              int delta);

// Double-chest connection & placement helpers.
Direction chestConnectedDirection(const BlockState &state);
Pos chestConnectedBlockPos(Pos pos, const BlockState &state);
BlockState chestPlacementState(World &world, Pos pos, const BlockState &base,
                               Direction playerHorizontal, Direction clickedFace,
                               bool sneaking);
BlockState barrelPlacementState(const BlockState &base,
                                Direction primaryLookDir);
BlockState dispenserPlacementState(const BlockState &base,
                                   Direction primaryLookDir);
BlockState hopperPlacementState(World &world, Pos pos, const BlockState &base,
                                Direction clickedFace);

// BlockBehaviour callbacks bound in bindBlockBehaviours().
void chestOnPlace(World &world, Pos pos, const BlockState &state,
                  const BlockState &oldState, bool movedByPiston);
void chestAffectRemoval(World &world, Pos pos, const BlockState &state,
                        bool movedByPiston);
BlockState chestUpdateShape(World &world, Pos pos, const BlockState &state,
                            Direction direction, Pos neighborPos,
                            const BlockState &neighbor);

void barrelOnPlace(World &world, Pos pos, const BlockState &state,
                   const BlockState &oldState, bool movedByPiston);
void barrelAffectRemoval(World &world, Pos pos, const BlockState &state,
                         bool movedByPiston);
void barrelTick(World &world, Pos pos, const BlockState &state);

void dispenserOnPlace(World &world, Pos pos, const BlockState &state,
                      const BlockState &oldState, bool movedByPiston);
void dispenserAffectRemoval(World &world, Pos pos, const BlockState &state,
                            bool movedByPiston);
void dispenserNeighborChanged(World &world, Pos pos, const BlockState &state,
                              const BlockRef &source, bool movedByPiston);
void dispenserTick(World &world, Pos pos, const BlockState &state);
void dropperTick(World &world, Pos pos, const BlockState &state);

void hopperOnPlace(World &world, Pos pos, const BlockState &state,
                   const BlockState &oldState, bool movedByPiston);
void hopperAffectRemoval(World &world, Pos pos, const BlockState &state,
                         bool movedByPiston);
void hopperNeighborChanged(World &world, Pos pos, const BlockState &state,
                           const BlockRef &source, bool movedByPiston);
void hopperEntityInside(World &world, Pos pos, const BlockState &state,
                        Entity &entity);

void composterOnPlace(World &world, Pos pos, const BlockState &state,
                      const BlockState &oldState, bool movedByPiston);
void composterTick(World &world, Pos pos, const BlockState &state);
bool isCompostableItem(std::string_view itemId);
float composterChance(std::string_view itemId);

// Utility used by storage / tests to access typed container entities.
std::shared_ptr<HopperBlockEntity> hopperEntityAt(const World &world, Pos pos);
std::shared_ptr<DispenserBlockEntity> dispenserEntityAt(const World &world,
                                                        Pos pos);
std::shared_ptr<ChestBlockEntity> chestEntityAt(const World &world, Pos pos);
std::shared_ptr<BarrelBlockEntity> barrelEntityAt(const World &world, Pos pos);

// Unified container inventory handle covering single containers and
// CompoundContainer double chests for GUI / Hopper access.
struct ContainerSlotRef {
  std::shared_ptr<ContainerBlockEntity> entity;
  int localSlot = 0;
};
struct ResolvedContainerView {
  std::shared_ptr<ContainerBlockEntity> first;
  std::shared_ptr<ContainerBlockEntity> second;
  int size() const {
    return (first ? first->containerSize() : 0) +
           (second ? second->containerSize() : 0);
  }
  bool valid() const { return first != nullptr; }
  const ItemStack &getItem(int index) const;
  void setItem(World &world, int index, const ItemStack &stack);
  void setChanged(World &world);
  std::vector<Pos> positions() const;
};
ResolvedContainerView resolveContainerView(World &world, Pos pos,
                                           bool ignoreBeingBlocked);

} // namespace redstone
