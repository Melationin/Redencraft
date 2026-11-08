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

任务已完成
