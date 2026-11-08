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
- 返工提交：`e76938a` — Fix the F-3 review findings: empty ticks, face sides, solidity and drops（4 个文件，+242 / -38）

### 返工（回应审查：4 阻塞）

**阻塞 1 — 空流体刻会删掉普通方块。** 属实：`tickFluidAt` 在身份匹配后就调 `fluidTick`，而石头等非液体方块的 `fluidStateOf` 是 `minecraft:empty`，能与 F-2 允许的空流体刻匹配；随后 `fluidTick` 因「无邻居」把方块写成空气。已修：

- `tickFluidAt` 在期望流体为空或当前位置流体为空时直接返回——`EmptyFluid.tick` 本来就是空实现（`Fluid.java:61-62`），刻被消费但不动方块。
- `fluidHeight` 对空状态返回 `0.0F`（`EmptyFluid.getHeight` 是常量 0），不再因为上下都是空而返回 1。

**阻塞 2 — 共享面取错目标面，且忽略移动活塞。** 属实：

- `mergedFaceOccludes` 现在让**源按 `direction` 取面、目标按 `opposite(direction)` 取面**（`Shapes.java:236-252` 按轴向决定哪个形状当 first）。之前两者都按同一方向取，向下传播时取的是目标的底面。
- `fluidCanPassThroughWall` 改用位置感知的 `collisionShape(world, pos, state)`，移动活塞因此按当前方块实体进度取形状，而不是恒为空的静态缓存（`MovingPistonBlock.java:105-108`）。

**阻塞 3 — 实体性语义与掉落顺序。** 属实：

- 新增 `isSolid`，逐字实现 `BlockBehaviour.calculateSolid`：碰撞盒包围盒的平均边长 ≥ `0.7291666` 或高度 ≥ 1.0。玻璃是满盒 → solid，可作双源转源支撑；地毯 `(1 + 1/16 + 1)/3 = 0.6875` → 非 solid，不阻挡横向水流。`blocksMotion` = `isSolid`（cobweb / bamboo_sapling 未注册）。
- `fluidNewLiquid` 的支撑判定从 `isRedstoneConductor` 改成 `isSolid`——玻璃不是导体但仍是固体。
- `spreadTo` 补上 `WaterFluid.beforeDestroyingBlock`：水在写流体**之前** `blockDrops` + `spawnItem`；岩浆仍是 fizz 的 TODO（`WaterFluid.java:81-85`、`LavaFluid.java:144-146`）。

**阻塞 4 — 用例没验证调度路径，副作用断言读错队列。** 属实：

- 新增 `stepScheduled`，走真实的 `scheduleFluidTick` + `World::tick()` 派发路径。
- 瀑布用例重建几何（y=62 平台），先用一次 `fluidSpread` 断言落下的是 falling 满态，再用 `stepScheduled` 断言**落地后沿平台向东、向南横流**，且平台未被替换。
- `queriesDoNotMutate` 改为同时断言方块队列与流体队列都不变。
- 新增 `emptyFluidTickIsInert`：石头上排 `Fluids::empty()` 刻，断言刻被消费、石头还在；再排一条指向空气的水刻，断言空气不会被写成水；同时断言空状态高度为 0。
- 新增 `soliditySemantics`：玻璃支撑双源转源；地毯被水替换并**在水写入前掉出地毯物品**。

**变异验证（每条独立恢复，恢复后 `git diff` 与预期一致）**：

| 变异 | 结果 |
|---|---|
| `blocksMotion` 退回「碰撞形状非空」 | `a 1/16 carpet does not block the fluid and is replaced by it, got white_carpet` |
| 删掉水冲毁前的 `blockDrops`/`spawnItem` | `the replaced carpet drops its item before the fluid lands` |
| 删掉 `tickFluidAt` 的空流体守卫 | `an empty-fluid tick does not delete the stone` |

**共享面方向变异（源与目标都按同一方向取面）本轮仍未被测试拒绝。** 原因是结构性限制：该变异只会让并集**变小**，因此只能把「遮挡」从真变假；要构造出能把正确实现判真、把变异判假的用例，需要一对**互补的部分形状**（例如源盖住面的一半、目标盖住另一半）。当前注册表里没有任何这样的形状对——部分形状只有地毯 1/16、珊瑚扇、栅栏门与墙，它们的面都无法互补覆盖 16×16。这条如实记为未覆盖，留给 F-7 用真实原版夹具验证；没有为了让变异失败而编造形状。

**实现过程中踩到的两个坑**（记录以免重犯）：

- 上一轮做 `blocksMotion` 变异后没有恢复，导致后续一直跑在变异代码上，地毯用例因此长期失败；定位靠临时调试钩子打印 `canHoldAnyFluid` 才确认。
- `git checkout` 恢复头文件时把 `fluid_flow.cpp` 的全部修复一起还原了，只能逐条重新应用。此后改用「先备份到临时文件、变异后从备份恢复」的方式。

**返工后的构建与测试**：全量构建通过，**CTest 18/18 通过**（12.49 s）。生产代码改动集中在 `src/fluid_flow.cpp` 与 `src/engine.cpp` 的空流体守卫。

### 审查记录（`f2201fc`）

1. **阻塞｜显式空流体刻可能删除普通方块。** `src/engine.cpp:1606–1619` 在身份匹配后对所有流体调用 `fluidTick`。石头等非液体方块的 `fluidStateOf` 返回 `minecraft:empty`（`src/fluid.cpp:158–167,181–183`），可匹配 F-2 允许的空流体刻。随后 `src/fluid_flow.cpp:485–495` 会把该方块写成空气。26.1 的 `EmptyFluid` 沿用 `Fluid.java:61–62` 的空 `tick`，其 `getHeight` 也恒为零（`EmptyFluid.java:46–53`），C++ 当前的 `fluidHeight` 在两个空状态上下相邻时却可返回 1。建议区分空流体与水/岩浆的派发，在石头上显式排 `Fluids::empty()` 并经 `World::tick()` 断言刻被消费且方块不变，另断言空流体高度为零。
2. **阻塞｜共享面取错目标面的方向，且忽略移动活塞的实际形状。** `src/fluid_flow.cpp:65–143` 对源与目标都按同一方向提取面。例如向下传播时，当前代码取目标底面，26.1 `Shapes.java:236–252` 取目标顶面。`src/fluid_flow.cpp:395–409` 又用静态碰撞形状查询，移动活塞因此始终表现为空形状（`src/shapes.cpp:156–159`）；原版 `MovingPistonBlock.java:105–108` 查询当前位置的 BE。建议分别用源方向和目标反方向取接触面，对移动活塞复用 `src/shapes.cpp:210–241` 的位置感知查询；按 W-06 验证不对称方向、互补遮挡以及同 state ID 不同进度。
3. **阻塞｜实体性判断使玻璃支撑无法转源，薄地毯也会误挡横向水流。** `src/fluid_flow.cpp:39–53,440–444` 分别用非空碰撞形状代替 `blocksMotion()`、用红石导体代替 `isSolid()`。26.1 `BlockBehaviour.java:486–501,540–547` 是两种明确的规则；玻璃是满碰撞但非红石导体（`Blocks.java:551–562`），应能在双源夹格下充当固体支撑。1/16 厚地毯的形状不满足 `legacySolid`（`CarpetBlock.java:17,29–31`），不能仅凭碰撞非空判为阻挡。建议按已注册状态补原版实体性，增加玻璃支撑和同层地毯的回归。水冲毁目标后还缺少原版掉落顺序，`src/fluid_flow.cpp:318–327` 写成了水无操作；`WaterFluid.java:81–85` 在写入流体前调用 `Block.dropResources`。对已支持掉落的冲毁对象复用 `blockDrops`/`spawnItem`，保留岩浆 fizz 与含水宿主的后续任务边界。
4. **阻塞｜现有传播用例没有验证计划刻派发写方块，副作用断言读错队列。** `tests/fluid_flow_tests.cpp:50–65,107–121,236–280` 直接调用算法，瀑布只检查第一格 falling，未覆盖落地后横流；`queriesDoNotMutate` 只查看 `scheduledTickCount()`。建议至少把一个水平或瀑布传播用例改为显式 `scheduleFluidTick` 后由 `World::tick()` 推进，按 F-3 的边界人工安排新格后续刻，并断言落地横流、W-05 的并列写序及查询前后两条队列都不变。F-5 的自然触发与 F-7 的真实原版差分仍各自验收。

独立 MSVC Release 构建和 CTest **18/18 通过**（8.29 s）。以上差异来自本地 26.1 源码对照，未运行真实服务端差分。当前先退回纠正行为及覆盖缺口；本轮未重跑世吞性能基准。

任务已完成
