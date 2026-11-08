# 任务交接

流程与状态定义见 [AGENTS.md](../AGENTS.md) 第 3 节；任务顺序见 [PLAN.md](../PLAN.md)。
本文件只记录**当前任务**。F-1…F-4 已验收，记录在 `53a5d20`…`8f67143`；
F-5 已验收（`任务通过`），实现提交 `f4f1b2c` → `1e11490` → `eb32e7b`，审查记录见 `89dc371`。

> **状态词**：`任务进行中` → `任务已完成`（coder）→ `审查中` → `审查结束` / `任务通过`（reviewer）。

---

## F-6 存档与投影的流体刻

### 目标

把流体刻接入存档与投影：Anvil 的 `fluid_ticks` 按各区块独立列表恢复精确流体身份、相对延迟与优先级，
`-n..-1` 的 subTick 基准按**各自列表**计算；投影的 `PendingFluidTicks` 按格式夹具导入而不是拒绝。
缺失/空名字、未知名字与显式 `minecraft:empty` 分别处理，禁止静默丢弃。

### 范围

1. `importWorld` 恢复 `SavedTickData{fluid=true}` 条目到世界的流体刻队列（目前被 `tick.fluid` 直接跳过）。
2. 方块刻与流体刻的 `-n..-1` subTick 基准**分别**计算（`LevelChunk.java:639-641` 调两个独立的 `LevelChunkTicks.unpack`）。
3. 流体标识解析：显式 `minecraft:empty`、未知名字、空名字三种情况分开处理。
4. Litematic `PendingFluidTicks`：坐标变换、`Time`/`Priority` 字段、精确流体身份。

### 源码依据

- `SavedTick.java:13,28-42,49-51`：`i`/`x`/`y`/`z`/`t`/`p` 六个字段，`unpack(currentTick, currentSubTick)` 返回 `trigger = currentTick + delay`。
- `LevelChunkTicks.java:95-118`：`unpack` 用 `subTickBase = -pendingTicks.size()` 递增，`scheduleUnchecked` 保留重复条目。
- `LevelChunk.java:639-641`：`blockTicks.unpack` 与 `fluidTicks.unpack` 是两个独立容器。
- `SerializableChunkData` 的 `fluid_ticks` 键与 `block_ticks` 同构。

### 验收判据

1. `fluid_ticks` 恢复后 `hasFluidTick` / `fluidTicks()` 能按**精确流体**查到，`flowing_water` 与 `water` 不互相命中。
2. 同一区块内方块刻与流体刻各自的 subTick 基准是 `-N..-1`，两族互不影响；重复条目被保留。
3. 到期刻按 `gameTime + t` 展开；负延迟与已过期条目按原版 `unpack` 直接算出到期刻。
4. 显式 `minecraft:empty` 可被恢复为一条真实身份条目；未知流体名与空名字给出明确错误，不是静默丢弃。
5. Litematic `PendingFluidTicks` 导入后坐标按投影偏移变换，`Time`/`Priority` 与 `Block` 类型保留，未知流体按选项报错。
6. 构建通过、CTest 全绿。

### 计划改动

- 修改：`src/storage.cpp`（`importWorld` 的流体刻恢复与名字校验）
- 修改：`src/litematic.cpp`（`PendingFluidTicks` 导入）
- 修改：`tests/storage_tests.cpp`、`tests/litematic_tests.cpp`
- 修改：`docs/storage.md`、`docs/compatibility.md`

### 实际做了什么

**Anvil `fluid_ticks` 恢复**（`src/storage.cpp` 的 `importWorld`）：

- 之前 `tick.fluid` 条目被直接 `continue` 跳过；现在按 `SavedTick.java:49-51` 的 `unpack` 语义恢复为一条流体刻，`trigger = gameTime + t`。
- **两族 subTick 基准分开**：`block_ticks` 与 `fluid_ticks` 各自维护 `-N..-1` 计数器（`LevelChunk.java:639-641` 调两个独立的 `LevelChunkTicks.unpack`）。共用一个计数器会让流体刻的 order 接着方块刻继续递增。
- 流体身份用 `FluidRef(name)` 解析：显式 `minecraft:empty` 是真身份、可恢复；未知名字与空名字抛 `nbt::Error`，不静默丢弃。
- 去掉 `importWorld` 上已经过时的「流体刻不模拟」TODO。

**Litematic `PendingFluidTicks` 导入**（`src/litematic.cpp`）：

- 之前 `PendingFluidTicks` 非空直接抛 `schematic fluid ticks are excluded`；现在按与 `PendingBlockTicks` 相同的 `Block`/`Time`/`Priority` 记录形状导入，坐标走同一套投影偏移变换，以 `fluid=true` 标记进 `LoadedWorld.ticks`。
- 未知流体名按 `substituteUnsupported` 选项决定「报错」还是「跳过」。

### 测试

`tests/storage_tests.cpp`：

- 既有夹具里的 `{2,82,5} minecraft:water` 条目现在断言 `fluidTickCount()==1`、`hasFluidTick` 命中 `water` 且**不**命中 `flowingWater`、`trigger==102`、`order==-1`。
- 新增混合夹具：同区块 2 条方块刻 + 2 条流体刻，断言 blockOrders 与 fluidOrders **各自**是 `{-2,-1}`，且 `water`/`flowingLava` 的精确身份与延迟（5/6）分别正确。
- 新增显式 `minecraft:empty` 恢复用例。
- 新增未知流体名与空名字两条拒绝用例。

`tests/litematic_tests.cpp`：

- 夹具加入一条 `PendingFluidTicks`（`flowing_water`，`Time=5`，`Priority=0`）；断言投影偏移后的坐标、延迟、优先级与精确身份，并验证导入世界后 `hasFluidTick` 命中流水而**不**命中源水。
- 新增未知流体名用例：默认拒收；`substituteUnsupported=true` 时跳过且只保留方块刻。

### 变异验证

| 变异 | 结果 |
|---|---|
| `importWorld` 恢复流体刻处改回 `continue` | `restored.fluidTickCount() == 1` 失败 |
| 两族共用一个 subTick 计数器 | `fluidTicks[0].first.order == -1` 失败 |
| litematic 未知流体名静默 `continue` | `rejected` 断言失败 |

第三条第一次**没被抓到**：当时只测了「合法流体名能导入」，没覆盖未知名分支。补了拒绝与放行两条用例后重跑变异才失败。每次变异后都从源码恢复并确认 `git diff` 只剩预期改动。

**明确没有做的事**：存档**写回**（`fluid_ticks` 的保存侧）不在本任务范围；真实 26.1 服务端差分（F-7）；桶交互与实体推流（P1）。

### 构建与测试

- 构建：MSVC Release + jemalloc 通过。
- CTest：**20/20 通过**。
- 证据口径：**源码对照 + C++ 回归测试**，未跑真实 26.1 服务端差分，不声称原版等价。

### 提交

- 主提交：`1c629a2` — F-6: restore fluid ticks from Anvil saves and schematics（7 个文件，+222 / -191）

### 审查记录（`1c629a2`）

1. **阻塞｜真实 Litematica 的流体刻读不进来。** `src/litematic.cpp:280–298` 要求 `Block`，`tests/litematic_tests.cpp:80–88` 的手造夹具也使用了同样的字段。Litematica 的 26.1 分支写入和读取 `PendingFluidTicks` 时用 `Fluid`（见 [LitematicaSchematic.java](https://github.com/sakura-ryoko/litematica/blob/edf9ced2af2244d56fc20086883a014bf051b1b8/src/main/java/fi/dy/masa/litematica/schematic/LitematicaSchematic.java#L1472-L1480)），并记录 `SubTick`（`:1549–1557,3187–3192`）。请按实际格式修正并补带投影偏移、负延迟、非零优先级和精确身份的夹具；核对 `SubTick` 是否参与导入的同刻排序，写清策略并断言顺序，独立于 Anvil 的 `-N..-1` 规则。
2. **阻塞｜没有验证 Anvil 读盘到派发的完整路径。** `tests/storage_tests.cpp:401–422,497–526` 直接构造 `LoadedWorld`，可以验证恢复函数，但即使 `src/storage.cpp:277–291` 的真实 `.mca` 读盘路径不再提取 `fluid_ticks`，这些用例也能通过。负延迟和重复流体刻也未在该入口验证。请复用现有磁盘夹具，让至少一个 `fluid_ticks` 列表走 `loadAnvilWorld` → `importWorld` → 派发，断言不同精确身份、逾期与未来到期刻、同区块顺序、重复条目、优先级和实际消费时刻。异常名至少检查一次真实磁盘入口的诊断。
3. **应修｜宽松导入静默丢弃未知流体。** `src/litematic.cpp:289–292` 在 `substituteUnsupported=true` 时直接跳过，`LitematicImport` 也没有对应报告字段；已有宽松实体路径会累计跳过计数（`:209–212`）。建议记录被跳过的流体名字/数量并让调用者能看到，保留默认拒绝路径，覆盖缺失、空名字、未知名称与显式 `minecraft:empty` 的区别。
4. **应修｜存档时间相加可能触发 C++ 有符号溢出。** `src/storage.cpp:555–562` 的 `int64_t gameTime + int delay` 可被 NBT `Time` 的极值触发，26.1 `SavedTick.java:49–51` 的 Java `long` 则按 64 位回绕。建议两族共用按位回绕的计算，增加一条极值用例，并按 `TickPriority.java:byValue` 核对 `p` 边界在读入后的排序。

独立 MSVC Release 构建通过，CTest **20/20 通过**（8.32 s）。本轮依据为 26.1 Java 源码、Litematica 读写源码和 C++ 回归；未运行真实服务端差分或性能对跑。导入格式和磁盘路径覆盖修正后再验收。
### 返工（针对 `1c629a2` 复审的 2 阻塞 + 2 应修）

**阻塞 1 — 真实 Litematica 的流体刻字段是 `Fluid` + `SubTick`**

已 clone 上游源码核对（`build-litematica-src`，sakura-ryoko/litematica）：

- `LitematicaSchematic.java:1442` 写 `PendingFluidTicks` 时 tag 名是 `"Fluid"`（方块刻是 `"Block"`，`:1437`）。
- `:1496-1498` 每条记录除 `x/y/z/Time/Priority` 外还写 `SubTick`（`entry.subTickOrder()`）。
- `:2834` 读取是 `tag.getLongOrDefault("SubTick", 0L)`；`:1804` 只有 `version >= 5` 有该列表；`:2794` 要求 `Time` 为 int。

改动：

- `src/litematic.cpp:292` 改为 `required(tick, "Fluid", 8)`（之前错用 `Block`）。
- `SavedTickData` 增加 `std::optional<std::int64_t> subTick`：**有** `SubTick` 就用它、**没有**才退回原版 `-N..-1`，两套规则不混用。
- 方块刻同样读 `SubTick`（上游两族走同一个 `writePendingTicksToData`）。
- 空名字（结构非法）与未知名字（本 build 不支持的流体）分开报错。

**阻塞 2 — 没有验证 Anvil 读盘到派发的完整路径**

- 真实 `.mca` 夹具补了 `block_ticks`（1 条）与 `fluid_ticks`（6 条）：未来源水、同格流水、已逾期（负延迟）、重复 `(pos, fluid)` 两条、越界优先级 `p=9`。
- 先断言 `loadAnvilWorld` 读出两族（`w.ticks.size()==7`，流体 6 条且各字段正确），再断言 `importWorld` 后的派发队列：`scheduledTickCount()==1`、`fluidTickCount()==6`、同格 `water`/`flowing_water` 各自命中且互不命中、逾期条目 `trigger == 42-9`、重复条目保留 2 条、`p=9` 夹到 3。

**应修 3 — 宽松导入静默丢弃未知流体**

- `LitematicImport` 新增 `skippedUnsupportedBlockTicks` / `skippedUnsupportedFluidTicks`（`name -> count`），与既有实体/方块实体跳过计数同构。
- 宽松路径累计计数而非 `continue` 了事；默认仍拒绝。
- 测试覆盖四态：未知名字（默认拒绝 / 宽松跳过并计数）、空名字（两种模式都拒绝）、缺失 `Fluid` 键（拒绝）、显式 `minecraft:empty`（正常导入且 `hasFluidTick(empty)` 命中）。

**应修 4 — 存档时间相加可能触发有符号溢出**

- 新增 `wrappingAdd`：走 `uint64_t` 相加再转回，得到 Java `long` 的回绕语义（`SavedTick.java:49-51`），消除 C++ 有符号溢出 UB。
- 新增 `tickPriorityByValue`：`p` 越界夹到 `[-3,3]`，对应 `TickPriority.java:21-29`。
- 测试用 `gameTime = INT64_MAX-1` + `delay = INT_MAX` 断言结果回绕为负、`p=99` 夹到 3、`p=-99` 夹到 -3。

**变异验证**

| 变异 | 结果 |
|---|---|
| litematic 读回 `Block` 键 | 抓到 |
| 磁盘读取不再提取 `fluid_ticks` | 抓到 |
| 忽略 `SubTick`，一律用 `-N..-1` | 抓到 |
| 宽松路径去掉跳过计数 | 抓到 |
| `wrappingAdd` 换回 `gameTime + delay` | **未抓到** |

第五条必须如实说明：MSVC/x64 上 `int64_t + int` 的运行时结果同样回绕，两种写法在 Release 下产生相同值，测试无法区分。`wrappingAdd` 的价值在于**消除未定义行为**（UB 允许编译器假定不回绕），这在 Release 构建里观察不到。保留它是正确的，但不能声称被变异测试覆盖。

**明确没有做的事**：存档写回（`fluid_ticks` 保存侧）；`SubTick` 参与同刻派发顺序的端到端验证（目前只验证 `order` 字段被正确保留）；真实 26.1 服务端差分；桶交互与实体推流（P1）。

### 构建与测试（返工后）

- 构建：MSVC Release + jemalloc 通过。
- CTest：**20/20 通过**。
- 证据口径：**源码对照（含已 clone 的 Litematica 上游源码）+ C++ 回归测试**，未跑真实 26.1 服务端差分，不声称原版等价。

任务已完成
