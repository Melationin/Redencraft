# 路线图

目标形态见 [AGENTS.md](AGENTS.md) 第 1 节。本文件只定义任务顺序与验收口径；**进度不写在这里**，进度只写在 [task/TASKS.md](task/TASKS.md)。

规则：coder 按本文件的顺序领取「第一个还没有被 reviewer 判定 `任务已完成` 的任务」，一个任务一次提交。

## 阶段 0 — 文档与流程对齐

- **M0-1 技术形态对齐与协作流程落地**
  把项目定位从「单维度只读红石模拟器」改为「对齐 26.1 的 C++ 服务端」，同步所有文档中的定位表述；建立 PLAN.md 与 task/TASKS.md 的 coder/reviewer 流程。
  验收：`AGENTS.md` 的目标系统表与当前实现边界分离且与实际代码一致；`docs/` 中不再把「单维度/只读/无生成/无加载票/无光照」写成项目定义（只能写成当前实现状态）；流程五态与职责写入 `AGENTS.md`。

## 阶段 1 — 流体

理由：流体是现有方块/刻系统内可独立完成的一层；光照与存档都要以流体状态定型后的区块数据为准。

- **F-1 流体状态模型**
  `Fluid` / `FluidState` / `Fluids` 三个概念：`minecraft:empty`、`water`、`flowing_water`、`lava`、`flowing_lava`；`amount`、`isSource`、`isFull`、`isSame`、`getOwnHeight`、`createLegacyBlock`、`getLegacyLevel`。
  给 `water` / `lava` 补 `falling` 属性，并核对 `level` 的值域与默认值。
  源码：`world/level/material/{Fluid,FluidState,Fluids,WaterFluid,LavaFluid}.java`、`world/level/block/LiquidBlock.java`。
  验收：状态与流体的双向映射（含 `level=8..15` 的 `falling`）有断言；`level=9..15` 仍保留方块属性不被截断。

- **F-2 流体计划刻队列**
  独立的 `LevelTicks` 容器，与方块刻分开收集/执行，共享 `Level.nextSubTickCount`；`ServerLevel.tick` 中位于方块刻之后、区块阶段之前；各自 65536 上限。
  源码：`ServerLevel.java`、`LevelTicks.java`、`LevelChunkTicks.java`、`ScheduledTickAccess.java`。
  验收：两条队列互不串扰、同一位置同类型去重、到期顺序与 `subTickOrder` 有断言。

- **F-3 `FlowingFluid` 传播**
  `spread`、`spreadToSides`、`getNewLiquid`、`getSpread`、`getSlopeDistance`、`canPassThroughWall`（含 200 项遮挡缓存）、`SpreadContext`、`isWaterHole`、`getFlow`。
  验收：退流、坡度优先、瀑布、独立水源三种最小场景逐刻断言。

- **F-4 水与岩浆参数差异**
  water：delay 5、dropOff 1、slopeFindDistance 4、`canConvertToSource=true`；lava：30 / 2 / 2、`canConvertToSource=false`、遇水成石（黑曜石/圆石）。
  验收：参数驱动的行为差异有断言，且参数不硬编码在传播逻辑里。

- **F-5 方块侧接线与排水**
  `LiquidBlock.onPlace` / `neighborChanged` / `updateShape` 的流体刻请求；`LiquidBlockContainer` 的放入/排出（`waterlogged` 方块、铁轨、墙、珊瑚扇、栅栏门）。
  验收：放置/破坏带水方块的状态变化与流体刻请求顺序有断言。

- **F-6 存档与投影的流体刻**
  导入 `fluid_ticks`（相对延迟 + priority + 容器 `-n..-1` 基准）；litematic 的 `PendingFluidTicks` 不再跳过。
  验收：导入后的出队时刻与原版重启一致。

- **F-7 验收与差分**
  按 [docs/fluid-testing.md](docs/fluid-testing.md) 的 P0 用例建 CTest；在授权后跑真实 26.1 服务端差分。未跑差分时只能标「源码对照 + C++ 回归通过」。

## 阶段 2 — 光照

- **L-1 光照数据模型**：区块的天空光/方块光数组、`Section` 存储扩展、与方块状态变更的挂钩点。
- **L-2 `LightEngine` 传播**：减少/增加队列、遮蔽、天空光按高度衰减；与 `LevelChunkSection` 的边界更新。
- **L-3 协议光照段**：区块包里的光照数据改为真实计算结果，移除占位值。
- **L-4 与更新/卸载的交互**：方块更新触发的光照更新时机、区块加载/卸载时的光照交接。

## 阶段 3 — 时间与世界保存

- **W-1 时间**：`gameTime` / `dayTime` 分离、`doDaylightCycle`、睡眠、命令 `/time` 语义。
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
