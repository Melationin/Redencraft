# 任务交接

流程与状态定义见 [AGENTS.md](../AGENTS.md) 第 3 节；任务顺序见 [PLAN.md](../PLAN.md)。
本文件只记录**当前任务**。F-1…F-4 已验收，记录在 `53a5d20`…`8f67143` 的提交内容里。

> **状态词**：`任务进行中` → `任务已完成`（coder）→ `审查中` → `审查结束` / `任务通过`（reviewer）。

---

## F-5 方块侧接线与排水

### 目标

把流体从「能排刻、能传播」接到真实游戏事件上：放置、`neighborChanged`、`updateShape` 三条路径各自请求流体刻，含水宿主能放入与排出流体，墙与枯珊瑚扇补上 `forceSolidOn`。**三条路径分开实现，不合并成统一的邻居刷新**。

### 范围

1. `LiquidBlock.onPlace`：方块变成液体时请求流体刻（`LiquidBlock.java:147-151`）。
2. `LiquidBlock.neighborChanged`：邻居变化时先跑 `shouldSpreadLiquid`（F-4 已实现），存活则请求流体刻。
3. `LiquidBlock.updateShape`：自身或邻居是源时请求流体刻（`LiquidBlock.java:167-187`）。
4. `SimpleWaterloggedBlock`：`waterlogged=true` 的宿主 `getFluidState` 返回水；`placeLiquid` / `pickupLiquid` / `canPlaceLiquid` 语义；含水宿主的 `forceSolidOn`。
5. 补注册墙（`cobblestone_wall`、`stone_brick_wall`）与五种枯珊瑚扇的 `forceSolidOn`（`Blocks.java:2600,4835-4889,4915-4969,5110`）。
6. `canHoldSpecificFluid` 接入 `LiquidBlockContainer`，替换 F-3 的「凡是能容纳就容纳」的占位。

### 源码依据

`LiquidBlock.java`、`SimpleWaterloggedBlock.java`、`BaseRailBlock.java`、`Blocks.java` 的 `forceSolidOn` 用法、`FlowingFluid.java` 的 `canHoldSpecificFluid`。

### 验收判据

1. 放置液体方块后产生一条流体刻请求；`updateShape` 与 `neighborChanged` 各自也能产生，且三条路径的触发条件不同。
2. 含水宿主（铁轨、墙、珊瑚扇）`fluidStateOf` 返回水源；砍掉支撑或破方块时排出并请求流体刻。
3. 墙与枯珊瑚扇 `isSolid` 为真（`forceSolidOn`），可在双源夹格下作支撑。
4. 布局在 `forceSolidOn` 与流体刻交互下保持稳定：水不会替换墙/珊瑚扇。
5. 干态世界（无流体）不因新增请求路径产生额外计划刻。
6. 构建通过、CTest 全绿（含新增 `fluid_block` 组）。

### 计划改动

- 修改：`src/fluid_flow.cpp` / `include/redstone/fluid_flow.hpp`（三条请求路径、`LiquidBlockContainer` 判定）
- 修改：`src/fluid.cpp`（waterlogged 宿主的 `fluidStateOf`）
- 修改：`src/engine.cpp`（`setBlock` 的 onPlace 与 `neighborChanged` / `updateShape` 接线）
- 修改：`src/state.cpp`（墙与枯珊瑚扇 `forceSolidOn`）
- 修改：`include/redstone/block.hpp`（若需要新的 metadata 位）
- 新增：`tests/fluid_block_tests.cpp`
- 修改：`CMakeLists.txt`、`docs/compatibility.md`

### 实际做了什么

（待填）

### 构建与测试

（待填）

### 提交

（待填）

任务进行中
