#pragma once
// 流体流动模拟与含水方块交互逻辑，对齐原版 `FlowingFluid.java` 与 `LiquidBlock.java`。
// 所有读写均经由 `World` 的标准方块级接口进行，确保流体刻遵守常规的邻接更新与计划刻链。
#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"

// 对应 `net/minecraft/world/level/material/FlowingFluid.java`。
//
// 流体流动模拟层：所有世界读写都复用核心引擎的方块接口，使流体更新作为正常计划刻运行，
// 不会绕过邻居通知、形状更新或状态变更观测机制。
namespace redstone {

// 对应 `FlowingFluid.getHeight`：正上方存在同族流体时液面升至整格高度（8/9 或 1），
// 否则返回流体自身高度（`amount / 9`）。
float fluidHeight(World &world, Pos pos, const FluidState &state);

// 计算 `FlowingFluid.getFlow` 的流动向量（`y` 分量恒为 0）。`state` 为 `pos` 处的当前流体状态。
Vec3 fluidFlow(World &world, Pos pos, const FluidState &state);

// 对应 `FlowingFluid.tick`：先重算非源头流体的目标液位并更新方块，随后向外扩散。
void fluidTick(World &world, Pos pos, const BlockState &blockState,
               const FluidState &state);

// 对应 `FlowingFluid.spread`：单独暴露以便测试按单步精确驱动流体扩散。
void fluidSpread(World &world, Pos pos, const BlockState &blockState,
                 const FluidState &state);

// 对应 `FlowingFluid.getNewLiquid`：结合水平邻居与上方方块计算当前位置应有的目标流体状态。
FluidState fluidNewLiquid(World &world, Pos pos, const BlockState &blockState,
                          const Fluid &self);

// 对应 `LiquidBlock.onPlace` 与 `neighborChanged`：先执行 `shouldSpreadLiquid`（水/熔岩接触转换），
// 若液体未被替换则调度一次流体计划刻。由于原版分别在不同生命周期回调中调用，因此保留为两个独立入口；
// 两者均严格按传入快照 `state` 所属的流体入队（与 `LiquidBlock.java:148-151` 和 `194-198` 一致，不重新读取世界）。
void liquidOnPlace(World &world, Pos pos, const BlockState &state);
void liquidNeighborChanged(World &world, Pos pos, const BlockState &state);

// 对应 `LiquidBlock.updateShape`：仅当自身或发生变化的邻居为流体源时才请求流体计划刻。
void liquidUpdateShape(World &world, Pos pos, const BlockState &state,
                       const BlockState &neighbor);

// 对应 `SimpleWaterloggedBlock`：判定方块是否为处于含水（`waterlogged=true`）状态的宿主方块。
bool isWaterloggedHost(const BlockState &state);

// 对应 `LiquidBlockContainer.canPlaceLiquid`：仅精确的水源（`Fluids.WATER`）可注入可含水宿主
// （`SimpleWaterloggedBlock.java:20-22` 严格比较 `type == Fluids.WATER`，拒绝流动水及两种熔岩）。
bool canPlaceLiquid(const BlockState &state, const Fluid &type);

// 对应 `LiquidBlockContainer.placeLiquid`：将干燥的含水宿主以 `flags=3` 设为 `waterlogged=true`
// 并调度其自身的水流体刻。若宿主已含水或流体非精确水源，则保持方块不变并返回 false
// （对应 `SimpleWaterloggedBlock.java:24-36`）。
bool placeLiquid(World &world, Pos pos, const BlockState &state,
                 const FluidState &fluidState);

// 对应 `FlowingFluid.getSpreadDelay`：计算下次重调度的流体刻延迟，
// 包含 `LavaFluid` 在液面上升且未处于下落态时延迟乘以 4 的原版特殊逻辑。
int fluidSpreadDelay(World &world, Pos pos, const Fluid &self,
                     const FluidState &oldState, const FluidState &newState);

// 对应 `LiquidBlock.shouldSpreadLiquid`：处理熔岩遇到水或“灵魂土+蓝冰”时的原地凝结转换
// （生成黑曜石、圆石或玄武岩），若发生转换则返回 false 通知调用方停止继续扩散。
bool liquidShouldSpreadLiquid(World &world, Pos pos, const BlockState &state);

// 对应 `Shapes.mergedFaceOccludes` 对一对已解析碰撞箱的共享面遮挡判定。
// 当前注册表中尚无两个非完整方块形状互补（仅有栅栏门横杆与墙柱，均无法共同封死整面），
// 因此测试路径使用合成半砖对来验证原版合并遮挡语义。此函数为纯只读查询，不会写方块或调度计划刻。
bool fluidShapesSealSharedFace(const VoxelShape &source,
                               const VoxelShape &target, Direction direction);

// 对应 `FlowingFluid.canPassThroughWall`：判定流体能否沿 `direction` 从 `sourcePos` 穿过贴合面流入 `targetPos`。
bool fluidCanPassThroughWall(World &world, Direction direction, Pos sourcePos,
                             const BlockState &sourceState, Pos targetPos,
                             const BlockState &targetState);

} // namespace redstone
