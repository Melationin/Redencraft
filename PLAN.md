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
  汇总 F-1…F-6 的聚焦用例，按 [流体测试规格](docs/fluid-testing.md) 验收已实现子集，包含 A-05 的稳定窗口、湿态与干态红石对比，以及已实现协议路径的 N-01/N-02/N-04。C++ 验收要求慢端积累跨区段流体帧、健康端继续推进，另有生产队列越过积压阈值的证据；同一个湿态慢端越阈值的完整证据归 F-9a。
  补验 W-06 的互补共享面，使用原版半砖或台阶夹具，分别断言单侧不封面、两侧合并封面。C++ 侧尚未注册的形状按依赖缺口记录。补回已消费的显式空流体刻和读到空气的陈旧刻的派发 trace，分别检查 `fluid_tick`、`fluid_tick_skipped`，两者均不得写方块。
  区分源码对照、C++ CTest、原版 JAR 离线探针、真实 26.1 服务端差分四种证据。真实差分使用官方 JAR、隔离世界、同输入逐刻 trace 与原版生成的黄金文件；只在具备运行授权时执行。核心运行验收至少覆盖 W-01、W-02、M-01、M-03、H-01 流体自然进入含水宿主的正反例以及 A-05 双观察者。其余差分及依赖按用例登记并由 F-9a 接续。
  协议回归使用固定 26.1 导出中的水、岩浆 32 条完整 `(协议 ID, 方块, 属性)`，在 `wire_tests` 核对 `Registry::stateId` 和 `stateById`。变异测试将两段整体移动并重排成连续 ID，仍应被拒绝。
  复审时保留流体前 C++ 固定基线，按 [性能基准](docs/fluid-testing.md#72-性能基准) 记录历史干态世吞回归，并用同配置 C++ A/B 验收持续侧向搜索与干态负载。与官方服务端的同阶段、同工作量对跑归 F-9b；计时边界不相同时不得给原版倍率或通过结论。

- **F-8 流体单例的所有权**
  处理 `src/fluid.cpp` 中五处由静态裸指针持有的 `new`。按 `AGENTS.md` 的所有权约束选择值类型或智能指针，核对静态初始化和销毁次序。用回归验证同一流体的身份稳定，默认空状态在正常运行路径始终可查询。

- **M0-2 交接与定位文档校正**
  在流体验收之后单独校正文档。`AGENTS.md` 的角色切换和验收统一以唯一的末行状态为准，时间目标对齐 26.1 World Clocks 与 `advance_time`。修复 `docs/compatibility.md` 的路线图链接及命令边界，`docs/handover.md` 区分历史段落与当前流体路线。
  剩余待办（M0-1 审查遗留，已于 `3d5af1e` 修复大部分）：仅剩 `AGENTS.md` 返工内容的追加位置说明（已在同一轮补上）；如发现新问题在此追加。

## 阶段 2 — 光照

按 26.1 `ThreadedLevelLightEngine` 分工：模拟线程提交区段、方块及区块生命周期变更；每个光照实例由后台执行器串行执行批次的 PRE_UPDATE → 传播 → POST_UPDATE，再发布完整的只读光照快照。天空光和方块光各有传播与存储规则。线程之间不得借用活 `World`、区段容器或可变注册表；正常 tick 不等待传播完成，不在写方块时扫描区段或遍历光照邻居。当前只有显式区块加载和单一世界，加载票与多维度集成待其对应阶段。

- **L-1 光照数据与线程交接**：依据 `DataLayer`、`LayerLightSectionStorage`、`LevelChunk.setBlockState`、`LightEngine.hasDifferentLightProperties` 和 `ThreadedLevelLightEngine`。实现 0..15 压缩光层、缺席/均匀/已分配状态、区段与上下边界光层坐标、方块状态光照属性、合批提交及安全发布的基础。发光、衰减与遮蔽面依据 26.1 源码或实际导出，遮蔽面与碰撞形状分别处理；未支持的方块明确登记。区块版本区分卸载前后的任务，先验收光照属性无变化且两态均无需形状遮蔽时不提交、需要形状遮蔽时按原版判据提交、负坐标、上下边界、并发读取只见完整批次、关闭时所有权安全。传播未实现前不向协议提供伪造的已完成快照。
- **L-2a 方块光传播**：依据 `BlockLightEngine` 与 `LightEngine` 实现增减队列、光源加入/撤除、状态发光变化、衰减、共享面遮挡与区段/区块边界传播。验收至少覆盖不同强度重叠光源、遮挡/开孔、跨边界增减、加载顺序变化及批次完成后的快照。为真实 26.1 服务端建立可冻结并等待光照完成的逐格对照夹具。
- **L-2b 天空光传播**：依据 `ChunkSkyLightSources`、`SkyLightEngine`、`SkyLightSectionStorage` 实现天空入口高度、满光向下传播、侧向和透光方块衰减、开关屋顶、负高度及空区段补光，支持无天空光配置。对照真实 26.1 服务端的露天、遮蔽、侧向漏光和跨区块夹具；同时检查两层互不串扰。
- **L-3 协议光照**：依据 `ClientboundLightUpdatePacketData` 和 `ChunkHolder`，用已发布光照替换初始区块占位，发送后续真实光照更新。对照 775 的上下额外区段、两层数据掩码及空掩码，严格解帧覆盖初始、更新和光清除。异步编码只读不可变数据，已卸载区块的过时结果不发送；现有方块广播和成功事件顺序保持不变。
- **L-4 生命周期与性能验收**：覆盖运行期放置/破坏、载入/卸载/重载、邻区补光、待处理积压、停服和真实 26.1 夹具。依据 `SerializableChunkData` 核对 Anvil `BlockLight`、`SkyLight` 和 `isLightOn`；读取有效光照或重新计算，保存写回待 W-2/W-3。未加载邻区、区块光照待完成、旧结果拒收须有可观察行为，不能静默丢更新或让主线程同步补算。

性能门槛：固定世吞投影及 SHA-256、输入和随机种子，MSVC Release + jemalloc，基线与启用真实光照的最终候选交错至少五组 × 1000 tick。主线程固定同一性能核，光照工作线程固定另一个物理核；基线与候选使用同一主线程计时边界，覆盖光照提交与结果接收，即使这些操作发生在 `World::tick` 外。记录主线程 CPU 时间及逐刻墙钟均值、中位数、P95、>50 ms 次数，报告后台 CPU、提交/处理量、队列高水位和排空耗时；逐个核对模拟指纹与终态。预热时完成初始照明，计时内必须执行实际动态传播与发布，另验持续运行的积压及初始载入成本。以所有有效配对的增幅中位数判定，相对前光照基线的主线程 CPU 和平均刻墙钟增幅均须 **<10%**；记录同构建关闭光照对照以定位开销。不能靠关闭传播、把整轮工作推迟到计时结束或改变模拟工作量达标。每阶段须构建并由 coder 执行最终候选 CTest，reviewer 核对日志与差分口径。

## 阶段 2.5 — 流体补充验收

- **F-9a 原版探针、存档与协议补证**：以 [流体证据矩阵](docs/fluid-acceptance.md#5-已知依赖与未跑项逐项) 的逐项缺口为清单。先在隔离官方 26.1 服务端扩展队列与移动活塞探针，补 Q-01…Q-06、H-02、P-01…P-04 及 A-01/A-02 同一 Anvil 副本重启；对缺少方块、玩家/桶、维度、加载票或写回的用例，明确所需系统及在对应阶段完成后补验。复现 N-04 同一湿态慢端越过队列阈值后的断连与健康端连续进展；在具备录包授权时补原版 775 N-01/N-02/N-04 帧序对照。证据不得以 C++ 回归代替原版。
- **F-9b 与原版同阶段性能对跑**：建立两端可加载的同输入世吞、静水和持续动态流体工作负载。先核对区块资格、状态、派发与写入等工作量，再对齐计时边界。保留固定 C++ 历史基线，分别记录匹配阶段与整服逐刻分布；未满足同工作量/同阶段的结果不用于原版性能结论。

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
