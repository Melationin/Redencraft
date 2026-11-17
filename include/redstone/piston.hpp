#pragma once
// 活塞推进与移动方块实体系统：定义 36 号方块对应的 `PistonMovingBlockEntity`、
// 遵循 12 格上限的 `PistonStructureResolver` 结构解析器，以及活塞事件与实体推动入口。
#include "redstone/engine.hpp"
#include <memory>
#include <span>
#include <vector>

namespace redstone {
// 移动中的活塞方块实体（对应原版 `PistonMovingBlockEntity` / 36 号方块状态）。
// 用于在 2 个游戏刻的推进动画期间暂存被推拉方块的原状态、推进方向与当前/上一刻插值进度，
// 供动态碰撞箱计算、方块完成落位以及客户端渲染插值使用。
struct PistonMovingBlockEntity {
  Pos pos;
  BlockState movedState;
  Direction direction;
  bool extending;
  bool source;
  float progress = 0, progressO = 0;
  long long lastTicked = -1;
  // 对应原版 `BlockEntity.removed`：当本实体在原地被替换或从 `World.pistonEntities`
  // 移除时置为 `false`。tick 遍历阶段直接检查此标志即可安全跳过失效槽位，
  // 无需在迭代期间再次哈希查找位置表。
  bool live = true;
  // 对应原版 `PistonMovingBlockEntity.getMovementDirection` 与 `getExtendedProgress`。
  Direction movementDirection() const {
    return extending ? direction : opposite(direction);
  }
  float extendedProgress() const {
    return extending ? progress - 1.0f : 1.0f - progress;
  }
};
// 活塞推动/拉回结构解析器（对应原版 `PistonStructureResolver`）：
// 从起始位置出发按推动方向搜集所有待移动方块列表（`toPush()`）与被挤碎方块列表（`toDestroy()`），
// 并在遇到粘液块/蜂蜜块时递归展开侧向粘连分支，同时保证粘液块与蜂蜜块互不粘连。
class PistonStructureResolver {
public:
  PistonStructureResolver(World &, Pos, Direction, bool);
  bool resolve();
  std::span<const Pos> toPush() const { return {push_.data(), pushCount_}; }
  const std::vector<Pos> &toDestroy() const { return destroy_; }

private:
  bool addBlockLine(Pos, Direction);
  bool addBranchingBlocks(Pos);
  World &world_;
  Pos piston_, start_;
  Direction facing_, movement_;
  bool extending_;
  // 对应原版 `PistonStructureResolver.PUSH_LIMIT`（12 格上限）：每次插入待推方块前都会检查，
  // 用定长数组直接表达上限并避免解析热路径上的动态堆分配。
  std::array<Pos, 12> push_;
  std::size_t pushCount_ = 0;
  std::vector<Pos> destroy_;
};
// 判定指定位置的方块能否被活塞朝 `d` 方向推动（含世界高度/边界、黑曜石等不可推方块与 `push_reaction` 检查）。
bool pistonIsPushable(World &, Pos, const BlockState &, Direction, bool,
                      Direction);
// 活塞 `BlockBehaviour` 回调：邻居更新检测激活、放置初始化与方块事件（伸出/缩回）执行。
void pistonNeighborChanged(World &, Pos, const BlockState &);
void pistonOnPlace(World &, Pos, const BlockState &, const BlockState &);
bool pistonBlockEvent(World &, Pos, const BlockState &, int, int);
// 共享的活塞推动实体路径：统一移动实体坐标，并在控制权返回 `World::tick` 之前
// 同步刷新实体区块分片索引（entity-section state）与持久化脏状态观察者，防止位置与索引脱节。
void moveEntityByPiston(World &world, Direction pistonDirection, Entity &entity,
                        double delta, Direction movement);
// 推进所有活跃的 `PistonMovingBlockEntity` 进度，并在到达 1.0 时落位最终方块。
void pistonTickBlockEntities(World &);
// 在移动活塞方块实体被外部破坏/替换前执行清理与最终落位副作用。
void pistonPreRemoveSideEffects(World &, Pos);
// 世界销毁或重置时释放活塞模块内部按世界索引的缓存状态。
void pistonForgetWorld(World &);
} // namespace redstone
