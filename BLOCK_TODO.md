# 26.1 方块实现清单

## 全量方块基础功能进度（26.1 对齐）

### 已完成（覆盖原版全部 1,168 个方块 / 29,873 个状态）
- **全部 92 个方块状态属性**：`PropertyKind`、`propertyKindFromName`、`propertyValueFromName` 与 `BlockState` 位打包已支持原版 26.1 全部 92 种状态属性及枚举/整数取值。
- **全量注册表与状态定义**：按 26.1 `BuiltInRegistries.BLOCK` 原版顺序完整注册 1,168 个方块、29,873 个状态（`minecraft:air` 为 0 号状态，最大状态 ID 为 29,872）。
- **基础元数据与类型标记**：全量导入 `hardness`、`explosionResistance`、`friction`、`speedFactor`、`pushReaction`、`conductor`、`liquid`、`liquidContainer`、`forceSolid`、`replaceable`、`ignitedByLava`、`instrument` 与 `BlockClassFlag`（35 类方块家族标记），并保留 `piston`/`sticky_piston` 红石导体与 `cobweb` `.forceSolidOn()` 语义。
- **全量形状与随机刻声明**：通过 26.1 `BlockBasicProbe` 导出并生成全量 29,873 个状态的静态 `collisionShape`、`outlineShape` 与 `isRandomlyTicking()` 布尔声明（不再返回 `std::nullopt`）。
- **全量光学状态与面遮挡**：已通过 26.1 `LightMetadataProbe` / `LightOcclusionProbe` 导出并注册全部 1,168 个方块 / **29,873 个状态**的光学属性（`emission`、`dampening`、`canOcclude`、`useShapeForLightOcclusion`、`propagatesSkylightDown`）及 **6,258 个**面遮挡形状（支持单面多矩形）；`lightPropertiesOf(state).supported` 已全量覆盖所有原版方块状态。
- **通用化基础方块行为（`World` / `shapes`）**：
  - `FallingBlock`（所有沙/砂砾/混凝土粉末/铁砧/龙蛋等）：放置与 `updateShape` 触发重力计划刻。
  - `SimpleWaterloggedBlock`（所有含水宿主）：`updateShape` 调度流体计划刻。
  - `CarpetBlock`（所有羊毛地毯与苔藓地毯）：下方方块变化时 `updateShape` 破坏掉落。
  - `WallBlock`、`FenceBlock`、`FenceGateBlock`、`StairBlock`、`IronBarsBlock`（铁栏杆/铜栏杆/玻璃板/染色玻璃板）、`SlabBlock`、`RotatedPillarBlock`（含铁链与铜链）、`GlazedTerracottaBlock`、`SnowyBlock`：基于 `BlockClassFlag` 与世界标签通用化处理连接、`in_wall`、楼梯转角形态、双层台阶不可含水、带釉陶瓦 4 向水平朝向、全量旋转柱轴向及特殊放置替换/合并语义。
  - `NoteBlock`：改用 `metadata().instrument` O(1) 查表，并处理上方生物头颅（`zombie`、`skeleton` 等）覆写音色。

### 明确未完成（待逐项实现与校验）
- **`Block::supported()` 行为审计标记**：全量光学状态已注册；在原有 167 个方块基础上，经对齐双层台阶不可含水及建筑方块（台阶 62、楼梯 58、墙 26、栅栏 13、栅栏门 12、柱状/链 66、栏杆/玻璃板 26、带釉陶瓦 16、染色玻璃/遮光玻璃 17、混凝土 16、陶瓦 16、铜格栅 8、苔藓地毯及已审计常规建筑块）特殊放置与形状更新规则后，目前共 **547 个**方块为 `true`；其余 **621 个方块（包含仅完成 `randomTick` 分支、缺少 `updateShape` / `tick` / 掉落 / 降水 / 树生成 / 工具与容器逻辑等的方块）的 `Block::supported()` 仍保持 `false`**。
- **逐方块 `randomTick()` 行为**：全量状态已完成 `isRandomlyTicking()` 声明；现已实现草方块/菌丝/菌岩、`LavaFluid` 火蔓延、铜全系列氧化、紫水晶母岩、蘑菇、藤蔓、滴水石柱、海带/洞穴藤蔓/垂泪藤/缠怨藤、竹子/竹笋、可可果/甘蔗/仙人掌/甜浆果/下界疣、作物/瓜茎、红石矿（灭灯）、雪层（光照 ≤ 11）、冰/霜冰、干涸恶魂（计划刻调度）及近水耕地湿润等。仍保留明确 `// TODO` 的未完成分支包括：树苗与红树胎生苗 `stage == 1` 成树、树叶衰减及高光融雪掉落（缺战利品表）、干耕地无水干涸（缺降水/生物群系）、以及 `turtle_egg`、`nether_portal`、`chorus_flower`、`open/closed_eyeblossom` 等。
- **方块实体与交互逻辑**：箱子、熔炉、漏斗、发射器/投掷器、合成器、雕纹书架、试炼刷怪笼、铜灯等方块实体内部状态、容器交互、红石比较器输出与 `useItemOn` 尚未全量实现。
- **特殊放置、工具、战利品、音效与火焰蔓延**：`getStateForPlacement`、破坏工具/掉落战利品表、方块音效/粒子、火焰蔓延、液体混合与跨维度机制待逐项对齐 26.1 源码与服务端差分。

---

目标是覆盖原版全部方块及其状态和行为。当前只实现了有限的方块集合；
`assets/protocol775/block_states.tsv` 是协议导出，并不意味着这些方块的
模拟功能已实现。未注册的方块依然作为不支持的导入占位符处理。
尚未支持行为但已注册状态的方块，受管理 Anvil 区块读取会明确拒绝，避免把
不完整模拟后的区块当作可安全写回的数据；这些区块的只读投影仍待实现。

方块标签默认使用官方 26.1 数据包经原版 TagLoader 解析的 248 个标签。现有
`World::reloadBlockTags` 支持显式按低到高优先级加载目录和普通 ZIP 数据包，
重新构建原始嵌套引用、追加/替换及可选项，失败的文件和必需引用产生诊断；
世界读取雪标签已使用自己的快照。可用重复的 `--data-pack` 参数显式指定顺序，控制台
`reload` 重载方块标签并同步已连接客户端；新连接也会收到相应配置标签。尚未从
玩家 `/reload` 和其他数据包资源的重载尚未接入；世界启动时可根据 `level.dat`
选择 `file/` 数据包，内置功能数据包等其他来源、`pack.mcmeta` 校验及原版对格式错误的
完整处理仍未实现；控制台重载沿用启动时的选择，不会重新发现启用列表；
尚不支持 ZIP64/多卷压缩包与单标签大于 16 MiB；
循环依赖的遍历顺序尚未与 Java HashMap/HashMultimap 逐项差分。各方块行为也尚未
全部接入世界标签查询，不能声称标签系统与原版完整等价。

- 随机刻：已接入世界 `World::tick()` 的 `tickChunks()` 阶段（对照 `ServerLevel.tick`
  与 Lithium `ServerLevelMixin` / `RandomTickingSectionDataHelper`），支持
  `randomTickSpeed`（含 `minecraft:random_tick_speed` 游戏规则读取）、按区块与子区块
  位掩码 O(1) 跳过、`<= 384` 的 17×248 微段稀疏抽取及 `> 384` 的原版采样，
  并经 `WorldRandomTickAccess` 对接光学遮挡、已发布/持久化方块与天空光照、`isSolidRender`、
  `isFaceSturdy`、AABB 碰撞检测、玩家火蔓延半径、`spreadVines`、`waterEvaporates` 与雪标签，
  已实现草方块/菌丝、菌岩、`LavaFluid`、紫水晶母岩、铜氧化系列、藤蔓、滴水石柱、
  海带与藤蔓攀缘植物、竹子/作物/瓜茎/甘蔗/仙人掌/甜浆果/下界疣/可可果、蘑菇、红石矿、
  冰/霜冰、低光雪层、干涸恶魂与近水耕地的随机刻逻辑。尚未实现降水/结冰结雪（`iceandsnow`
  及干耕地雨天分支需生物群系与天气）、成树（`TreeGrower`）、树叶腐烂/融雪掉落（需战利品表）
  以及海龟蛋/下界传送门/紫颂花/眼花等需实体生成或昼夜时序的方块。调用缺失系统依赖的分支
  时明确返回 `Unsupported` 且记录 `// TODO`。
- 海带、高/矮海草与气泡柱：已对照 `KelpBlock` / `KelpPlantBlock` / `SeagrassBlock` /
  `TallSeagrassBlock` / `BubbleColumnBlock` 与 `FlowingFluid.canHoldAnyFluid` 对齐永久
  水源 `fluidStateOf` 与不可含水 `LiquidBlockContainer` 拒水语义；`GrowingPlantHeadBlock`
  相关的 `updateShape`、`getStateForPlacement`、骨粉及气泡柱 tick/实体效果仍待补齐，
  因此 `kelp` / `kelp_plant` / `bubble_column` 暂保持 `supported = false`。
- 菌丝（`minecraft:mycelium`）：已对照 `MyceliumBlock` / `SpreadingSnowyBlock` / `SnowyBlock`
  实现放置、雪标签更新与随机蔓延/退化，`supported` 已开启；客户端粒子与通用掉落/音效待实现。
- 紫水晶母岩（`minecraft:budding_amethyst`）与铜氧化/涂蜡建筑系列（`copper_block` / `cut_copper` / `chiseled_copper` / `cut_copper_slab` / `cut_copper_stairs` / `copper_bars` / `copper_chain` / `copper_grate` 及对应 `waxed_*`）：已完整实现方块侧状态、`WeatheringCopper` 随机刻、建筑放置/连接与双层台阶不可含水规则，`supported` 已开启；斧刮除与蜜脾涂蜡的物品交互及铜门/活板门/铜灯/灯笼/箱子/雕像/避雷针等仍保持 `supported = false`。
- 作物、树苗、树叶、藤蔓、花及其他植物：生长、存活、掉落和邻居更新待实现。
- 短草、蕨、枯死灌木、灌木和两种枯草已实现注册、无碰撞及基于世界标签的
  存活形状更新及可替换属性；放置时同种物品的例外、骨粉、掉落、选择轮廓与偏移
  尚未实现，仍标记为不支持。
- 海草和高海草已注册状态并实现水源、液体容器拒绝注水、支撑与上下半部形状
  更新；放置、骨粉、掉落、选择轮廓及高海草创造模式破坏细节尚未实现，仍标记为不支持。
- 16 种羊毛已注册单状态、完整方块基础属性与音符盒吉他音色；火焰传播和音效尚未实现。
- 蒲公英至铃兰的 15 种花已注册单状态、无碰撞和支撑标签更新；放置、炖菜/蜜蜂
  效果、凋零玫瑰实体效果、轮廓偏移和音效尚未实现，仍标记为不支持。
- 棕色和红色蘑菇已注册及实现方块侧随机传播；生产世界的光照、支撑和标签存活
  查询适配、形状更新、巨大蘑菇骨粉生成、轮廓和掉落尚未接入，仍标记为不支持。
- 金块、铁块、砖块及书架已注册基础属性及音符盒音色；工具限制和书架掉落、
  燃烧与音效尚未实现，仍标记为不支持。
- 箱子、熔炉、漏斗、刷怪笼等带方块实体的方块：其方块状态、碰撞、放置、
  更新和随机刻行为同样待逐个对照实现；方块实体内部逻辑和持久化另行实现。
- 门、床、楼梯、台阶、栅栏、墙、红石元件及含水方块：现有支持范围之外的状态、
  碰撞形状、放置、连接和更新待逐项对照原版实现。
- 火、冰雪、传送门、流体交互、爆炸与战利品：特殊行为及相关系统待实现。
- 本次新增的六种石材和两种泥土已注册唯一状态及现有模拟器所用的基础属性；
  光照数据已通过官方 26.1 服务端重新导出；挖掘工具、战利品、音效等尚未实现，
  不能据此声称方块全部行为已完成。
- `minecraft:oak_planks`：已对照 `Blocks.java` 注册单状态、硬度、爆炸抗性、
  导电和音符盒音色；燃烧、掉落及音效仍未实现。
- `minecraft:oak_log`：已对照 `Blocks.java`、`RotatedPillarBlock.java` 注册
  `axis` 三状态、默认轴、放置轴、旋转、基本属性和音符盒音色；创造模式下可
  以七种原版斧剥皮并保持轴向。剥皮音效、游戏事件、进度以及生存模式物品耐久、
  燃烧、掉落及音效仍未实现。
- `minecraft:stripped_oak_log`：已注册三种轴状态、放置和旋转；燃烧、掉落及
  音效仍未实现。
- `minecraft:spruce_planks`、`minecraft:spruce_log` 和
  `minecraft:stripped_spruce_log`：已分别对照原版注册状态和基础属性；原木支持
  按点击面放置、旋转及创造模式下斧剥皮。燃烧、掉落、剥皮音效和游戏事件待实现。
- `minecraft:birch_planks`、`minecraft:birch_log` 和
  `minecraft:stripped_birch_log`：已分别对照原版注册状态和基础属性；原木支持
  按点击面放置、旋转及创造模式下斧剥皮。燃烧、掉落、剥皮音效和游戏事件待实现。
- `minecraft:jungle_planks`、`minecraft:jungle_log` 和
  `minecraft:stripped_jungle_log`：已分别对照原版注册状态和基础属性；原木支持
  按点击面放置、旋转及创造模式下斧剥皮。燃烧、掉落、剥皮音效和游戏事件待实现。
- 按原版顺序首项 `minecraft:air`：已核对空气标识、空碰撞、可替换、光照和非随机刻；
  客户端不可见渲染形态尚未纳入服务端模型。
- `minecraft:stone`：已核对单状态、硬度、爆炸抗性、导电、光照、音符盒底鼓和非随机刻；
  工具需求、战利品和音效仍待实现。
- `minecraft:granite`、`minecraft:polished_granite`、`minecraft:diorite`、
  `minecraft:polished_diorite`、`minecraft:andesite`、`minecraft:polished_andesite`：
  已逐项对照原版注册单状态及基础属性；工具需求、战利品和音效仍待实现。
- `minecraft:grass_block`：已注册 `snowy` 状态、邻居形状更新和方块侧随机刻的
  衰变/传播逻辑；随机刻仍需生产世界环境查询适配器，骨粉、战利品、声音待实现。
- `minecraft:podzol`：已对照 `SnowyBlock` 加入两个状态、上方雪标签触发更新、
  放置时状态、基础属性和非随机刻；工具相关战利品与声音仍待实现。
- `minecraft:cobblestone`：已核对单状态、硬度、爆炸抗性、导电、光照、音符盒底鼓
  和非随机刻；工具需求、战利品和音效仍待实现。
- `minecraft:acacia_planks`：已核对单状态、硬度、爆炸抗性、导电、音符盒音色和
  非随机刻；燃烧、战利品和音效仍待实现。
- `minecraft:acacia_log`：已对照 `RotatedPillarBlock` 注册轴状态、放置、旋转及创造模式剥皮；
  燃烧、战利品和音效仍待实现。
- `minecraft:cherry_log`、`minecraft:dark_oak_log`、`minecraft:pale_oak_log`、
  `minecraft:mangrove_log`：逐项对照 `RotatedPillarBlock` 注册轴状态、放置和旋转；
  创造模式下可剥皮；燃烧、战利品和音效仍待实现。
- `minecraft:mangrove_roots`：已对照注册含水状态、水源持有与邻居形状更新的水计划刻；
  燃烧、战利品、音效和客户端相邻面渲染规则仍待实现。
- `minecraft:muddy_mangrove_roots`：已对照 `RotatedPillarBlock` 注册轴状态、放置和
  基础属性；音效和战利品仍待实现。
- `minecraft:bamboo_block`：已对照 `RotatedPillarBlock` 注册轴状态、放置、基本属性和
  音符盒音色及创造模式剥皮；燃烧、战利品和音效仍待实现。
- `minecraft:stripped_acacia_log`、`minecraft:stripped_cherry_log`、
  `minecraft:stripped_dark_oak_log`、`minecraft:stripped_pale_oak_log`、
  `minecraft:stripped_mangrove_log`、`minecraft:stripped_bamboo_block`：逐项注册轴状态、
  放置和音色，并接入相应原木的创造模式剥皮；燃烧、战利品和音效仍待实现。
- `minecraft:oak_wood`：已注册轴状态、放置、旋转、音色和创造模式剥皮；燃烧、战利品及音效待实现。
- `minecraft:spruce_wood`：已注册轴状态、放置、旋转、音色和创造模式剥皮；燃烧、战利品及音效待实现。
- `minecraft:birch_wood`：已注册轴状态、放置、旋转、音色和创造模式剥皮；燃烧、战利品及音效待实现。
- `minecraft:jungle_wood`：已注册轴状态、放置、旋转、音色和创造模式剥皮；燃烧、战利品及音效待实现。
- `minecraft:acacia_wood`、`minecraft:cherry_wood`、`minecraft:dark_oak_wood`、
  `minecraft:mangrove_wood`：已分别核对轴状态、放置、旋转、音色和创造模式剥皮；燃烧、战利品及音效待实现。
- `minecraft:cherry_planks`：已核对单状态、硬度、爆炸抗性、导电、音符盒音色和
  非随机刻；燃烧、战利品和樱花木音效仍待实现。
- `minecraft:dark_oak_planks`：已核对单状态、硬度、爆炸抗性、导电、音符盒音色和
  非随机刻；燃烧、战利品和音效仍待实现。
- `minecraft:pale_oak_wood`：已注册轴状态、放置、旋转、基本属性、音色和创造模式剥皮；
  燃烧、战利品和音效仍待实现。
- `minecraft:pale_oak_planks`：单状态、基本属性、音符盒音色及非随机刻已核对；
  燃烧、战利品和音效待实现。
- `minecraft:mangrove_planks`：单状态、基本属性、音符盒音色及非随机刻已核对；
  燃烧、战利品和音效待实现。
- `minecraft:bamboo_planks`：单状态、基本属性、音符盒音色及非随机刻已核对；
  燃烧、战利品和竹木音效待实现。
- `minecraft:bamboo_mosaic`：单状态、基本属性、音符盒音色及非随机刻已核对；
  燃烧、战利品和竹木音效待实现。
- `minecraft:oak_sapling`：已对照 `SaplingBlock` 注册 stage 两状态、碰撞/轮廓及
  方块侧随机刻判断和 stage 0→1；stage 1 的树生成由环境接口提供，当前世界生成、
  地面标签、存活/放置、骨粉与战利品未实现，因此尚未开放为受支持方块。
- `minecraft:spruce_sapling`：已对照 `SaplingBlock(TreeGrower.SPRUCE)` 注册状态、
  形状及方块侧随机刻；树生成、地面标签、存活/放置、骨粉与战利品未实现，仍不支持。
- `minecraft:birch_sapling`：已对照 `SaplingBlock(TreeGrower.BIRCH)` 注册状态、
  形状及方块侧随机刻；树生成、地面标签、存活/放置、骨粉与战利品未实现，仍不支持。
- `minecraft:jungle_sapling`：已对照 `SaplingBlock(TreeGrower.JUNGLE)` 注册状态、
  形状及方块侧随机刻；树生成、地面标签、存活/放置、骨粉与战利品未实现，仍不支持。
- `minecraft:acacia_sapling`：已对照 `SaplingBlock(TreeGrower.ACACIA)` 注册状态、
  形状及方块侧随机刻；树生成、地面标签、存活/放置、骨粉与战利品未实现，仍不支持。
- `minecraft:cherry_sapling`、`minecraft:dark_oak_sapling` 和
  `minecraft:pale_oak_sapling`：已分别对照原版树生成器注册状态、形状及方块侧随机刻；
  树生成、地面标签、存活/放置、骨粉、战利品和音效未实现，仍不支持。
- `minecraft:mangrove_propagule`：已查阅 `MangrovePropaguleBlock.java`，其 `age`、
  `stage`、`hanging`、`waterlogged` 四属性、悬挂生长与含水计划刻、双分支随机刻、
  偏移形状、标签存活和树生成尚未实现；当前不注册简化状态。
- `minecraft:bedrock`：已核对单状态、负硬度、爆炸抗性、普通活塞反应、活塞不可推动、
  音符盒音色及非随机刻；实体生成判据和音效尚未实现。
- `minecraft:water`：已有 16 级方块状态与流体计划刻/传播，方块侧随机刻为否；
  水桶、实体推流、气泡柱等行为仍待实现。
- `minecraft:lava`：已有 16 级方块状态与流体计划刻/传播，方块侧随机刻为是；
  `LavaFluid.randomTick` 的玩家附近火焰蔓延与 `BaseFireBlock.getState` 对齐逻辑已实现；
  水桶、火焰方块自身销毁/燃烧扩散和气泡柱相关行为仍待实现。
- `minecraft:sand`：已有单状态、计划刻触发下落方块、音符盒音色及非随机刻；
  音效、粒子、工具与战利品尚未实现。
- `minecraft:suspicious_sand`：已查阅 `BrushableBlock` 注册；`dusted` 四状态、
  方块实体交互、刷子、掉落及落地破坏行为待实现。
- `minecraft:red_sand`：已对照 `SandBlock` 加入单状态、计划刻下落和音符盒军鼓音色；
  落沙颜色、音效及战利品尚未实现。
- `minecraft:gravel`：已对照 `ColoredFallingBlock` 加入单状态、计划刻下落和音符盒
  军鼓音色；落沙颜色、音效及燧石战利品尚未实现。
- `minecraft:suspicious_gravel`：`BrushableBlock` 的状态、方块实体交互及落地破坏待实现。
- `minecraft:gold_ore`、`minecraft:deepslate_gold_ore`、`minecraft:iron_ore`、
  `minecraft:deepslate_iron_ore`、`minecraft:coal_ore`、`minecraft:deepslate_coal_ore`、
  `minecraft:nether_gold_ore`：已分别对照 `DropExperienceBlock` 注册单状态及基础属性；
  工具需求、各自的掉落、经验值和音效尚未实现。

源码依据：26.1 `net.minecraft.world.level.block.Blocks` 及对应方块类。
兼容性需要分别记录源码对照、C++ 测试与真实 26.1 服务端差分结果。

- 九种去皮木头：已分别核对原版轴状态、基本属性和音色，并接入创造模式剥皮；
  燃烧、战利品及音效仍待实现。
- `minecraft:oak_leaves`：已对照 `LeavesBlock` 注册 28 个状态及按距离/持久性判断
  随机刻资格；支撑面、邻居距离计划刻、衰减掉落、含水与放置尚未实现，保持未支持。
- `minecraft:spruce_leaves`、`minecraft:birch_leaves`、`minecraft:jungle_leaves`、
  `minecraft:acacia_leaves`、`minecraft:cherry_leaves`、`minecraft:dark_oak_leaves`、
  `minecraft:pale_oak_leaves`、`minecraft:mangrove_leaves`、`minecraft:azalea_leaves`、
  `minecraft:flowering_azalea_leaves`：已逐项对照注册状态与随机刻资格；距离传播、
  衰减、含水和放置尚未实现，红树叶生成胎生苗等各自特殊行为也未实现，保持未支持。
- `minecraft:sponge`：已核对 `SpongeBlock`；放置和邻居变化触发的最大深度 6、最多
  64 个水方块的广度优先吸水、方块实体和掉落处理尚未实现，不注册简化行为。
- `minecraft:wet_sponge`：已核对 `WetSpongeBlock`；放置时按环境属性蒸发、事件和
  音效的行为尚未实现，不注册简化行为。
- `minecraft:glass`：已核对单状态、无导电、完整碰撞、光照、音符盒踩镲与非随机刻；
  工具与战利品、音效和客户端相邻面渲染仍待实现。
- `minecraft:lapis_ore` 和 `minecraft:deepslate_lapis_ore`：已对照单状态、强度及底鼓音色；
  原版 2–5 点经验、正确工具和战利品尚未实现。
- `minecraft:lapis_block`：已对照单状态、强度及默认竖琴音色；正确工具和战利品尚未实现。
- `minecraft:dispenser`：已对照 `DispenserBlock` 注册六朝向 × 两触发状态；放置、
  红石边沿、4 刻触发、方块实体、发射行为和容器移除仍未实现，保持未支持。
- `minecraft:sandstone`、`minecraft:chiseled_sandstone`、`minecraft:cut_sandstone`：
  已对照单状态、强度和音符盒底鼓；正确工具、战利品和音效待实现。
- `minecraft:note_block`：已补齐 26.1 全部 27 种乐器及 1350 个状态；头颅覆盖音色、
  自定义头颅声音、铜类小号映射及乐器实际声音等仍待实现。
- 16 色床：已按 `BedBlock` 分别注册 16 个状态；头脚两格放置/破坏、碰撞形状、
  睡眠、维度爆炸、方块实体及战利品尚未实现，保持未支持。
- `minecraft:cobweb`：已对照 `WebBlock` 注册单状态、无碰撞及强制固体属性；
  实体进入减速、盘丝效果、正确工具和战利品尚未实现，保持未支持。

## 原版方块逐项核对清单

在完成相应基础行为、未实现项说明与测试前，各方块均保留在下列清单中。
清单由 `python3 scripts/update_block_todo.py` 根据官方协议导出刷新；是否含
方块实体不影响方块本身的实施范围。
每项的原版 Java 类、默认状态、状态数、随机刻状态数和方块实体状态数见
`assets/block26_1/block_inventory.tsv`；它由经校验的官方 26.1 服务端导出，
顺序及状态数量另与现有协议导出交叉检查。后续按此注册顺序逐项实现与验收。

<!-- official-block-inventory:start -->

共 1168 个原版方块，顺序与 26.1 官方导出一致。
该清单表示仍需逐个核对基础行为，不代表每项都完全没有实现。

- `minecraft:air`、`minecraft:stone`、`minecraft:granite`、`minecraft:polished_granite`、`minecraft:diorite`、`minecraft:polished_diorite`、`minecraft:andesite`、`minecraft:polished_andesite`
- `minecraft:grass_block`、`minecraft:dirt`、`minecraft:coarse_dirt`、`minecraft:podzol`、`minecraft:cobblestone`、`minecraft:oak_planks`、`minecraft:spruce_planks`、`minecraft:birch_planks`
- `minecraft:jungle_planks`、`minecraft:acacia_planks`、`minecraft:cherry_planks`、`minecraft:dark_oak_planks`、`minecraft:pale_oak_wood`、`minecraft:pale_oak_planks`、`minecraft:mangrove_planks`、`minecraft:bamboo_planks`
- `minecraft:bamboo_mosaic`、`minecraft:oak_sapling`、`minecraft:spruce_sapling`、`minecraft:birch_sapling`、`minecraft:jungle_sapling`、`minecraft:acacia_sapling`、`minecraft:cherry_sapling`、`minecraft:dark_oak_sapling`
- `minecraft:pale_oak_sapling`、`minecraft:mangrove_propagule`、`minecraft:bedrock`、`minecraft:water`、`minecraft:lava`、`minecraft:sand`、`minecraft:suspicious_sand`、`minecraft:red_sand`
- `minecraft:gravel`、`minecraft:suspicious_gravel`、`minecraft:gold_ore`、`minecraft:deepslate_gold_ore`、`minecraft:iron_ore`、`minecraft:deepslate_iron_ore`、`minecraft:coal_ore`、`minecraft:deepslate_coal_ore`
- `minecraft:nether_gold_ore`、`minecraft:oak_log`、`minecraft:spruce_log`、`minecraft:birch_log`、`minecraft:jungle_log`、`minecraft:acacia_log`、`minecraft:cherry_log`、`minecraft:dark_oak_log`
- `minecraft:pale_oak_log`、`minecraft:mangrove_log`、`minecraft:mangrove_roots`、`minecraft:muddy_mangrove_roots`、`minecraft:bamboo_block`、`minecraft:stripped_spruce_log`、`minecraft:stripped_birch_log`、`minecraft:stripped_jungle_log`
- `minecraft:stripped_acacia_log`、`minecraft:stripped_cherry_log`、`minecraft:stripped_dark_oak_log`、`minecraft:stripped_pale_oak_log`、`minecraft:stripped_oak_log`、`minecraft:stripped_mangrove_log`、`minecraft:stripped_bamboo_block`、`minecraft:oak_wood`
- `minecraft:spruce_wood`、`minecraft:birch_wood`、`minecraft:jungle_wood`、`minecraft:acacia_wood`、`minecraft:cherry_wood`、`minecraft:dark_oak_wood`、`minecraft:mangrove_wood`、`minecraft:stripped_oak_wood`
- `minecraft:stripped_spruce_wood`、`minecraft:stripped_birch_wood`、`minecraft:stripped_jungle_wood`、`minecraft:stripped_acacia_wood`、`minecraft:stripped_cherry_wood`、`minecraft:stripped_dark_oak_wood`、`minecraft:stripped_pale_oak_wood`、`minecraft:stripped_mangrove_wood`
- `minecraft:oak_leaves`、`minecraft:spruce_leaves`、`minecraft:birch_leaves`、`minecraft:jungle_leaves`、`minecraft:acacia_leaves`、`minecraft:cherry_leaves`、`minecraft:dark_oak_leaves`、`minecraft:pale_oak_leaves`
- `minecraft:mangrove_leaves`、`minecraft:azalea_leaves`、`minecraft:flowering_azalea_leaves`、`minecraft:sponge`、`minecraft:wet_sponge`、`minecraft:glass`、`minecraft:lapis_ore`、`minecraft:deepslate_lapis_ore`
- `minecraft:lapis_block`、`minecraft:dispenser`、`minecraft:sandstone`、`minecraft:chiseled_sandstone`、`minecraft:cut_sandstone`、`minecraft:note_block`、`minecraft:white_bed`、`minecraft:orange_bed`
- `minecraft:magenta_bed`、`minecraft:light_blue_bed`、`minecraft:yellow_bed`、`minecraft:lime_bed`、`minecraft:pink_bed`、`minecraft:gray_bed`、`minecraft:light_gray_bed`、`minecraft:cyan_bed`
- `minecraft:purple_bed`、`minecraft:blue_bed`、`minecraft:brown_bed`、`minecraft:green_bed`、`minecraft:red_bed`、`minecraft:black_bed`、`minecraft:powered_rail`、`minecraft:detector_rail`
- `minecraft:sticky_piston`、`minecraft:cobweb`、`minecraft:short_grass`、`minecraft:fern`、`minecraft:dead_bush`、`minecraft:bush`、`minecraft:short_dry_grass`、`minecraft:tall_dry_grass`
- `minecraft:seagrass`、`minecraft:tall_seagrass`、`minecraft:piston`、`minecraft:piston_head`、`minecraft:white_wool`、`minecraft:orange_wool`、`minecraft:magenta_wool`、`minecraft:light_blue_wool`
- `minecraft:yellow_wool`、`minecraft:lime_wool`、`minecraft:pink_wool`、`minecraft:gray_wool`、`minecraft:light_gray_wool`、`minecraft:cyan_wool`、`minecraft:purple_wool`、`minecraft:blue_wool`
- `minecraft:brown_wool`、`minecraft:green_wool`、`minecraft:red_wool`、`minecraft:black_wool`、`minecraft:moving_piston`、`minecraft:dandelion`、`minecraft:golden_dandelion`、`minecraft:torchflower`
- `minecraft:poppy`、`minecraft:blue_orchid`、`minecraft:allium`、`minecraft:azure_bluet`、`minecraft:red_tulip`、`minecraft:orange_tulip`、`minecraft:white_tulip`、`minecraft:pink_tulip`
- `minecraft:oxeye_daisy`、`minecraft:cornflower`、`minecraft:wither_rose`、`minecraft:lily_of_the_valley`、`minecraft:brown_mushroom`、`minecraft:red_mushroom`、`minecraft:gold_block`、`minecraft:iron_block`
- `minecraft:bricks`、`minecraft:tnt`、`minecraft:bookshelf`、`minecraft:chiseled_bookshelf`、`minecraft:acacia_shelf`、`minecraft:bamboo_shelf`、`minecraft:birch_shelf`、`minecraft:cherry_shelf`
- `minecraft:crimson_shelf`、`minecraft:dark_oak_shelf`、`minecraft:jungle_shelf`、`minecraft:mangrove_shelf`、`minecraft:oak_shelf`、`minecraft:pale_oak_shelf`、`minecraft:spruce_shelf`、`minecraft:warped_shelf`
- `minecraft:mossy_cobblestone`、`minecraft:obsidian`、`minecraft:torch`、`minecraft:wall_torch`、`minecraft:fire`、`minecraft:soul_fire`、`minecraft:spawner`、`minecraft:creaking_heart`
- `minecraft:oak_stairs`、`minecraft:chest`、`minecraft:redstone_wire`、`minecraft:diamond_ore`、`minecraft:deepslate_diamond_ore`、`minecraft:diamond_block`、`minecraft:crafting_table`、`minecraft:wheat`
- `minecraft:farmland`、`minecraft:furnace`、`minecraft:oak_sign`、`minecraft:spruce_sign`、`minecraft:birch_sign`、`minecraft:acacia_sign`、`minecraft:cherry_sign`、`minecraft:jungle_sign`
- `minecraft:dark_oak_sign`、`minecraft:pale_oak_sign`、`minecraft:mangrove_sign`、`minecraft:bamboo_sign`、`minecraft:oak_door`、`minecraft:ladder`、`minecraft:rail`、`minecraft:cobblestone_stairs`
- `minecraft:oak_wall_sign`、`minecraft:spruce_wall_sign`、`minecraft:birch_wall_sign`、`minecraft:acacia_wall_sign`、`minecraft:cherry_wall_sign`、`minecraft:jungle_wall_sign`、`minecraft:dark_oak_wall_sign`、`minecraft:pale_oak_wall_sign`
- `minecraft:mangrove_wall_sign`、`minecraft:bamboo_wall_sign`、`minecraft:oak_hanging_sign`、`minecraft:spruce_hanging_sign`、`minecraft:birch_hanging_sign`、`minecraft:acacia_hanging_sign`、`minecraft:cherry_hanging_sign`、`minecraft:jungle_hanging_sign`
- `minecraft:dark_oak_hanging_sign`、`minecraft:pale_oak_hanging_sign`、`minecraft:crimson_hanging_sign`、`minecraft:warped_hanging_sign`、`minecraft:mangrove_hanging_sign`、`minecraft:bamboo_hanging_sign`、`minecraft:oak_wall_hanging_sign`、`minecraft:spruce_wall_hanging_sign`
- `minecraft:birch_wall_hanging_sign`、`minecraft:acacia_wall_hanging_sign`、`minecraft:cherry_wall_hanging_sign`、`minecraft:jungle_wall_hanging_sign`、`minecraft:dark_oak_wall_hanging_sign`、`minecraft:pale_oak_wall_hanging_sign`、`minecraft:mangrove_wall_hanging_sign`、`minecraft:crimson_wall_hanging_sign`
- `minecraft:warped_wall_hanging_sign`、`minecraft:bamboo_wall_hanging_sign`、`minecraft:lever`、`minecraft:stone_pressure_plate`、`minecraft:iron_door`、`minecraft:oak_pressure_plate`、`minecraft:spruce_pressure_plate`、`minecraft:birch_pressure_plate`
- `minecraft:jungle_pressure_plate`、`minecraft:acacia_pressure_plate`、`minecraft:cherry_pressure_plate`、`minecraft:dark_oak_pressure_plate`、`minecraft:pale_oak_pressure_plate`、`minecraft:mangrove_pressure_plate`、`minecraft:bamboo_pressure_plate`、`minecraft:redstone_ore`
- `minecraft:deepslate_redstone_ore`、`minecraft:redstone_torch`、`minecraft:redstone_wall_torch`、`minecraft:stone_button`、`minecraft:snow`、`minecraft:ice`、`minecraft:snow_block`、`minecraft:cactus`
- `minecraft:cactus_flower`、`minecraft:clay`、`minecraft:sugar_cane`、`minecraft:jukebox`、`minecraft:oak_fence`、`minecraft:netherrack`、`minecraft:soul_sand`、`minecraft:soul_soil`
- `minecraft:basalt`、`minecraft:polished_basalt`、`minecraft:soul_torch`、`minecraft:soul_wall_torch`、`minecraft:copper_torch`、`minecraft:copper_wall_torch`、`minecraft:glowstone`、`minecraft:nether_portal`
- `minecraft:carved_pumpkin`、`minecraft:jack_o_lantern`、`minecraft:cake`、`minecraft:repeater`、`minecraft:white_stained_glass`、`minecraft:orange_stained_glass`、`minecraft:magenta_stained_glass`、`minecraft:light_blue_stained_glass`
- `minecraft:yellow_stained_glass`、`minecraft:lime_stained_glass`、`minecraft:pink_stained_glass`、`minecraft:gray_stained_glass`、`minecraft:light_gray_stained_glass`、`minecraft:cyan_stained_glass`、`minecraft:purple_stained_glass`、`minecraft:blue_stained_glass`
- `minecraft:brown_stained_glass`、`minecraft:green_stained_glass`、`minecraft:red_stained_glass`、`minecraft:black_stained_glass`、`minecraft:oak_trapdoor`、`minecraft:spruce_trapdoor`、`minecraft:birch_trapdoor`、`minecraft:jungle_trapdoor`
- `minecraft:acacia_trapdoor`、`minecraft:cherry_trapdoor`、`minecraft:dark_oak_trapdoor`、`minecraft:pale_oak_trapdoor`、`minecraft:mangrove_trapdoor`、`minecraft:bamboo_trapdoor`、`minecraft:stone_bricks`、`minecraft:mossy_stone_bricks`
- `minecraft:cracked_stone_bricks`、`minecraft:chiseled_stone_bricks`、`minecraft:packed_mud`、`minecraft:mud_bricks`、`minecraft:infested_stone`、`minecraft:infested_cobblestone`、`minecraft:infested_stone_bricks`、`minecraft:infested_mossy_stone_bricks`
- `minecraft:infested_cracked_stone_bricks`、`minecraft:infested_chiseled_stone_bricks`、`minecraft:brown_mushroom_block`、`minecraft:red_mushroom_block`、`minecraft:mushroom_stem`、`minecraft:iron_bars`、`minecraft:copper_bars`、`minecraft:exposed_copper_bars`
- `minecraft:weathered_copper_bars`、`minecraft:oxidized_copper_bars`、`minecraft:waxed_copper_bars`、`minecraft:waxed_exposed_copper_bars`、`minecraft:waxed_weathered_copper_bars`、`minecraft:waxed_oxidized_copper_bars`、`minecraft:iron_chain`、`minecraft:copper_chain`
- `minecraft:exposed_copper_chain`、`minecraft:weathered_copper_chain`、`minecraft:oxidized_copper_chain`、`minecraft:waxed_copper_chain`、`minecraft:waxed_exposed_copper_chain`、`minecraft:waxed_weathered_copper_chain`、`minecraft:waxed_oxidized_copper_chain`、`minecraft:glass_pane`
- `minecraft:pumpkin`、`minecraft:melon`、`minecraft:attached_pumpkin_stem`、`minecraft:attached_melon_stem`、`minecraft:pumpkin_stem`、`minecraft:melon_stem`、`minecraft:vine`、`minecraft:glow_lichen`
- `minecraft:resin_clump`、`minecraft:oak_fence_gate`、`minecraft:brick_stairs`、`minecraft:stone_brick_stairs`、`minecraft:mud_brick_stairs`、`minecraft:mycelium`、`minecraft:lily_pad`、`minecraft:resin_block`
- `minecraft:resin_bricks`、`minecraft:resin_brick_stairs`、`minecraft:resin_brick_slab`、`minecraft:resin_brick_wall`、`minecraft:chiseled_resin_bricks`、`minecraft:nether_bricks`、`minecraft:nether_brick_fence`、`minecraft:nether_brick_stairs`
- `minecraft:nether_wart`、`minecraft:enchanting_table`、`minecraft:brewing_stand`、`minecraft:cauldron`、`minecraft:water_cauldron`、`minecraft:lava_cauldron`、`minecraft:powder_snow_cauldron`、`minecraft:end_portal`
- `minecraft:end_portal_frame`、`minecraft:end_stone`、`minecraft:dragon_egg`、`minecraft:redstone_lamp`、`minecraft:cocoa`、`minecraft:sandstone_stairs`、`minecraft:emerald_ore`、`minecraft:deepslate_emerald_ore`
- `minecraft:ender_chest`、`minecraft:tripwire_hook`、`minecraft:tripwire`、`minecraft:emerald_block`、`minecraft:spruce_stairs`、`minecraft:birch_stairs`、`minecraft:jungle_stairs`、`minecraft:command_block`
- `minecraft:beacon`、`minecraft:cobblestone_wall`、`minecraft:mossy_cobblestone_wall`、`minecraft:flower_pot`、`minecraft:potted_torchflower`、`minecraft:potted_oak_sapling`、`minecraft:potted_spruce_sapling`、`minecraft:potted_birch_sapling`
- `minecraft:potted_jungle_sapling`、`minecraft:potted_acacia_sapling`、`minecraft:potted_cherry_sapling`、`minecraft:potted_dark_oak_sapling`、`minecraft:potted_pale_oak_sapling`、`minecraft:potted_mangrove_propagule`、`minecraft:potted_fern`、`minecraft:potted_dandelion`
- `minecraft:potted_golden_dandelion`、`minecraft:potted_poppy`、`minecraft:potted_blue_orchid`、`minecraft:potted_allium`、`minecraft:potted_azure_bluet`、`minecraft:potted_red_tulip`、`minecraft:potted_orange_tulip`、`minecraft:potted_white_tulip`
- `minecraft:potted_pink_tulip`、`minecraft:potted_oxeye_daisy`、`minecraft:potted_cornflower`、`minecraft:potted_lily_of_the_valley`、`minecraft:potted_wither_rose`、`minecraft:potted_red_mushroom`、`minecraft:potted_brown_mushroom`、`minecraft:potted_dead_bush`
- `minecraft:potted_cactus`、`minecraft:carrots`、`minecraft:potatoes`、`minecraft:oak_button`、`minecraft:spruce_button`、`minecraft:birch_button`、`minecraft:jungle_button`、`minecraft:acacia_button`
- `minecraft:cherry_button`、`minecraft:dark_oak_button`、`minecraft:pale_oak_button`、`minecraft:mangrove_button`、`minecraft:bamboo_button`、`minecraft:skeleton_skull`、`minecraft:skeleton_wall_skull`、`minecraft:wither_skeleton_skull`
- `minecraft:wither_skeleton_wall_skull`、`minecraft:zombie_head`、`minecraft:zombie_wall_head`、`minecraft:player_head`、`minecraft:player_wall_head`、`minecraft:creeper_head`、`minecraft:creeper_wall_head`、`minecraft:dragon_head`
- `minecraft:dragon_wall_head`、`minecraft:piglin_head`、`minecraft:piglin_wall_head`、`minecraft:anvil`、`minecraft:chipped_anvil`、`minecraft:damaged_anvil`、`minecraft:trapped_chest`、`minecraft:light_weighted_pressure_plate`
- `minecraft:heavy_weighted_pressure_plate`、`minecraft:comparator`、`minecraft:daylight_detector`、`minecraft:redstone_block`、`minecraft:nether_quartz_ore`、`minecraft:hopper`、`minecraft:quartz_block`、`minecraft:chiseled_quartz_block`
- `minecraft:quartz_pillar`、`minecraft:quartz_stairs`、`minecraft:activator_rail`、`minecraft:dropper`、`minecraft:white_terracotta`、`minecraft:orange_terracotta`、`minecraft:magenta_terracotta`、`minecraft:light_blue_terracotta`
- `minecraft:yellow_terracotta`、`minecraft:lime_terracotta`、`minecraft:pink_terracotta`、`minecraft:gray_terracotta`、`minecraft:light_gray_terracotta`、`minecraft:cyan_terracotta`、`minecraft:purple_terracotta`、`minecraft:blue_terracotta`
- `minecraft:brown_terracotta`、`minecraft:green_terracotta`、`minecraft:red_terracotta`、`minecraft:black_terracotta`、`minecraft:white_stained_glass_pane`、`minecraft:orange_stained_glass_pane`、`minecraft:magenta_stained_glass_pane`、`minecraft:light_blue_stained_glass_pane`
- `minecraft:yellow_stained_glass_pane`、`minecraft:lime_stained_glass_pane`、`minecraft:pink_stained_glass_pane`、`minecraft:gray_stained_glass_pane`、`minecraft:light_gray_stained_glass_pane`、`minecraft:cyan_stained_glass_pane`、`minecraft:purple_stained_glass_pane`、`minecraft:blue_stained_glass_pane`
- `minecraft:brown_stained_glass_pane`、`minecraft:green_stained_glass_pane`、`minecraft:red_stained_glass_pane`、`minecraft:black_stained_glass_pane`、`minecraft:acacia_stairs`、`minecraft:cherry_stairs`、`minecraft:dark_oak_stairs`、`minecraft:pale_oak_stairs`
- `minecraft:mangrove_stairs`、`minecraft:bamboo_stairs`、`minecraft:bamboo_mosaic_stairs`、`minecraft:slime_block`、`minecraft:barrier`、`minecraft:light`、`minecraft:iron_trapdoor`、`minecraft:prismarine`
- `minecraft:prismarine_bricks`、`minecraft:dark_prismarine`、`minecraft:prismarine_stairs`、`minecraft:prismarine_brick_stairs`、`minecraft:dark_prismarine_stairs`、`minecraft:prismarine_slab`、`minecraft:prismarine_brick_slab`、`minecraft:dark_prismarine_slab`
- `minecraft:sea_lantern`、`minecraft:hay_block`、`minecraft:white_carpet`、`minecraft:orange_carpet`、`minecraft:magenta_carpet`、`minecraft:light_blue_carpet`、`minecraft:yellow_carpet`、`minecraft:lime_carpet`
- `minecraft:pink_carpet`、`minecraft:gray_carpet`、`minecraft:light_gray_carpet`、`minecraft:cyan_carpet`、`minecraft:purple_carpet`、`minecraft:blue_carpet`、`minecraft:brown_carpet`、`minecraft:green_carpet`
- `minecraft:red_carpet`、`minecraft:black_carpet`、`minecraft:terracotta`、`minecraft:coal_block`、`minecraft:packed_ice`、`minecraft:sunflower`、`minecraft:lilac`、`minecraft:rose_bush`
- `minecraft:peony`、`minecraft:tall_grass`、`minecraft:large_fern`、`minecraft:white_banner`、`minecraft:orange_banner`、`minecraft:magenta_banner`、`minecraft:light_blue_banner`、`minecraft:yellow_banner`
- `minecraft:lime_banner`、`minecraft:pink_banner`、`minecraft:gray_banner`、`minecraft:light_gray_banner`、`minecraft:cyan_banner`、`minecraft:purple_banner`、`minecraft:blue_banner`、`minecraft:brown_banner`
- `minecraft:green_banner`、`minecraft:red_banner`、`minecraft:black_banner`、`minecraft:white_wall_banner`、`minecraft:orange_wall_banner`、`minecraft:magenta_wall_banner`、`minecraft:light_blue_wall_banner`、`minecraft:yellow_wall_banner`
- `minecraft:lime_wall_banner`、`minecraft:pink_wall_banner`、`minecraft:gray_wall_banner`、`minecraft:light_gray_wall_banner`、`minecraft:cyan_wall_banner`、`minecraft:purple_wall_banner`、`minecraft:blue_wall_banner`、`minecraft:brown_wall_banner`
- `minecraft:green_wall_banner`、`minecraft:red_wall_banner`、`minecraft:black_wall_banner`、`minecraft:red_sandstone`、`minecraft:chiseled_red_sandstone`、`minecraft:cut_red_sandstone`、`minecraft:red_sandstone_stairs`、`minecraft:oak_slab`
- `minecraft:spruce_slab`、`minecraft:birch_slab`、`minecraft:jungle_slab`、`minecraft:acacia_slab`、`minecraft:cherry_slab`、`minecraft:dark_oak_slab`、`minecraft:pale_oak_slab`、`minecraft:mangrove_slab`
- `minecraft:bamboo_slab`、`minecraft:bamboo_mosaic_slab`、`minecraft:stone_slab`、`minecraft:smooth_stone_slab`、`minecraft:sandstone_slab`、`minecraft:cut_sandstone_slab`、`minecraft:petrified_oak_slab`、`minecraft:cobblestone_slab`
- `minecraft:brick_slab`、`minecraft:stone_brick_slab`、`minecraft:mud_brick_slab`、`minecraft:nether_brick_slab`、`minecraft:quartz_slab`、`minecraft:red_sandstone_slab`、`minecraft:cut_red_sandstone_slab`、`minecraft:purpur_slab`
- `minecraft:smooth_stone`、`minecraft:smooth_sandstone`、`minecraft:smooth_quartz`、`minecraft:smooth_red_sandstone`、`minecraft:spruce_fence_gate`、`minecraft:birch_fence_gate`、`minecraft:jungle_fence_gate`、`minecraft:acacia_fence_gate`
- `minecraft:cherry_fence_gate`、`minecraft:dark_oak_fence_gate`、`minecraft:pale_oak_fence_gate`、`minecraft:mangrove_fence_gate`、`minecraft:bamboo_fence_gate`、`minecraft:spruce_fence`、`minecraft:birch_fence`、`minecraft:jungle_fence`
- `minecraft:acacia_fence`、`minecraft:cherry_fence`、`minecraft:dark_oak_fence`、`minecraft:pale_oak_fence`、`minecraft:mangrove_fence`、`minecraft:bamboo_fence`、`minecraft:spruce_door`、`minecraft:birch_door`
- `minecraft:jungle_door`、`minecraft:acacia_door`、`minecraft:cherry_door`、`minecraft:dark_oak_door`、`minecraft:pale_oak_door`、`minecraft:mangrove_door`、`minecraft:bamboo_door`、`minecraft:end_rod`
- `minecraft:chorus_plant`、`minecraft:chorus_flower`、`minecraft:purpur_block`、`minecraft:purpur_pillar`、`minecraft:purpur_stairs`、`minecraft:end_stone_bricks`、`minecraft:torchflower_crop`、`minecraft:pitcher_crop`
- `minecraft:pitcher_plant`、`minecraft:beetroots`、`minecraft:dirt_path`、`minecraft:end_gateway`、`minecraft:repeating_command_block`、`minecraft:chain_command_block`、`minecraft:frosted_ice`、`minecraft:magma_block`
- `minecraft:nether_wart_block`、`minecraft:red_nether_bricks`、`minecraft:bone_block`、`minecraft:structure_void`、`minecraft:observer`、`minecraft:shulker_box`、`minecraft:white_shulker_box`、`minecraft:orange_shulker_box`
- `minecraft:magenta_shulker_box`、`minecraft:light_blue_shulker_box`、`minecraft:yellow_shulker_box`、`minecraft:lime_shulker_box`、`minecraft:pink_shulker_box`、`minecraft:gray_shulker_box`、`minecraft:light_gray_shulker_box`、`minecraft:cyan_shulker_box`
- `minecraft:purple_shulker_box`、`minecraft:blue_shulker_box`、`minecraft:brown_shulker_box`、`minecraft:green_shulker_box`、`minecraft:red_shulker_box`、`minecraft:black_shulker_box`、`minecraft:white_glazed_terracotta`、`minecraft:orange_glazed_terracotta`
- `minecraft:magenta_glazed_terracotta`、`minecraft:light_blue_glazed_terracotta`、`minecraft:yellow_glazed_terracotta`、`minecraft:lime_glazed_terracotta`、`minecraft:pink_glazed_terracotta`、`minecraft:gray_glazed_terracotta`、`minecraft:light_gray_glazed_terracotta`、`minecraft:cyan_glazed_terracotta`
- `minecraft:purple_glazed_terracotta`、`minecraft:blue_glazed_terracotta`、`minecraft:brown_glazed_terracotta`、`minecraft:green_glazed_terracotta`、`minecraft:red_glazed_terracotta`、`minecraft:black_glazed_terracotta`、`minecraft:white_concrete`、`minecraft:orange_concrete`
- `minecraft:magenta_concrete`、`minecraft:light_blue_concrete`、`minecraft:yellow_concrete`、`minecraft:lime_concrete`、`minecraft:pink_concrete`、`minecraft:gray_concrete`、`minecraft:light_gray_concrete`、`minecraft:cyan_concrete`
- `minecraft:purple_concrete`、`minecraft:blue_concrete`、`minecraft:brown_concrete`、`minecraft:green_concrete`、`minecraft:red_concrete`、`minecraft:black_concrete`、`minecraft:white_concrete_powder`、`minecraft:orange_concrete_powder`
- `minecraft:magenta_concrete_powder`、`minecraft:light_blue_concrete_powder`、`minecraft:yellow_concrete_powder`、`minecraft:lime_concrete_powder`、`minecraft:pink_concrete_powder`、`minecraft:gray_concrete_powder`、`minecraft:light_gray_concrete_powder`、`minecraft:cyan_concrete_powder`
- `minecraft:purple_concrete_powder`、`minecraft:blue_concrete_powder`、`minecraft:brown_concrete_powder`、`minecraft:green_concrete_powder`、`minecraft:red_concrete_powder`、`minecraft:black_concrete_powder`、`minecraft:kelp`、`minecraft:kelp_plant`
- `minecraft:dried_kelp_block`、`minecraft:turtle_egg`、`minecraft:sniffer_egg`、`minecraft:dried_ghast`、`minecraft:dead_tube_coral_block`、`minecraft:dead_brain_coral_block`、`minecraft:dead_bubble_coral_block`、`minecraft:dead_fire_coral_block`
- `minecraft:dead_horn_coral_block`、`minecraft:tube_coral_block`、`minecraft:brain_coral_block`、`minecraft:bubble_coral_block`、`minecraft:fire_coral_block`、`minecraft:horn_coral_block`、`minecraft:dead_tube_coral`、`minecraft:dead_brain_coral`
- `minecraft:dead_bubble_coral`、`minecraft:dead_fire_coral`、`minecraft:dead_horn_coral`、`minecraft:tube_coral`、`minecraft:brain_coral`、`minecraft:bubble_coral`、`minecraft:fire_coral`、`minecraft:horn_coral`
- `minecraft:dead_tube_coral_fan`、`minecraft:dead_brain_coral_fan`、`minecraft:dead_bubble_coral_fan`、`minecraft:dead_fire_coral_fan`、`minecraft:dead_horn_coral_fan`、`minecraft:tube_coral_fan`、`minecraft:brain_coral_fan`、`minecraft:bubble_coral_fan`
- `minecraft:fire_coral_fan`、`minecraft:horn_coral_fan`、`minecraft:dead_tube_coral_wall_fan`、`minecraft:dead_brain_coral_wall_fan`、`minecraft:dead_bubble_coral_wall_fan`、`minecraft:dead_fire_coral_wall_fan`、`minecraft:dead_horn_coral_wall_fan`、`minecraft:tube_coral_wall_fan`
- `minecraft:brain_coral_wall_fan`、`minecraft:bubble_coral_wall_fan`、`minecraft:fire_coral_wall_fan`、`minecraft:horn_coral_wall_fan`、`minecraft:sea_pickle`、`minecraft:blue_ice`、`minecraft:conduit`、`minecraft:bamboo_sapling`
- `minecraft:bamboo`、`minecraft:potted_bamboo`、`minecraft:void_air`、`minecraft:cave_air`、`minecraft:bubble_column`、`minecraft:polished_granite_stairs`、`minecraft:smooth_red_sandstone_stairs`、`minecraft:mossy_stone_brick_stairs`
- `minecraft:polished_diorite_stairs`、`minecraft:mossy_cobblestone_stairs`、`minecraft:end_stone_brick_stairs`、`minecraft:stone_stairs`、`minecraft:smooth_sandstone_stairs`、`minecraft:smooth_quartz_stairs`、`minecraft:granite_stairs`、`minecraft:andesite_stairs`
- `minecraft:red_nether_brick_stairs`、`minecraft:polished_andesite_stairs`、`minecraft:diorite_stairs`、`minecraft:polished_granite_slab`、`minecraft:smooth_red_sandstone_slab`、`minecraft:mossy_stone_brick_slab`、`minecraft:polished_diorite_slab`、`minecraft:mossy_cobblestone_slab`
- `minecraft:end_stone_brick_slab`、`minecraft:smooth_sandstone_slab`、`minecraft:smooth_quartz_slab`、`minecraft:granite_slab`、`minecraft:andesite_slab`、`minecraft:red_nether_brick_slab`、`minecraft:polished_andesite_slab`、`minecraft:diorite_slab`
- `minecraft:brick_wall`、`minecraft:prismarine_wall`、`minecraft:red_sandstone_wall`、`minecraft:mossy_stone_brick_wall`、`minecraft:granite_wall`、`minecraft:stone_brick_wall`、`minecraft:mud_brick_wall`、`minecraft:nether_brick_wall`
- `minecraft:andesite_wall`、`minecraft:red_nether_brick_wall`、`minecraft:sandstone_wall`、`minecraft:end_stone_brick_wall`、`minecraft:diorite_wall`、`minecraft:scaffolding`、`minecraft:loom`、`minecraft:barrel`
- `minecraft:smoker`、`minecraft:blast_furnace`、`minecraft:cartography_table`、`minecraft:fletching_table`、`minecraft:grindstone`、`minecraft:lectern`、`minecraft:smithing_table`、`minecraft:stonecutter`
- `minecraft:bell`、`minecraft:lantern`、`minecraft:soul_lantern`、`minecraft:copper_lantern`、`minecraft:exposed_copper_lantern`、`minecraft:weathered_copper_lantern`、`minecraft:oxidized_copper_lantern`、`minecraft:waxed_copper_lantern`
- `minecraft:waxed_exposed_copper_lantern`、`minecraft:waxed_weathered_copper_lantern`、`minecraft:waxed_oxidized_copper_lantern`、`minecraft:campfire`、`minecraft:soul_campfire`、`minecraft:sweet_berry_bush`、`minecraft:warped_stem`、`minecraft:stripped_warped_stem`
- `minecraft:warped_hyphae`、`minecraft:stripped_warped_hyphae`、`minecraft:warped_nylium`、`minecraft:warped_fungus`、`minecraft:warped_wart_block`、`minecraft:warped_roots`、`minecraft:nether_sprouts`、`minecraft:crimson_stem`
- `minecraft:stripped_crimson_stem`、`minecraft:crimson_hyphae`、`minecraft:stripped_crimson_hyphae`、`minecraft:crimson_nylium`、`minecraft:crimson_fungus`、`minecraft:shroomlight`、`minecraft:weeping_vines`、`minecraft:weeping_vines_plant`
- `minecraft:twisting_vines`、`minecraft:twisting_vines_plant`、`minecraft:crimson_roots`、`minecraft:crimson_planks`、`minecraft:warped_planks`、`minecraft:crimson_slab`、`minecraft:warped_slab`、`minecraft:crimson_pressure_plate`
- `minecraft:warped_pressure_plate`、`minecraft:crimson_fence`、`minecraft:warped_fence`、`minecraft:crimson_trapdoor`、`minecraft:warped_trapdoor`、`minecraft:crimson_fence_gate`、`minecraft:warped_fence_gate`、`minecraft:crimson_stairs`
- `minecraft:warped_stairs`、`minecraft:crimson_button`、`minecraft:warped_button`、`minecraft:crimson_door`、`minecraft:warped_door`、`minecraft:crimson_sign`、`minecraft:warped_sign`、`minecraft:crimson_wall_sign`
- `minecraft:warped_wall_sign`、`minecraft:structure_block`、`minecraft:jigsaw`、`minecraft:test_block`、`minecraft:test_instance_block`、`minecraft:composter`、`minecraft:target`、`minecraft:bee_nest`
- `minecraft:beehive`、`minecraft:honey_block`、`minecraft:honeycomb_block`、`minecraft:netherite_block`、`minecraft:ancient_debris`、`minecraft:crying_obsidian`、`minecraft:respawn_anchor`、`minecraft:potted_crimson_fungus`
- `minecraft:potted_warped_fungus`、`minecraft:potted_crimson_roots`、`minecraft:potted_warped_roots`、`minecraft:lodestone`、`minecraft:blackstone`、`minecraft:blackstone_stairs`、`minecraft:blackstone_wall`、`minecraft:blackstone_slab`
- `minecraft:polished_blackstone`、`minecraft:polished_blackstone_bricks`、`minecraft:cracked_polished_blackstone_bricks`、`minecraft:chiseled_polished_blackstone`、`minecraft:polished_blackstone_brick_slab`、`minecraft:polished_blackstone_brick_stairs`、`minecraft:polished_blackstone_brick_wall`、`minecraft:gilded_blackstone`
- `minecraft:polished_blackstone_stairs`、`minecraft:polished_blackstone_slab`、`minecraft:polished_blackstone_pressure_plate`、`minecraft:polished_blackstone_button`、`minecraft:polished_blackstone_wall`、`minecraft:chiseled_nether_bricks`、`minecraft:cracked_nether_bricks`、`minecraft:quartz_bricks`
- `minecraft:candle`、`minecraft:white_candle`、`minecraft:orange_candle`、`minecraft:magenta_candle`、`minecraft:light_blue_candle`、`minecraft:yellow_candle`、`minecraft:lime_candle`、`minecraft:pink_candle`
- `minecraft:gray_candle`、`minecraft:light_gray_candle`、`minecraft:cyan_candle`、`minecraft:purple_candle`、`minecraft:blue_candle`、`minecraft:brown_candle`、`minecraft:green_candle`、`minecraft:red_candle`
- `minecraft:black_candle`、`minecraft:candle_cake`、`minecraft:white_candle_cake`、`minecraft:orange_candle_cake`、`minecraft:magenta_candle_cake`、`minecraft:light_blue_candle_cake`、`minecraft:yellow_candle_cake`、`minecraft:lime_candle_cake`
- `minecraft:pink_candle_cake`、`minecraft:gray_candle_cake`、`minecraft:light_gray_candle_cake`、`minecraft:cyan_candle_cake`、`minecraft:purple_candle_cake`、`minecraft:blue_candle_cake`、`minecraft:brown_candle_cake`、`minecraft:green_candle_cake`
- `minecraft:red_candle_cake`、`minecraft:black_candle_cake`、`minecraft:amethyst_block`、`minecraft:budding_amethyst`、`minecraft:amethyst_cluster`、`minecraft:large_amethyst_bud`、`minecraft:medium_amethyst_bud`、`minecraft:small_amethyst_bud`
- `minecraft:tuff`、`minecraft:tuff_slab`、`minecraft:tuff_stairs`、`minecraft:tuff_wall`、`minecraft:polished_tuff`、`minecraft:polished_tuff_slab`、`minecraft:polished_tuff_stairs`、`minecraft:polished_tuff_wall`
- `minecraft:chiseled_tuff`、`minecraft:tuff_bricks`、`minecraft:tuff_brick_slab`、`minecraft:tuff_brick_stairs`、`minecraft:tuff_brick_wall`、`minecraft:chiseled_tuff_bricks`、`minecraft:calcite`、`minecraft:tinted_glass`
- `minecraft:powder_snow`、`minecraft:sculk_sensor`、`minecraft:calibrated_sculk_sensor`、`minecraft:sculk`、`minecraft:sculk_vein`、`minecraft:sculk_catalyst`、`minecraft:sculk_shrieker`、`minecraft:copper_block`
- `minecraft:exposed_copper`、`minecraft:weathered_copper`、`minecraft:oxidized_copper`、`minecraft:copper_ore`、`minecraft:deepslate_copper_ore`、`minecraft:oxidized_cut_copper`、`minecraft:weathered_cut_copper`、`minecraft:exposed_cut_copper`
- `minecraft:cut_copper`、`minecraft:oxidized_chiseled_copper`、`minecraft:weathered_chiseled_copper`、`minecraft:exposed_chiseled_copper`、`minecraft:chiseled_copper`、`minecraft:waxed_oxidized_chiseled_copper`、`minecraft:waxed_weathered_chiseled_copper`、`minecraft:waxed_exposed_chiseled_copper`
- `minecraft:waxed_chiseled_copper`、`minecraft:oxidized_cut_copper_stairs`、`minecraft:weathered_cut_copper_stairs`、`minecraft:exposed_cut_copper_stairs`、`minecraft:cut_copper_stairs`、`minecraft:oxidized_cut_copper_slab`、`minecraft:weathered_cut_copper_slab`、`minecraft:exposed_cut_copper_slab`
- `minecraft:cut_copper_slab`、`minecraft:waxed_copper_block`、`minecraft:waxed_weathered_copper`、`minecraft:waxed_exposed_copper`、`minecraft:waxed_oxidized_copper`、`minecraft:waxed_oxidized_cut_copper`、`minecraft:waxed_weathered_cut_copper`、`minecraft:waxed_exposed_cut_copper`
- `minecraft:waxed_cut_copper`、`minecraft:waxed_oxidized_cut_copper_stairs`、`minecraft:waxed_weathered_cut_copper_stairs`、`minecraft:waxed_exposed_cut_copper_stairs`、`minecraft:waxed_cut_copper_stairs`、`minecraft:waxed_oxidized_cut_copper_slab`、`minecraft:waxed_weathered_cut_copper_slab`、`minecraft:waxed_exposed_cut_copper_slab`
- `minecraft:waxed_cut_copper_slab`、`minecraft:copper_door`、`minecraft:exposed_copper_door`、`minecraft:oxidized_copper_door`、`minecraft:weathered_copper_door`、`minecraft:waxed_copper_door`、`minecraft:waxed_exposed_copper_door`、`minecraft:waxed_oxidized_copper_door`
- `minecraft:waxed_weathered_copper_door`、`minecraft:copper_trapdoor`、`minecraft:exposed_copper_trapdoor`、`minecraft:oxidized_copper_trapdoor`、`minecraft:weathered_copper_trapdoor`、`minecraft:waxed_copper_trapdoor`、`minecraft:waxed_exposed_copper_trapdoor`、`minecraft:waxed_oxidized_copper_trapdoor`
- `minecraft:waxed_weathered_copper_trapdoor`、`minecraft:copper_grate`、`minecraft:exposed_copper_grate`、`minecraft:weathered_copper_grate`、`minecraft:oxidized_copper_grate`、`minecraft:waxed_copper_grate`、`minecraft:waxed_exposed_copper_grate`、`minecraft:waxed_weathered_copper_grate`
- `minecraft:waxed_oxidized_copper_grate`、`minecraft:copper_bulb`、`minecraft:exposed_copper_bulb`、`minecraft:weathered_copper_bulb`、`minecraft:oxidized_copper_bulb`、`minecraft:waxed_copper_bulb`、`minecraft:waxed_exposed_copper_bulb`、`minecraft:waxed_weathered_copper_bulb`
- `minecraft:waxed_oxidized_copper_bulb`、`minecraft:copper_chest`、`minecraft:exposed_copper_chest`、`minecraft:weathered_copper_chest`、`minecraft:oxidized_copper_chest`、`minecraft:waxed_copper_chest`、`minecraft:waxed_exposed_copper_chest`、`minecraft:waxed_weathered_copper_chest`
- `minecraft:waxed_oxidized_copper_chest`、`minecraft:copper_golem_statue`、`minecraft:exposed_copper_golem_statue`、`minecraft:weathered_copper_golem_statue`、`minecraft:oxidized_copper_golem_statue`、`minecraft:waxed_copper_golem_statue`、`minecraft:waxed_exposed_copper_golem_statue`、`minecraft:waxed_weathered_copper_golem_statue`
- `minecraft:waxed_oxidized_copper_golem_statue`、`minecraft:lightning_rod`、`minecraft:exposed_lightning_rod`、`minecraft:weathered_lightning_rod`、`minecraft:oxidized_lightning_rod`、`minecraft:waxed_lightning_rod`、`minecraft:waxed_exposed_lightning_rod`、`minecraft:waxed_weathered_lightning_rod`
- `minecraft:waxed_oxidized_lightning_rod`、`minecraft:pointed_dripstone`、`minecraft:dripstone_block`、`minecraft:cave_vines`、`minecraft:cave_vines_plant`、`minecraft:spore_blossom`、`minecraft:azalea`、`minecraft:flowering_azalea`
- `minecraft:moss_carpet`、`minecraft:pink_petals`、`minecraft:wildflowers`、`minecraft:leaf_litter`、`minecraft:moss_block`、`minecraft:big_dripleaf`、`minecraft:big_dripleaf_stem`、`minecraft:small_dripleaf`
- `minecraft:hanging_roots`、`minecraft:rooted_dirt`、`minecraft:mud`、`minecraft:deepslate`、`minecraft:cobbled_deepslate`、`minecraft:cobbled_deepslate_stairs`、`minecraft:cobbled_deepslate_slab`、`minecraft:cobbled_deepslate_wall`
- `minecraft:polished_deepslate`、`minecraft:polished_deepslate_stairs`、`minecraft:polished_deepslate_slab`、`minecraft:polished_deepslate_wall`、`minecraft:deepslate_tiles`、`minecraft:deepslate_tile_stairs`、`minecraft:deepslate_tile_slab`、`minecraft:deepslate_tile_wall`
- `minecraft:deepslate_bricks`、`minecraft:deepslate_brick_stairs`、`minecraft:deepslate_brick_slab`、`minecraft:deepslate_brick_wall`、`minecraft:chiseled_deepslate`、`minecraft:cracked_deepslate_bricks`、`minecraft:cracked_deepslate_tiles`、`minecraft:infested_deepslate`
- `minecraft:smooth_basalt`、`minecraft:raw_iron_block`、`minecraft:raw_copper_block`、`minecraft:raw_gold_block`、`minecraft:potted_azalea_bush`、`minecraft:potted_flowering_azalea_bush`、`minecraft:ochre_froglight`、`minecraft:verdant_froglight`
- `minecraft:pearlescent_froglight`、`minecraft:frogspawn`、`minecraft:reinforced_deepslate`、`minecraft:decorated_pot`、`minecraft:crafter`、`minecraft:trial_spawner`、`minecraft:vault`、`minecraft:heavy_core`
- `minecraft:pale_moss_block`、`minecraft:pale_moss_carpet`、`minecraft:pale_hanging_moss`、`minecraft:open_eyeblossom`、`minecraft:closed_eyeblossom`、`minecraft:potted_open_eyeblossom`、`minecraft:potted_closed_eyeblossom`、`minecraft:firefly_bush`

<!-- official-block-inventory:end -->
