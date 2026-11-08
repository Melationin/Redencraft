# 任务交接

流程与状态定义见 [AGENTS.md](../AGENTS.md) 第 3 节；任务顺序见 [PLAN.md](../PLAN.md)。
本文件只记录**当前任务**。F-1（流体状态模型）的完整记录在 `53a5d20`、`6a2e26f`、`40fcecc` 与 `3cd502d` 的提交内容里；它的两项结转项已登记为 `PLAN.md` 的 F-7（协议固定映射）与 F-8（单例所有权）。

---

## F-2 流体计划刻队列

### 目标

按 `ServerLevel` 的两个独立 `LevelTicks` 容器实现流体计划刻队列：收集与执行同方块刻分开，共享 `Level.nextSubTickCount`，位于方块刻之后、区块阶段之前，各自 65536 上限。本任务只交付**队列与派发**，不实现流动算法（F-3）。

### 范围

1. `LevelTicks` / `LevelChunkTicks` 泛型化：容器按「被刻对象的身份」去重，方块队列用 `Block`，流体队列用 `Fluid`，两条队列的容器表、去重集与 drain 状态完全独立。
2. 两个队列共享 `Level.nextSubTickCount` 的单调计数器（`subTickOrder` 跨队列唯一）。
3. `World::tick` 顺序：`blockTicks` → `fluidTicks` → 区块广播 → 方块事件 → 实体 → 方块实体。
4. 派发使用**精确身份** `fluidState.is(type)`（`ServerLevel.tickFluid`），既不是 `isSame` 家族判定，也不是 `isSourceOfType`。
5. `FluidRef` 句柄（与 `BlockRef` 对称）与 `findFluidByName` 解析：未知名字保持空，不注册。

### 源码依据

- `net/minecraft/server/level/ServerLevel.java`：`blockTicks` / `fluidTicks` 两个 `LevelTicks` 字段（211-212 行）；`tick` 中 `blockTicks.tick(tick, 65536, this::tickBlock)` 紧接着 `fluidTicks.tick(tick, 65536, this::tickFluid)`（379-382 行）；`tickFluid` 的 `fluidState.is(type)`（797-803 行）。
- `net/minecraft/world/level/ticks/{LevelTicks,LevelChunkTicks,ScheduledTick}.java`：容器结构、`DRAIN_ORDER`、`UNIQUE_TICK_HASH`、`INTRA_TICK_DRAIN_ORDER`。
- `net/minecraft/world/level/ScheduledTickAccess.java`：方块刻与流体刻分派到不同 `LevelTicks`。
- `net/minecraft/world/level/block/state/TypedInstance.java`：`is` 是精确身份。

### 验收判据

1. 两个队列互不串扰：方块刻不出现在流体队列的计数、快照与去重集里，反之亦然。
2. 同一位置同一流体重复排刻被拒绝；同一位置不同流体（水 vs 岩浆）是两个独立条目。
3. 流体刻在方块刻之后执行，且都在方块事件之前。
4. 陈旧刻按精确身份跳过：排了水的刻、执行前方块变成岩浆时不触发。
5. 流动刻不匹配同位置的源水；源刻也不匹配流动水。
6. 未加载区块的刻保留并延后，重新加载后继续。
7. 两条队列各有独立预算，互不饿死。
8. 恢复刻（`scheduleUnchecked`）保留重复条目，去重集只记一个。
9. 构建通过、CTest 全绿（含新增 `fluid_tick` 组）。
10. 干态世界（无流体刻）的 tick 开销不因新增队列明显变差。

### 计划改动

- 修改：`include/redstone/engine.hpp`（`TickQueueOf` / `ChunkTicksOf` 模板声明、流体队列成员与 API、`tickBlockAt` / `tickFluidAt`）
- 修改：`src/engine.cpp`（队列泛型化、两阶段 tick、流体刻派发）
- 修改：`include/redstone/fluid.hpp` / `src/fluid.cpp`（`FluidRef`）
- 新增：`tests/fluid_tick_tests.cpp`
- 修改：`CMakeLists.txt`、`docs/compatibility.md`

### 实际做了什么

**队列泛型化。** `ChunkTicksOf<Target>` 与 `TickQueueOf<Target>` 以被刻对象的类型为模板参数，堆元素是 `QueuedTick<Target>{tick, target}`，去重键是 `TickKey<Target>{pos, target}`。`World` 持有 `tickQueue_`（`TickQueueOf<Block>`）与 `fluidTickQueue_`（`TickQueueOf<Fluid>`）两份，容器、`nextTickForContainer`、`containersToTick`、`toRunThisTick` 全部独立。没有用 `void*` 擦除，也没有第二套去重实现。

**两阶段 tick。** `World::tick` 现在是 `blockTicks.tick(...)` → `fluidTicks.tick(...)` → 区块广播 → 方块事件 → 实体 → 方块实体，各自 65536 预算。两条队列的 `subTickOrder` 都取自同一个 `nextSubTickCount()`。

**派发。** 原 `World::runScheduled` 拆成 `tickBlockAt(pos, expected)` 与 `tickFluidAt(pos, expected)`，对应 `ServerLevel.tickBlock` / `tickFluid`：先读当前位置，身份不符就跳过。流体侧用 `fluidStateOf(state).is(*expected)` 精确匹配。

**`FluidRef`。** 与 `BlockRef` 对称的句柄；`FluidRef(std::string_view)` 走 `findFluidByName`，未知名字保持空（不注册），`str()` 对空句柄返回空串。

**`fluid_tick` 测试组**（9 个场景）：队列独立性、阶段顺序、陈旧身份跳过、流动 vs 源、跨刻保留、独立预算、恢复刻保留重复、未加载区块排刻被丢弃、快照按 trigger 排序并带流体身份。

**实现过程中修掉的问题**：
- `tickFluidAt` 最初在身份校验**之前**写 `fluid_tick` trace，导致"跳过"与"执行"在日志里无法区分，测试据此误判。改为按校验结果分别记 `fluid_tick` / `fluid_tick_skipped`。
- 模板移到命名空间作用域后无法访问 `World::chunkIndex`；改用 `pos.x >> 4`（与该私有静态函数同义，原本就是算术右移）。
- `scheduleFluidTick(Pos, ...)` 原返回 `void`，测试无法断言排刻是否成功，改为返回 `bool`。

**明确没有做的事**：
- 没有实现 `fluidState.tick()` 的流动逻辑（`spread` / `getNewLiquid` / `getFlow`），`tickFluidAt` 里留了 `// TODO` 指向 F-3。本任务**只证明队列与派发正确**，改变不了任何水位。
- 没有实现 F-4 的水/岩浆参数差异（delay 5 vs 30 等）；当前排刻延迟由调用方给出。
- 没有接存档 `fluid_ticks`（F-6）、没有 `waterlogged` 宿主（F-5）、没有单例所有权整改（F-8）。
- 没有跑真实 26.1 服务端差分。

### 构建与测试

```powershell
cmake -S . -B build-jemalloc -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static"
cmake --build build-jemalloc --config Release -j 8
ctest --test-dir build-jemalloc -C Release --output-on-failure
```

结果：构建通过；**CTest 17/17 通过**（原 16 组 + 新增 `fluid_tick`）。

已知警告（均为既有、不在本次改动内）：`include/redstone/net/outbox.hpp` 的 C4324 对齐填充（`redstone_vanilla_server` 目标）、`src/litematic.cpp:204` 的 C4456 变量遮蔽。本次新增/修改的文件零警告。

**性能口径**：新增的第二条队列在没有流体刻时 `nextTickForContainer` 为空，`sortContainersToTick` 的 `forEach` 不进入循环；干态世界每刻只多一次空表遍历。按 reviewer 在「后续性能复审」中的要求，**以流体前提交 `c8e9eb1` 为基线的 3000 tick 干态世吞对比由 reviewer 执行**；本轮没有自行建立基线，因此这里只陈述结构上的开销来源，不声称"无回退"。

证据口径：**源码对照 + C++ 回归测试**。队列结构与阶段顺序逐个对照 `ServerLevel.java` 与 `LevelTicks.java`；**没有跑真实 26.1 服务端差分**，不能声称与原版逐位等价。

### 提交

- 主提交：`3c7e360` — Add a separate fluid tick queue and dispatch on exact fluid identity
  （10 个文件，+557 / -299；含 `include/redstone/engine.hpp`、`src/engine.cpp`、`include/redstone/fluid.hpp`、`src/fluid.cpp`、`tests/fluid_tick_tests.cpp`、`CMakeLists.txt`、`docs/compatibility.md`，以及 reviewer 前一轮加入的 `PLAN.md` / `docs/fluid-testing.md` 性能回归要求）

任务完成
