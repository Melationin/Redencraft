#pragma once
// 铁轨方块系统：提供 `RailShape` 形状辅助查询、矿车沿轨寻路出口计算，
// 以及普通/动力/探测/激活铁轨的放置、信号与交互回调。
#include "redstone/engine.hpp"
#include <string>

namespace redstone {
// `net.minecraft.world.level.block.state.properties.RailShape` 定义在 `property.hpp` 中
// （由 `block.hpp` 引入以参与规范属性位域编码），此处直接复用以避免循环依赖。
RailShape railShapeFromName(std::string_view name);
std::string railShapeName(RailShape shape);
// 判断铁轨形状是否为四个爬坡状态之一。
bool railShapeIsSlope(RailShape shape);
RailShape railShapeOf(const BlockState &state);
// 判断是否为仅限直轨/斜坡的铁轨类型（即动力、探测、激活铁轨，不包含可转弯的普通铁轨）。
bool railIsStraight(const BlockState &state);

// 对应原版 `BaseRailBlock` / `PoweredRailBlock` / `DetectorRailBlock` / `RailBlock` 回调：
// 分别负责铁轨放置连接、邻接红石/方块变化、移除后邻居刷新与探测铁轨计划刻。
void railOnPlace(World &world, Pos pos, const BlockState &state,
                 const BlockState &oldState, bool movedByPiston);
void railNeighborChanged(World &world, Pos pos, const BlockState &state,
                         const BlockRef &sourceBlock,
                         bool movedByPiston = false);
void railAffectNeighborsAfterRemoval(World &world, Pos pos,
                                     const BlockState &state,
                                     bool movedByPiston);
void railScheduledTick(World &world, Pos pos, const BlockState &state);
// 读取探测铁轨输出的红石信号强度（激活时 15，否则 0）。
int railSignal(const BlockState &state);
// 检查当前位置下方方块是否满足铁轨的 `SupportType::Rigid` 刚性支撑要求。
bool railCanSurvive(World &world, Pos pos, const BlockState &state);
// 根据玩家水平朝向与周围已有铁轨计算放置时的初始连接形状。
BlockState railPlacementState(World &world, Pos pos, Direction horizontalFacing);
// 返回给定 `RailShape` 的两个连通出口相对坐标偏移，供矿车沿轨移动与出轨判定使用。
std::pair<Pos, Pos> railExits(RailShape shape);
// 矿车等实体进入铁轨格时的触发回调（用于激活探测铁轨）。
void railEntityInside(World &world, Pos pos, const BlockState &state,
                      Entity &entity);
} // namespace redstone
