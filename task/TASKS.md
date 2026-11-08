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
- 返工提交：`61657e3` — Reject empty fluid handles and make the tick tests bite（3 个文件，+248 / -27）
- 返工二提交：`4756fb0` — Back both tick queues before the same tick in the budget test（2 个文件，+42 / -17；仅测试与交接记录，未改生产代码）

### 返工（回应审查：2 阻塞）

**阻塞 1 — 空句柄可入队，快照解引用空指针。** 属实：`scheduleFluidTick(Pos, …)` 在 1508 行有 `fluid.empty()` 检查，但 `ScheduledTick` 重载与 `restoreFluidTick` 都没有，`fluidTicks()` 的 `FluidRef(*entry.target)` 会解引用空指针。已修：

- 两个入口都拒绝空句柄并返回 `false`，与延迟重载一致。
- 明确区分「空句柄」与「有身份的 `Fluids::empty()`」：只拒绝 `fluid.empty()`（指针为空），`minecraft:empty` 仍是合法身份，可以入队、可以被快照正确报告。
- 新增 `nullFluidHandleIsRejected`：默认句柄在三个入口都被拒绝、队列计数与快照保持为空、`hasFluidTick` 返回假；未知名字（`minecraft:not_a_fluid`）同样解析为空句柄并被拒绝；`FluidRef(Fluids::empty())` 可入队且快照能读出 `minecraft:empty` 而不解引用空指针。

**阻塞 2 — 回归用例没有真正保护验收判据 3/6/7。** 三条都属实，已重写：

1. **阶段顺序**：原用例只排流体刻。新增 `phaseOrderWithinOneTick` 在同一刻真实触发四件事——方块刻（`smooth_stone` 的到期刻）、流体刻（`flowing_water` 的到期刻）、区块广播（`blockUpdatesBroadcastObserver` 写入 trace）、方块事件（音符盒事件被 `runEvents` 接受并 trace）——并从**同一条 `world.log` 时间线**断言 `block_tick < fluid_tick < broadcast < block_event`。关键修正：不能把 observer 收集到独立 vector（那会在 tick 结束后才回填，顺序失真），必须让 observer 也写进 `world.log`。另加 `dueFluidTickRunsInTheSameTick` 断言当刻到期的流体刻在第一刻就派发，而不是延后一刻。
2. **跨卸载保留**：原用例从未卸载。`unloadedChunkHoldsTicks` 现在先在到期前 `unloadChunk(0,0)`，再连跑 5 刻断言条目**仍在队列里**（既没被丢弃也没被执行），随后 `loadChunk` 重载并断言**恰好派发一次**、队列清空。
3. **65536 上限**：原用例只有 8 个条目。`budgetsAreSeparate` 现在给两条队列**各排入 65537 个到期条目**（16×2 个区块，`x=i&255`、`y=64+((i>>8)&255)`、`z=i>>16` 保证位置唯一），断言第一刻各消费 65536 各留 1、第二刻各自清空，并交叉断言另一条队列不被影响。

**变异验证（证明新断言真的能失败，不是又一批空用例）**：

| 变异 | 结果 |
|---|---|
| 把流体阶段整体移到方块阶段之前 | `the block tick phase precedes the fluid tick phase` |
| 删掉两个入口的 `fluid.empty()` 检查 | `the explicit-tick overload rejects a null handle` |
| 把方块队列预算从 65536 改成 65535 | `the block queue also stops at 65536` |

三次都被拒绝；变异后均已用备份恢复 `src/engine.cpp`，恢复后的 `git diff` 只含本次新增的空句柄检查。

**返工后的构建与测试**：全量构建通过，**CTest 17/17 通过**（12.45 s）。`fluid_tick` 组单独运行 1.05 s。

**没有做的事**：reviewer 在「独立验证」里提到的两点观察（现有基准不输出超过 50 ms 的刻数、本轮未覆盖湿态负载）属于基准工具与后续任务的范畴，本轮未处理，也未据此声称性能结论。

### 返工二（回应复审：1 阻塞 + 1 建议）

**阻塞 — 预算用例没有验证同刻互不争抢。** 属实：原用例在第 1、2 刻只推进流体队列，排空后才在第 3、4 刻填入方块队列，两条队列从未在同刻同时积压。若两条队列共用每刻重置的 65536 总预算，该用例仍会通过；我上一轮的预算变异也只在单侧（只看方块队列上限）能生效。

已改：`budgetsAreSeparate` 在**第一次 `world.tick()` 之前**把两条队列各填到 65537 个到期条目（同一循环里同时排流体刻与方块刻，位置复用），随后断言：

- 第 1 刻后**两条队列各剩 1 条**——方块刻先跑并耗尽自己的 65536，流体刻仍在同一刻消耗自己的 65536，证明预算不共享；
- 第 2 刻后两条队列都清空。

**变异验证**：把流体队列的每刻预算从 65536 改成 1（模拟「预算被方块队列吃掉后只剩 1」）→ 测试报 `the fluid queue still consumes 65536 in that same tick`，被拒绝。恢复后 `git diff src/engine.cpp` 为空。

**建议 — 空流体用例注释与实现不符。** 属实：非液体方块经 `fluidStateOf` 返回默认 `FluidState`（`minecraft:empty`），执行时仍按精确身份检查，所以「没有方块报告空流体、该条目不会派发」是错的。注释已收窄为只声称验证入队与快照，不再断言派发行为。

**返工二之后的构建与测试**：全量构建通过，**CTest 17/17 通过**（12.27 s）；`fluid_tick` 组 1.21 s。生产代码未改动，沿用上轮 reviewer 已通过的无车版性能对比，本轮未重跑基准。

### 审查记录

1. **阻塞｜空流体句柄可入队，读取队列快照时解引用空指针。** `src/fluid.cpp:203` 将未知名称解析为空句柄。`src/engine.cpp:1514–1521` 的显式排刻和恢复入口都可以把空指针写入已注册的区块队列，`src/engine.cpp:1542–1543` 随后在快照中对它解引用。建议两个入口都按 `src/engine.cpp:1508` 拒绝空句柄，区分空句柄与有有效身份的 `Fluids::empty()`，并用已加载区块验证默认句柄和未知名称均被拒绝，队列计数与快照保持为空。
2. **阻塞｜回归用例未触发验收判据 3、6、7 要保护的行为。** `tests/fluid_tick_tests.cpp:73–89` 只有流体刻，把流体阶段移到方块刻前或方块事件后仍可通过。建议同一刻实际触发方块刻、流体刻、区块广播和方块事件，并断言观察顺序；方块刻回调可顺带验证当刻排入的到期流体刻当刻执行。`tests/fluid_tick_tests.cpp:142–153` 从未卸载区块。建议在刻到期前卸载，断言到期后仍保留，再重载并验证恰好派发一次。`tests/fluid_tick_tests.cpp:157–169` 仅排入 8 个流体刻且方块队列为空。建议两个队列各排入 65537 个到期条目，断言第一刻各消费 65536 个、各留 1 个，下一刻清空。

#### 独立验证

- `cmake --build build-jemalloc --config Release -j 6` 通过。`ctest --test-dir build-jemalloc -C Release --output-on-failure -j 4` **17/17 通过**（8.33 s）。
- 对照上一级 `net/minecraft/server/level/ServerLevel.java:380–382`、`:797–803` 和 `net/minecraft/world/ticks/LevelTicks.java`。主路径的两次队列调用和精确身份派发符合该源码。本轮为源码审查和 C++ 回归验证，未运行真实 26.1 服务端差分。
- 世吞基准按本轮要求从 3000 改为 **1000 tick**。同一 800×83×800 干态投影、音符盒 `(2,101,3)` 触发，SHA-256 为 `7ad76ac215039e66594a81feb45dae70896827c768328133d19b86d3e8d91e52`。使用 MSVC x64 Release、jemalloc，关闭网络、分配计数和正式计时中的指纹。流体前基线 `c8e9eb1` 与待审 `3c7e360` 交替各跑 3 次，进程固定逻辑 CPU 2 并使用高优先级。下表取各指标三次结果的中位数。

  | 指标 | `c8e9eb1` | `3c7e360` | 变化 |
  |---|---:|---:|---:|
  | 平均每 tick | 3.096 ms | 3.114 ms | +0.58% |
  | 每 tick 中位数 | 2.248 ms | 2.269 ms | +0.93% |
  | 每 tick P95 | 6.469 ms | 6.464 ms | −0.08% |
  | 1000 tick 主线程 CPU | 2906.250 ms | 2953.125 ms | +1.61% |

- 本轮干态 tick 未见明显性能回退，中位数和 P95 均未触及 5% 复测线。两版另跑的第 250、500、750、1000 刻指纹全部相同，导入与触发记录以及计时终态计数也一致。原始结果见本地 `build-review-f2/summary.json`。现有基准不输出超过 50 ms 的刻数，本轮也未覆盖湿态负载。

#### 无车版世吞补测

- 上表的 3.096 ms/tick 对应 800×83×800 音符盒投影的前 1000 刻。同一基线和投影另跑一次 3000 刻，平均 5.868 ms、P95 32.112 ms。两种时间窗口覆盖的负载不同。
- 此前性能分析中的 87.989 ms 来自 2006×81×100 无车版在第 250 刻的单刻值，彼时有 8017 个移动活塞。该次带采样的 1000 刻均值为 30.89 ms，见本地 `build-profile-lto/no-cart-second-gate-1000-ticks.log`。
- 补测使用同一无车版投影（SHA-256 `5b9f31fb5aa08eb6926a2c0df23e01307ea08d7dd12c14da2c6dc17956a603db`），用既有 `build-profile-lto/timing-no-cart.cpp` 触发第二处白桦栅栏门 `(4,87,93)`。两个提交各自链接相同配置的 MSVC x64 Release、jemalloc 核心，固定逻辑 CPU 2，高优先级，无网络、无采样，交替各跑 3 次，每次 1000 刻。

  | 每 tick 耗时 | `c8e9eb1` | `3c7e360` | 变化 |
  |---|---:|---:|---:|
  | 平均 | 20.37 ms | 20.48 ms | +0.54% |
  | 中位数 | 15.78 ms | 15.51 ms | −1.71% |
  | P95 | 73.98 ms | 73.42 ms | −0.76% |

- 表中取各指标三次结果的中位数。六次的终态方块及实体位置指纹、抽查的计划刻与移动活塞数、爆炸统计均一致，每次均有 26386 次爆炸。无车版干态负载也未见明显性能回退。完整数据见本地 `build-review-f2/heavy-summary.json`，该计时程序同样未输出超过 50 ms 的刻数。

本轮退回修正两项阻塞，干态性能项通过。

### 返工复审（`61657e3`）

- **已解决｜空句柄、阶段顺序、卸载恢复。** `src/engine.cpp:1514–1528` 的两个入口都拒绝无身份的句柄；已注册的 `Fluids::empty()` 仍可入队并出现在快照。`tests/fluid_tick_tests.cpp:76–134` 在同一条时间线上断言方块刻 → 流体刻 → 区块广播 → 方块事件。`tests/fluid_tick_tests.cpp:205–232` 在到期前卸载，验证重载后恰好派发一次。
- **阻塞｜预算用例验证了各自的上限，仍未验证同刻互不争抢。** `tests/fluid_tick_tests.cpp:249–275` 在第 1、2 刻只推进流体队列，排空后才为第 3、4 刻填入方块队列。如果两条队列共用每刻重置的 65536 总预算，该用例仍会通过；返工中的预算变异只验证了单侧上限。建议在第一次 `world.tick()` 前把两条队列同时填到各 65537 个到期条目，同一刻断言各留 1 个，第二刻断言都清空。可把两处入队放进现有的同一个循环。
- **建议｜修正空流体用例注释。** `tests/fluid_tick_tests.cpp:329–330` 称没有方块报告空流体且该条目不会派发。非液体方块实际返回默认 `FluidState`（`src/fluid.cpp:181–183`），执行时照常按精确身份检查（`src/engine.cpp:1605–1618`）。该用例验证入队和快照，注释可限于这两个行为。

独立 Release 构建通过，CTest **17/17 通过**（8.32 s）。返工的生产代码只改了流体入队入口，沿用上轮无车版 1000 刻的性能对比，本次未重跑基准。仍须补齐验收判据 7 的同刻独立预算用例。

任务完成
