# 路线图

目标形态见 [AGENTS.md](AGENTS.md) 第 1 节。本文件只定义任务顺序与验收口径；**进度不写在这里**，进度只写在 [task/TASKS.md](task/TASKS.md)。

规则：coder 按本文件的顺序领取「第一个还没有被 reviewer 判定 `任务通过` 的任务」。每个任务一次**主提交**；把 commit hash 与末行状态写回 `task/TASKS.md` 的**状态记录提交**不计入「一次提交」限制。

## 阶段 0 — 文档与流程对齐

- **M0-1 技术形态对齐与协作流程落地**
  把项目定位从「单维度只读红石模拟器」改为「对齐 26.1 的 C++ 服务端」，同步所有文档中的定位表述；建立 PLAN.md 与 task/TASKS.md 的 coder/reviewer 流程。
  验收：`AGENTS.md` 的目标系统表与当前实现边界分离且与实际代码一致；`docs/` 中不再把「单维度/只读/无生成/无加载票/无光照」写成项目定义（只能写成当前实现状态）；流程五态与职责写入 `AGENTS.md`。

## 阶段 1 — 流体

理由：流体是现有方块/刻系统内可独立完成的一层；光照与存档都要以流体状态定型后的区块数据为准。

- **F-1 流体状态模型**
  `Fluid` / `FluidState` / `Fluids` 三个概念：`minecraft:empty`、`water`、`flowing_water`、`lava`、`flowing_lava`；`amount`、`isSource`、`isFull`、`isSame`、`getOwnHeight`、`createLegacyBlock`、`getLegacyLevel`。
  `falling` 属于 `FluidState`。水和岩浆的方块状态保留原版 `level=0..15`，默认 `0`。读取时 `8..15` 都解码成 amount 8 的 falling 流体，重新生成的 legacy 方块为 `level=8`。原先读取的方块状态保留自己的 `level`，协议和 Anvil schema 沿用 26.1 导出。
  依据 `world/level/material/{Fluid,FluidState,Fluids,FlowingFluid,WaterFluid,LavaFluid}.java` 和 `world/level/block/LiquidBlock.java`。执行 [流体测试规格](docs/fluid-testing.md#41-状态和调度) 的 F-01/F-02 中的状态映射部分，枚举两种方块的全部 16 档，比较源/流动的精确身份、同族关系、amount、`isSource`、`isFull`、`falling`、固有高度和 legacy 映射。跨格高度随 F-3，含水宿主随 F-5。

- **F-2 流体计划刻队列**
  独立的 `LevelTicks` 容器，与方块刻分开收集/执行，共享 `Level.nextSubTickCount`；`ServerLevel.tick` 中位于方块刻之后、区块阶段之前；各自 65536 上限。
  依据 `ServerLevel.java`、`LevelTicks.java`、`LevelChunkTicks.java`、`ScheduledTickAccess.java`。执行 Q-01…Q-06，覆盖源与流动类型共存、陈旧刻按精确身份跳过、跨队列同刻收集、积压区块队首和各自上限。
  派发使用 `FluidState::is(const Fluid&)` 的精确身份匹配，依据 `ServerLevel.java:797-803` 和 `TypedInstance.java:22-24`。分别验证流水刻对流水执行、源水刻对流水跳过，以及同格水/岩浆条目不会串扰。
  复审时按 [性能回归](docs/fluid-testing.md#72-性能基准) 跑既有干态世吞基准，检查空流体队列进入每刻循环后对旧负载的影响。

- **F-3 `FlowingFluid` 传播**
  实现 `FlowingFluid.java` 的 `spread`、`spreadToSides`、`getNewLiquid`、`getSpread`、`getSlopeDistance`、`canPassThroughWall`、`SpreadContext`、`isWaterHole`、`getHeight` 和 `getFlow`。以预置方块和显式流体刻隔离算法。F-5 接通自然更新后用同一夹具复验。
  执行 W-01…W-07，验证水平扩散、退流、瀑布、源转换、坡度同距写序、共享面和动态形状。若引入原版 200 项遮挡缓存，另证实冷/热缓存结果相同且动态形状不复用静态缓存。
  依赖补充：F-3 同时补 `FluidState::getHeight`（`FlowingFluid.getHeight`，`FlowingFluid.java:472-475`：正上方同族时 `1.0F`，否则 `getOwnHeight()`），F-1 只交付了固有高度 `getOwnHeight`；`getFlow` 的向量部分仍按本节原口径在 F-3 实现。

- **F-4 水与岩浆参数差异**
  依据 `WaterFluid.java`、`LavaFluid.java`、`LiquidBlock.java`。主世界水 delay/dropOff/slope 为 5/1/4，岩浆为 30/2/2。水源转换规则默认开、岩浆默认关，分别验证规则开关。岩浆液面抬升的随机延迟、水接触方向、下落成石及魂土/蓝冰成玄武岩都有独立用例。
  执行 L-01…L-03 和 M-01…M-03。L-04 的 `FAST_LAVA` 使用原版扩展对照，项目支持随维度环境属性接入。
  依赖补充：本节需要 `FluidTags.WATER` / `FluidTags.LAVA`（`FluidTags.java:8-9`）作为独立于 `Fluid.isSame` 的判定面，`WaterFluid.canBeReplacedWith` 用 `!other.is(FluidTags.WATER)`、`LavaFluid.canBeReplacedWith` 用 `other.is(FluidTags.WATER)`、`LiquidBlock.shouldSpreadLiquid` 用 `is(FluidTags.WATER)`，`isSame` 不能替代。

- **F-5 方块侧接线与排水**
  依据 `LiquidBlock.java`、`SimpleWaterloggedBlock.java`、`BaseRailBlock.java` 和各宿主源码。分别接通放置、`neighborChanged`、`updateShape` 的流体刻请求，以及铁轨、墙、珊瑚扇等原版支持宿主的含水与排水。栅栏门用于开闭时的流体阻挡对照。
  执行 H-01…H-03、P-01…P-04 和 W-01 的正常放置路径，断言含水属性、排水或保水、队列请求、活塞普通/提前完成和干态回归。桶交互与实体推流继续按测试规格的 P1 分项交付。
  补齐已注册墙与枯珊瑚扇的 `forceSolidOn`（26.1 `Blocks.java:2600,4835–4889,4915–4969,5110`）。分别验证 `LiquidBlockContainer` 容纳水的优先规则，以及该宿主作为双源转换支撑时的 `isSolid()` 判定，覆盖无碰撞或空碰撞状态。

- **F-6 存档与投影的流体刻**
  依据 `SavedTick.java`、`LevelChunkTicks.java`、`SerializableChunkData.java` 和对应版本的 Litematica 格式实现。导入 `fluid_ticks` 的精确流体标识、相对延迟与优先级，按每个区块的独立流体列表恢复 `-n..-1`。投影 `PendingFluidTicks` 单独按格式夹具验证位置变换、字段与错误路径。
  执行 A-01…A-04。Anvil 以同一存档副本的原版重启为基准。投影用经格式核对的夹具和原版运行快照作对照。验证 C++ 只读输入，并区别缺失/空名字、未知名字与显式 `minecraft:empty`。

- **F-7 验收与差分**
  汇总 F-1…F-6 的聚焦用例，按 [流体测试规格](docs/fluid-testing.md) 补齐 P0 集成对照，包含 A-05 的稳定窗口、湿态与干态红石对比，以及已实现协议路径的 N-01/N-02/N-04。缺失依赖分别登记。
  补验 W-06 的互补共享面，使用原版半砖或台阶夹具，分别断言单侧不封面、两侧合并封面。C++ 侧尚未注册的形状按依赖缺口记录。补回已消费的显式空流体刻和读到空气的陈旧刻的派发 trace，分别检查 `fluid_tick`、`fluid_tick_skipped`，两者均不得写方块。
  区分源码对照、C++ CTest、原版 JAR 离线探针、真实 26.1 服务端差分四种证据。真实差分使用官方 JAR、隔离世界、同输入逐刻 trace 与原版生成的黄金文件；只在具备运行授权时执行。
  协议回归使用固定 26.1 导出中的水、岩浆 32 条完整 `(协议 ID, 方块, 属性)`，在 `wire_tests` 核对 `Registry::stateId` 和 `stateById`。变异测试将两段整体移动并重排成连续 ID，仍应被拒绝。
  复审时保留流体前 C++ 固定基线，另用官方 26.1 服务端对跑已核对行为和工作量的干态世吞、静水和动态流体夹具。按 [性能基准](docs/fluid-testing.md#72-性能基准) 分别验收 C++ 历史回归、原版同阶段耗时与整服耗时。原版对照结果记录计时边界、实际工作量和逐刻分布。

- **F-8 流体单例的所有权**
  处理 `src/fluid.cpp` 中五处由静态裸指针持有的 `new`。按 `AGENTS.md` 的所有权约束选择值类型或智能指针，核对静态初始化和销毁次序。用回归验证同一流体的身份稳定，默认空状态在正常运行路径始终可查询。

- **M0-2 交接与定位文档校正**
  在流体验收之后单独校正文档。`AGENTS.md` 的角色切换和验收统一以唯一的末行状态为准，时间目标对齐 26.1 World Clocks 与 `advance_time`。修复 `docs/compatibility.md` 的路线图链接及命令边界，`docs/handover.md` 区分历史段落与当前流体路线。
  剩余待办（M0-1 审查遗留，已于 `3d5af1e` 修复大部分）：仅剩 `AGENTS.md` 返工内容的追加位置说明（已在同一轮补上）；如发现新问题在此追加。

## 阶段 2 — 光照

- **L-1 光照数据模型**：区块的天空光/方块光数组、`Section` 存储扩展、与方块状态变更的挂钩点。
- **L-2 `LightEngine` 传播**：减少/增加队列、遮蔽、天空光按高度衰减；与 `LevelChunkSection` 的边界更新。
- **L-3 协议光照段**：区块包里的光照数据改为真实计算结果，移除占位值。
- **L-4 与更新/卸载的交互**：方块更新触发的光照更新时机、区块加载/卸载时的光照交接。

## 阶段 3 — 时间与世界保存

- **W-1 时间**：对齐 26.1 的 `gameTime` 与 World Clocks，包括默认时钟、`advance_time`、睡眠跳转、命令 `/time` 及实际持久化路径。依据 `ServerLevel.tickTime`、`ServerClockManager` 和 `TimeCommand`。
- **W-2 世界保存骨架**：`level.dat` 写回、Anvil `.mca` 写出（含压缩版本）、区块 palette 序列化。
- **W-3 持久化内容**：方块刻/流体刻、方块实体、实体、强加载列表、gamerules。
- **W-4 保存-重启差分**：写盘后重启读回，逐区块/逐刻对照。

## 阶段 4 — 加载票

- **T-1 票模型**：`TicketType` / `Ticket` / 等级与超时、`ChunkMap` 的 `ChunkHolder` 状态推导。
- **T-2 视距与强制加载**：玩家视距、`forceload`、出生点票、实体刻所需的 ticking 等级。
- **T-3 可刻判定替换**：把现有「区块已加载即 tick」换成原版的 ticking 等级判定。

## 阶段 5 — 多维度

- **D-1 维度注册**：`level.dat` 的维度列表、`dimensions/<ns>/<path>` 读写、维度类型与高度。
- **D-2 每维度独立刻**：`ServerLevel` 级的世界实例、独立实体列表与随机源。
- **D-3 跨维度**：传送门与传送语义。

## 阶段 6 — 世界生成

- **G-1 密度函数与噪声**：`DensityFunction` 树、噪声路由、`RandomState` 的种子派生。
- **G-2 生物群系源**：`BiomeSource` / 多层噪声 / 生物群系放置。
- **G-3 地表规则与结构**：`SurfaceRules`、`StructurePlacement`、特征放置。
- **G-4 种子差分**：同一 seed 下逐区块与原版对照。

## 说明

- 阶段 1–4 都可能在阶段 5–6 到位后需要返工；返工必须作为**新任务**记录在 `task/TASKS.md`，不能顺手夹带在别的任务里。
- 每个任务都要在 `task/TASKS.md` 写清「源码依据」，未移植的依赖照 `AGENTS.md` 第 2 节写 `// TODO`。
