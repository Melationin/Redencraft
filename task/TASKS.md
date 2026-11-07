# 任务交接

流程与状态定义见 [AGENTS.md](../AGENTS.md) 第 3 节；任务顺序见 [PLAN.md](../PLAN.md)。
本文件只记录**当前任务**，历史任务在 git 提交记录里查。

---

## M0-1 技术形态对齐与协作流程落地

### 目标

把项目定位从「单维度、只读、无生成、无加载票、无光照的红石模拟器」改为「对齐 Minecraft Java 26.1 的 C++ 服务端」，并建立 coder / reviewer 的交接流程。

### 范围

1. `AGENTS.md`：第 1 节技术形态改为「目标系统表 + 当前子集边界」；新增第 3 节协作流程（五态、职责、子智能体用法）。
2. `PLAN.md`：新建分期路线（流体 → 光照 → 时间与保存 → 加载票 → 多维度 → 世界生成）。
3. `task/TASKS.md`：新建本交接文件。
4. 同步定位表述：`README.md`、`docs/compatibility.md`、`docs/entities.md`、`docs/handover.md`、`docs/client.md`、`docs/protocol775.md`、`docs/fluid-testing.md`、`validation/safesave_order/README.md`。
5. 删除根目录下重复的空 `TASKS.md`（实际交接文件是 `task/TASKS.md`）。

### 源码依据

本任务只改文档，不改行为。判断「当前实现边界」的代码依据：

- `include/redstone/engine.hpp:192-216`：`World` 的高度范围由构造参数固定，单实例即单维度。
- `include/redstone/storage.hpp:13-31`：`LoadedWorld` 只持有一份 region 数据，`loadAnvilWorld` 只读不写。
- `src/engine.cpp:1541`：`// TODO: random ticks, fluid ticks, weather, and world systems are absent.`
- `src/engine.cpp:1542-1570`：`World::tick` 的实际阶段顺序（不含流体刻、不含光照）。

### 验收判据

1. `AGENTS.md` 的「目标形态」与「当前实现边界」两段互不矛盾，且边界每一条都能在上面的代码位置找到对应。
2. `docs/`、`README.md` 中不再把「单维度 / 只读 / 无生成 / 无加载票 / 无光照」写成**项目定义**，只能写成当前阶段状态，并指向 `AGENTS.md` 第 1 节的目标形态。
3. 不修改 `src/`、`include/`、`tests/`、`CMakeLists.txt`；未触及代码，因此本任务不要求重跑 CTest（若触及代码则必须重跑）。
4. 五态流程与子智能体用法写入 `AGENTS.md`。

### 计划改动

- 重写：`AGENTS.md`
- 新建：`PLAN.md`、`task/TASKS.md`
- 定位表述：`README.md`、`docs/compatibility.md`、`docs/entities.md`、`docs/handover.md`、`docs/client.md`、`docs/protocol775.md`、`docs/fluid-testing.md`、`validation/safesave_order/README.md`
- 删除：根 `TASKS.md`（空文件、重复）

### 实际做了什么

1. **重写 `AGENTS.md`**
   - 第 1 节「技术形态」改成目标系统表（多维度 / 加载票 / 世界生成 / 世界保存 / 时间 / 光照），并单独列出「这些系统目前都还没有实现」的当前子集边界，每条边界都能对应到具体代码位置。
   - 第 2 节保留原有通用约束，只把「分别处理」一条补成「方块计划刻、流体计划刻」。
   - 第 3 节新增 coder / reviewer 协作流程：五态定义与写入者、双方职责、进入下一任务的条件、子智能体派发规则（可派子智能体判断 `task/TASKS.md` 状态；reviewer 默认由子智能体承担且只读）。
2. **新建 `PLAN.md`**：阶段 0 文档对齐 → 阶段 1 流体（F-1…F-7）→ 阶段 2 光照（L-1…L-4）→ 阶段 3 时间与保存（W-1…W-4）→ 阶段 4 加载票（T-1…T-3）→ 阶段 5 多维度（D-1…D-3）→ 阶段 6 世界生成（G-1…G-4），每条带源码依据与验收口径。
3. **新建 `task/TASKS.md`**（本文件），并删除根目录下重复的空 `TASKS.md`。
4. **把 `docs/fluid-testing.md` 纳入版本管理**：它是此前未跟踪的流体测试规格，现在 `PLAN.md`、`docs/compatibility.md`、`docs/entities.md` 都链接到它，不提交会让文档链接指向不存在的文件。
5. **同步定位表述**，把「单维度 / 只读 / 无生成 / 无加载票 / 无光照」从项目定义降级为「当前阶段状态」：
   - `README.md`：开头的项目目标陈述；「本批范围」中只读导入与流体刻两条。
   - `docs/compatibility.md`：文件开头加「本文记录当前已实现边界，不是项目目标」；流体段改成「当前阶段排除、阶段 1 恢复」。
   - `docs/entities.md`：开头范围、液体高度近似、铁轨 `waterlogged` 三处。
   - `docs/handover.md`：文件开头加 2026-11-08 方向变更说明（下面的历史记录不改写）。
   - `docs/client.md`：「仍不保存文件」一句。
   - `docs/protocol775.md`：不生成缺失地图、区块自动加载、无保存三处。
   - `docs/fluid-testing.md`：单维度/只读/无生成/无加载票那一段。
   - `validation/safesave_order/README.md`：记录口径与「不在当前目标内」的排除项。

**明确没有做的事**：

- 没有改动任何 `src/`、`include/`、`tests/`、`CMakeLists.txt`，本任务不产生任何行为变更。
- 没有实现流体、光照、保存、加载票、多维度、世界生成中的任何一项；它们仍是 `PLAN.md` 里的待办。
- 没有改写 `docs/handover.md` 的历史记录，也没有改 `docs/update-order-audit.md` 的审查结论。

### 构建与测试

本任务没有改代码，`build-jemalloc` 不需要重新编译。仍然跑了现有套件确认基线未受影响：

```powershell
ctest --test-dir build-jemalloc -C Release --output-on-failure
```

结果：**15/15 通过**（protocol、protocol_socket、protocol_backpressure、safesave_order_parity、engine、entity、piston、storage、litematic、allocator、shapes、client_inventory、command 等）。

口径说明：这只是「C++ 回归测试保持通过」，不是与原版等价的证据；本任务没有跑真实 26.1 服务端差分。

### 提交

- 主提交：`6e7ab6d` — Adopt full-vanilla technical target and coder/reviewer task workflow
  （11 个文件，+652 / -18；含 `AGENTS.md`、`PLAN.md`、`task/TASKS.md`、`docs/fluid-testing.md` 与 7 个文档的定位表述）
- 状态记录提交：见本行记录之后的 bookkeeping 提交（只改本文件的「提交」段与末尾状态）。

### Reviewer 审查意见

审查提交 `6e7ab6d` 及交接记录 `731606e`。Luna 只读状态判定与主线程复核一致，交接进入审查前的末行是唯一的 `任务完成`，必填项齐全。提交差异只涉及文档，当前基线的 `build-jemalloc/Testing/Temporary/LastTest.log` 记录了 15 项通过、0 项失败。本次是文档审查，引用既有 CTest 记录。

1. **阻塞 · 交接通过路径会产生歧义。** `AGENTS.md:50` 以最后一行定义状态，`AGENTS.md:74` 同时要求追加 `任务已完成` 结论行和设置相同的末行，`AGENTS.md:78` 又以文件中出现该字样作为领取下一任务的条件。照字面执行可能出现两条独立状态行，或把正文里的引用误判成验收通过。请规定验收理由写普通正文，唯一的独立状态行放在最后；所有角色切换和领取条件都读取同一末行状态。正常验收与异常恢复流程分别写清。
2. **应修 · 时间目标仍使用旧版模型和规则名。** `AGENTS.md:15` 使用 `dayTime` / `doDaylightCycle` 描述 26.1，`AGENTS.md:24` 又把 `level.dat` 的 `DayTime` 当成当前缺失点。原版 `world/level/gamerules/GameRules.java:24` 注册 `advance_time`，`world/clock/ServerClockManager.java:27`、`:65` 管理 `world_clocks` 与推进规则，`server/commands/TimeCommand.java:48` 使用默认 World Clock。C++ 的 `src/storage.cpp:479`、`:513` 已读取 `level.dat` 的 `Time` 并恢复 `gameTime`。请把目标写成 `gameTime` 与 26.1 World Clocks，当前边界写明已恢复并内存推进 `gameTime`，时钟和持久化待实现。路线图 W-1 已按原版术语更新。
3. **应修 · 定位文档链接及入口信息有冲突。** `docs/compatibility.md:59` 的 `PLAN.md` 相对链接解析到不存在的 `docs/PLAN.md`，应改为 `../PLAN.md`。`docs/handover.md:24` 的「下一步」仍写「水/流体刻仍不做」，与阶段 1 相冲突。请标明该段是历史记录，并将当前计划指向 `PLAN.md`。`docs/compatibility.md:70` 也可随本次同步区分已实现的 `/tick` 命令骨架和待补的命令系统，见 `src/command.cpp:310` 和 `docs/protocol775.md:59`。

### 后续规划与测试要求

以下是 reviewer 已更新的后续规划。用户已指定直接领取 F-1。

- **F-1 流体状态模型。** 按 [流体测试规格 §4.1](../docs/fluid-testing.md#41-状态和调度) 的 F-01/F-02 状态映射部分建立聚焦测试。水、岩浆各枚举 `level=0..15`，断言空流体、精确类型、同族关系、amount、`isSource`、`isFull`、固有高度和 legacy 映射。`falling` 放在 `FluidState`，由方块 `level` 解码；导入的 `level=9..15` 保留原值，显式生成 legacy 方块时才归一到 `8`。断言同一查询不会写方块或发计划刻，所有水/岩浆方块状态继续命中现有 26.1 协议表。依据 `LiquidBlock.java:68`、`:123`、`:242` 和 `FlowingFluid.java:55`、`:463`。跨格液面随 F-3，含水宿主随 F-5。
- **F-2 至 F-6 随任务交付回归。** `PLAN.md` 已逐项映射流体测试编号。优先用 Q-01…Q-06 锁定精确身份、两队列相位和积压队首。传播与接线再分别用 W/L/M、H/P 两组验证。原版 `FenceGateBlock.java:203` 只定义朝向、开关、受电及 `in_wall`，栅栏门用于通流几何对照。Anvil 与投影分别按 A-01…A-04 验证，采用各自的基准夹具。
- **F-7 原版对照。** 按 [流体测试规格 §3](../docs/fluid-testing.md#3-制作原版对照) 制作隔离的 26.1 原版采集夹具，从官方 JAR 采集块/流体状态、schedule/dispatch、写序和关键副作用。逐刻报告首差；原版离线 JAR、真实服务端差分及 CTest 分开记录。W-01 水槽作为首个可复跑端到端夹具。

**结论**　用户决定结束 M0-1 的本轮审查，直接进入 F-1。上面的三条意见尚未修复，已转入路线图的后续文档校正任务。此处的末行状态用于释放下一任务的领取条件。

### 独立复核（reviewer 2）

说明：本节由第二个 reviewer 在发现本文件已被主线程更新为「任务已完成 + F-1」后追加，只登记独立复核结果，不替代上面的结论，也不改变末行状态；补充项归入后续文档校正。

- **代码依据（全部属实）**：`include/redstone/engine.hpp:192-216` 的 `World(int minY=-64, int maxY=320, ...)`、`kSectionCapacity=24` 与 `:201-202` 的 `loadChunk`/`unloadChunk`；`include/redstone/storage.hpp:13-31` 的 `LoadedWorld` 只有一份 `blocks`/`chunks`，`loadAnvilWorld` 只读，`src/storage.cpp`、`src/litematic.cpp` 无写回路径；`src/engine.cpp:1541` 的 TODO 与 `:1542-1570` 的 `World::tick` 一致（方块刻 → 广播 → 实体跟踪 → 方块事件 → 实体 → 方块实体，无流体刻、无光照）；`src/net/wire.cpp:562-578` 的天空光全 255、方块光 0 是占位。`AGENTS.md:20-24` 可在这些位置找到对应；其中 `:15`/`:24` 的时间表述已在本次 AGENTS.md 更新里改为 `Time` + `world_clocks`，与 `src/storage.cpp:479`/`:513` 一致（该项已解决）。
- **链接复核**：以 UTF-8 重新扫描本次 11 个文件，唯一断链是 `docs/compatibility.md:59` 的 `[路线图](PLAN.md)`，应改为 `../PLAN.md`；其余新增链接均可解析。另：`docs/fluid-testing.md:33` 等 7 处 `../../net/...` 只在本工作区布局成立，已跟踪文档应说明该前提。
- **历史与测试**：`docs/handover.md` 只加开头说明，历史章节未改；`docs/update-order-audit.md` 未动；`validation/safesave_order/README.md` 只调整时态/范围，七项结论未改，未发现历史结论被改写。`build-jemalloc/Testing/Temporary/LastTest.log` 记录 15 项通过、0 项失败（04:58:52，早于提交 04:59:16），与 15/15 说法一致；本任务无代码改动，按 `AGENTS.md:41` 不重跑构建可接受，且已注明不是原版等价。
- **`docs/fluid-testing.md` 入版本库**：理由成立（`PLAN.md` 的 F-1/F-7、`docs/compatibility.md:59`、`docs/entities.md:34` 都链接它），作为已存在文件放进 M0-1 可以接受；更干净的做法是先原样提交再改定位段，便于 60KB 内容可 diff。
- **补充流程问题（0 应修 + 1 建议）**：`PLAN.md:5` 已改为「每个任务一次主提交；状态记录提交不计入」，与 `AGENTS.md:67` 一致，原冲突已解决。剩余建议：`AGENTS.md:68` 未说明返工内容追加在哪里（状态行应始终最后一行）。`AGENTS.md:89` 的纠错写入者已明确为主智能体；`:87` 已改为「只读代码」；`:100` 与 `:105` 的冲突已由「本条是 §3.7 第二条的唯一例外」解决。
- **AGENTS.md/PLAN.md/docs 更新与 M0-2 覆盖**：本轮更新（`3d5af1e`）已解决：§3.1（`AGENTS.md:50`）唯一裸状态行；§1（`:15`/`:24`）26.1 World Clocks；§3.5（`:87`/`:89`）reviewer 措辞与主智能体纠错写入；§3.6（`:100`）reviewer 优先的 §3.7 例外；`PLAN.md:5` 主提交/状态记录提交分离；`docs/compatibility.md:59` 断链、`:70` 命令边界、`docs/handover.md:24`、`README.md:1`、`docs/protocol775.md:55`。我确认 `task/TASKS.md` 只有末行 `任务已完成` 是裸状态行，符合 §3.1。`PLAN.md:46-48` 的 M0-2 遗留清单仍列了本轮已修的项（AGENTS 纠错写入、reviewer 措辞、handover 交叉引用、README/协议表述），应同步收窄到唯一未修的 `AGENTS.md:68`（返工内容追加位置），避免重复完成。

**复核结论**：通过（无未解决应修）。`3d5af1e` 已修复主审查的阻塞、时间应修、断链/命令边界/handover 与 README/协议表述；M0-1 判 `任务已完成` 并释放 F-1 符合 §3.6。仅剩 1 条建议（`AGENTS.md:68` 返工追加位置），请把 `PLAN.md:48` 的 M0-2 遗留清单收窄到它。

### 下一任务交给 coder · F-1 流体状态模型

- **目标和范围**　实现独立的 `Fluid` / `FluidState` / `Fluids` 和水、岩浆方块到流体状态的查询。仅交付 F-1 的状态模型与聚焦测试。方块 `level=0..15` 保持 26.1 schema，`falling` 由状态解码。
- **原版依据**　`LiquidBlock.java:68`、`:123`、`:242`，`FlowingFluid.java:55`、`:463`，以及 `FluidState.java`、`WaterFluid.java`、`LavaFluid.java`。
- **验收**　按 [F-01/F-02](../docs/fluid-testing.md#41-状态和调度) 枚举水和岩浆全部档位，检查精确类型、同族关系、amount、`isSource`、`isFull`、`falling`、固有高度、legacy 映射及协议状态表。导入的 `9..15` 保留原值。查询无方块或队列副作用。构建并运行 CTest，记录证据口径。
- **预计触及**　`include/redstone/` 和 `src/` 的流体模型与状态查询，相关 `tests/`、必要的 `CMakeLists.txt` 接线和流体文档。coder 领取时列出确切文件并更新末行状态。

任务已完成
