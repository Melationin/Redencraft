#pragma once
// 方块体素形状与几何连接更新：提供按状态缓存的碰撞/轮廓 `VoxelShape`、
// 面支撑强度（`SupportType`）判定，以及墙、栅栏、铁栏杆/玻璃板、楼梯的形状更新逻辑。
#include "redstone/engine.hpp"
#include <vector>
namespace redstone {
// 对应原版 `SupportType`：分别检查方块面是否覆盖整面（`Full`）、
// 中心 2x2 像素（`Center`，如火把）或外圈 2 像素框（`Rigid`，如铁轨支撑）。
enum class SupportType { Full, Center, Rigid };
// 静态方块状态的碰撞形状不可变，因此按 `BlockState::Id` 懒构造并长期缓存：
// 返回的引用在进程生命周期内始终有效（对齐原版 `BlockStateBase`），避免热路径反复分配 `VoxelShape`。
const VoxelShape &collisionShape(const BlockState &);
// 获取方块轮廓形状（部分方块如红石线、按钮无碰撞体但仍有可交互轮廓）。
VoxelShape outlineShape(const BlockState &);
// 位置敏感的碰撞形状查询：专门处理 `MOVING_PISTON` 等需要读取 `World::pistonEntities`
// 实时推进进度与 `pistonNoClip` 标志的动态形状。
VoxelShape collisionShape(World &, Pos, const BlockState &);
// 判定静态方块状态指定朝向的面是否满足 `SupportType` 支撑要求。
bool faceSturdy(const BlockState &, Direction,
                SupportType = SupportType::Full);
// 位置敏感的面支撑判定：当目标为 `MOVING_PISTON` 时会先构造其当前帧的动态碰撞形状。
bool faceSturdy(World &, Pos, const BlockState &, Direction,
                SupportType = SupportType::Full);
// 判定失活珊瑚扇/墙扇能否附着在相邻支撑面上。
bool coralCanSurvive(World &, Pos, const BlockState &);
// 判定是否为铁栏杆、铜栏杆或玻璃板类薄片连接方块。
bool isIronBarsBlock(const BlockState &);
// 对应原版 `Block.isExceptionForConnection`：树叶、屏障、南瓜、西瓜、潜影盒等
// 即使具有完整实体面，也不应与墙、栅栏或铁栏杆相连。
bool isExceptionForConnection(const BlockState &);
// 判定栅栏门在给定水平方向上是否允许与墙/栅栏形成连接（朝向需垂直于连接轴）。
bool fenceGateConnectsToDirection(const BlockState &, Direction);
// 墙、栅栏、铁栏杆/玻璃板与楼梯的形状更新辅助函数。
// 分别提供直接读取世界与传入 `capturedNeighbor` 两种重载，后者用于在 `updateShape`
// 回调中严格复用更新链捕获到的邻居快照，保持与原版一致的更新时序。
BlockState wallUpdateShape(World &, Pos, const BlockState &, Direction);
BlockState wallUpdateShape(World &, Pos, const BlockState &, Direction,
                           const BlockState &capturedNeighbor);
BlockState fenceUpdateShape(World &, Pos, const BlockState &, Direction);
BlockState fenceUpdateShape(World &, Pos, const BlockState &, Direction,
                            const BlockState &capturedNeighbor);
BlockState ironBarsUpdateShape(World &, Pos, const BlockState &, Direction);
BlockState ironBarsUpdateShape(World &, Pos, const BlockState &, Direction,
                               const BlockState &capturedNeighbor);
BlockState stairUpdateShape(World &, Pos, const BlockState &);
BlockState stairUpdateShape(World &, Pos, const BlockState &, Direction,
                            const BlockState &capturedNeighbor);
} // namespace redstone
