# F-7 纯服务端流体性能交接

仓库位于 `F:\source\minecraft-merged-07da9a845a-26.1-sources\redstone-cpp`。新对话先读 [AGENTS.md](../AGENTS.md) 和 [TASKS.md](TASKS.md) 中最新的「本轮返工范围 · 纯服务端流体性能」。任务状态只看 `TASKS.md` 末行，本文件用于恢复上下文。

## 当前基线

- 分支 `main`，HEAD 为 `96fcafd`，上一实现提交为 `ecd6d6a`。
- `TASKS.md` 末行为 `审查结束`。工作区已经写入复审记录和本轮范围，四项优化尚未改动源代码。
- 当前实现的 Release + jemalloc 构建通过，CTest 为 22/22。官方 26.1 已有水槽 117 项及水岩浆接触 56 项黄金观测。更完整的原版差分和同阶段性能验收缺项保留在 `TASKS.md`。
- 保留现有未跟踪文件。两份 `build-f7-nmake-*.log`、`scripts/wait-tasks.ps1` 和 `validation/vanilla26_1/fluid-cpp-states.json` 与本轮提交无关。

## Coder 本轮范围

1. 在 `src/fluid_flow.cpp` 的 `fluidNewLiquid` 中复用已有的 `blockState`，同时检查调用点是否还能去掉一次读取。原版 `FlowingFluid.getNewLiquid` 直接使用传入状态。
2. 移除 `SpreadContext` 和四方向结果中的逐项堆分配。搜索范围由流体坡度半径限制，可以用有界存储。保持方向顺序、同距离分支、下落口和替换条件。测试最大半径、负坐标和同距扩散。
3. 静态共享面按源状态、目标状态和方向缓存，整块目标先短路。移动活塞等动态形状仍按当前位置和进度现场计算。验证互补面、方向差异和动态形状的结果。
4. 按方块状态 ID 复用 `fluidStateOf` 的计算。核对静态初始化、后注册及扩展状态，保留现有干块、含水、源、流动和下落语义。避免为常见干块查找引入额外堆分配或明显回退。

上一级的 26.1 Java 源码只读。对照 `FlowingFluid.java` 的液面重算、坡度搜索和共享面缓存，以及 `BlockBehaviour.BlockStateBase` 的流体状态初始化。发包不属于本轮。按仓库流程由一名 coder 实现和提交，主代理负责复审。若 `.git` 写入受限，在 `TASKS.md` 记录实际状态和证据。

## 验证

- `build-f7-nmake` 已配置 MSVC Release + jemalloc。重建前保存基线库和 benchmark 二进制，A/B 使用同一构建配置。
- 跑 CTest，覆盖已有的原版黄金回归和必要的聚焦测试。若沙箱拒绝 `127.0.0.1`，把失败原因和测试名交给 reviewer。
- 湿态基准需要持续触发侧向搜索，记录派发数和写入数，核对两版工作量一致。原有 W-01 小水槽只产生 12 次派发、6 次写入，适合行为检查。
- 干态使用 `build-review-f2/workload.litematic` 跑 1000 tick，核对终态并记录均值、中位数和 P95。已有历史对比的 P95 为 6.959 → 7.480 ms，重复区间重叠且构建生成器不同，仍需同配置复测。原版性能结论需要同阶段和同工作量。

## 新对话如何启动

用户要求通过 **V1 子代理工具**派发，coder 模型为 **`command-code/deepseek-deepseek-v4.1-flash`**，思考强度 **`high`**。当前旧对话固定使用 V2，两次派发均因 `unreadable_encrypted_agent_task` 失败。机器上的 `multi_agent_v2` 已关闭，新会话生效。临时 CLI 会话明确使用本地路由后，V1 `spawn_agent` 已返回 `pending_init`，随即按用户指示结束，未收到 coder 交付。

可在新对话发送这条指令。

> 请读 `task/F7-FLUID-PERF-HANDOFF.md` 和 `task/TASKS.md`。用 V1 子代理工具派一名 `command-code/deepseek-deepseek-v4.1-flash`、思考强度 `high` 的 coder，完成 F-7 的四项纯服务端流体性能优化。按仓库流程复审，并汇报 CTest 和湿态、干态前后对照。
