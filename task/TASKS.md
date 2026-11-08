# 任务交接

流程与状态定义见 [AGENTS.md](../AGENTS.md) 第 3 节；任务顺序见 [PLAN.md](../PLAN.md)。
本文件只记录**当前任务**。F-1（流体状态模型）与 F-2（流体计划刻队列）已验收，记录在 `53a5d20`…`4756fb0` 的提交内容里。

---

## F-3 `FlowingFluid` 传播

### 目标

实现 `FlowingFluid.java` 的传播算法，让流体刻真正改变水位：`tick`、`spread`、`spreadToSides`、`getNewLiquid`、`getSpread`、`getSlopeDistance`、`canPassThroughWall`、`SpreadContext`、`isWaterHole`、`getHeight`、`getFlow`。以预置方块与显式流体刻隔离算法；方块侧自然更新（`onPlace` / `neighborChanged` / `updateShape`）仍属 F-5。

### 范围

1. `FluidState::getHeight(level, pos)`：正上方同族流体时 `1.0F`，否则 `getOwnHeight()`。
2. 传播参数（`getDropOff` / `getSlopeFindDistance` / `getTickDelay` / `canConvertToSource`）：F-3 需要它们才能运行，先按 `WaterFluid` / `LavaFluid` 的常量落地；**参数差异的专项验证归 F-4**。
3. `canHoldAnyFluid` / `canHoldSpecificFluid` / `canPassThroughWall`（含共享面遮挡判定）。
4. `getNewLiquid`：邻居最高水位、双源转源、上方同族转 falling。
5. `spread` / `spreadToSides` / `getSpread` / `getSlopeDistance` / `SpreadContext`（state 与 hole 缓存）。
6. `tick`：非源先重算液面（空则变空气并 `setBlock`，否则写回新状态并重新排刻），再 `spread`。
7. `getFlow`：实体推挤用的流向向量（本任务只实现并单测；实体接线不在本任务）。

### 源码依据

- `net/minecraft/world/level/material/FlowingFluid.java`：`getFlow`（60-105）、`spread`（123-146）、`spreadToSides`（148-164）、`getNewLiquid`（166-201）、`canPassThroughWall`（203-255）、`getFlowing`/`getSource`（257-267）、`spreadTo`（271-281）、`getSlopeDistance`（285-311）、`isWaterHole`（313-319）、`canPassThrough`（321-347）、`sourceNeighborCount`（355-367）、`getSpread`（369-408）、`canHoldAnyFluid`（410-427）、`canHoldSpecificFluid`（433-435）、`getSpreadDelay`（439-441）、`tick`（443-461）、`getLegacyLevel`（463-465）、`hasSameAbove`（467-469）、`getHeight`（471-474）、`getOwnHeight`（476-479）、`SpreadContext`（503-540）。
- `net/minecraft/world/level/material/WaterFluid.java` / `LavaFluid.java`：`canBeReplacedWith`、`getDropOff`、`getSlopeFindDistance`、`getTickDelay`、`canConvertToSource`。
- `net/minecraft/world/level/block/state/BlockBehaviour.java`：`blocksMotion`（540-543）、`isSolid`（546-548）。
- `net/minecraft/world/level/material/FluidState.java`：`canBeReplacedWith`（117-119）。

### 验收判据

1. **水平扩散**：源水在平地上按 `8-level` 逐格衰减，第 7 格后停止。
2. **退流**：移除源后，流动水按 `getNewLiquid` 逐刻回退，最终变空气。
3. **瀑布**：源水向下流时下方为 falling 满态；落地后在平面扩散。
4. **源转换**：两个相邻源水夹一格时该格转源（水 `canConvertToSource=true`）；岩浆默认不转源。
5. **坡度优先**：`getSpread` 只保留距离最小的方向，同距保留多个。
6. **水位重算**：流动水取水平邻居最高水位减 dropOff；上方同族时取 falling 满态。
7. **`getHeight`**：正上方同族为 `1.0`，否则 `ownHeight`。
8. **`getFlow`**：两侧水位差产生水平分量；falling 且前方有阻挡时附加 `-6.0` 的 Y 分量后归一化。
9. **无副作用边界**：`getFlow` / `getHeight` 不写方块、不排刻。
10. 构建通过、CTest 全绿（含新增 `fluid_flow` 组）。

### 计划改动

- 新增：`include/redstone/fluid_flow.hpp`、`src/fluid_flow.cpp`
- 修改：`src/engine.cpp`（`tickFluidAt` 接上 `fluidTick`）
- 修改：`include/redstone/fluid.hpp`（参数与 `getHeight` 声明）
- 新增：`tests/fluid_flow_tests.cpp`
- 修改：`CMakeLists.txt`、`docs/compatibility.md`

### 实际做了什么

（待填）

### 构建与测试

（待填）

### 提交

（待填）

任务进行中
