# 26.1 行为基线与验收边界

本文件记录**当前已实现的边界**，不是项目目标。项目目标是「对齐 26.1 的 C++ 服务端」（多维度、加载票、世界生成、保存、时间、光照），见 [AGENTS.md](../AGENTS.md) 第 1 节与 [PLAN.md](../PLAN.md)。本文中写「未实现」的系统只表示**当前阶段**尚未实现，不代表被排除在项目之外。

## 必须保留的阶段与顺序

`ServerLevel.tick` 中本项目关注的顺序为：推进 gameTime → block scheduled ticks（每刻上限 65536）→ fluid scheduled ticks → chunkSource.tick → block events → entities → block entities → entity management。天气、睡眠、袭击等省略的系统需要在实现处标记 TODO。实体阶段现已实际执行简单实体；仍不能将移动活塞方块实体提前到实体阶段或方块事件之前。实体列表按注册顺序刻，刻内新增实体延后一刻，未加载区块的实体暂停，详见 [简单实体、铁轨与物品](entities.md)。

计划刻不是简单的全局按时间排序队列：区块内使用 triggerTick、priority、subTickOrder；到期区块之间用 priority、subTickOrder 决定 drain 次序。先收集本刻待执行项，再执行回调；执行期间新加入的到期刻不能自动混入这次收集。唯一性依据为位置与类型。区块不 tick 时保留待执行刻。

C++ 侧按 `LevelTicks`/`LevelChunkTicks` 实现：每个已加载区块一个容器（`priority_queue` 用 `ScheduledTick.DRAIN_ORDER`，另有 `(pos, type)` 哈希集合做 `UNIQUE_TICK_HASH` 去重），`nextTickForContainer` 只为"头部到期"的区块登记，收集时按 `INTRA_TICK_DRAIN_ORDER` 逐容器 drain（`drainFromCurrentContainer` 比较当前容器头与下一容器头），再统一执行 `toRunThisTick`，上限 65536。`subTickOrder` 来自 `Level.nextSubTickCount` 的单调计数器；存档恢复的刻按 `LevelChunkTicks.unpack` 以每区块 `-N..-1` 的负数基准写入，因此恢复刻总是先于新刻，计数器无需回看已有队列。`unloadChunk` 视为"停止 tick"（与原版区块卸载丢弃容器的差异见下文），drain 时用区块已加载判定代替 `LevelTicks` 的 `tickCheck`。

方块事件按首次插入顺序去重，处理过程中产生的新事件允许继续执行；不能处理的区块事件保留到后续。事件必须再次检查当前位置方块类型；活塞事件再次检查电源，不能仅相信入队时状态。

## 更新不是一种通知

- `Level.setBlock` / `LevelChunk.setBlockState`：flags、回调、方块实体移除/创建、递归深度。
- `CollectingNeighborUpdater`：嵌套更新由 addedThisLayer 与栈驱动；多邻居通知可以暂停以处理新层。
- `NeighborUpdater.UPDATE_ORDER` 与 `Block.UPDATE_SHAPE_ORDER`：分别对照，不能混用。
- `neighborChanged`：用于准连接、电源变化等，不等于 shape update。
- `updateShape` 与间接 shape update：侦测器触发、墙体连接、珊瑚支撑依赖这些路径。
- 放置/移除及移动标志：尤其避免活塞移动中错误提前通知或漏通知。

不要求哈希表跨区块迭代顺序一致；局部方向顺序、事件顺序、subTickOrder 仍属于兼容目标。默认以非实验性红石规则为基线，实验 redstone_experiments 另行移植。

## 源码索引（相对上一级）

| 内容 | Java 源码 |
|---|---|
| 世界阶段 | `net/minecraft/server/level/ServerLevel.java` |
| 更新标志与回调 | `net/minecraft/world/level/Level.java`、`chunk/LevelChunk.java`、`block/Block.java` |
| 连锁更新 | `net/minecraft/world/level/redstone/{NeighborUpdater,CollectingNeighborUpdater}.java` |
| 计划刻 | `net/minecraft/world/ticks/{LevelTicks,LevelChunkTicks,ScheduledTick,SavedTick}.java` |
| 活塞 | `net/minecraft/world/level/block/piston/{PistonBaseBlock,PistonHeadBlock,MovingPistonBlock,PistonStructureResolver}.java` |
| 移动方块实体 | `net/minecraft/world/level/block/piston/PistonMovingBlockEntity.java` |
| 侦测器/音符盒/TNT | `net/minecraft/world/level/block/{ObserverBlock,NoteBlock,TntBlock}.java` |
| 实体阶段与实体列表 | `net/minecraft/server/level/ServerLevel.java`、`world/level/entity/EntityTickList.java` |
| 实体基础/物品/TNT/下落方块 | `net/minecraft/world/entity/{Entity,EntityDimensions}.java`、`entity/item/{ItemEntity,PrimedTnt,FallingBlockEntity}.java` |
| 矿车 | `net/minecraft/world/entity/vehicle/minecart/{AbstractMinecart,Minecart,OldMinecartBehavior}.java`、`vehicle/VehicleEntity.java` |
| 铁轨 | `net/minecraft/world/level/block/{BaseRailBlock,RailBlock,PoweredRailBlock,DetectorRailBlock,RailState}.java` |
| 爆炸 | `net/minecraft/world/level/{ServerExplosion,Explosion,ExplosionDamageCalculator}.java` |
| 区块读取 | `net/minecraft/world/level/chunk/storage/{RegionFile,SerializableChunkData}.java` |
| 压缩索引 | `net/minecraft/world/level/chunk/storage/RegionFileVersion.java` |

## 分阶段验收

1. 编译与 CTest：存储边界、脉冲延迟、更新模式、事件时序、活塞推动/拉回/限制。
2. 固定实际存档回放：导入状态、计划刻和移动中的活塞，逐刻导出比较结果（本版不写回 Minecraft 存档）。
3. 26.1 服务端差分：先用原版服务端命令控制冻结/单步并检查状态与物品；更细的逐回调轨迹需要 Fabric 测试夹具。完整覆盖目标包括准连接、短脉冲丢块、粘液蜂蜜分离、12 方块上限、跨区块与负坐标、侦测器移除/放置、更新抑制。

尚未做第 3 项时，不能宣称已验证“几乎全部原版特性”。具体当前限制在实现完成后追加。

2026-11-06 已完成用户截图中的地毯复制结构的实际原版 26.1 服务端比较；原版与 C++ 均保留移动后的地毯并产生一个额外地毯物品。现在 C++ 产生的是真实 `ItemEntity`，并由 `piston` CTest 断言。证据见 `validation/vanilla26_1`。其他结构仍主要依赖源码审查与回归测试；更新顺序审查记录见 `docs/update-order-audit.md`。

## 当前实现与剩余差异

后续新增独立可选原生协议 775 服务端，支持创造编辑和简单实体同步，详见 [原版连接说明](protocol775.md)。网络只观察核心，在原版区块广播阶段合并脏位置，成功方块事件立即发送；没有真实客户端验收。下述未实现的玩家指服务端物理、玩家实体和完整交互系统。

本批（当前阶段）排除水/流体刻，保留水和岩浆的存档状态、空碰撞和实体接触判定；不模拟流动、液面重算和流体推挤。流体刻不是被永久排除，它是 [路线图](../PLAN.md) 阶段 1，测试规格见 [水与岩浆对照测试设计](fluid-testing.md)。

流体**状态模型与计划刻队列已经实现**（F-1、F-2）：`include/redstone/fluid.hpp` / `src/fluid.cpp` 提供 `Fluid` / `FluidState` / `Fluids` 五个单例，以及 `fluidStateOf(BlockState)` 的 `level=0..15` 解码（`level=0` 源、`1..7` 流动 amount `8-level`、`8..15` 归一到 falling 满态，索引按原版 `Math.min(level,8)` 截断而方块属性保留原值）。`World` 现在有第二条独立的 `LevelTicks`（`fluidTicks`），容器、去重集与 65536 预算都与方块刻分开，在 `World::tick` 中紧随方块刻、早于区块阶段；派发按 `fluidState.is(type)` 精确身份匹配，陈旧刻不触发。查询与排刻不写方块。水与岩浆的**参数差异与反应规则已实现**（F-4）：`FluidTags.WATER`/`LAVA` 作为独立于 `isSame` 的判定面，`canBeReplacedWith` 改用标签；源转换改由世界的 `waterSourceConversion`/`lavaSourceConversion` 开关控制（默认开/关）；`fluidSpreadDelay` 实现岩浆液面抬升时 `nextInt(4) != 0` 的 4 倍延迟；`liquidShouldSpreadLiquid` 实现岩浆遇水成黑曜石（源）/圆石（流动）、下方灵魂土邻接蓝冰成玄武岩。新增 `cobblestone`/`basalt`/`soul_soil`/`blue_ice` 四个方块支撑上述反应。方块侧接线已实现（F-5）：`LiquidBlock` 的放置、`neighborChanged`、`updateShape` 三条路径分别请求流体刻（不合并成统一邻居刷新）；`SimpleWaterloggedBlock` 宿主（`waterlogged=true`）经 `fluidStateOf` 返回水源，`isWaterloggedHost` 标识 `LiquidBlockContainer`；墙与五种枯珊瑚扇补上 `forceSolidOn`。仍未实现：存档与投影的 `fluid_ticks` 恢复（F-6）、桶交互与实体推流（测试规格 P1）。

流体**传播算法已实现**（F-3）：`include/redstone/fluid_flow.hpp` / `src/fluid_flow.cpp` 逐方法移植 `FlowingFluid` 的 `tick`、`spread`、`spreadToSides`、`getNewLiquid`、`getSpread`、`getSlopeDistance`、`canPassThroughWall`（含共享面遮挡）、`isWaterHole`、`getHeight`、`getFlow`，`World::tickFluidAt` 已接上 `fluidTick`。水平扩散、退流、瀑布、双源转源、坡度优先、水位重算与流向向量都有 C++ 回归断言。**尚未接线的是方块侧触发**：`LiquidBlock.onPlace` / `neighborChanged` / `updateShape` 的流体刻请求属于 F-5，因此从存档或命令放置的流体目前不会自动开始流动，需要显式排刻。默认关闭实验性红石。玩家物理、生物、乘客和一般实体互撞仍不模拟；简单实体（物品、TNT、下落方块、矿车）已实际推进，TNT 爆炸和方块掉落生成真实实体。移动活塞作为方块实体实际执行，并非仅日志占位。实体刻位于方块事件与方块实体之间，详见 [简单实体、铁轨与物品](entities.md)。

后续已添加 Windows 调试客户端（见 client.md）、16 色羊毛地毯、简单实体与四种铁轨。地毯参考 `CarpetBlock.java`、`WoolCarpetBlock.java` 与 `Blocks.java`：下方非空气即可支撑，任意方向的形状更新均检查存活，厚度 1/16，推动反应 NORMAL，失去支撑时生成对应颜色的物品实体。铁轨的碰撞为空，支撑/自动连接/动力/探测/激活行为按源码移植，`waterlogged` 仅保留属性。

已经区分 Simple（执行时读取状态）与 Full（使用捕获状态）的 neighbor update、可被嵌套层暂停的 Multi update、Shape update、间接形状更新入口、移除/放置回调。更新标志、递归限制、更新总数限制分别处理。已实现本批方块的 shape 路径，包括侦测器调度、音符盒乐器属性、草方块 snowy、墙与珊瑚支撑、活塞头存活。当前支持的方块没有间接形状更新的覆写；接口保留以便未来接入红石线等。

活塞包括 12 方块限制、粘液/蜂蜜分离与分支、准连接、伸出/回缩/丢块事件、每刻 0.5 的移动进度、强制 finalTick、移动实体移除副作用、源/目标通知顺序及结束时重新计算形状。移动方块实体按注册顺序推进，执行过程中新增的实体留到下一次方块实体阶段；区块卸载时暂停，重新加载后恢复。`handlingTick` 在方块事件后、方块实体前清除。移动方块现在会在每次推进进度前处理实体：按 `PistonMath` 计算扫掠区域，执行 `MoverType.PISTON` 的 0.51/轴/刻限制、NOCLIP 碰撞豁免、回缩源活塞的基座挤出，以及粘液块 `causeBounce`（沿移动轴把简单实体速度设为 ±1）和蜂蜜块水平粘性位移。完整实体-实体碰撞和载具乘客仍为 TODO。

以下仍须明确区分：

- 区块与子区块按原版 `LevelChunk`/`LevelChunkSection` 分层，子区块内部是 palette + 索引的压缩存储（单值 0 位、8 位、16 位三档切换，`hasOnlyAir` 快速路径），区块表是哈希表。每个方块类型是进程级单例 `Block`（对应原版 `Blocks` 的静态字段），构造时按 `StateDefinition` 注册自身与全部状态；`BlockState` 持有 `Block*` 与 32 位注册 ID，拷贝是两个机器字，`state.is(Blocks.SAND)` 就是原版 `getBlock() == Blocks.SAND` 的指针比较，`with` 是 `StateHolder.setValue` 的变体索引转移，属性读取按属性名二分查找。详见下文“方块状态与区块存储”。
- 未实现随机刻、光照、高度图、天气、睡眠、世界边界动画、袭击和完整命令系统。已有 `/tick` 命令骨架（`src/command.cpp:310`、`docs/protocol775.md`），不含原版命令树、权限、选择器与其余命令。草方块随机蔓延/退化不在当前循环中。
- 注册表覆盖本批方块、物品与实体类型，不包含全游戏内容。未支持的方块保留状态，碰撞形状保守按整方块处理；其回调和硬度/推动反应不保证原版一致。因此不能用含其他机制方块的存档推断本批逻辑错误或正确。
- 墙及活塞的盒形状实现覆盖本批方块；与未实现方块标签、特殊面的互动仍待扩充。简单实体的 AABB/方块碰撞、粘液块落地反弹、移动活塞/粘液块的实体推挤与蜂蜜粘性位移已实现；实体间碰撞与载具乘客仍为 TODO。
- 音效、粒子、客户端通知、游戏事件、完整掉落表（精准采集/时运/工具条件/经验）和其他方块实体尚未实现。音符盒只保证本批范围内的更新路径；不模拟头颅自定义声音。
- 简单实体只覆盖物品、TNT、下落方块和空矿车；生物、掉落经验、箱子/漏斗/熔炉/TNT 矿车、实验性矿车改进不在本批。玩家拾取、物品栏和漏斗交互未实现。
- 爆炸的射线/抗性/伤害/链式 TNT 已移植，但 `HashSet` 迭代顺序和随机序列不追求逐位一致；火焰生成未实现（TNT 本身 `fire=false`）。没有真实玩家和死亡/击杀归属。
- 区块控制是显式加载开关，无加载票，也无原版加载阶段/相邻区块等级计算。卸载的数据保留在内存以便重新加载，未实现磁盘按需驱逐。
- 计划刻按 `LevelTicks`/`LevelChunkTicks` 的区块容器结构实现，收集与执行阶段分开；`scheduledTicks()` 只是按区块和 `DRAIN_ORDER` 排序的只读快照，不再参与模拟。与仍存在的差异：原版区块卸载会丢弃该区块的待执行刻，本项目的 `unloadChunk` 只停止 tick 并保留容器（沿用"卸载数据保留在内存"的既有边界）；原版 `tickCheck` 用区块的 ticking 等级，本项目用"区块已加载"。存档导入不含原版 DataFixer；只接受 26.1 DataVersion 4786、完整生成的区块。
- 存档中的 gamerules、持久化强加载列表与自定义维度元数据未自动导入；API/CLI 显式设置高度与加载区块，TNT 游戏规则默认开启。实体从同维度独立 `entities` 区域及遗留地形列表读取，未支持类型保留原始标签；种子读取 26.1 独立 world_gen_settings saved data。默认主世界路径优先 `dimensions/minecraft/overworld/region`，兼容旧根 region。详见 [读取说明](storage.md)。

后续验收优先补真实服务端逐刻差分。状态驻留、`Block` 单例身份、palette 存储、碰撞形状缓存、计划刻区块容器和方块事件插入序哈希集合均已落地；流体刻、随机刻仍未实现。不得用性能优化改变更新语义。

## 计划刻与方块事件队列（2026-11-06，按原版结构）

| 原版 | C++ |
|---|---|
| `LevelChunkTicks`：`PriorityQueue`（`ScheduledTick.DRAIN_ORDER`）+ `ticksPerPosition`（`UNIQUE_TICK_HASH` = pos+type） | 每区块 `ChunkTicks`：`vector` + `push_heap`/`pop_heap` + 开放寻址的 `TickKeySet`（键 = 位置 + `Block` 单例指针） |
| `LevelTicks`：`allContainers`、`nextTickForContainer`、`containersToTick`（`INTRA_TICK_DRAIN_ORDER`）、`toRunThisTick` | 同名四件套；容器/索引是打包 long 的开放寻址表（等价 fastutil `Long2Object`/`Long2LongOpenHashMap`），`containersToTick` 是 `vector` + `make_heap`，按容器头比较 |
| `sortContainersToTick` 只访问 `nextTickForContainer` 中 `nextTick <= gameTime` 的区块 | 同（不再每刻扫全部刻） |
| `drainContainers` + `drainFromCurrentContainer` + `rescheduleLeftoverContainers` | 同 |
| `runCollectedTicks` 先收后跑 | 同；执行期新增刻进入容器，下一刻才参与收集 |
| `ServerLevel.blockEvents` = `ObjectLinkedOpenHashSet` + `blockEventsToReschedule` | `deque` + `unordered_set`（插入序 + O(1) 去重/弹出）+ 复用延后列表；`BlockEventData.block` 对应 `Block` 单例指针 |

常数项（2026-11-06 第二、三轮）：`ScheduledTick.block`、`BlockEvent.block` 与更新来源 `Update::source` 是 `BlockRef`（`Block` 单例指针），去重键是“三个 int + 一个指针”，因此 `scheduleTick`、去重、`poll`、`hasScheduledTick` 既**不查名字也不分配字符串**，与原版 `ScheduledTick<Block>` 的对象身份一致；`subTickOrder` 只由 `Level.nextSubTickCount` 计数器给出，`rescheduleLeftoverContainers` 原地遍历，`cleanupAfterTick` 保留容量。`BlockEventQueue` 的去重索引仍是节点式 `unordered_set`（事件量远低于计划刻，尚未扁平化）。

验证：`safesave_order_parity` 的 7 个原版录制用例（`fixture_ticks`/`dedup_ticks`/`overdue_ticks`/`negative_orders`/`fixture_events`/`moving_entities`/`moving_completion_order`）逐字段一致；`engine`/`storage`/`piston`/`entity` 等 CTest 通过。`observer_reschedule` 仍是 native-only（无原版录制）。

## 爆炸射线（按 lithium 26.1 的 `ServerExplosionMixin`）

`ServerExplosion.calculateExplodedPositions` 的逐步射线按 lithium 的做法重写，采样顺序、每步阻力扣减和 0.225 常量衰减保持原版：

| 优化 | lithium 依据 | C++ 实现 |
|---|---|---|
| 直接映射方块缓存 | `DirectMappedExplosionBlockCache`（512 项） | 栈上 512 项缓存：打包位置 → 状态 ID、空气/流体标志、每步阻力 `(阻力+0.3)*0.3`、是否已加载 |
| 同方块步复用 | `performRayCast` 的 `prevX/prevY/prevZ` | 0.3 步长通常还在同一方块内，直接用上一格的阻力与空气标志，跳过缓存探测 |
| 步进向量外提 | `normX/Y/Z = dir/len*0.3` 预先算出 | 每条射线先算好 `stepX/Y/Z`，循环内只做加法 |
| 同一方块只记录一次 | `tryMarkBlockForDestruction` 的 `LongOpenHashSet` 去重 | 强度只降不升，首次进入方块时记录即可；之后再 `sort`+`unique` 仍然是同一集合 |
| 跳过空气 | `skip_air/ServerExplosionMixin` | 空气位置不进入破坏列表（没有火焰/计数需求） |
| 取整 | `Mth.floor` | 内联 `floorToInt`（截断后修正负数），避免射线循环里调用 libm `floor` |

没有改变的部分：16³ 外壳射线、`radius * (0.7 + rand * 0.6)` 的初始强度、每步先扣阻力再判断破坏、以及 `interactWithBlocks` 的破坏/掉落/连锁 TNT 顺序。

## 方块状态与区块存储（2026-11-06 性能重构）

性能采样把当轮 CPU 的约 64% 定位在“每次读方块都重建字符串与堆对象”，10.7% 在树查找，模拟逻辑本身不足 1%（见 `validation/litematic/performance-profile.md`）。据此按原版模型重塑了方块状态与区块存储，不改变更新语义：

| 内容 | 原版依据 | C++ 实现 |
|---|---|---|
| 方块身份 | `Blocks` 的静态 `Block` 字段；`state.is(Blocks.X)` 比较 `getBlock()` 引用 | 59 个进程级 `Block` 单例（`Blocks::AIR` …，按声明顺序注册，`air` 为 ID 0）；`BlockState` 持有 `Block*`，`state.is(Blocks::X)` 是一次指针比较 |
| 状态驻留 | `StateDefinition` 枚举属性笛卡尔积；`Block.BLOCK_STATE_REGISTRY` 给每个状态编号 | `Block` 构造时注册；53 种注册方块 + 6 种仅保留身份的原版方块共 1,780 个状态（26.1 属性值集合取自 `assets/protocol775/block_states.tsv`），无属性方块也各有一个默认状态 |
| 状态转移 | `StateHolder.setValue` 查 `neighbors[property][value]` | 混合进制变体索引：`variant + (newValue - oldValue) * stride`，返回已注册邻居状态 |
| 状态读取 | `BlockStateBase.getValue` 为属性数组下标 | 已注册状态按属性名在有序属性表上二分查找，`getView` 返回 `string_view` 不分配；扩展状态查附带属性表 |
| 方块读取 | `Level.getBlockState` → `LevelChunk.getBlockState` → 子区块 `hasOnlyAir` → `PalettedContainer.get` | 一次哈希查区块 → 一次数组取子区块 → `palette[index]` |
| 方块写入 | `Level.setBlock` → `LevelChunk.setBlockState` → `PalettedContainer.getAndSet` + 非空计数 | 同顺序；空子区块写空气直接返回，第二个非空气值触发 8 位、超过 256 种触发 16 位 |
| 碰撞查询 | `BlockCollisions` 迭代器 | 展平的世界坐标盒缓冲；`noCollision` 复用缓冲；每状态碰撞形状按状态 ID 缓存 |

方块的类型判定（活塞/活塞头/侦测器/铁轨/液体/地毯/墙/珊瑚扇等）是 `Block` 上的标志位或 `is(Blocks::X)` 指针比较，热路径上不再有方块名比较；`BlockMetadata` 现在按方块保存一份，读取返回 `const BlockMetadata&`，仅扩展活塞基座的导体判定由 `isRedstoneConductor` 按状态补充。导入的存档/投影若含未注册方块，会按名字惰性创建一个未支持 `Block` 单例，并按（名字，属性集合）注册扩展状态且可序列化；这类方块保持“状态原样保留、回调和硬度不保证原版一致”的既有边界。属性值不在已注册 StateDefinition 内的组合同样注册为扩展状态，不会静默合并到默认值对应的状态。

验证状态：`ctest -C Release` 12/12 通过（engine/entity/piston/storage/litematic/shapes/wire/protocol/socket 等）。本轮**没有**运行 26.1 服务端逐刻差分，因此只能声称“C++ 回归测试保持通过”，不能声称与原版等价。

2026-11-07 后续减少纯服务端临时分配：活塞 12 格推动列表与移动状态快照改为栈内存，销毁列表仍动态存储；属性读取/转换使用字符串视图；普通支撑面借用缓存形状，形状缓存用 `deque` 保证引用稳定；实体移动复用碰撞盒缓冲，死亡实体按原注册顺序稳定压缩。保留实体刻与移动活塞刻的快照、更新阶段和通知顺序。源码对照与新增 C++ 行为断言见 [性能记录第 10 节](performance.md#10-纯服务端减少分配与临时构造2026-11-07)。本轮未新增真实 26.1 服务端差分验证。

随后快速优化复用上次子区块指针、缩小爆炸缓存并预计算固定射线方向。缓存随加载/卸载、section 创建及 World 移动失效，读取仍使用当前状态；射线顺序、随机消耗与更新阶段保持。Release CTest 12/12 和投影前后 12 个检查点指纹通过，未运行真实服务端差分；详见 [性能记录第 13 节](performance.md#13-快速实现与最小验证2026-11-07)。

同日发包优化：每连接有界 SPSC 队列和冻结区块快照后台编码，tick 内提交不等待写线程锁/socket；积压只关闭该连接。普通块变化继续在区块广播时读取当前状态，成功方块事件立即提交。实体跟踪广播对照 `ServerChunkCache → ChunkMap/ServerEntity` 移至方块事件/实体运动之前，注册 spawn 及落地修正/移除有顺序断言。TODO：移除通知目前在实体清理或方块实体刻末观察，同阶段内尚未完整复刻原版 `Entity.setRemoved → levelCallback.onRemove` 的即时交错。聊天耗时改为每秒报告前 1 秒完成 tick 的平均值。Release 构建、CTest **14/14**、官方 codec **294 包**及有限时长背压/投影 TCP 冒烟通过；这些分别属于源码对照、C++ 回归和协议编码验证，未进行本轮真实 26.1 服务端逐刻差分。详见 [发包优化记录](performance.md#14-发包隔离广播阶段与每秒耗时显示2026-11-07)。
