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

- 修改：`src/fluid_flow.cpp` / `include/redstone/fluid_flow.hpp`（三条请求路径、`LiquidBlockContainer` 判定）
- 修改：`src/fluid.cpp`（waterlogged 宿主的 `fluidStateOf`）
- 修改：`src/engine.cpp`（`setBlock` 的 onPlace 与 `neighborChanged` / `updateShape` 接线）
- 修改：`src/state.cpp`（墙与枯珊瑚扇 `forceSolidOn`）
- 修改：`include/redstone/block.hpp`（若需要新的 metadata 位）
- 新增：`tests/fluid_block_tests.cpp`
- 修改：`CMakeLists.txt`、`docs/compatibility.md`

### 实际做了什么

**三条请求路径**（`include/redstone/fluid_flow.hpp` / `src/fluid_flow.cpp`），各自独立实现，没有合并成统一邻居刷新：

- `liquidOnPlace`：`LiquidBlock.onPlace`。先跑 `liquidShouldSpreadLiquid`（岩浆可能就地变石/黑曜石，此时不再排刻），存活则按当前流体类型与 `tickDelay` 排刻。
- `liquidNeighborChanged`：`LiquidBlock.neighborChanged`，同样是「先转化判定、再排刻」，但入口与放置不同（`src/engine.cpp` 的 `executeNeighbor` 新增液体分支）。
- `liquidUpdateShape`：`LiquidBlock.updateShape`，只在自身或变化邻居是**源**时排刻（`src/engine.cpp` 的 `updateShapeState` 新增液体分支）。

排刻用的流体类型取自方块当前的 `FluidState`——因此流动水排的是 `flowing_water` 而不是 `water`，与原版一致（`Fluids` 注册五个流体，`water` 只是源变体；`LiquidBlock.onPlace` 用的是 `state.getFluidState().getType()`）。

**含水宿主**：`fluidStateOf` 对 `waterlogged=true` 的方块返回水源 `FluidState`（`SimpleWaterloggedBlock.getFluidState`），新增 `isWaterloggedHost` 标识 `LiquidBlockContainer` 宿主。

**`forceSolidOn` 扩展**：墙（`cobblestone_wall`、`stone_brick_wall`）与五种枯珊瑚扇一并标记，和 F-4 的栅栏门/移动活塞同一条 `calculateSolid` 优先路径。

**新增 `fluid_block` 测试组**（6 个场景）：放置排刻、`neighborChanged` 排刻、`updateShape` 的源条件（非源邻居不排、源邻居排）、含水宿主的上报与 `isWaterloggedHost`、墙作双源支撑且不被水替换、干态世界不产生流体刻。

**F-5 改变了三组既有测试的前置条件，已按新行为修正断言**（不是放宽，是行为确实变了）：

- `fluid_tick` 的「预算」用例改用**空流体**排刻：真实流体刻现在会写方块并给邻居排刻，队列计数不再是精确 65536。该用例本来就只测队列预算。
- `fluid_tick` 的「跨卸载保留」改为断言**旧条目**（trigger 5）被消费，因为同一位置会出现新的续排刻。
- `fluid_flow` 与 `fluid_params` 的排刻计数改成**按位置/按条目**过滤，不再假设队列里只有一条。

**变异验证**：

| 变异 | 结果 |
|---|---|
| 去掉 `onPlace` 的液体分支 | `placing a liquid requests a fluid tick` |
| 去掉 `neighborChanged` 的液体分支 | `a neighbour change requests a fluid tick` |

（`forceSolid` 机制本身在 F-4 已用栅栏门变异验证过；本轮墙用例是正例断言，没有再单独变异。）

**明确没有做的事**：桶交互、实体推流（P1 后置）；`updateShape` 的含水宿主排水路径只做到「含水宿主上报水源」，宿主被破坏时的 `pickupLiquid`/掉落尚未接通；没有跑真实 26.1 服务端差分。

### 构建与测试

结果：构建通过；**CTest 20/20 通过**（原 19 组 + 新增 `fluid_block`）。

证据口径：**源码对照 + C++ 回归测试**，未跑真实 26.1 服务端差分。

### 提交

- 主提交：`f4f1b2c` — Wire the liquid block request paths and waterlogged hosts（12 个文件，+339 / -20）


### 返工（针对 `f4f1b2c` 的四条阻塞）

**阻塞 4 — 同种液体改 level 漏 `onPlace`，排刻身份偏离回调状态**

- `src/engine.cpp` 的 `setBlock`：`onPlace` 条件从 `isLiquidBlock(state) && !old.sameBlock(state)` 改为 `isLiquidBlock(state)`，与 `LevelChunk.java:339-341`（除 flags 512 外每次写入都调 `onPlace`，`LiquidBlock.java:148-151` 无同类型早退）一致。去重仍由 `LevelChunkTicks.schedule` 的 (位置, 精确流体) 键负责（`LevelChunkTicks.java:54-59`）。
- `src/fluid_flow.cpp`：`requestFluidTick` 改为接收入参 `const FluidState &`，三条入口（`liquidOnPlace` / `liquidNeighborChanged` / `liquidUpdateShape`）分别传 `fluidStateOf(state)`、`fluidStateOf(state)`、`self`（回调状态），不再重读 `world.getBlock(pos)`。
- 测试：`callbackStateDecidesIdentity`（回调给流水但世界是源 → 队列里是 `flowing_water`；反向亦然）、`sameFluidLevelChangeRunsOnPlace`（同级改 level 排刻、flags 512 不排、同状态写入不排）。

**阻塞 1 — 含水容器没接进传播路径**

- `include/redstone/block.hpp`：`BlockMetadata` 新增 `liquidContainer`，`Block::isLiquidContainer()`。`src/state.cpp` 在注册时对铁轨 / 墙 / 枯珊瑚扇置位（本注册表内声明 `waterlogged` 的方块恰好就是这些 `SimpleWaterloggedBlock`）。
- `src/fluid_flow.cpp`：`canHoldAnyFluid` 对宿主恒真；新增 `canHoldSpecificFluid`（宿主走 `canPlaceLiquid`，其余恒真）与 `canHoldFluid`（`FlowingFluid.java:429-431`），`isWaterHole` 改用它；`spreadTo` 对宿主调 `placeLiquid` 并直接返回，不再 `beforeDestroyingBlock` + 写 legacy 方块。
- 新增公开 `canPlaceLiquid(const BlockState&, const Fluid&)` 与 `placeLiquid(World&, Pos, const BlockState&, const FluidState&)`，对应 `SimpleWaterloggedBlock.java:20-36`：只接受精确 `minecraft:water`；已含水则拒绝；成功时写 `waterlogged=true`（flags 3）并排自己的水刻。
- **一处由测试纠正的实现认知**：`getSpread` 用目标格**新算出的流体**调 `canHoldSpecificFluid`（`FlowingFluid.java:378-403`），单源侧流得到的是 `flowing_water`，容器拒绝；只有双源转源得到的精确水源才会注入宿主。测试按这个真实语义写（`spreadFillsHosts`：双源夹格成功、单源侧流不注入）。

**阻塞 2 — 含水宿主的形状排刻与移除保水缺失**

- `src/engine.cpp` 的 `updateShapeState`：铁轨 / 墙 / 枯珊瑚扇在 `waterlogged=true` 时排自己的水刻，对应 `BaseRailBlock.java:283-299`、`WallBlock.java:134-156`、`BaseCoralPlantTypeBlock.java:69-87`、`BaseCoralWallFanBlock.java:57-73`。四个入口保持分离，只共享「是否含水」的判断。
- 新增 `World::removeBlock(pos, movedByPiston)`（`Level.java:276-280`）：写回**当前流体的 legacy 方块**而非空气，`movedByPiston` 置 flags 64。`src/rail.cpp` 的断支撑移除与 `src/net/session.cpp` 的创造破坏改走它。
- `World::updateOrDestroy` 的空气分支改为写 `fluidStateOf(current)` 的 legacy 方块（`Level.destroyBlock` 语义，`Level.java:288,298`），珊瑚扇失去支撑时保留水。
- `World::coralPlacementState` 按目标格现有流体初始化 `waterlogged`（满态水源才为 true），不再硬写 `false`。
- 测试：`hostDrainOnSupportLoss`（湿珊瑚扇保留水源 / 干珊瑚扇变空气）、`railDrainKeepsWater`、`hostUpdateShapeSchedulesWater`（铁轨 / 墙 / 珊瑚扇各自排刻，干宿主不排）、`pistonMovesWaterloggedHost`（P-01：活塞搬运动力完成后清 `waterlogged`）。
- `tests/engine_tests.cpp` 的珊瑚用例断言随之从「变空气」改为「变水」，并补了干态对照组。

**阻塞 3 — 玩家放置宿主时强制清除含水初态**

- `src/net/session.cpp` 的 `useItemOn`：读**目标格**（`BlockPlaceContext.getClickedPos()`，`BlockPlaceContext.java:48-51`）的原有流体，按 `BaseRailBlock.java:139-147` / `WallBlock.java:112-131`（精确水源）与 `BaseCoralPlantTypeBlock.java:57-62`（水标签 + 满态）初始化 `waterlogged`。

**测试与变异验证**

`tests/fluid_block_tests.cpp` 由 6 个场景扩到 12 个，新增 `containerFillRules`、`spreadFillsHosts`、`flowingWaterDoesNotFillHosts`、`hostDrainOnSupportLoss`、`railDrainKeepsWater`、`hostUpdateShapeSchedulesWater`、`callbackStateDecidesIdentity`、`sameFluidLevelChangeRunsOnPlace`、`pistonMovesWaterloggedHost`。

| 变异 | 结果（均被抓到） |
|---|---|
| `onPlace` 恢复 `!old.sameBlock(state)` 早退 | `re-levelling water onto water requests a tick` |
| `canHoldAnyFluid` 去掉宿主分支 | `the spread waterlogs the wall` |
| `updateOrDestroy` 空气分支写回空气 | `the drained cell keeps the water source` |
| 三条入口改回重读世界位置 | `the entry carries the callback's flowing_water, not the world's water source` |

每次变异后都从源码恢复并确认 `git diff` 只剩预期改动。

**`tests/fluid_params_tests.cpp` 的既有断言随行为改变而修正**（不是放宽）：岩浆液面抬升的 120 延迟现在总是先被 `setBlock` 里 `onPlace` 请求的 30 抢到队列槽，`LevelChunkTicks` 去重后只留一条。用例改为断言「恰好一条、且是 30」，4 倍分支本身由同文件直接调 `fluidSpreadDelay` 的用例覆盖。

**明确没有做的事**：桶交互（`BucketItem` / `pickupBlock`）仍属 P1；实体推流仍属 P1；活珊瑚的死亡逻辑（`tryScheduleDieTick` / `scanForWater`）未接；`WallBlock` 的活珊瑚墙形态未注册；没有跑真实 26.1 服务端差分。

### 构建与测试（返工后）

- 构建：MSVC Release + jemalloc（`build-jemalloc`）通过，无新增警告。
- CTest：**20/20 通过**。
- 证据口径：**源码对照 + C++ 回归测试**，未跑真实 26.1 服务端差分，因此不声称原版等价。
### 审查记录（`f4f1b2c`）

1. **阻塞｜含水容器仍未接到实际流体传播。** `src/fluid_flow.cpp:70–81,368–390` 仍按普通方块判断能否容纳并直接替换目标；新增的 `isWaterloggedHost` 未用于这两处。原版 `FlowingFluid.java:410–435,271–279` 先识别容器、调用 `canPlaceLiquid` 和 `placeLiquid`。`SimpleWaterloggedBlock.java:18–35` 允许精确的源水身份，实际注水成功时保留宿主、置 `waterlogged=true` 并排水刻。当前流动水可能直接冲掉干铁轨，墙和枯珊瑚扇也无法通过容器接收源水。按 H-01/H-03 增加实际传播的源水成功、流水与岩浆拒绝、重复注水不重写和去重断言。
2. **阻塞｜含水宿主的形状排刻和正常移除保水缺失。** `src/engine.cpp:1142–1156,1223–1240` 只给液体方块接 `updateShape`，珊瑚支撑失效后的销毁写空气；`src/rail.cpp:572–579` 的断支撑移除和 `src/net/session.cpp:849–865` 的创造破坏也写空气。26.1 的湿铁轨、墙和珊瑚分别在 `BaseRailBlock.java:283–298`、`WallBlock.java:134–151`、`BaseCoralPlantTypeBlock.java:69–86`、`BaseCoralWallFanBlock.java:57–75` 排水刻；`Level.java:277–299` 的移除和销毁写回原流体的 legacy 状态。建议保留各宿主的更新入口，普通销毁与移除保水，直接置空气仍遵从调用方；按 H-02 验证支撑破坏、掉落、保水和排刻，至少加一组含水宿主的 P-01/P-02 活塞联动回归。桶交互继续留在 P1。
3. **阻塞｜玩家放置宿主时强制清除含水初态。** `src/net/session.cpp:984–985` 对带属性的所有宿主写 `waterlogged=false`，而 `BaseRailBlock.java:139–146` 与 `WallBlock.java:112–131` 根据目标格精确水源决定初态，`BaseCoralPlantTypeBlock.java:57–62` 用水标签和满态判定。建议在真实放置入口读取被替换格的流体，按宿主规则初始化，并补 H-01 的空气、源水、流水对照。
4. **阻塞｜同种液体的水位变化漏掉 `onPlace`，排刻身份也偏离回调传参。** `src/engine.cpp:1081–1084` 以换方块类型为条件，26.1 `LevelChunk.java:318–341` 对未被 flags 512 跳过的状态写入调用 `onPlace`，`LiquidBlock.java:148–151` 无同类型早退。`src/fluid_flow.cpp:712–752` 又重读世界位置来决定刻身份，原版三条路径分别使用回调状态（`LiquidBlock.java:148–151,167–179,194–198`）。建议保留三条入口，按传入状态请求，并验证同种液体改 level、flags 512 及捕获状态。补测岩浆在 `setBlock` 回调中请求基础延迟与随后显式请求竞争时的接受顺序，两端 RNG 序列独立。

#### 独立验证

- MSVC Release 构建通过，CTest **20/20 通过**（8.60 s）。
- `c8e9eb1` 与 `f4f1b2c` 用同一 800×83×800 干态音符盒投影交替各跑 3 次，每次 1000 tick，Release + jemalloc，固定逻辑 CPU 2，高优先级。平均 **3.084 → 3.130 ms**（+1.49%），单刻中位数 **2.263 → 2.316 ms**（+2.34%），P95 **6.461 → 6.581 ms**（+1.86%），均低于历史 C++ 回归复测线。另跑的四个检查点指纹一致。完整结果见本地 `build-review-f2/f5-summary.json`。
- 原版依据是本地 26.1 源码，本轮未运行真实服务端差分或原版性能对跑。湿态行为仍有上述缺口，待同一夹具的工作量核对后再做对比。

任务进行中
