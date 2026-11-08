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

**新增 `include/redstone/fluid_flow.hpp` + `src/fluid_flow.cpp`**，逐方法移植 `FlowingFluid`：

- `fluidHeight`：对应 `hasSameAbove` + `getHeight`，正上方同族时 `1.0F`，否则 `getOwnHeight()`。
- `fluidCanPassThroughWall`：`Shapes.block()` 快速路径、双空通过，其余走 `mergedFaceOccludes`。后者按 1/16 网格逐格判定两面形状的并集是否覆盖整个共享面——本仓库注册的形状（地毯 1/16、墙 6..10、珊瑚扇、栅栏门）都在这个网格上，所以判定是精确的；非网格形状留 TODO。
- `canHoldAnyFluid` / `canHoldSpecificFluid`：`LiquidBlockContainer` 未移植，按「无碰撞即容纳」处理，宿主排水留 F-5。
- `fluidNewLiquid`：水平邻居最高水位、双源转源（仅 `canConvertToSource`）、上方同族转 falling 满态。
- `getSpread` / `getSlopeDistance` / `SpreadContext`：距离 0 表示是洞口、只保留最小距离的方向；`SpreadContext` 复刻原版按 `(dx+128, dz+128)` 打包的 state/hole 缓存。
- `fluidSpread` / `spreadToSides` / `spreadTo`：先试向下，若 `sourceNeighborCount >= 3` 同时向侧面扩散；否则源或非水洞时向侧面扩散。
- `fluidTick`：非源先按 `fluidNewLiquid` 重算——空则变空气、有变化则写回并重新排刻，最后 `spread`。
- `fluidFlow`：`getFlow` 的水平水位差与 falling 分支（`-6.0` 后归一化）。

**参数**：`Fluid` 增加 `dropOff()` / `slopeFindDistance()` / `tickDelay()` / `canConvertToSource()`，`WaterFluid` 为 1/4/5/true，`LavaFluid` 为 2/2/30/false。**这些值的专项验证归 F-4**，本任务只为让算法可运行。

**接线**：`World::tickFluidAt` 的 TODO 换成 `fluidTick(*this, pos, state, fluidState)`。

**新增 `tests/fluid_flow_tests.cpp`**（10 个场景）：水平扩散到第 7 格停止、退流成空气、瀑布 falling 满态、双源转源（水转/岩浆不转）、坡度优先（有洞的方向才扩散）、水位重算（邻居最高减 dropOff、上方同族转 falling、无邻居则空）、`getHeight`、`getFlow` 的水平与 falling 分量、查询无副作用、`fluidTick` 的重新排刻。

**实现过程中修掉的三个问题**：

1. `canBeReplacedWith` 我起初按「扩散方」的流体规则判断，导致水平方向永远不扩散。原版是由**目标格当前的流体**派发（`FluidState.java:117-119`），空格是 `EmptyFluid` 返回 `true`——这才是空气可被替换的来源。已改为按目标流体派发。
2. `fluidFlow` 的 falling 分支照抄时把 `direction == UP` 的判断带进了水平循环，是死分支且漏了真正的 `isSolidFace` 语义；已按原版 `isSolidFace`（同族流体不算、`faceSturdy` 判定）重写。
3. 测试里把流体刻计数写成了 `scheduledTickCount()`（方块队列），应为 `fluidTickCount()`。

**测试驱动方式**：`LiquidBlock.onPlace` / `neighborChanged` 的流体刻请求属于 F-5，因此测试用 `stepFluids` 每轮遍历所有流体方块各调一次 `fluidTick`，不依赖尚未接线的方块回调。

**明确没有做的事**：
- 方块侧自然触发（`onPlace` / `neighborChanged` / `updateShape`）未接线（F-5）。因此从存档或命令放置的流体目前**不会自动流动**，必须显式 `scheduleFluidTick`。
- 参数差异专项验证、fast lava、水岩浆互相转化（黑曜石/圆石/玄武岩）、岩浆点燃与随机延迟（F-4）。
- `LiquidBlockContainer` 的放入/排出、`waterlogged` 派发（F-5）。
- 存档 `fluid_ticks` 与投影 `PendingFluidTicks`（F-6）。
- 没有跑真实 26.1 服务端差分。

### 构建与测试

```powershell
cmake -S . -B build-jemalloc -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static"
cmake --build build-jemalloc --config Release -j 8
ctest --test-dir build-jemalloc -C Release --output-on-failure
```

结果：构建通过；**CTest 18/18 通过**（原 17 组 + 新增 `fluid_flow`）。`fluid_flow` 组 0.03 s。

既有警告（不在本次范围）：`include/redstone/net/outbox.hpp` 的 C4324 对齐填充、`src/litematic.cpp:204` 的 C4456。本次新增/修改文件零警告。

**变异验证**：

| 变异 | 结果 |
|---|---|
| 水 `dropOff` 从 1 改成 0 | `the four neighbours get level 1, got 0` |
| 水 `canConvertToSource` 改成 false | `two neighbouring sources convert the middle to a source` |

两次都被拒绝；之后从备份恢复 `include/redstone/fluid.hpp`，恢复后 `git diff` 只含本次新增的参数访问器。

证据口径：**源码对照 + C++ 回归测试**。算法逐个方法对照 `FlowingFluid.java`；**未跑真实 26.1 服务端差分**，不能声称与原版逐位等价。

### 提交

- 主提交：`f2201fc` — Implement FlowingFluid spread, level recomputation and flow vectors
  （8 个文件，+991 / -10；新增 `include/redstone/fluid_flow.hpp`、`src/fluid_flow.cpp`、`tests/fluid_flow_tests.cpp`，修改 `include/redstone/fluid.hpp`、`src/engine.cpp`、`CMakeLists.txt`、`docs/compatibility.md`）

任务完成
