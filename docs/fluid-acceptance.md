# F-7 验收证据分级（返工后）

本文件记录 F-1…F-7 的流体验收证据，并**严格区分四类证据**。结论只能引用它实际拥有的那一类。

| 证据类别 | 含义 | 本次状态 |
|---|---|---|
| **A. 源码对照** | 逐方法比对只读 26.1 Java 源码 | 已做 |
| **B. C++ CTest** | 本仓库回归测试 | **22/22 通过**（18.39 s） |
| **C. 原版 JAR 离线探针** | 解包的原版 JAR 直接调用其类 | 共享面探针由 reviewer 完成；本仓库未新增离线 JAR 探针 |
| **D. 真实 26.1 服务端差分** | 官方 dedicated server、隔离世界、逐刻 trace | **已执行**：W-01 水槽、M-01 混流、双观察者；另有整服计时 |

> 没有跑到的夹具在这里明确写成“未跑/依赖”，不以 C++ 回归替代 D 类，也不声称整体“与原版等价”。

## 1. 本轮阻塞项的关闭证据

### N-02：真实 tick 阶段、完整解帧、包序与同格的去重

`tests/protocol_tests.cpp` 的 `fluidTicksAndPistonSameTick` 不再手工合并包：

- 安装生产同构观察器：`blockUpdateObserver` → `Session::blockUpdate`，`blockEventObserver` → `Session::blockEvent`，`blockUpdatesBroadcastObserver` → `flushBlockUpdates`（`src/net/server_main.cpp:663-681`）。
- 第 5 刻同时到期：封闭 5×5 池的源水刻（4 个邻格写入 → `section_blocks_update`）和一个独立竖井的单格下落写入（→ `block_update`）。
- 第 5 刻广播后、事件前：解码到的两个广播帧都在活塞 `block_event` 之前；事件帧的坐标 `(6,64,0)`、type `0`、data `East`、block registry id 全部逐字段断言，帧尾必须为空。
- 活塞事件写 `(6,64,0)` 的 `extended=true`；该写入只出现在**第 6 刻下一次广播**的帧里，且帧值与该次广播时的活世界一致。`moving_piston` 的 arm/destination 写使用 flags 324（不含 client update），测试显式断言它们不在该广播中，但活世界状态确为 `moving_piston`。
- 位置位序按生产编码 `x<<8 | z<<4 | y`（`src/net/session.cpp:64-87`）解码；此前测试按相反位序，已修正。
- 同一湿宿主在 tick 5 前连续写三次、`Session::blockUpdate` 三次，广播里只出现一次，且携带最后一次状态。
- 两类包都真实出现（`sectionPacketsAt5 >= 1 && blockPacketsAt5 >= 1`），不是“零包也能通过”。

**变异验证**：把 `World::tick` 的广播阶段移到 `runEvents` 之后，重新编译 `engine.cpp` 并用临时静态库链接测试，失败于 `firstBroadcast < firstEvent`；从备份恢复 `engine.cpp` 后复跑通过。本仓库当前 `src/engine.cpp` 已恢复为广播先于事件。

### N-01：负区块、跨 y section、完整计数

`fluidSectionSnapshot` 改为 chunk `(-1,-1)`：

- 第一段 y=-64：水、流动岩浆、含水墙、干墙 → 非空 4、流体 3；第二段 y=-48：水、含水墙、干墙 → 非空 3、流体 2；其余段 0/0。
- 全部 24 个 section 都解码并逐段核对包头两个 `i16` 与逐格统计；流体计数使用官方 `state_flags` 的 fluid 标志（含水宿主计入），不是按方块名重推。
- chunk 坐标 `-1/-1` 与 section 坐标都从帧中还原；每格完整 `BlockState` 与官方协议 ID 双向核对。
- 快照冻结后再改活世界，编码仍保留冻结状态。

### N-04：真实 socket、湿态负载、慢端积压与健康端进展

`validation/protocol775/backpressure_test.py` 现在：

- 用 `--fluid-basin` 启动真实协议服务端：9 个跨 y=48..63/64..79 的封闭竖井 + 9 个源水小池。
- 一个 `SlowFluidReader` 登录后不读包；测试结束时在服务端仍运行时排空它的 TCP 帧并完整解码：186 帧、207 个水格，section 3 和 4 都出现。
- 一个 `SlowReader` 持续灌入 `ping_request` 且不读包；服务端 `STOPPED ... backpressure_disconnects=1`，证明慢端越过 `OutboundQueue` 阈值并断开。
- 健康端严格解码：看到 `block_update` / `section_blocks_update`、具体坐标、section 3+4 的水、`system_chat` 的 tick 严格不减，放置/破坏仍成功；服务端完成 320 tick 正常退出。
- 这是 C++ 真实 socket 证据，不是原版协议差分；原版 N-04 未跑。

### A-05：重复输入、双观察者、湿/干共享格

- **重复外部输入**：保留 `externallyRearmedInputIsBounded`，每窗口显式切换源，预热 20 刻后两个独立窗口都有事件、至少 2 个位置/2 种事件类型、每窗口上界 ≤64；注释明确它是“外部重复输入”，不是自激。
- **双观察者**：C++ 测试改为正常 `setBlock` 放置两个面对面的观察者（放置产生的 shape update 是夹具的一部分），20 刻内两者各至少 4 次跳变且末态不同。
  真实 26.1 服务端 `validation/vanilla26_1/observer_validation.py` 同一夹具 20 刻记录：A/B 状态按 `(0,0)→(1,1)→(1,1)→(0,0)→…` 周期变化，A 有 7 个边沿、B 有 8 个边沿。**原版与 C++ 都认为该夹具持续振荡**；此前“两个观察者会锁存”的说法是只在 `rawSetBlock` 上探测得到的错误结论，已删除。
- **湿/干共享活塞格**：`wetAndDryPistonSharedCell` 把含水铁轨放在 `(1,64,0)`，用真实支撑变化触发 `BaseRailBlock.updateShape` 的水刻（due T5），活塞事件在 T1 移动同一格。逐刻断言：湿态在 T5 对 `(1,64,0)` 记录 `fluid_tick_skipped`（该格已是移动活塞/活塞头），干态不派发任何流体刻；两侧活塞写 trace 逐刻完全相同；普通完成把目的地 `(2,64,0)` 的铁轨 `waterlogged` 清为 false。
- 该夹具的**原版逐刻** P-01 未跑；原版依据是 `PistonMovingBlockEntity.tick` 完成路径（见第 5 节依赖）。

### 原版 runner 修复与实跑

`validation/vanilla26_1/fluid_validation.py`：

- 每次运行使用唯一目录 `build-fluid-validation/fluid-runs/<timestamp>-<pid>-<uuid>`（构建产物目录已被 `.gitignore` 的 `/build*` 覆盖），不再复用固定世界。
- `pick_port()` 先 bind 一个 loopback 临时端口再使用，避免固定端口碰撞。
- 采样按 `time query gametime` 精确同步；`tick step` 必须恰好到达目标刻，过冲直接失败。
- 修复了“区块已 loaded 但尚未 tickable”导致首刻延迟的问题：T0 前先 `tick step 100`，再定义 T0；若首刻仍被延迟，与黄金的逐刻比较会失败，而不是静默写黄金。
- 默认只写运行目录（`server.log`、`fluid-vanilla-states.candidate.json`、`run-manifest.json`）；只有 `--promote-golden` 才写仓库黄金，已有黄金还须 `--force-golden`。
- JAR SHA-1 在运行前核对为 `3872a7f07a1a595e651aef8b058dfc2bb3772f46`。

实跑结果：候选 117 条观测与现有 `fluid-vanilla-states.json` **全部一致**（`candidate matches golden over 117 observations`），T0=121（含 100 刻预热），原始日志在当次运行目录。C++ 侧 `fluid_differential` 继续通过。

## 2. P0 覆盖矩阵

标记：A=源码对照，B=C++ CTest，C=原版 JAR 离线探针，D=真实 26.1 服务端差分。

| 用例 | A | B | C | D | 说明与下一步 |
|---|---|---|---|---|---|
| F-01/F-02 状态映射 | 有 | 有 | 部分（共享面） | 未跑 | 需要原版 runtime 全状态导出；现有 32 条水/岩浆 ID 已在 `wire_tests` 钉住。 |
| Q-01…Q-06 队列 | 有 | 有 | 无 | **阻塞** | 控制台无法注入任意 ScheduledTick/BlockTick；需要扩展现有 `ExactAgent`，下一步是 queue hook。 |
| W-01 水平水槽 | 有 | 有 | 无 | **有** | 117 格观测全一致。 |
| W-02 竖井下落 | 有 | 有 | 无 | 未跑 | C++ `waterfall` 覆盖；原版夹具可用同一 console 路线补。 |
| W-03/L-02 源转换 | 有 | 有 | 无 | 未跑 | 需先补半砖/台阶注册和支撑形状。 |
| W-04 向下/侧流计数 | 有 | 有 | 无 | 未跑 | C++ 聚焦用例覆盖，原版需队列 trace。 |
| W-05 坡度 | 有 | 有 | 无 | 未跑 | 同上。 |
| W-06 共享面 | 有 | 有 | **有**（reviewer JAR） | 未跑 | C++ 纯形状互补对已覆盖；半砖/台阶方块级仍是依赖缺口。 |
| W-07 冲毁/含水目标 | 部分 | 部分 | 无 | 未跑 | 红石粉等未注册，按 unsupported 登记。 |
| L-03 岩浆 RNG | 有 | 有 | 无 | 未跑 | 需原版命中样本；两端 RNG 独立。 |
| L-04 FAST_LAVA | 有 | 无（维度属性未接） | 无 | 未跑 | 维度环境依赖。 |
| M-01 水岩浆接触 | 有 | 有 | 无 | **有** | 源岩浆→黑曜石、流动岩浆→圆石，T0/T1/T2/T10 × 7 格共 56 格一致（`fluid_mixing_differential`）。 |
| M-02 向下成石 | 有 | 有 | 无 | 未跑 | 需要原版容器/带水宿主分支。 |
| M-03 玄武岩 | 有 | 有 | 无 | 未跑 | 方向优先顺序的独立原版夹具未做。 |
| H-01/H-02 含水宿主 | 有 | 有 | 无 | 未跑 | C++ 铁轨/墙/珊瑚扇覆盖；原版放置/破坏夹具未跑。 |
| P-01/P-02 活塞 | 有 | 有 | 无 | 未跑 | C++ 有移动含水铁轨共享格；原版逐刻 `movedState`/进度需 agent。 |
| P-03/P-04 红石/相位 | 有 | 有 | 无 | 未跑 | 与 A-05 湿干对照部分重叠。 |
| A-01/A-02 存档恢复 | 有 | 有 | 无 | 未跑 | C++ `storage_tests` 覆盖导入/队列；原版“保存—重启—同副本”未跑。 |
| A-03/A-04 边界/投影 | 有 | 有 | 无 | 未跑 | 原版加载票/实体就绪夹具未接。 |
| A-05 稳定/循环/湿干 | 有 | 有 | 无 | **部分** | 稳定/交汇/断源 + 重复输入 + 湿干共享格为 B；双观察者为 D。 |
| N-01/N-02/N-04 协议 | 有 | 有 | 无 | 未跑 | C++ 解码/真实 socket；未与原版录包逐帧比较。 |
| N-03/N-05 桶/混合 | 未实现 | 无 | 无 | 未跑 | 桶、玩家状态与 level event 未接。 |

## 3. 真实原版夹具的原始证据与命令

### W-01 封闭水槽（D）

```powershell
python validation/vanilla26_1/fluid_validation.py
```

脚本在 `build-fluid-validation/fluid-runs/<unique>/server.log` 留下原始日志，并在同目录写候选 JSON 与 manifest；默认不覆盖仓库黄金。黄金见 `validation/vanilla26_1/fluid-vanilla-states.json`。本次结果：117/117 一致。

### M-01 混流（D）

```powershell
python validation/vanilla26_1/fluid_mixing_validation.py
```

原版 26.1 结果（两个隔离格、7 个采样格、10 刻）：

- 源岩浆 + 北侧水：T1 起岩浆格为 `obsidian`，水格保持 `water0`，T10 仍稳定。
- `lava[level=2]` + 北侧水：T1 起为 `cobblestone`，水格保持 `water0`，T10 仍稳定。
- 完整逐刻 JSON 在运行目录；仓库内的 `fluid-vanilla-mixing.json` 是压缩后的 transition + 端点黄金，`fluid_mixing_differential` 逐格比较 56 格。

### 双观察者（D）

```powershell
python validation/vanilla26_1/observer_validation.py
```

20 刻记录（A=东观察者，B=西观察者，均正常 `setblock` 放置）：A 有 7 个边沿、B 有 8 个边沿，周期约 8 刻；C++ `facingObserversOscillate` 同夹具 20 刻各 ≥4 个边沿，测试通过。原版结果保存在当次运行目录的 `server.log`。

### 整服计时（D，边界不同）

```powershell
python validation/vanilla26_1/fluid_sprint_validation.py
```

W-01 水槽，`tick sprint 1000`，三次重复：约 **11559/14065/14835 tick/s，0.09/0.07/0.07 ms/tick**；`tick query` 的 P50/P95/P99 为 0.0/0.1/0.3、0.0/0.1/0.4、0.0/0.1/0.2 ms。**这是整服**，包含原版光照、随机刻、保存循环与网络事件循环。

## 4. 性能实测与边界

### 4.1 C++ 聚焦（不是整服）

同一 W-01 水槽、1000 tick、MSVC Release + jemalloc；`fluid_differential_probe` 计时的是 `World::tick`：

| 重复 | mean_ms | median_ms | P95_ms | max_ms | >50 ms | 活跃刻 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 0.0053 | 0.0001 | 0.0002 | 0.1008 | 0 | 1000 |
| 2 | 0.0052 | 0.0001 | 0.0002 | 0.1001 | 0 | 1000 |
| 3 | 0.0108 | 0.0001 | 0.0002 | 0.0989 | 0 | 1000 |

三次 `BENCH` 的 `fluid_dispatches=12`、`fluid_writes=6`、`pending_fluid=0`：1000 tick 里只有首轮水槽传播的 12 次派发/6 次成功写入，之后水槽静止，无待执行流体刻。计数是 `World::tickFluidAt` 派发数和流体 tick 内成功 `setBlock` 数，不是整服网络写入。

C++ dedicated server 的 `--fluid-basin` 1000 tick：`STOPPED mean_ms≈0`、`max_ms=0.3`（服务器输出只保留整轮均值/最大值，没有逐刻 P95；不作为与下表的百分比依据）。

### 4.2 原版整服（W-01）

见 §3 的 `fluid_sprint_validation.py`：三次重复 0.09/0.07/0.07 ms/tick，P50/P95/P99 最好/最差范围 0.0/0.1/0.2–0.4 ms。两侧输入相同，但计时阶段不同：C++ 是 `World::tick`，原版是整服。**因此本轮不按 10% 线作结论**；要作结论，需给 C++ 加上同阶段的网络/光照/保存边界，或将原版探针缩到 fluid stage。

### 4.3 干态基准

```powershell
build-f7-nmake\redstone_tick_benchmark.exe build-review-f2\workload.litematic 3000
```

三次同机重复的 `TOTAL`：`mean_ms=12.176/13.051/12.230`，`median_ms=6.807/9.299/7.551`，`p95_ms=60.286/65.448/63.323`；`FINAL entities=1197 scheduled=1197 moving_pistons=1995 events=0`。这是历史干态投影的 C++ 基线，**不是**与原版同阶段的对跑；机器负载和投影输入未做逐项校正，不能据此声称 5% 回归/提升。

## 5. 已知依赖与未跑项（逐项）

1. **Q-01…Q-06**：需要 Java agent 在 `LevelTicks.schedule` / `tickFluid` 注入并记录，纯 console 无法构造任意 due/priority/subTick；现有 `validation/safesave_order/vanilla/ExactAgent` 可扩展，但本轮未改 agent。
2. **W-02/W-03/W-04/W-05/W-07、L-03/L-04、M-02/M-03、H-01/H-02、P-01/P-02/P-03/P-04、A-01/A-02/A-03/A-04**：需要各自的隔离世界与逐刻 trace；本轮只对 W-01、M-01 和双观察者做了真实运行。未跑的项不以 C++ 结果冒充 D。
3. **N-01/N-02/N-04 原版协议帧**：没有启动原版客户端/录包；C++ 解码和 socket 证据只证明本仓库路径。需要原版连接或代理录包才能逐帧对照。
4. **W-06 方块级半砖/台阶**：C++ 未注册；纯形状合并路径已覆盖，方块级仍是依赖缺口。
5. **性能同阶段**：C++ 没有原版光照、随机刻、保存和网络 event loop；原版也没有 C++ `World::tick` 的裸调用。没有匹配边界前不写倍率、不按 10% 判定。
6. **干态世吞整服对跑**：需要原版可加载同一投影的等价世界、相同区块资格与工作量；当前 C++ 投影输入和原版世界不可直接互认，按 blocker 保留 C++ 历史基准。
7. **协议桶/玩家状态/level event**：N-03/N-05 未实现。
8. **`fluid_ticks` 写回**：仍不在范围；本文件只验收读入/运行时队列。

## 6. 行为差异表与可能的高于原版热路径

| 行为/路径 | 26.1 依据 | 本仓库 | 证据/判定 |
|---|---|---|---|
| tick 阶段顺序：方块刻 → 流体刻 → 区块广播 → 事件 → 实体/BE | `ServerLevel.tick`；`src/engine.cpp:1725-1762` | 同序；N-02 用真实阶段和帧序钉住 | B（N-02），A |
| 普通方块变化在广播时读当前位置、事件即时发送 | `ChunkHolder.broadcastChanges`；`src/net/session.cpp:665-725` | 同语义；单格/区段两形式，同格去重 | B（N-02） |
| 区块包 local 位序 | `SectionPos.sectionRelativePos`；`src/net/session.cpp:64-67` | `x<<8｜z<<4｜y`；N-02 解码按此 | B |
| 移动活塞完成清 `waterlogged` | `PistonMovingBlockEntity.tick`；`src/piston.cpp:624-642` | 同；A-05 共享格夹具 | B |
| 水/岩浆接触 | `LiquidBlock.shouldSpreadLiquid`、`LavaFluid.spreadTo`；`src/fluid_flow.cpp:391-632` | 源→黑曜石、流动→圆石 | D（M-01 56 格） |
| 含水宿主子集 | `SimpleWaterloggedBlock`；墙/铁轨/珊瑚扇 | 部分宿主；桶/玩家/容器未接 | A+B，D 未跑 |
| 观察者行为 | `ObserverBlock.tick/updateShape` | 正常放置持续振荡 | D（20 刻）+B |
| 光源/保存/网络 event loop | 原版整服 | C++ 无光照、无写回、网络只在本仓库 | 推断：C++ dedicated tick 不能与整服直接比 |

可能高于原版的热路径（本轮没有逐项采样，均标“推断”，除 W-01 的 `World::tick` 实测）：

- `Session::flushBlockUpdates` 每 broadcast 对 `changedBlocks_` 做一次 `std::sort`（`src/net/session.cpp:665-680`），再逐 section 建 `SectionChanges`；原版 ChunkHolder 用 fastutil 每 section `ShortOpenHashSet`。大量脏格时 C++ 的 `O(n log n)` 排序和 `ChangedBlock` 结构复制可能更贵。本轮 N-02 的夹具只覆盖小数百格，未做量化。
- `World::setBlock` 的观察器/`Update` 队列与 `BlockState` 复制（`src/engine.cpp:1059-1126`）；干态 3000 tick 基准包含此路径，但没有与原版拆分比较。
- `prepareChunkSnapshot`/`snapshotChunk` 对区块 palette 的复制（`src/net/wire.cpp`、`src/net/session.cpp`）；只在发区块时发生，本轮未计时。
- `tickFluidAt` 当前只在 `traceUpdates` 时写 log（`src/engine.cpp:1655-1691`）；常规路径不分配 trace，但每次派发仍有函数调用/队列遍历，W-01 实测 1000 tick 的 `World::tick` 已包含它。
- `PistonTickerSlot`/`PosMap` 的每 tick 扫描（`src/piston.cpp:600-653`）；干态投影有 1995 个移动活塞，属于实测负载的一部分。

## 7. 构建与 CTest

```powershell
# 本机 DSH 沙箱下 build-jemalloc 为只读；本轮用独立 NMake 构建目录。
$env:TMP = "$PWD\build-f7-nmake\tmp"; $env:TEMP = $env:TMP
cmake -S . -B build-f7-nmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release `
  -DREDSTONE_ALLOCATOR=jemalloc -DREDSTONE_BUILD_BENCHMARKS=ON `
  -DREDSTONE_BUILD_VANILLA_SERVER=ON -DREDSTONE_BUILD_CLIENT=OFF `
  -DBUILD_TESTING=ON
cmake --build build-f7-nmake
ctest --test-dir build-f7-nmake -C Release --output-on-failure
```

结果：**22/22 通过**，22.13 s（干净重建后；增量轮曾为 18.39 s）；包含 `fluid_differential`、`fluid_mixing_differential`、`protocol`、`protocol_socket`、`protocol_backpressure`、`storage`。`storage_tests` 在本沙箱下需要把 `TMP/TEMP` 指到工作区内（默认 `dsh-*` 临时目录对新建进程不可写），这是环境约束，不是测试失败。
