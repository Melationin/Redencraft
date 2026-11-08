# 水与岩浆的 26.1 对照测试设计

本文是实现前的测试规格。用例、采集器、输出格式和命令均供后续实现使用。示例预期分为源码推导和待原版实测两类，当前没有新的运行结果或黄金文件。

## 1. 范围与验收口径

对照版本固定为 **Minecraft Java Edition 26.1**，协议 775、DataVersion 4786。以上一级只读 Java 源码解释行为，以同版本 Mojang 服务端实际输出裁决差异。原版世界使用默认数据包、常规红石和主世界属性。使用已有原版导出验证版本和文件哈希，禁止把 26.1.x 其他补丁版本、Paper、Pumpkin 或手写模拟结果混入原版黄金文件。 [26.1 官方发行说明](https://www.minecraft.net/en-us/article/minecraft-java-edition-26-1) · [本地导出清单](../assets/protocol775/manifest.json)

| 优先级 | 本阶段必须验证的行为 | 完成条件 |
| --- | --- | --- |
| P0 | `BlockState` 到 `FluidState`、水与岩浆计划刻、传播与退流、源转换、水岩浆反应、带水方块、现有活塞与红石、Anvil 待执行流体刻 | 实际状态、排队与执行顺序、关键副作用均有正负例，核心 CTest 和真实服务端差分通过 |
| P1 | 物品、TNT、下落方块、空矿车的流体交互，爆炸阻力与液体标签，协议 775 区块与增量，桶交互 | 各子项在纳入实现时通过对应的对照；桶还需验证库存和操作确认 |
| P2 | 气泡柱、海绵、海藻、冰、混凝土粉末、滴水石与炼药锅、岩浆随机点火、与维度或玩家物理耦合的机制 | 分项登记前置依赖，使用原版结果作为后续验收依据 |

**当前阶段**的实现仍保持单维度、只读世界、无世界生成和加载票；这只是迁移起点，不是项目定义（目标形态含多维度、加载票与生成，见 [AGENTS.md](../AGENTS.md) 第 1 节）。原版测试的加载票只用来保证原版用例能推进。C++ 用例显式加载共同的测试区块，比较区块边界行为时另设专门的加载策略用例。P0 包含已注册方块与流体有交互的行为。用例需要的新方块应先列出碰撞形状、流体接口、掉落、计划刻等依赖。缺失路径按项目约束标 `// TODO`，报告标为 `unsupported`。

现状参考 [兼容性](compatibility.md)、[实体](entities.md)、[存档](storage.md) 和 [协议](protocol775.md)。当前已有水和岩浆的存档方块状态、部分 `waterlogged` 属性、近似实体接触和爆炸阻力。`World::tick` 尚无流体阶段，`SavedTickData` 保留 `fluid` 标志但导入跳过流体刻，投影导入会拒绝 `PendingFluidTicks`。这些是迁移入口和旧行为基线。对照报告必须分开列出 **源码推导、C++ 回归、原版 JAR 离线探针、真实原版服务端差分**。

## 2. 需要从原版制作的对照逻辑

### 2.1 状态、标识与查询

| 方块状态 | 实际 `FluidState` 的类型 | `amount` | `source` | `falling` |
| --- | --- | ---: | --- | --- |
| 空气或干燥普通方块 | `minecraft:empty` | 0 | false | 属性不存在 |
| 水或岩浆 `level=0` | `minecraft:water` 或 `minecraft:lava` | 8 | true | false |
| `level=1..7` | `minecraft:flowing_water` 或 `minecraft:flowing_lava` | `8 - level` | false | false |
| `level=8..15` | 对应的流动类型 | 8 | false | true |
| 实现含水接口且 `waterlogged=true` 的方块 | 由该方块的 `getFluidState` 给出，简单含水方块通常为水源 | 8 | true | false |

`LiquidBlock.getFluidState` 将读取索引限制到 8，输入 `level=9..15` 仍可保留各自的**方块属性**。因此快照同时记录方块状态和流体状态。`createLegacyBlock` 的规则为源写 `level=0`，流动态写 `8 - min(amount, 8) + (falling ? 8 : 0)`。在测试原版运行时，直接查询其返回值。不要把解码后再次编码得到的状态覆盖导入状态。`isFull` 检查 `amount==8`，与 `isSource` 独立。`getOwnHeight=amount/9.0F`，同族流体在正上方时 `getHeight=1.0F`。

比较调度使用**精确类型**。水源与流动水是两个 registry key，同理岩浆。传播、液面以及同族匹配按调用点使用 `isSame`。伤害、浇灭、混合和特殊容器按实际源码路径使用 `FluidTags` 或精确类型。`BlockState.isAir`、`blocksMotion`、`isSolid`、共享面是否封闭、容器是否接收分别采集，不能从单一“可替换”标记推断。 [LiquidBlock](../../net/minecraft/world/level/block/LiquidBlock.java) · [FluidState](../../net/minecraft/world/level/material/FluidState.java) · [FlowingFluid](../../net/minecraft/world/level/material/FlowingFluid.java)

### 2.2 放置、形状更新与计划刻

以下流程供实现者对齐观测点。预期跟踪来自原版 JAR 的这些实际调用。

1. `Level.setBlock` 经区块写入，引发移除、放置和后续通知。记录请求 flags、实际状态写入和回调上下文。回调可能同步重入并替换刚写入的方块。
2. `LiquidBlock.onPlace` 和 `neighborChanged` 走 `shouldSpreadLiquid`。如果岩浆就地转化为固体，此路径结束。否则为当前位置的**当前精确流体类型**请求计划刻。气泡柱另有方块刻请求。
3. `LiquidBlock.updateShape` 在自身或指定邻居是源时请求当前位置流体刻。单测要传真实更新方向和捕获的邻居状态。含水铁轨、墙、珊瑚扇等分别沿各自的 `updateShape` 调用路径请求水刻。查询碰撞、液面或放置候选不得隐式调用这些更新回调。
4. `LevelAccessor.createTick` 以当前 game time 加 delay 得到目标刻，使用 level 的 `nextSubTickCount`。`ScheduledTickAccess` 将 Block 与 Fluid 分派给不同的 `LevelTicks`。跨队列共享计数来源，两个队列分别收集和执行。
5. `ServerLevel.tick` 推进 game time 后，依次执行 block scheduled、fluid scheduled、区块阶段、方块事件、实体、方块实体。每个计划刻队列各自上限 65,536。一个队列先收集再执行。方块回调投给稍后才收集的流体队列可在同刻运行，前提是延迟和资格满足。流体回调向已结束的方块队列、或向已经收集的流体队列投递的条目进入后续刻。
6. `ServerLevel.tickFluid` 在出队时读当前位置方块及其流体，只有 `fluidState.is(type)` 才调用流体逻辑。记录 `dispatch` 与 `run` 两条语义，旧源刻遇到流水时属于已分派、未执行。

队列观测必须区分**请求、真正入队、出队分派、实际执行**。只要条目仍在容器内，同队列、同位置、同精确类型的重复请求保留先入队者。另一精确流体类型或另一队列允许共存。普通 `createTick` 在尝试入队之前已经取得 subTick，重复请求也可能留下序号间隙。`hasScheduledTick` 检查容器内队列，`willTickThisTick` 对应本轮已收集、待运行部分。原版先按 trigger、priority、subTick 决定区块队首，再在符合资格的到期区块队首之间按 priority、subTick 竞争。积压用例能检测把所有条目全局按 trigger 排序的错误。相同到期、优先级和 subTick 的条目应标记比较器同键，保留原始 trace，并用独立观测判断并列是否影响传播。

[ServerLevel](../../net/minecraft/server/level/ServerLevel.java) · [ScheduledTickAccess](../../net/minecraft/world/level/ScheduledTickAccess.java) · [LevelTicks](../../net/minecraft/world/ticks/LevelTicks.java) · [LevelChunkTicks](../../net/minecraft/world/ticks/LevelChunkTicks.java) · [ScheduledTick](../../net/minecraft/world/ticks/ScheduledTick.java)

### 2.3 重算、扩散与写入次序

```text
流体回调入口读取当前方块和精确流体状态
如果不是源
    new := getNewLiquid(当前位置, 当前方块)
    delay := getSpreadDelay(old, new)
    new 为空时以 flags=3 写空气
    new 与 old 不同且非空时以 flags=3 写 new 的 legacy 方块
        然后为 new 的精确类型请求 delay
使用此回调持有的状态继续 spread
```

- `getNewLiquid` 按 `Direction.Plane.HORIZONTAL` 探测，顺序是北、东、南、西。仅通过共享面的同族邻居贡献最大 amount 和源数量。至少两个源，且对应源转换规则启用时，检查正下方 `isSolid()` 或同族水源/岩浆源。成功即返回新源。然后才查正上方，能通过面的同族流体产生 `amount=8, falling=true`。最后采用最大邻居 amount 减去衰减，结果不大于零时为空。
- 水基础 delay 5、衰减 1、坡度递归参数 4。普通主世界岩浆为 30、2、2。原版 `FAST_LAVA` 通过维度环境属性决定快速岩浆的 10、1、4。原版水源转换默认开启，岩浆默认关闭。岩浆的非空、非 falling 新旧态中，液面抬升时才进入随机 `nextInt(4)` 分支，非零时将基础 delay 乘 4。记录实际入队延迟。写方块触发的同步回调可能先抢到同位置同类型的队列槽。
- `spread` 先尝试向下。可写下方时先执行下方写入，随后以当前位置的水平源计数检查 `>=3`，满足时仍尝试侧流。这个计数与源转换中计入通透性的计数用途不同。向下未写时，源或下方不满足 `isWaterHole` 时才尝试侧流。`isWaterHole` 先查面，再看下格同族或可容纳对应流动类型。已有同族源是洞判定的一种情形，但不是可替换目标。
- `spreadToSides` 先用 `amount-dropOff` 检查可扩散量，falling 时该门槛用 7。实际各目的格的新状态仍由目的格 `getNewLiquid` 产生。`getSpread` 查各方向的最近落差，等距保留。坡度递归排除来向，临界深度和距离在源码探针中记录。最低距离的更新和候选能否替换是两个条件，测试要包括“低距离但拒绝写入”的候选。
- 方向**探测**按北、东、南、西，`getSpread` 返回 `EnumMap`，实际**侧向写入**按 `Direction` 枚举次序北、南、西、东。每个目的格写前重新读它的当前方块。采集顺序时记录这两条序列，并保留第一个写入的重入副作用。
- `canPassThroughWall` 查两侧碰撞形状，完整立方体直接挡，两个空形状直接通。其他情形用 `Shapes.mergedFaceOccludes` 判断相对两面的并集。动态形状绕过状态对缓存。候选通透性、`canHoldAnyFluid`、具体容器接受类型、已有流体是否能被替换均有独立负例。
- 普通目标经 `spreadTo`，水冲毁会先按原版路径掉落，岩浆冲毁走 fizz；容器走 `placeLiquid`。`LiquidBlock.shouldSpreadLiquid` 对岩浆按上、北、南、西、东查看。单个邻位先检查水，再检查魂土加蓝冰，命中即结束。水平或上方水可使源岩浆变黑曜石、非源岩浆变圆石。岩浆计划刻向下遇水时走 `LavaFluid.spreadTo`，目标是 `LiquidBlock` 时在下格生成石头。下格为带水宿主时，单独观察 fizz 和宿主保留。比较写入、计划刻与 `levelEvent(1501)` 顺序，方块事件队列与这个 `levelEvent` 不是同一接口。

[FlowingFluid](../../net/minecraft/world/level/material/FlowingFluid.java) · [WaterFluid](../../net/minecraft/world/level/material/WaterFluid.java) · [LavaFluid](../../net/minecraft/world/level/material/LavaFluid.java) · [Direction](../../net/minecraft/core/Direction.java)

### 2.4 宿主和现有机制的接缝

| 接缝 | 必须分别触发的原版路径与采集点 |
| --- | --- |
| 含水与桶 | `SimpleWaterloggedBlock.canPlaceLiquid` 的精确水源身份、`placeLiquid` 的 dry/wet 判断、flags=3 与水刻。`pickupBlock` 清除含水，必要时破坏不能存活的宿主。`LiquidBlock.pickupBlock` 只对 `level=0` 成功、写入 flags=11。`BucketItem` 决定点中的位置、外侧回退、玩家或发射器输入和库存变化。 |
| 摧毁与支撑 | `Level.removeBlock` 和 `destroyBlock` 可用原流体的 `createLegacyBlock` 替换宿主。显式写空气、因形状变化而 `updateOrDestroy`、玩家破坏分别走各自入口，记录实际保水、掉落和水刻。 |
| 活塞 | `PistonBaseBlock.moveBlocks` 的移动占位写入、旧位置清除、间接形状与 `neighborChanged`。`PistonMovingBlockEntity.tick` 正常完成先重算形状，对非空气的最终状态清 `waterlogged` 后写入。`finalTick` 走提前完成路径，对非源活塞直接使用形状重算返回值。测试分别固定提前收尾、普通完成、移动后的支撑失效，并保存移动方块实体内的 `movedState` 和进度。 |
| 铁轨和珊瑚扇 | `BaseRailBlock` 放置取源水判断，湿轨形状更新请求水刻。墙/地珊瑚扇自身的含水默认值、支撑与形状回调分别验证。活珊瑚的死亡逻辑属于单独依赖。 |
| 下落方块 | `FallingBlockEntity.fall` 携带去水的方块态，同时源位置写原流体 legacy 状态。落地按当前位置精确水源和可带水属性判断是否重新带水。混凝土粉末另有接触和高速路径采样。 |
| 爆炸 | 用同一坐标比较干宿主、含水宿主、纯水、纯岩浆的抗性和破坏结果。`ExplosionDamageCalculator` 在空气且无流体时返回空，其余取方块与流体抗性的最大值。先对照单条射线或局部封闭结构，再做有随机数的整体爆炸。 |

[SimpleWaterloggedBlock](../../net/minecraft/world/level/block/SimpleWaterloggedBlock.java) · [PistonMovingBlockEntity](../../net/minecraft/world/level/block/piston/PistonMovingBlockEntity.java) · [FallingBlockEntity](../../net/minecraft/world/entity/item/FallingBlockEntity.java) · [BucketItem](../../net/minecraft/world/item/BucketItem.java)

## 3. 制作原版对照

### 3.1 三种采集方式

| 方式 | 制作方法 | 可用于验收的输出 |
| --- | --- | --- |
| JAR 离线探针 | 使用 `validation/vanilla26_1` 已解包的内部 JAR 和全部 bundled libraries。参考 `VanillaOrderProbe`。初始化原版注册表后直接构造 `LevelTicks<Fluid>`，使用真 `Fluids` 单例和原版 `ScheduledTick`、`LevelChunkTicks`。对状态映射直接调用 JAR 的 `BlockState.getFluidState`。 | 所有 legacy level、流体精确身份、队列去重、优先级、积压、上限、存档 pack/unpack。用原版 `BlockGetter` 夹具还可验证液面与流向查询。 |
| 真实原版服务端加只读采集器 | 在隔离临时目录启动官方 JAR，参考 `ExactAgent.java` 的 `java.lang.classfile` agent 和 `run_exact_server.py` 的进程管理。采集器只安排用例输入和观察原版调用。 | 放置和传播、混合、红石与活塞、实体推进、存档重启、同一 tick 内的中间状态和回调顺序。P0 黄金文件以这条路径为主。 |
| 无采集器原版服务器 | 用原版控制台 `/tick freeze`、`/tick step`、`/time query gametime`、`/execute if block`。必要时在冻结状态保存副本并读 NBT。 | 确认少量关键格、方块实体和实体状态，用来交叉检查采集器是否扰动世界。控制台状态检查无法给出完整的同刻执行顺序。 |

复用方法和读取工具，使用新测试目录和端口。未来建议将源码放 `validation/fluid/vanilla/`，夹具放 `validation/fluid/cases/`，已评审的小型黄金结果放 `validation/fluid/golden/26.1/`。JAR、class、临时世界、全量 trace 和性能日志放仓库内忽略的 `build-fluid-validation/`。复用 [离线队列和完整服务端对照示例](../validation/safesave_order/vanilla/README.md) · [原版控制台示例](../validation/vanilla26_1/README.md)。

### 3.2 创建隔离服务端与确定起点

1. 从已解包的 `validation/vanilla26_1/versions/26.1/server-26.1.jar` 和 `libraries/**/*.jar` 构造 classpath，或使用经 SHA-1 核对的 `server.jar` 启动纯控制台路线。核对版本和 JAR 哈希，对照 agent 推荐沿用现有 Java 26 工具链。通过 `-javaagent` 和 `net.minecraft.server.Main nogui` 启动时，agent 本身的 class/JAR 也进入 classpath。针对当前 JAR 验证被 hook 的方法描述符、注入次数和首次执行；已经加载的类需要显式 retransform 时记录校验结果。采集器启动但关键 hook 缺失时本轮失败。
2. 每个 case 使用独立世界副本，绑定 `127.0.0.1` 的独立端口，`spawn-protection=0`。由测试操作者在测试目录自行提供已接受的 [Minecraft EULA](https://aka.ms/MinecraftEULA)。测试 runner 等待 `Done` 和采集器 ready 标记，按响应同步，预设超时，退出时发 `stop` 并等进程结束。记录退出码、日志、端口、世界路径及本地 JDK。没有玩家的跑法须核对空服暂停配置，原版例程可用 `pause-when-empty-seconds=-1` 保持循环；实际是否推进以 `gameTime` 和阶段钩子确认。
3. 世界建议由平坦模板生成或从干净原版副本克隆。主世界 y 范围固定 `[-64,320)`。覆盖用例观察区、预期扩散范围及读取余量，通常用 2 区块空白外圈并加封闭墙。原版对每个参加比较的区块调用 `setChunkForced`，等待 `ChunkStatus.FULL`、块可刻及 `ServerLevel.isPositionTickingWithEntitiesLoaded(chunkKey)` 都满足。实体交互用例还确认实体区段就绪。用普通的 `isLoaded` 或区块列表代替实际流体刻资格，会让一些用例静默不运行。
4. 关闭干扰，固定 `random_tick_speed=0`、`spawn_mobs=false`、`advance_weather=false`、无活动天气，明确设置 `water_source_conversion` 和 `lava_source_conversion`。开启 `tnt_explodes` 的用例单独列出。保留 tick 推进，记录真实 `gameTime`。实体、掉落和爆炸用例记录随机数控制策略，非随机用例也记录种子。规则设置完成后读回并写入 manifest。
5. 夹具分成 `prepare` 与 `actions`。先建外壳、支撑、静态机械和初始流体，再安装精确的初始待执行队列。用原版 Anvil 初态、明确的 `strict` 命令，或验证过的 setup-only 写入路径。存档用例通过实际存档加载，正常放置用例经过真实回调。准备后在原版和 C++ 检查静态状态、流体状态、两条队列和区块资格。`strict`、普通放置、测试框架原始写入各自单独编号。禁止为了对齐快照把正常放置产生的条目事后删掉。 [原版 SetBlockCommand](../../net/minecraft/server/commands/SetBlockCommand.java)
6. 定义 `T=0` 为两端准备通过后的 `gameTime`。`actions.after_tick=k` 表示 **第 k 次完整世界刻结束后**，在下一次推进前执行输入。`after_tick=0` 表示首刻前。用例内同刻多动作按给定数组顺序。需要注入“在 block 阶段请求 fluid”时，单独给出 hook 阶段和目标回调，避免从控制台睡眠间隔推断相位。
7. 采集原版在每个 `ServerLevel.tick` 完整返回后的快照。`MinecraftServer.tickServer(BooleanSupplier)` 返回可用于安排下一批外部动作；仅消费主世界 tick，冻结循环、启动 tick 和其他维度回调不能计入。无 agent 手工路线读 `time query gametime`，明确等到目标值再检查。长用例批进度可以按 checkpoint，仍保留关键 trace 的逐 tick 序列。

### 3.3 采集器接口与行为约束

| 观测点 | 导出数据 | 需要防止的误读 |
| --- | --- | --- |
| `LevelTicks.schedule` 和 `LevelChunkTicks.scheduleUnchecked` | `queue_kind`、位置、精确 type、trigger、priority、subTick、是否真正入队、请求来源 | 跟踪 LevelTicks 实例与主世界两条队列的绑定。存档解包的 `scheduleUnchecked` 标 `restore`。记录未加载容器拒绝和同键拒绝。 |
| `ServerLevel.tickBlock` / `tickFluid` 入口及匹配分支 | 执行顺序、当时方块、当前流体、计划类型、`executed` | stale 条目在入口可见，匹配分支才是实际运行。为匹配条目从已出队记录关联 trigger/priority/subTick，别从当前位置反推。 |
| `Level.setBlock` / `LevelChunk.setBlockState` | 调用前、实际区段写入的 old/new、返回、flags、updateLimit、栈深 | 只采集 `setBlock` 返回值会漏掉同步重入的临时写入。需要精确写序时在区段写入后、后续回调前采样。失败写和同态短路单独记录。 |
| `LiquidBlock.onPlace`、`neighborChanged`、`updateShape`、`FlowingFluid.tick/spreadTo` | 方法分支、相邻方向、源及目的位置、读到的宿主、候选和实际写入 | 记录进入与退出，`spreadTo` 调用也可能由容器拒绝。坡度详细探针仅用于小用例，不能进入性能采集。 |
| `Block.updateOrDestroy`、容器 `placeLiquid/pickupBlock`、`levelEvent` | 支撑失效路径、结果物品、是否 fizz、阻断前后的计划刻 | fizz 使用自己的 level event 序列，成功活塞 block event 单独记录。 |
| `doBlockEvent`、实体 tick、`PistonMovingBlockEntity.tick/finalTick` | 进入/成功状态、实体逻辑 ID、移动方块实体的状态与进度、tick 内次序 | 实体阶段与方块实体阶段分开。记录实体实际添加与移除时点。 |
| 区块广播及网络编码 | 聚合前脏标记、客户端可见的包、区段内局部位置、广播时状态、事件与序列确认 | 最终状态可从合并包得到，中间内部写入只在核心 trace。没有真实连接的原版核心运行不用于声称包已发出。 |
| 原版完整世界刻返回 | 观察范围的 block/fluid 状态、两队列待执行项、关键 BE/实体、运行边界 | 用显式区块状态查询，禁止采集触发额外区块加载。待执行列表的容器遍历顺序与实际执行顺序分开保存。 |

用 tick 内单调递增 `seq` 串联相位，嵌套调用保存 `depth`、`parent_seq`。在目标线程把原始字段复制成不可变记录，在 case 结束后再序列化。不要在回调内打日志、修改世界或额外消耗随机数。实体无玩家用例不额外启动客户端。协议用例以本地 socket 录包/已有协议测试驱动为主，真人显示验证另列。每种 P0 核心夹具至少做一个无详细探针的原版终态复验，证明观测本身没有改变结果。

### 3.4 可手工复现的 W-01 最小用例

坐标 `P=(0,80,0)`，X 正方向为水槽。石质底板覆盖 `x=-1..10,z=-1..1,y=79`，在 y=80 封两侧和两端。内部 `x=0..9,z=0` 为空气，顶部无其他流体。先强加载包括边墙在内的区块并等待可刻。下面的控制台命令在世界冻结后执行，行首斜杠按控制台惯例省略。

```text
tick freeze
gamerule random_tick_speed 0
gamerule spawn_mobs false
gamerule advance_weather false
gamerule water_source_conversion true
gamerule lava_source_conversion false
forceload add -16 -16 31 31
fill -1 79 -1 10 79 1 minecraft:stone
fill -1 80 -1 10 80 -1 minecraft:stone
fill -1 80 1 10 80 1 minecraft:stone
setblock -1 80 0 minecraft:stone
setblock 10 80 0 minecraft:stone
fill 0 80 0 9 80 0 minecraft:air
time query gametime
setblock 0 80 0 minecraft:water[level=0]
tick step 4
```

如果槽内本来全是空气，清空槽的 `fill` 可能返回零变化，先确认内部状态再继续。每次 `tick step` 后等待世界 `gameTime` 达到目标再执行 `execute if block <x> 80 0 <state>`。下面为源码推导的关键点，首次黄金记录时逐 tick 核对它们。`/tick step` 的控制台回执是请求被接受的反馈，不能直接用作本轮完成标记。

| T | 检查 |
| ---: | --- |
| 0 | `x=0` 为源水，第一条源水请求 due=5，`x=1` 仍为空气 |
| 4 | `x=1` 仍为空气 |
| 5 | `x=1` 为 `water[level=1]`，`x=2` 仍为空气 |
| 10 | `x=2` 为 `water[level=2]` |
| 35 | `x=7` 为 `water[level=7]`，`x=8` 为空气 |
| 40 | 记录稳态和待执行队列。在本刻之后使用普通写入将 `x=0` 改为空气，逐刻记录退流至恢复干燥 |

同一结构换成 `lava[level=0]`，普通主世界下检查 T=29 无首次传播、T=30/60/90 的 x=1/2/3 分别为 `level=2/4/6`。退流与源切换用真实 trace 确定，禁止把旧水槽的退流时间直接套到岩浆上。控制台 `setblock` 有自己的 flags 和邻居通知调用，手工用例比关键状态。精确 trace 用例在两端都通过指定的 `set_block(flags=3)` 输入。

### 3.5 夹具与结果格式

一个 case 输入应包含以下最小字段。静态方块用规范名和属性，队列保存 signed delay/priority/subTick，未指定的状态由 case 内的明确空域规则给出。下例中的矩形是**块坐标**，而 `chunks` 是明确的区块坐标列表。

```json
{
  "case": "W-01-water-channel",
  "version": "26.1",
  "data_version": 4786,
  "dimension": "minecraft:overworld",
  "rng_mode": "no_random_effect_on_asserted_state",
  "origin": [0, 80, 0],
  "bounds": {"min": [-1, 79, -1], "max": [10, 81, 1], "inclusive": true},
  "chunks": [[-1, -1], [-1, 0], [0, -1], [0, 0]],
  "gamerules": {"random_tick_speed": 0, "water_source_conversion": true, "lava_source_conversion": false},
  "initial": {"layout": "W-01 sealed channel", "block_ticks": [], "fluid_ticks": []},
  "actions": [
    {"after_tick": 0, "op": "set_block", "pos": [0, 80, 0], "state": "minecraft:water[level=0]", "flags": 3},
    {"after_tick": 40, "op": "set_block", "pos": [0, 80, 0], "state": "minecraft:air", "flags": 3}
  ],
  "capture_until": 240
}
```

该例的 `chunks` 表示布局直接占用的四块，实际服务端跑法再添加外圈和资格检查。正式夹具需展开 `initial.layout` 为可读的 `fill`、`set`、容器或 BE 输入。导出 `manifest.json` 记录 case 内容哈希、JAR 哈希、源码依据、C++ commit、JDK、构建和分配器配置、种子、规则、区块集合、初始时间、期望运行长度和所有捕获点。世界数据和协议文件的修改属于后续实现阶段。

每行 NDJSON 表示一个记录。以下四行是格式示意，省略了中间的通知和原版内部顺序。

```json
{"tick":0,"phase":"input","seq":0,"kind":"schedule","queue":"fluid","pos":[0,80,0],"type":"minecraft:water","due":5,"priority":0,"sub_tick":0,"result":"accepted"}
{"tick":5,"phase":"fluid_ticks","seq":0,"kind":"dispatch","pos":[0,80,0],"type":"minecraft:water","executed":true}
{"tick":5,"phase":"fluid_ticks","seq":1,"kind":"write","pos":[1,80,0],"old":"minecraft:air","new":"minecraft:water[level=1]","flags":3}
{"tick":5,"phase":"after_world_tick","kind":"cell","pos":[1,80,0],"block":"minecraft:water[level=1]","fluid":{"type":"minecraft:flowing_water","amount":7,"source":false,"falling":false}}
```

有 schedule 请求未入队时，`result` 写 `duplicate`、`missing_container` 或实际原因。有 stale dispatch 时另存 `state_type_at_dispatch`。正式快照记录 fluid 属性、ownHeight、实际 height 与需要时的 flow 向量，实体记录位置、速度、落地、火焰/受伤和类型专有字段，BE 记录逻辑状态。`seq` 是跟踪序号，`sub_tick` 是队列排序键，二者独立。

## 4. P0 用例目录

除专门的边界用例外，表中 `P` 可取 `(0,80,0)`，所有偏移相对它。竖直支撑用石头，外壳远离观测格。每个写入都注明走正常回调或静态夹具。源码能直接导出的断言写进 C++ 聚焦用例，复杂序列用原版采集确定。`T+n` 指输入或准备后的目标游戏刻。

### 4.1 状态和调度

| ID | 夹具与动作 | 必须检查的结果 |
| --- | --- | --- |
| F-01 | 分别枚举水、岩浆 `level=0..15`；邻域分别为空气、正上同族流动、正上异族。另放空气、石头、四种 dry/wet 铁轨、墙、地面/墙面枯珊瑚扇。 | 方块原始属性、精确流体 ID、amount、source/falling、ownHeight、height、legacy 结果。额外比较 `level=8` 与 `15` 在流体身份上的相等和在方块快照中的不同。查询前后两队列和块版本号相同。 |
| F-02 | 从原版 runtime 对含水兼容方块导出 `BlockState` 的相关属性、`blocksMotion`、`isSolid`、默认流体、精确 tag、各面几何。对状态导出表中的全部水/岩浆协议状态回查。 | 不由方块名推断 `isSolid`，含水使用宿主的属性；基础状态和网络 ID 与官方导出一致。上一级源码只读，动态形状用具体邻域查询。 |
| Q-01 | 同一 P，依次投 `water` due=10 NORMAL、同 key due=5 HIGH、`flowing_water` due=5 NORMAL，同时投一个 Block 队列条目。P 初始放静态源水，第二轮在 T=4 用静态路径改为 `water[level=2]`。 | 第一个源水请求保留；流水请求可共存；另一队列独立。T=5/T=10 的实际 `dispatch` 和 `executed` 随当前精确类型互换。先用离线队列探针隔离，真服务器再验证身份检查。 |
| Q-02 | 在每个边界 `b=0,16,512,8192` 分别向 `x=b,b-1,b+1,b-2` 按给定顺序投相同 due 的 HIGH/NORMAL 交替条目，在相邻两次流体请求间插一次 block 请求。沿 X/Z 两轴各跑一次。 | 两队列共享的 `subTick` 单调增长，各自只按自己的 priority/subTick 执行；本刻 block dispatch 全部在 fluid dispatch 之前。同类型重复不改变实际队首。负坐标向下取整。 |
| Q-03 | 在同一区块投 `A0:due=T-10,NORMAL,sub=0` 和 `A1:due=T-9,HIGH,sub=1`，另一块投 `B:due=T-3,HIGH,sub=2`，只在 T 开放资格。 | 预期 B、A0、A1。A1 不可跨过同容器内早到期的 A0。重复在资格恢复前取快照，所有延迟已过的条目保留至允许执行。 |
| Q-04 | 离线夹具和真实服务端测试注入各准备一条 block 与 fluid 到期项。block 回调内向两个队列投 delay=0，fluid 回调再向两个队列投 delay=0。位置和 type 各异。另加“当前已收集项同键再调度”小例。 | block→fluid 的新项可在当前 fluid 阶段入收集，block→block 与 fluid→fluid 等待下一次各自收集，fluid→block 也在下一刻。记录 `hasScheduledTick` 和 `willTickThisTick`，检验已收集项的 key 生命周期。注入回调属于调度用例控制，流体规则仍由官方实现执行。 |
| Q-05 | 在两条队列中各投 65,537 个到期项，使用已加载区块内互异位置和保证 stale 的类型，记录第 65,536、65,537 项。另在未注册容器上发一次请求。 | 两类各自收集 65,536 个，剩余项下一刻分派，stale 一样消耗分派名额。缺容器拒绝没有世界写入或伪造条目。用慢速回归档运行。 |
| Q-06 | 主线程外部动作顺序分别为「放置流体后更换宿主」「先更换后请求」「同族不同身份」「更换为异族」。记录待处理项，再推进 1、5、30 刻。 | 已排项到期时才检查当前精确类型，旧类型不得执行。新类型按自己实际请求的 due 执行。各路径无意外的计划刻取消或合并。 |

### 4.2 水、岩浆与几何

| ID | 夹具与动作 | 必须检查的结果 |
| --- | --- | --- |
| W-01 / L-01 | 按 §3.4 构建水平封闭槽，水/岩浆分别从 `level=0` 开始。稳态后去源；另一分支在前沿前一格加挡板再移除。 | 逐格的第一到达刻、legacy level、源/流水身份、阻断后的传播、重开后的请求与退流。原版黄金记录覆盖到静止或预定截断。 |
| W-02 | 竖井中心 `y=83..80`，底板 y=79。y=83 放水源，y=80 有一个东侧出口并给出口铺底，其他同层方向封死。以相同夹具换普通岩浆。 | 水在 T=5/10/15 到达 y=82/81/80，均为 `level=8`；T=20 检查底层东侧 `level=1`。对应岩浆验证 falling 与落到底后的实际衰减，观测边界和原版最终状态。 |
| W-03 / L-02 | 中央 P，东西各一源，旁路封闭；支撑替换为石头、同族源、同族非源、空气、半高形状。每种支撑将 source conversion 规则设为 true/false，两侧之一再替换为单源或精确带水宿主。 | 比较 P 在首次可写时是否升为源、是否 falling、后续独立入队。孤立单源和规则关的负例。支撑用原版 `isSolid`/同族源结论；几何负例由通面决定是否计入邻源。 |
| W-04 | 关闭源转换，P 原有流动水，正下有通路，周围分别安排 2、3、4 个可识别的源邻居；有空余时留一个方向作侧出口。给下方另换为同族源、空气、满方块、容器各一轮。 | 向下实际写、写后判断侧流、已有源导致不能写但仍被视为洞的分支；检查下方写入与侧流顺序。四源全占的组用于计数观测，侧向结果由可写格决定。 |
| W-05 | 石质平面 y=79，P 在 y=80。给东/西开由墙限制的一格宽通道，在各自的 `d=1/2/3/4/5/6` 处挖一个 y=79 落口。组合距离为等距、近远、无可达洞。北/南相同重复。 | 当前批候选、最短距离、回避来向、递归临界点、并列写序。无洞情况保留可写方向。再做“距离更小但不可替换”，保持面通且使用不同流体或特定容器，记录最低值是否排除远侧。 |
| W-06 | P 两侧放上/下半砖、台阶、石墙、打开/闭合的栅栏门；另用可手工建的移动活塞形状。每种状态测南北、东西、上下，改变第二格状态制造对面形状互补。 | 测试两块单独不封整面、合并后能封整面；非实体满立方和部分形状分开。动态活塞在相同 state ID、不同 BE 进度下重算面。热/冷缓存、多坐标排列结果相同。半砖/台阶等未注册前按依赖阻塞报告。 |
| W-07 | 在平槽中预置枯珊瑚、羊毛地毯、铁轨、红石粉、玻璃等位置。水和岩浆分别向这些位置流。夹具区分实际可冲毁对象、只可含水宿主和完全阻断对象。 | 目标方块、掉落和 fizz 序列，独立验证水的掉落先于替换、岩浆反应路径。红石粉等依赖尚缺时保持 `unsupported`，不能把注册占位当行为正确。 |
| L-03 | 静态预置 P=`lava[level=6]`、侧面岩浆源、完整地板，显式安排 P 的 `flowing_lava` 刻。对照无抬升、old/new falling、同族在上方导致实际高度变 1 等分支。 | 流动液面抬升时的实际 RNG 分支与 30/120 delay，验证未走分支时不额外取随机数。捕获请求和接受结果，避免把同步回调的较早入队当成随机分支没执行。两条随机分支均用原版实际命中样本。 |
| L-04 | 原版扩展用例在启用 `FAST_LAVA` 的维度环境中重复 L-01、坡度深度边界和 L-03。主世界项目用例只覆盖普通参数。 | 原版采集并归档 10/1/4 和 10/40；项目范围内若以后提供单维度参数化，再映射到同一测试。维度水蒸发是桶路径单列。 |

### 4.3 水岩浆接触和含水

| ID | 夹具与动作 | 必须检查的结果 |
| --- | --- | --- |
| M-01 | P 分别为源岩浆、`level=2`、`level=8`。只在北侧或正上加水，分别先放岩浆和先放水。另用含水墙/铁轨替代水格。 | 检查岩浆位置变黑曜石或圆石的实际调用相位、是否先写流体、fizz 次数及旧条目 stale；按流体标签识别含水宿主。加同族和对角线的无接触负例。 |
| M-02 | P 下方为水，侧壁封死，依次使用源水、流动水、带水宿主。源岩浆从上落下。另用水向下接触浅/深岩浆，覆盖 `0.44444445F` 阈值两侧。 | 普通水格在实际向下 `spreadTo` 时变石头。含水宿主的容器接受判定可能在进入 `spreadTo` 前即拒绝，分别看候选、fizz 与宿主。反向用例记录 replacement 判定和两端最终材料。 |
| M-03 | P 下方魂土，侧面蓝冰。做缺魂土、缺蓝冰、蓝冰在下方，以及水与蓝冰分布在不同检查方向的对照。 | 普通组生成玄武岩，对照组使用原版遍历的第一次匹配。特别记录“更早方向蓝冰、更晚方向水”和交换方向后的不同，不把水判断提升为全局优先。 |
| H-01 | 在支撑上放一段 `rail`/`powered_rail` 或墙。分别从源水、流动水和空气中执行同一放置动作。另在干宿主上直接调用真实容器 `placeLiquid`，参数分别为水源、流水、岩浆。 | 放置的 `waterlogged`、几何/朝向/动力属性、宿主对应 `getFluidState` 和新增水刻；容器拒绝流水和岩浆的负例，不破坏原宿主。 |
| H-02 | 同一含水宿主依次分组做邻居 shape 更新、neighborChanged、破坏下方支撑、正常 destroy、显式 set air；墙珊瑚扇用其实际支撑侧。 | 捕获对应路径、掉落、保留的流体与实际 schedule。形状重新计算无副作用的查询单独验证。旧计划刻碰到空格、水源或其他宿主时按精确身份执行。 |
| H-03 | 可带水宿主周围有两个源，中央格从干变湿。邻域改成仅可向下填充、相对面遮挡、源转换关闭，以及填充后再次请求。 | 原版 `getNewLiquid` 可能先产出水源并允许容器接收。记录“可把某格视为洞”“选中该方向”“容器接受”“真正变湿”四个状态。已有湿宿主重复调用、计划刻去重、失败时库存无变化。 |

### 4.4 活塞、红石与相位

| ID | 夹具与动作 | 必须检查的结果 |
| --- | --- | --- |
| P-01 | `(0,80,0)` 朝东普通活塞，待移动宿主放 `(1,80,0)`，目的格 `(2,80,0)`。优先用经原版确认可推动的含水墙，干/湿源和干/水目的地组合，独立为原位置及目的地安排流体刻。`(-1,80,0)` 红石块上电。 | 记录方块事件内的实际写序、源位置去向、`moving_piston` 与 BE 的 movedState、每刻进度、正常完成后的含水清理。目的地流体原值可触发不同更新，逐刻以原版为准。 |
| P-02 | 沿用 P-01，在进度未完成时撤电或在原版允许的替换入口提前打断移动方块；另一组用搬到无支撑位置的珊瑚/相关宿主，按其原版 push reaction 选择普通完成或先破坏。 | 提前 `finalTick`、正常 `tick`、因 `updateOrDestroy` 变空气分支分别覆盖，按实际原版路径比较含水、宿主和掉落。记录 `progress`、`progressO`、source/extending 和 `lastTicked`。 |
| P-03 | 在水槽前沿依次布置观察者、音符盒或可掉落地毯，再连接活塞。用石壁隔离液体的方向，提供独立触发面。在同一 tick 为流体、观察者和活塞安排到期输入。 | 比较 shape 更新触发的计划刻、`neighborChanged`、block event 的插入和实际执行，实体/BE 仍在各自阶段；湿分支与干对照均保留。覆盖液体到达恰好在活塞动作前和晚一刻两个时间关系。 |
| P-04 | 先用最小干态 Carpet/TNT 装置的已知回归，再加一格水或含水支持，且明确相对位置和起始时刻。另在完全隔水边界重复原干态回归。 | 已有复制或破坏行为按各自原版轨迹对照，不因流体支持把移动更新重排。对 TNT 记录 primed 实体数、位置、引信、爆炸及附近方块，掉落物与方块移动分别统计。 |

### 4.5 存档、边界和长期收敛

| ID | 夹具与动作 | 必须检查的结果 |
| --- | --- | --- |
| A-01 | 用真实原版在一个水槽有未来流体刻时冻结并保存。解析 `fluid_ticks` 的 `i,x,y,z,t,p` 与 `block_ticks`，记录各自列表顺序及保存 gameTime。原版重启读取同一副本，C++ 走 `loadAnvilWorld` / `importWorld`，输入文件只读。 | `t` 为相对延迟，`due=unpack 时 gameTime+t`。精确类型保留 `flowing_water` 等。按各容器、各类型队列恢复 `-n..-1`，与新请求竞争；出队时刻、priority 和状态与原版重启对照。检查输入文件哈希前后相同。 |
| A-02 | 精确存档夹具包含相同 delay 的 HIGH/NORMAL、已逾期的负 delay、同位置 Block/Fluid、源/流水两个精确类型，另有含水宿主及正在移动的活塞。用真实原版 loader 验证夹具有效。 | 还原后的负 subTick 不变成大无符号数；块刻和流体刻的负序列分别由各自保存列表构造。保存队列迭代次序无法代表原进程原始入队顺序，因此用“同存档重启”作基准。保留异常/无效 tag 的解析诊断。 |
| A-03 | 设置地形跨 `x/z=0,16,512`，以及 `y=-64,-1,0,15,16,319` 的合法上下邻格。边界外 `y=-65,320` 只作为越界输入或读取点。测试在同一合格区块集、C++ 未加载邻块和原版不可刻/实体未就绪状态分别进行。 | 合格区块组逐刻完全对照。加载策略差异组分别记录缺容器、暂停/恢复和外侧写拒绝；C++ 不能借流体创建区块。不得把原版加载票推进所致结果归为流体规则失败。 |
| A-04 | 导入投影中纯流体状态、带水状态、`PendingFluidTicks` 和未知流体类型。分别测试格式支持、错误路径与读入后的首次更新。 | Anvil 与 Litematic 入口独立。若阶段内未实现投影计划刻，保留明确错误并分类；支持后核对坐标变换、相对延迟、priority 和精确流体身份，禁止静默丢弃。 |
| A-05 | 封闭槽、交汇池、断源瀑布各跑足够长，终态再跑一个覆盖最长已知延迟的窗口。额外造一个故意无限重复更新的红石输入。 | 稳态类无新增非预期流体写入、实体或队列积压。循环类按定义窗口和事件序列比较，不把“队列非空”直接判为错误。失败时保留最早偏离的 tick。 |

[SavedTick](../../net/minecraft/world/ticks/SavedTick.java) · [原版区块保存](../../net/minecraft/world/level/chunk/storage/SerializableChunkData.java) · [项目只读存档](storage.md)

## 5. P1 和 P2 接入测试

### 5.1 实体的高度、推流和伤害

原版 `EntityFluidInteraction` 用缩小 `0.001` 的交互 AABB，扫描之前还在 X/Z 多看一格以确认区块 `FULL` 与区段有流体。相交检查使用位置相关的液面，实际接触高度相对于实体包围盒底边。逐格流向使用 `FlowingFluid.getFlow`，包含相邻液面落差、边缘下格、falling 流体贴实面时的下拉。浅水的累积规则使用当前 tracker 的高度，多个格子的累积保持扫描次序。对非玩家实体，原版归一化总流向后乘水 `0.014` 或普通主世界岩浆 `0.0023333333333333335`。水平速度两个分量均小于 `0.003` 且冲量长度低于 `0.0045` 时存在最小冲量分支。判断是否推流、初始接触更新与实体自身的推进位置按各类型调用点验证。 [EntityFluidInteraction](../../net/minecraft/world/entity/EntityFluidInteraction.java) · [Entity](../../net/minecraft/world/entity/Entity.java)

| ID | 布置与变量 | 验收数据 |
| --- | --- | --- |
| E-01 | 预置静态液体，关闭该观察期的流体推进来源。给相同实体 AABB 的底面安排在液面下、正好相切、液面上。以源、`level=4`、falling、上下叠置、带水铁轨、异族正上和缺邻区块重复。 | `getFluidHeight`、入水/入岩浆、是否在该刻重置 tracker、进出后残留状态。用原版 float 计算临界液面后生成相切坐标，再在其两侧加小偏移。 |
| E-02 | `P` 周围组合低液面、同高液面、阻挡侧面及无阻挡的下一级落差。falling 加一侧实壁。多个液格的流向设为同向、相反和近乎抵消。改变初始水平速度以跨过最小冲量门槛。 | 保存每格流向、实际合计冲量、施力前后速度以及是否触发最小冲量。对比水与普通岩浆、开启/禁止实体受流体推动、干世界同一坐标基线。 |
| E-03 | 创建固定姿态和速度的掉落物，依次经过静水、流水、出水、岩浆。距离 `0.1F` 接触深度阈值两侧各布点。增加水中已有火焰的情况。 | 逐刻位置、速度、阻尼/浮力、火焰刻、伤害和移除。以原版 `ItemEntity` 的接触更新时间核对，一次刻内推进前后的接触数据分开。随机生成初速度属于单独分支。 |
| E-04 | 固定 TNT 初始位置、速度和引信，水槽中分别经过静水、falling、流水和离水。设引信刚到期与普通刻两个分组，再做水阻挡爆炸用例。 | 逐刻位置、速度、落地、引信、移除与爆炸位置。检查死亡路径与有剩余引信路径的 `updateFluidInteraction` 差异。爆炸对方块与实体的结果独立比对。 |
| E-05 | 下落沙子和其他已支持下落方块穿过水层再落到实体地板。普通沙子与能触发水固化的混凝土粉末分开。使用带水可下落宿主时记录起始的流体释放。 | 源位置、携带态、是否提前落地、落地置块、流体变化、实体移除。高速混凝土粉末额外验证碰撞线段能查到水，低速与同族流动状态都要覆盖。 |
| E-06 | 干轨和带水轨、流水进入轨道附近、空矿车离轨入液面；固定方向、初速度及轨道序列。 | 轨道状态、矿车速度及入水方向，普通矿车刻与流体速度更新顺序。现有矿车算法与原版若有独立差异，先报告干态偏差，再用局部原版回调定位流体差异。 |
| E-07 | 水/岩浆屏障、带水墙和干墙分别置于同一个爆炸中心的射线路径。追加对称爆炸和实体遇流碰撞的干湿对比。 | 第一层只核对方块与流体抗性和单射线通过/停止，第二层核对破坏集合、掉落、实体击退。记录随机数序列和现有爆炸实现未移植的相关差异。 |

用例应同时包含静态液体专测和真正由流体计划刻动态形成的水流。动态组在同一 tick 先采集 fluid 阶段写入，再采集实体接触，防止实现延迟一个实体刻才看到新水。最小分支测量用固定构造参数，整体测试用共享夹具。带玩家的水流、窒息、游泳、载具乘员和泡泡推力独立列为未来范围。

### 5.2 桶与协议 775

| ID | 夹具与输入 | 验收数据 |
| --- | --- | --- |
| N-01 | 快照区段中混放纯水、流动岩浆、带水铁轨/墙及干对照；跨区段 y 正负边界及负区块 X/Z。 | 冻结快照、官方状态 ID、non-empty/fluid 计数、palette、现有高度图策略、区段坐标编码正确。内容以 `assets/protocol775` 导出和原版解码器验证。测试快照冻结后世界再变动的后台编码。 |
| N-02 | 一个刻内反复改同一湿格，另外让流体刻生成多格，再触发同刻活塞事件。加入较远的另一段同区块和正在进入视野的区块。 | 普通块变化在原有区块广播阶段读取最新位置状态，同格去重。单格和多格包分别覆盖。发生在该阶段前的流体变化可于本刻广播，活塞事件之后的块变化留到下一次广播。验证活塞事件时客户端能重建其输入结构。跨区块总序按协议文档的现有边界分类。 |
| N-03 | 真实协议输入分别用空桶点水/岩浆源、点流水、点湿宿主，用水桶点干宿主、实心块外侧与可替换方块，再用岩浆桶点同一宿主。固定玩家视角、射线起点、权限、创造/生存模式前提和序列号。 | 根据实际 `BucketItem` 和 `BucketPickup` 对照命中模式、是否回退邻格、方块/流体、库存、音效和修正顺序。纯回调测试先于协议输入。源桶取出及 `SimpleWaterloggedBlock` 分支各自覆盖成功/失败。协议未实现完整玩家状态时分层登记。 |
| N-04 | 连接一个不读包的会话和一个正常会话，让大面积水槽跨多个 section 更新。另在异步区块快照排队时放水、移除水。 | 慢会话可断开，健康会话 FIFO 和核心流体队列继续推进。核对脏块包与区块批次边界，后台不读活世界。以 `Verifier` 路线严格解码完整负载。 |
| N-05 | 有 fizz、实体水中移动、爆炸或落地的混合 case。 | level event、block event、实体速度/位置/数据和普通块包分开核对。原版验证使用同类连接录包；C++ codec 回读单独记证据。可见画面另设用户授权的客户端验证，网络包通过只证明编码和所测顺序。 |

`BucketItem` 正常使用涉及射线和上下文。直接调用 `placeLiquid` 的结果可以成为容器契约黄金文件，但它无法证明协议端的视角、距离、预测或库存正确。对增量包使用已有 `protocol_tests`/`socket_test` 框架，并把原版码流中的位置列表与核心写入列表分开。 [协议验证流程](../validation/protocol775/README.md) · [项目协议实现](protocol775.md)

### 5.3 扩展机制的最小独立用例

| ID | 机制与夹具 | 必须保留的前置条件与输出 |
| --- | --- | --- |
| X-01 | 气泡柱在全源水竖井底部放灵魂沙或岩浆块，插入一格流动或异族流体截断，再撤底块。 | 用 26.1 `bubble_columns_can_occupy` 和两个底座方块标签。检查方块刻、流体刻、柱段状态、方向与实体进入/离开。区分普通水与承载气泡柱的方块。 |
| X-02 | 海绵靠近水源、流水、带水方块和可清除水生方块，逐步增加至距离/数量边界并做闭墙负例。 | 比较原版遍历访问顺序、吸水上限、`BucketPickup` 路径、宿主是否保留、湿海绵状态和水刻。其他维度的蒸发路径另测。 |
| X-03 | 水生植物从源/流水处放置、长高、砍断支撑。以海带、海草与含水普通块对照。 | 检查真实流体标签、支撑依赖和源转换。随机生长条件单独打开并记录 RNG，P0 条件下保持关闭。 |
| X-04 | 冰在有/无下部支撑时破坏、受光融化；混凝土粉末接触不同方向的源水、流水或带水宿主，并覆盖下落高速路径。 | 用不同 case 隔离随机刻、光照、形状和实体落地，输出原版回调和生成物。 |
| X-05 | 滴水石上方给水源或岩浆，下方放不同炼药锅；岩浆随机点火放可燃/不可燃邻域。 | 分别核对来源、距离、规则、随机输入和方块计划刻。不开随机刻的 P0 测试应无此类副作用。 |

为 P2 用例记录所需的游戏规则、原版标签和新增方块依赖。玩法扩展的失败不应被并入“基础流体已通过”的数字。

## 6. 差分、随机性和失败定位

### 6.1 比较顺序

1. **先核输入。** 对比版本、维度规则、方块初态、实体初态、初始队列、区块资格、计划 tick 数与输入动作。任意一项不同立即记 `fixture_error`。原版 hook 没命中、目标区块从未可刻、没有取得最终标记、runner 超时均属于 `invalid_run`。
2. **按相位逐刻核对。** 先比 schedule 请求/接受与 dispatch/run，再比有语义的写入、通知、块事件、实体和 BE，然后比刻末状态。报告首次差异所在 tick、phase、seq、坐标及前后状态。流体退流和写入顺序不得只比最后一帧。若出队后最终世界完全相同，队列或副作用的顺序差异仍按发生位置报告。
3. **按语义标准化。** 方块 ID 与属性名排序，流体保存原版 registry key、明确 source/falling，空流体与未知流体不同。观察区以固定坐标排序，未加载格单列。初始和待执行快照中的 map 可排序，实际有序 trace、保存列表、实体注册顺序和协议 FIFO 保留。NDJSON 先检查记录完整性与 schema 版本再计算指纹。
4. **时间、数字和身份。** 外部对照统一用相对游戏刻。`due` 和 `sub_tick` 均有符号，live subTick 可减共同基准并保留相对次序；恢复的负序列和与 live 请求的跨零关系独立核对。绝对 JVM UUID、内部状态 ID、内存地址、线程、包的墙钟时间不入语义 diff。实体用 fixture 逻辑 ID 或已验证的生成次序匹配。液面及速度按用例设定容差，默认液面/流向分量 `1e-6`、短固定输入轨迹位置和速度 `1e-6`，跨越分支阈值的命中结果精确比较。出现 NaN、无穷或缺值立即失败。长轨迹需要更宽容差时说明数值来源和上限，并仍校验关键事件的 tick。
5. **别消掉真实顺序差异。** 对相等比较器键记录集合及原版实际序列。只在原版重复运行证明确实没有稳定全序、并且互换不会改变本用例的可观察结果时，允许限定键内的顺序差异。不同 block/fluid 阶段、通道/目的格依赖、P0 流体写入和区块内协议 FIFO 不能整段排序后比较。跨区块广播的实现边界沿用项目协议说明。
6. **核对副作用和等待边界。** 最后比较待执行项、队列总量、收敛类的静止窗口、生成/移除实体及掉落。脱离观测范围的流体写入也要计数并定位，防止因裁剪使溢出或缺写被掩盖。稳定指纹可用于快速筛选，失败时展开原始记录。

建议的最小差异报告如下。此例仅描述输出格式。

```text
case: Q-01-exact-fluid-owner / phase: fluid_ticks / relative tick: 5
first divergence: dispatch at (0,80,0)
original: expected minecraft:flowing_water, current minecraft:water, executed=false
candidate: expected minecraft:flowing_water, current minecraft:water, executed=true
preceding evidence: accepted schedule seq=2, due=5, priority=NORMAL
next observation: fluid phase wrote (1,80,0), pending queue lengths differ
classification: scheduler identity mismatch
```

### 6.2 随机数的取证

- **确定性夹具**关闭随机刻和生物，把断言限制在与 RNG 无关的字段，或直接固定实体初始位置、速度和引信。`levelSeed` 只证明用例选择了同一个种子，不能证明两边的 `RandomSource` 已消耗相同序列。
- **分支夹具**覆盖岩浆 30/120 刻、掉落、TNT 速度和爆炸。两端各自使用并记录随机序列，跨端验收比较触发条件、合法结果、概率和可观察的回调顺序。原版 agent 在实际随机调用点记录输入界限和返回值，不主动探测或额外调用。控制原版启动与输入路径并重复采集，直到观察到所有分支。若项目需要注入固定序列，标为受控 RNG 回归，独立于未注入的原版服务端对照。不得生成伪造的原版输出补齐分支。
- **整体夹具**保存 RNG 策略、种子、初始状态和 trace。若已知某一路径的随机流不可对齐，仍核对确定性项，如目标可写性、是否发生反应、标称范围、何时请求更新。精确的落点、掉落数量或爆炸半径应根据该 case 实际定义标 `inconclusive` 或有证据的已知差异。

### 6.3 失败定位顺序

| 表现 | 首查内容 |
| --- | --- |
| 原版和 C++ 都没有流动 | 是否真正调用 `onPlace`、初始有无入队、游戏刻是否推进、区块资格是否就绪、流体类型是不是 stale |
| T=0 就不同 | `strict` 与普通输入混用、夹具静态写副作用、旧模板留下的 fluid ticks、规则或隐含水源 |
| 每次差一刻 | `T=0` 定义、采集点是否在完整 tick 返回、due 是否基于 gameTime 而非日夜时钟、是否把两个队列合并收集 |
| 去源后依然稳定保水 | 是否形成新源、宿主是否真实带水、退流时同族邻居和下方判定、旧精确类型是否失配 |
| 岩浆频率不稳定 | 是否命中液面抬升 RNG、实际入队被哪次同步请求抢先、普通/FAST_LAVA 属性是否一致 |
| 东西南北不对称 | 探测顺序与 EnumMap 写序、形状面的方向、hash/排序引入的优先级变化、区块符号坐标 |
| 只在含水或活塞完成后有差异 | 移动 BE 中的 movedState、最终/提前完成、普通破坏与直接置空、相邻形状更新是否投水刻 |
| 服务端状态一致，客户端仍不同 | 广播时的真实状态、快照和增量顺序、含水状态的 registry ID、区段计数、客户端加载区块情况 |

## 7. 落地测试和性能验收

### 7.1 测试分层与命令

未来实现时，核心聚焦测试放进常规 CTest，例如新增 `fluid_tests` 或按现有目标分组；存档扩展进 `storage_tests`，实体扩展进 `entity_tests`，协议扩展进 `protocol_tests`/`wire_tests`。每个关键分支至少一条有具体状态及顺序断言的测试。旧活塞、实体、存储、客户端和协议回归继续参与构建验证。真实原版差分单独运行，普通 CTest 不依赖联网、JAR 下载或 EULA。建议由离线检查器对已签核黄金文件做可选回归，明确标记它没有重新启动原版。以下命令供后续实现落地后执行。

```powershell
cmake --build build-jemalloc --config Release -j 6
ctest --test-dir build-jemalloc -C Release --output-on-failure
```

构建目录应预先配置 `BUILD_TESTING=ON`。协议分组需要 `REDSTONE_BUILD_VANILLA_SERVER=ON`，Windows 客户端仍由独立 `REDSTONE_BUILD_CLIENT` 控制。原版对照按 §3 新建 runner 后运行。基础 CTest 报告测试名、通过数和失败数，原版报告每个 case 的首差及其证据文件。阈值压力用例和大地图性能用例分档，避免默认测试下载大文件或启动真实客户端。

### 7.2 性能基准

先通过功能差分，再跑 Release、相同分配器和机器设置的计时。性能配置尽量沿用现有 [性能记录与基准流程](performance.md)。测试包含下面四类。

| 档位 | 固定工作负载 | 主要计数 |
| --- | --- | --- |
| 干态 | 同一已知世吞/活塞测试结构，固定输入和 tick 数，保持无流体 | 现有移动活塞、实体、方块刻与方块事件计数，空流体阶段开销 |
| 静水 | `1×1`、`8×8` 个区块的稳定源水/含水地形，合法读取范围固定 | 静止时的待执行量、空队列成本、是否出现每格每刻重新扫描 |
| 动态 | 多个同形状的封闭水槽、瀑布及水岩浆交汇，各档同样开源和去源，跨区块布局固定 | 流体刻请求、接受、执行、stale、写入、最大队列长度、形状/坡度探测及分配次数 |
| 混合与协议 | 动态流体 + 固定干态活塞 + 固定简单实体，同一视距连接协议驱动 | 核心和网络单独计时，包数、字节、积压、客户端间隔和 tick 分布 |

同一夹具重复至少三次，记录 seed、线程/处理器、编译器、构建选项、分配器、tick 数、区块集合和终态指纹。报告平均、中位数、P95、P99、最大值及超过 50 ms 的 tick 数。分别记录启动/导入、传播峰值、退流和静稳窗口；只比计算 tick 的耗时，睡眠、启动和测量工具开销单列。原版与 C++ 比较注明实际执行的方块刻、流体刻、实体和 BE 数及阶段范围。差分 trace、分配计数和采样器分别运行，避免和无探针数据拼成性能倍率。

**Reviewer 性能回归。** F-2、后续修改 tick 热路径的任务和 F-7，使用同一个基准程序与输入，比较流体前的固定提交 `c8e9eb1`、待审提交；需要归因时再加前一个通过审查的提交。分别在隔离构建目录使用同版编译器、Release、相同优化/链接选项和 jemalloc。在同一台机器上交替运行，每边至少三次。常规 CTest 与性能基准分开。

- 干态沿用 [历史纯服务端基准](performance.md#可复现基准) 的 800×83×800 投影、原有音符盒触发、3000 tick 和 `redstone_tick_benchmark`。比较前核对投影哈希与有效 tick 数。另跑指纹以核对原有工作量；正式计时关闭指纹、分配计数、采样和网络。历史耗时用作参考，性能结论基于同轮重跑。
- 看每次的平均、每 tick 中位数、P95 及超过 50 ms 的刻数。中位数或 P95 较基线持续增加超过 5%，且高于同版本重复波动时，增加交替复测。若仍可复现且工作量相同，在审查中作为回归处理。5% 是复测触发线，接近阈值或机器负载有明显噪声时报告实测范围。
- 新增流体负载和干态基准分别报告。流体前没有对应的流动工作量，首个行为通过的版本用于建立湿态基线。若改动导致干态状态指纹或关键实体、计划刻、事件计数不同，先说明差异并确认输入与实现，随后才计算性能变化。

**Reviewer 原版性能对照。** 行为判据以本地 26.1 Java 源码为依据，真实运行差分用固定的官方 26.1 服务端 JAR。对已实现且能复跑的共同场景，将官方服务端加入计时基线，保留上面的 C++ 历史回归对照。F-4 等分段任务可先验证源码和 C++ 聚焦用例；依赖自然更新、含水宿主或存档接线的端到端湿态负载在相应功能接通后进入计时，F-7 汇总并验收。

- 两端从同一夹具初态开始，固定规则、加载且可刻的区块集合、激活时刻、有效 tick 数和观察窗口，分别记录各自的种子。干态选择已核对初态和触发的世吞，湿态选择静水及可重复的封闭水槽、瀑布和水岩浆反应。优先选择确定性夹具；含随机分支的样本按实际分支和工作量配对。测试前后核对关键方块状态、实际执行的方块刻和流体刻、实体与移动方块实体工作量。行为或工作量不等价时保留各自结果和差异原因，先解决差异再计算倍率。
- 在同一台机器上交替跑，原版记录 JAR 哈希、JDK 和 JVM 参数，C++ 记录提交、Release 编译选项和 jemalloc。原版先用独立夹具预热 JVM，两端计时都从核验过的初态开始。每边至少三次。流体核心耗时只比较两边对应的模拟阶段，整服耗时另用同等加载和网络条件报告。原版 `/tick sprint` 的平均值可作交叉核对，P95 和超过 50 ms 的刻数从逐刻样本计算。详细 trace、采样和分配统计单独跑。
- 原版比较暂以 **中位数或 P95 慢超过 10%** 作为复测触发线。只有工作量与计时边界相同、差距超出重复波动且交替复测仍可重现时，才作为性能阻塞，先定位变慢阶段再验收。既有 C++ 历史基线仍用 5% 复测线。缺少可比夹具或原版实测时，在审查中标为待测，保留对应后续验收项。

### 7.3 验收与归档清单

- [ ] 记录 26.1 JAR 哈希、运行参数、规则、加载区块和种子，原版探针实际命中预期钩子，输出能复跑。
- [ ] P0 每一支路有正负例。状态、队列、共享面、写序和存档重启分别有明确断言。
- [ ] 至少完成 W-01、W-02、Q-01 至 Q-04、M-01 至 M-03、H-01/H-02、P-01/P-02、A-01/A-02 的真实原版对照，包含干态回归和原版无探针抽查。其余 P0 用例的缺项逐条列出。
- [ ] 构建并运行全量 CTest。记录真实原版差分、离线 JAR 探针和协议 codec 的各自结果与范围。
- [ ] 每个 case 归类 `pass`、`fail`、`unsupported`、`invalid_run`、`inconclusive`。若接受已知的项目范围差异，写明原因、影响路径、对应 TODO、覆盖用例及复验条件。
- [ ] 保存首差、golden 生成记录、原版输出和候选输出的引用。黄金文件由原版 runner 产生，经复核再更新，禁止从 C++ 结果反写。
- [ ] 读入世界的文件哈希不变，未生成 C++ 世界区块。流体更新在网络积压、断连或客户端可选构建关闭时继续推进。

验收结论按机制和夹具分别写。例如“主世界普通水/岩浆计划刻与表中 P0 用例已通过 26.1 差分”。带水扩展、随机刻、其他维度、玩家物理和投影计划刻依实现状态单列。
