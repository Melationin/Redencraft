#pragma once
// 容器方块实体系统：定义箱子（含双箱合并）、木桶、发射器、投掷器与漏斗的库存基类、
// 缓存摘要机制、休眠漏斗观察者唤醒、世界生命周期集成以及方块行为回调接口。

#include "redstone/entity.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {

// 容器方块实体类型标识：用于在无 RTTI 开销下区分容器种类、槽位容量及 NBT 类型名。
enum class ContainerKind : std::uint8_t {
  Chest = 0,
  TrappedChest = 1,
  Barrel = 2,
  Dispenser = 3,
  Dropper = 4,
  Hopper = 5,
};

struct HopperBlockEntity;

// 容器方块实体的基础状态（对应原版 RandomizableContainerBlockEntity 子集）。
// 维护类似 Lithium 的 modCount 与惰性槽位摘要缓存，避免未修改的容器在空/满判断或
// 比较器信号查询时反复线性扫描；同时在槽位变动或容器被移除时唤醒已注册的休眠漏斗观察者。
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

// 箱子/陷阱箱方块实体（固定 27 槽）。
struct ChestBlockEntity : ContainerBlockEntity {
  explicit ChestBlockEntity(Pos pos, bool trapped = false)
      : ContainerBlockEntity(trapped ? ContainerKind::TrappedChest
                                     : ContainerKind::Chest,
                             pos, 27) {}
};

// 木桶方块实体（固定 27 槽）。
struct BarrelBlockEntity : ContainerBlockEntity {
  explicit BarrelBlockEntity(Pos pos)
      : ContainerBlockEntity(ContainerKind::Barrel, pos, 27) {}
};

// 发射器/投掷器方块实体（固定 9 槽）。
struct DispenserBlockEntity : ContainerBlockEntity {
  explicit DispenserBlockEntity(Pos pos, bool dropper = false)
      : ContainerBlockEntity(dropper ? ContainerKind::Dropper
                                     : ContainerKind::Dispenser,
                             pos, 9) {}
  // 对应原版 DispenserBlockEntity.getRandomSlot：使用 World::random 进行蓄水池抽样选取非空槽。
  int getRandomSlot(World &world) const;
  // 对应原版 DispenserBlockEntity.insertItem：优先合并进已有堆叠，其次放入首个空槽；返回剩余物品。
  ItemStack insertItem(World &world, const ItemStack &stack);
};

// 漏斗方块实体（固定 5 槽）：继承 enable_shared_from_this 以便向相邻容器注册 weak_ptr 观察者，
// 在无物品可传输时进入休眠，仅当邻近库存变化时被精确唤醒以减少逐刻轮询开销。
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

// 容器方块分类与 NBT 实体类型标识辅助函数。
bool isAnyChestBlock(const BlockState &state);
bool canChestsConnect(const BlockState &a, const BlockState &b);
std::optional<ContainerKind> containerKindForState(const BlockState &state);
std::string_view containerEntityTypeId(ContainerKind kind);
bool isContainerEntityTypeId(std::string_view id);
std::shared_ptr<ContainerBlockEntity>
makeContainerEntityForState(const BlockState &state, Pos pos);

// 世界生命周期集成接口：由 World::setBlock、World::tick 与 World::unloadChunk 调用。
void ensureContainerBlockEntity(World &world, Pos pos, const BlockState &state);
void preRemoveContainerSideEffects(World &world, Pos pos,
                                   const BlockState &oldState, bool dropItems);
void dropContainerContents(World &world, Pos pos, ContainerBlockEntity &entity);
void tickContainerBlockEntities(World &world);

// 容器方块状态查询与界面辅助函数（支持大箱子双端合并逻辑）。
int containerComparatorSignal(World &world, Pos pos, const BlockState &state);
int trappedChestSignal(const World &world, Pos pos);
void containerUpdateOpenCount(World &world, Pos pos, const BlockState &state,
                              int delta);

// 大箱子连接方向判定及各类容器的放置方块状态计算辅助函数。
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

// 在 bindBlockBehaviours() 中绑定的原版 BlockBehaviour 回调。
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

// 供存档序列化与单元测试直接获取强类型容器方块实体的辅助函数。
std::shared_ptr<HopperBlockEntity> hopperEntityAt(const World &world, Pos pos);
std::shared_ptr<DispenserBlockEntity> dispenserEntityAt(const World &world,
                                                        Pos pos);
std::shared_ptr<ChestBlockEntity> chestEntityAt(const World &world, Pos pos);
std::shared_ptr<BarrelBlockEntity> barrelEntityAt(const World &world, Pos pos);

// 统一容器库存视图句柄：将单容器与双箱（CompoundContainer）封装为统一接口，供 GUI 与漏斗访问。
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
