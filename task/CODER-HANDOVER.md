# Coder 交接（F-5 返工已提交，等待审查）

> **这份文件是 coder 写给下一个 coder 会话的交接记录**，不是 reviewer 的审查记录。
> 审查意见的唯一权威位置是 [`task/TASKS.md`](TASKS.md) 的「审查记录」小节；两者冲突时以 `TASKS.md` 为准。
> 本文件在 F-5 通过后可以删除。

最后更新：2026-11-08 20:0x（四条阻塞返工完成并提交）

---

## 1. 当前状态快照

| 项 | 值 |
|---|---|
| 分支 | `main` |
| HEAD | `f662194` — task: record the F-5 rework commit 1e11490 |
| 上一个代码提交 | `1e11490` — rework F-5（14 文件，+547 / -41） |
| 工作树 | 干净 |
| `task/TASKS.md` 末行 | `任务已完成` |
| CTest | **20/20 通过**（在 `1e11490` 上验证） |
| 构建目录 | `build-jemalloc`（MSVC Release + jemalloc，`-DREDSTONE_BUILD_BENCHMARKS=ON` 已开） |

**下一步**：等 reviewer 对 `1e11490` 出结论。F-6 要等 F-5 被判定 `任务通过`。
四条阻塞的修复位置、新测试与 4 条变异验证记录在 `task/TASKS.md` 的「返工」小节。

---

## 2. 已通过的任务（F-1…F-4）

| 任务 | 状态 | 提交 | 内容 |
|---|---|---|---|
| F-1 流体状态模型 | 通过 | `53a5d20`…`40fcecc` | `Fluid`/`FluidState`/`Fluids` 五单例、`fluidStateOf` 的 level 0..15 解码 |
| F-2 流体计划刻队列 | 通过 | `3c7e360`…`4756fb0` | 独立 `LevelTicks`、精确身份派发、65536 预算 |
| F-3 `FlowingFluid` 传播 | 通过 | `f2201fc`…`efd54ac` | `spread`/`getNewLiquid`/`getSlopeDistance`/`canPassThroughWall`/`getFlow`/`getHeight` |
| F-4 水与岩浆参数差异 | 通过 | `ba6484c`…`8f67143` | 流体标签、源转换开关、岩浆随机延迟、遇水成石/黑曜石/玄武岩 |

F-5 首轮实现（`f4f1b2c`）**被退回**，仍在返工中。

---

## 3. F-5 的四条阻塞（已在 `1e11490` 逐条修复）

> 本节保留首轮退回时的原始描述，作为「当时要修什么」的备忘；**实际改法与证据见
> `task/TASKS.md` 的「返工（针对 `f4f1b2c` 的四条阻塞）」小节**。

权威原文见 `task/TASKS.md` 的「审查记录（`f4f1b2c`）」。摘要与要点：

### 阻塞 1 — 含水容器没接进传播路径

- **事实**：`src/fluid_flow.cpp` 里 `isWaterloggedHost` 已定义但**没有任何调用点**；`canHoldAnyFluid` / `spreadTo` 仍按普通方块处理。
- **后果**：流动水会直接冲掉干铁轨；墙与枯珊瑚扇无法接收源水。
- **依据**：`FlowingFluid.java:410-435`（`canHoldSpecificFluid` 走 `LiquidBlockContainer.canPlaceLiquid`）、`FlowingFluid.java:271-279`（`spreadTo` 先试 `placeLiquid`）、`SimpleWaterloggedBlock.java:18-35`。
- **要做**：
  - `canHoldAnyFluid`：`isWaterloggedHost(state) → true`（容器总能容纳），否则退回 `!blocksMotion`。
  - 新增 `canPlaceLiquid`：宿主且**未**含水，且流体是**精确水源**（水标签 + `isSource()`）。
  - 新增 `placeLiquid`：`setBlock(pos, state.with("waterlogged","true"), 3)`，返回 `true`。
  - `canHoldSpecificFluid`：宿主走 `canPlaceLiquid`，其余走 `canHoldAnyFluid`。
  - `spreadTo`：目标若是含水宿主且自身是水，调 `placeLiquid` 并 `return`（不替换方块）。
  - 测试：源水成功、流水拒绝、岩浆拒绝、重复注水不重写、去重断言。

### 阻塞 2 — 含水宿主的形状排刻与移除保水缺失

- **事实**：`src/engine.cpp` 的 `updateShapeState` 只给 `isLiquidBlock` 接了 `liquidUpdateShape`，宿主走不到；销毁路径（`engine.cpp` 的 `updateOrDestroy`、`src/rail.cpp:572-579` 断支撑）、`src/net/session.cpp:849-865` 创造破坏都直接写空气。
- **后果**：珊瑚扇/铁轨/墙失去支撑后消失且**不排水**，其它方块变化也不能触发宿主重新评估含水。
- **依据**：`BaseRailBlock.java:283-298`、`WallBlock.java:134-151`、`BaseCoralPlantTypeBlock.java:69-86`、`BaseCoralWallFanBlock.java:57-75`、`Level.java:277-299`。
- **要做**：保留各宿主自己的 `updateShape` 入口（不要合并成通用刷新）；普通销毁与移除时若宿主含水，先排水/保水语义再写空气（直接置空气的调用方仍遵从）；补 H-02 与一组宿主 + 活塞联动（P-01/P-02）。

### 阻塞 3 — 玩家放置宿主时强制清除含水初态

- **事实**：`src/net/session.cpp:984-985` 对带属性的宿主一律写 `waterlogged=false`。
- **依据**：`BaseRailBlock.java:139-146`、`WallBlock.java:112-131`、`BaseCoralPlantTypeBlock.java:57-62` —— 按**目标格被替换前的流体**决定初态。
- **要做**：放置入口读取目标格原流体，按各宿主规则初始化（铁轨/墙看是否精确水源；珊瑚看流体标签/满态）；补 H-01 的空气/源水/流水三态对照。

### 阻塞 4 — 同种液体改 level 漏 `onPlace`，排刻身份偏离回调状态

- **事实**：`src/engine.cpp:1081-1084` 用 `!old.sameBlock(state)` 做条件，所以同种液体改水位不触发 `onPlace`；`src/fluid_flow.cpp:712-752` 的三个入口又**重读世界位置**来定刻身份，而原版三条路径各自使用**回调传入的 state**。
- **依据**：`LevelChunk.java:318-341`（未被 flags 512 跳过的写入都调 `onPlace`）、`LiquidBlock.java:148-151,167-179,194-198`。
- **要做**：条件改为「非 flags 512 的状态写入」；三条入口改用传入的 `state`（`fluidStateOf(state)` + `tickDelay()`），不重读 `getBlock`；补测同种液体改 level、flags 512、捕获状态；补测「`setBlock` 回调请求基础延迟」与「随后显式请求」的接受顺序竞争。

---

## 4. 返工顺序建议

1. **阻塞 4** 先做（最小、最独立，只动 `engine.cpp` + 三个入口函数）
2. **阻塞 1** 次之（`fluid_flow.cpp` 的容器语义 + 传播接线）
3. **阻塞 2**（跨 `engine.cpp` / `rail.cpp` / `session.cpp`，面最大）
4. **阻塞 3**（`session.cpp` 放置入口，依赖 1 的 `canPlaceLiquid` 语义）

每做完一条就构建 + 跑相关测试，别攒到最后。

---

## 5. 流程约束（来自 `AGENTS.md` 第 3 节）

- 状态词只有五个：`任务进行中` / `任务已完成`（coder）→ `审查中` / `审查结束` / `任务通过`（reviewer）。
- **末行是唯一状态位**，全文只能有这一行是「裸状态行」；正文提到状态词要加反引号或写在句子里。
- 返工内容追加在 `task/TASKS.md` 的「审查记录」小节**之前**，状态行始终留在文件最后一行。
- 提交后把 commit hash 记进「提交」小节，末行改回 `任务已完成`。
- **不要动 reviewer 的审查记录**（`TASKS.md` 里未提交时也一样，先把它一并提交）。
- 结论措辞：没跑真实 26.1 服务端差分就不能说原版等价。

---

## 6. 工具踩坑记录（血泪）

- **不要用 shell 做 C++ 字符串替换。** Windows PowerShell 里 `-replace` 的大小写不敏感、单引号不解释 `` `r`n ``、内嵌双引号（如 `"waterlogged"`）会直接把脚本解析搞坏。本轮与上一轮多次因此失败，还出现过把 `block.hpp` 整个文件换行吞掉、只能 `git checkout` 恢复的事故。**一律用 `edit` / `write` 工具改代码。**
- `edit` 报 “file changed since it was read” 时，**重新 `read` 再改**，不要改走 shell。
- `Get-Content` 必须带 `-Encoding UTF8`，否则 PowerShell 显示乱码，会误判状态词。
- 读文件用 `-Raw`；`Set-Content -NoNewline` 配数组会把换行全部吞掉。
- **小种子下 `Random::nextInt` 的首次输出高位相关性极强**（seed 0..63 首次 `nextInt(4)` 从不为 0）。测试要覆盖两种分支时，先用同一个流抽样多次，或预热两次再抽样；不要按 seed 逐个重播种取首次值。
- 变异验证必须「改完→构建→跑测试→**从备份恢复**→确认 `git diff` 只剩预期改动」。上一轮忘了恢复 `blocksMotion` 的变异，导致后续测试长期跑在变异代码上。

---

## 7. 流体相关代码地图

| 文件 | 职责 |
|---|---|
| `include/redstone/fluid.hpp`、`src/fluid.cpp` | `Fluid`/`FluidState`/`Fluids`/`FluidRef`/`FluidTag`、`fluidStateOf`（含含水宿主） |
| `include/redstone/fluid_flow.hpp`、`src/fluid_flow.cpp` | 传播算法、`fluidTick`、`fluidFlow`、`liquidShouldSpreadLiquid`、`liquidOnPlace`/`liquidNeighborChanged`/`liquidUpdateShape`、`fluidSpreadDelay`、`canHold*` |
| `src/engine.cpp` | `tickFluidAt`（派发）、`setBlock`（onPlace）、`executeNeighbor`（neighborChanged）、`updateShapeState`（updateShape）、两条 `LevelTicks` 队列 |
| `src/state.cpp` | `forceSolidOn` 名单（栅栏门、移动活塞、墙、枯珊瑚扇）、`Axis` 属性 |
| `tests/fluid*_tests.cpp` | `fluid`（状态模型）、`fluid_tick`（队列）、`fluid_flow`（传播）、`fluid_params`（参数与反应）、`fluid_block`（方块侧接线） |

**已知未覆盖**（F-5 之外，勿混入本轮）：桶交互、实体推流（测试规格 P1）；存档/投影的 `fluid_ticks` 恢复（F-6）；真实 26.1 服务端差分（F-7）。

---

## 8. 恢复工作时的第一条命令

```powershell
cd "F:\source\minecraft-merged-07da9a845a-26.1-sources\redstone-cpp"
git log --oneline -3; git status --short; Get-Content task\TASKS.md -Encoding UTF8 | Select-Object -Last 1
```

确认末行仍是 `任务进行中`、工作树干净后再动手。若 reviewer 在此期间写了新意见（工作树出现 `M task/TASKS.md`），**先读它**再决定动作。
