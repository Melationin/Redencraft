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

（待填）

任务进行中
