# 交接索引与历史记录

## 当前状态（2026-11-09）

本项目目标是实现对齐 Minecraft Java Edition 26.1 的 C++ 服务端，不是把历史阶段中「单维度 / 只读 / 无生成 / 无光照」等描述当作项目定义。目标范围见 [AGENTS.md](../AGENTS.md) 第 1 节，唯一任务顺序与验收口径见 [PLAN.md](../PLAN.md)，角色切换和状态唯一写入 [task/TASKS.md](../task/TASKS.md) 的末行。

F-1…F-8、M0-2 与 L-1 已按当前任务定义验收。L-2a 的方块光增减传播正在按 `TASKS.md` 末行验收；它沿用 L-1 的官方光学元数据与 opt-in 单写者交接，不包含天空光传播、协议光照或 Anvil 光照写回。任何构建、差分和性能状态仍以 `TASKS.md` 为准。

- 当前流体：状态、计划刻、传播、参数、宿主、存档读入和单例所有权已实现；W-01、W-02、M-01、M-03、H-01 与双观察者已有对应真实差分。未跑分支及同阶段性能补证归 [F-9a/F-9b](../PLAN.md#阶段-25--流体补充验收)，见[流体验收证据](fluid-acceptance.md)与[测试规格](fluid-testing.md)。
- 当前光照：L-1 双层数据、官方状态光学元数据、四张 final-chunk 高度图和 opt-in 后台交接已实现；L-2a 方块光增减传播已实现，天空光传播仍未实现，协议区块光照段仍是占位，默认生产不提供完整“已照明”数据。详见[光照底座](light-engine.md)；任务与性能边界见 [PLAN.md](../PLAN.md#阶段-2--光照)。
- 当前时间与保存：`gameTime` 只从 `level.dat` 的 `Time` 恢复并在内存递增；26.1 World Clocks、`world_clocks`、`advance_time`、睡眠跳转、`/time` 语义和时钟持久化尚未实现。Anvil 仍只读，不写回 `level.dat`、`.mca`、方块刻/流体刻或实体。目标见 [PLAN.md](../PLAN.md#阶段-3--时间与世界保存)。
- 当前加载与世界：区块仍由显式选择/加载强加载控制，没有原版加载票和 ticking 等级；没有世界生成、缺失区块自动生成或完整多维度。原生协议 775 服务端是可选的创造编辑入口，不是完整原版多人服务端，详见[原版连接说明](protocol775.md)。
- 当前能力边界以[兼容性边界](compatibility.md)为准；本文件从“历史交接追加”起的未提交状态、测试数字和 TODO 都是带日期的历史快照，不再代表当前工作区。

## 历史交接追加（2026-11-06，归档）

> **2026-11-08 方向变更**：项目定位已从「单维度只读红石模拟器」改为「对齐 Minecraft Java 26.1 的 C++ 服务端」（多维度、加载票、世界生成、世界保存、时间、光照）。下面所有「单维度 / 不保存 / 无流体刻 / 无加载票」的表述一律按**当时的实现状态**理解，不是项目定义。目标形态见 [AGENTS.md](../AGENTS.md) 第 1 节，分期见 [PLAN.md](../PLAN.md)。

### 当时追加（2026-11-06）

当时追加：用户要求加载无沟 V3.1 世吞投影并启动，已实现沙子、石砖墙、红石灯，告示牌跳过。新增 `litematic.hpp/cpp` 快照导入、跨 long/负尺寸/实体坐标验证、铁砧替换黑曜石、全部投影实体独立 UUID；实际导入 70,111 方块、133 矿车，强加载 2,916 区块（含两区块边缘）。协议改为玩家周围视野分批发送，大世界每批最多 2 区块并等待 ACK；不再有全世界 64 区块上限。每 tick 结束以 `system_chat overlay=false` 向聊天栏发本 tick 耗时。下落方块成功落地包立即发送给跟踪该实体的玩家，并且先于实体移除。MSVC Release/jemalloc CTest **12/12**，实际投影读取测试和两次完整投影 TCP 100 tick 测试通过；其中一次右键前端音符盒后，100 tick 时有 401 个计划刻及 266 个移动活塞，未做原版整机差分。更新后的协议测试 **255 个包**已由官方 Java codec 严格回读。详见[litematic.md](litematic.md)和 `validation/litematic/server[-active]-smoke-result.json`。当时服务端曾启动于 127.0.0.1:25565，出生点 (2.5,102,-1.5)，前端音符盒 (2,101,3)；没有启动客户端。该快照的所有改动在当时尚未提交。

用户已确认路线为**原生 C++ 协议服务端，而且需要放置/破坏方块**。无需再次询问原生/桥接或只读/交互路线。用户要求本轮不启动客户端或接管操作，已遵守；没有实际客户端连接/动画验收。

- 审查原未提交实体/铁轨/活塞实现并修复多项：跨格碰撞盒、地毯下摩擦/速度因子、空形状脱困出口、铁轨 RIGID 支撑、T 路口电源切向、非标准活塞进度先扫掠再截断、物品分数伤害取整、建造高度边界冻结/虚空移除与下落方块超时边界；保留 TNT 独立虚空行为。详见[更新顺序审查](update-order-audit.md)，关键路径有实际行为测试。
- 存储审查修正 26.1 主世界 `dimensions/minecraft/overworld/region` 默认路径、同维度独立实体区域和规范 `data/minecraft/world_gen_settings.dat` 种子；遗留列表/UUID去重/数值边界都有回归。当时已有原版存档单区块副本由 CLI 只读导入成功，读取 43,594 个方块、4 个简单实体和 2 个未模拟实体，不启动服务端/客户端；证据见[存档说明](storage.md)。
- 新增独立可选 `REDSTONE_BUILD_VANILLA_SERVER`（默认 OFF）和 `redstone_vanilla_server`；当时本机 build-jemalloc 设为 ON。文件在 `include/redstone/net/`、`src/net/`；默认监听 127.0.0.1:25565，离线协议 775，固定已加载区域、创造物品选择/编辑、活塞事件和简单实体同步。使用方法、验证边界与具体 TODO 见[原版连接说明](protocol775.md)。
- `assets/protocol775/` 来自官方 26.1 jar：完整状态/物品/实体/serializer ID、28 动态注册表和 tags、section 样例、状态标志、manifest 来源哈希。重建脚本 `validation/protocol775/export.ps1` 不启动真实服务端或客户端。
- 网络普通方块变化只记脏位置，在计划刻后、方块事件前的区块广播入口读取当前状态并合并发送；事件阶段生成的普通变化留至下一刻广播。成功方块事件立即发出。**不能把源位置 air 提前发到活塞事件前**。预测修正独立立即发。
- 放置侦测器不能用 `updateFromNeighbourShapes` 预查询，否则凭空调度脉冲。已改为按方块放置规则处理，仅墙/草/音符盒初始化其邻居属性。支撑、碰撞、珊瑚候选、矿车坡道、负预测序列等已有检查。
- 连接输出有上限；压缩已发送前缀；映射错误/积压只关闭对应连接，不能中断核心更新链。核心无网络开启时这些观察钩子均为空。
- 默认 jemalloc / MSVC Release 构建成功；启用网络后 CTest **11/11 通过**（原 8 组 + wire/protocol/protocol_socket）。TCP 测试实际运行本地 C++ 服务端，测试完成已退出；无客户端进程。
- 原版 Java GameProtocols codec 已严格回读 C++ 最新 **182** 个 play 数据包（无剩余字节），包括 4 份区块各 24 个 section；另 30 个配置包也已回读。证据见 `validation/protocol775/verification-result.json`。这是编码验证，不是真实客户端或机制差分。
- 当时所有改动仍未提交；没有替用户提交或丢弃原改动。Java 临时 classes 和项目根 logs 已忽略。

当时下一步：完整玩家/容器与库存点击处理、更多 BlockPlaceContext/物品组件、真实光照/biome、动态客户端区块跟踪，以及在用户允许后由用户进行实际客户端连接验证。下面第 1–11 节保留该轮之前的交接历史；若与当前状态冲突，以本文件开头和[原版连接说明](protocol775.md)为准。（原文「水/流体刻仍不做」是**历史结论**；流体现在已经实现并通过 F-7 当前核心验收。）

## 1. 历史快照：仓库状态（2026-11-06）

- 工作目录：`F:\source\minecraft-merged-07da9a845a-26.1-sources\redstone-cpp`
- 基线提交：`6b543a3 Initial C++20 redstone simulator and vanilla order validation`
- 当时**没有任何新提交**；本批所有改动都在工作区，处于“可构建、可测试、未提交”状态。
- 上一级 `F:\source\minecraft-merged-07da9a845a-26.1-sources` 是只读的 26.1 Java 行为依据，不要修改。
- 构建目录 `build-jemalloc/` 等已被 `.gitignore` 忽略；`assets/` 下新增的铁轨纹理和 `validation/vanilla26_1/` 下的物品轨迹探针/日志是未跟踪文件。

## 2. 本批完成的内容

### 2.1 简单实体框架

新增：

- `include/redstone/entity.hpp`
- `src/entity.cpp`

实现：

- `Vec3`、`Aabb`、Java `java.util.Random` 兼容随机流（随机序列不作为兼容硬要求）。
- `Entity` 基类：尺寸、脚底中心位置、AABB、方块碰撞、重力、速度、落地/水平碰撞、液体高度、岩浆/火焰伤害、`moveTowardsClosestSpace`、世界底部移除。
- `World` 实体列表：按注册顺序 tick；每刻快照；刻内新增实体延后一刻；移除实体跳过；未加载区块的实体暂停。
- 方块碰撞盒逐轴扫掠，轴顺序 `Y → (X/Z 按水平位移大小)`，容差与 `Direction.axisStepOrder` 对照。
- 史莱姆块落地反弹、`stepOn` 水平减速。

### 2.2 简单实体类型

- **ItemEntity**：重力 0.04、水/岩浆阻尼、方块摩擦、落地反弹、合并、`Age` 6000 刻消失、`PickupDelay`、生命值。没有玩家拾取。
- **PrimedTnt**：引信 80、重力、移动、爆炸；被红石信号点燃的 TNT 方块生成真实实体。
- **FallingBlockEntity**：重力、落地写回、超时/越界掉落。没有注册沙子/砂砾等自然触发方块。
- **Minecart**：非实验性 `OldMinecartBehavior`，直轨/坡道/离轨、动力轨、探测轨、激活轨、矿车互推。

### 2.3 TNT 与爆炸

- `World::explode` 对应 `ServerExplosion`：16³ 边界射线、0.3 步长、抗性公式、实体遮挡采样、伤害/击退、方块掉落合并（单堆 16）、TNT 链式引信 `nextInt(20) + 10`。
- 方块抗性：黑曜石 1200、基岩 3,600,000、移动活塞 0；水/岩浆 100。
- TNT 方块不参与掉落；爆炸后生成随机引信 TNT 实体。
- `tntExplodes` 对应 `TNT_EXPLODES`。
- 火焰生成仍为 TODO（TNT 自身 `fire=false`）。

### 2.4 铁轨方块

- 注册 `rail`、`powered_rail`、`detector_rail`、`activator_rail`，状态含 `shape`、`powered`、`waterlogged`。
- 无碰撞、支撑检查、`RailState` 自动连接/软连接、坡道、动力链搜索、探测铁轨 20 刻复查与断电、激活铁轨。
- `waterlogged` 仅保留属性；流体刻未实现。

### 2.5 物品与掉落

- `itemRegistry()`、`itemType()`、最大堆叠（矿车 1，其余 64）。
- `blockItem()`、`blockDrops()`：空工具下的本批掉落表；草方块→泥土；玻璃/枯珊瑚扇/活塞头/移动活塞/基岩/不稳定 TNT 无掉落。
- `updateOrDestroy`、活塞破坏、方块爆炸都生成真实 `ItemEntity`，不再只写 `entity_spawn` 日志；日志保留用于观察。

### 2.6 存档

- `LoadedWorld` 读取区块 `Entities`，导入 item/TNT/falling_block/minecart；读取 `level.dat` 的 `RandomSeed`。
- 其他实体只保留原始标签并计数，不创建实体。
- 仍不读 gamerules、持久化强加载列表、其他区块实体。

### 2.7 调试客户端

- 状态栏显示实体数量；简单实体以碰撞盒调试立方体绘制（TNT 红、矿车灰蓝、物品按物品名着色）。
- 创造列表自动包含四种铁轨；放置时按视线选择初始 `shape`，随后由核心 `onPlace` 自动连接。
- 新增 8 张铁轨纹理（`assets/textures/block/*rail*`），导入脚本已更新。
- 客户端不做实体模型、玩家拾取、物品栏/漏斗交互。

## 3. 最新的活塞/粘液块修复（重点）

用户报告：地毯复制结构破坏红石块后，额外物品实体应被回缩的粘液块弹向左边，但 C++ 运动不同。

根因：

- 原版 `PistonMovingBlockEntity.tick` 在非最终分支中先调用 `moveCollidedEntities` / `moveStuckEntities`，再推进 `progress`。
- C++ 之前只推进 progress，并写一条 TODO 日志，完全没有实体扫掠/推动。

已修复（`src/piston.cpp`、`src/entity.cpp`、`src/shapes.cpp`）：

- 实现 `PistonMath.getMovementArea`、`moveByPositionAndProgress`、`getMovement`、`moveCollidedEntities`、`moveStuckEntities`、`fixEntityWithinPistonBase`。
- 在 moving_piston 方块实体阶段、`progress += 0.5` 之前处理实体。
- `MoverType.PISTON` 每轴每刻累计 0.51 限制。
- NOCLIP：推动期间同方向 moving_piston 忽略移动状态碰撞盒；回缩源活塞保留伸出头碰撞盒。
- 粘液块 `causeBounce`：非玩家实体沿移动轴速度设为 ±1，然后仍执行扫掠位移；下一次实体刻按 `-1, -0.98, -0.9604, ...` 运动。
- 蜂蜜块水平 `moveStuckEntities`。
- 回缩源活塞基座内实体挤出。

验证：

- `tests/piston_tests.cpp`
  - `slimePistonBounce`：单刻获得 `-1.0` X 速度并被扫掠推动。
  - `movingPistonStageOrder`：实体刻先于 moving_piston 方块实体刻；若顺序相反，物品位置会明显不同。
  - `carpetDuplicator`：真实原版结构的额外地毯物品存在，并被回缩粘液块向西弹出。
- `validation/vanilla26_1/item_motion_validation.py` 和 `item-motion-validation.log`：实际 26.1 服务端读取 `Pos`/`Motion`；粘液块命中后 X 速度序列 `-1.0, -0.98, -0.9604, ...` 与 C++ 一致。
- 注意：原版命令探针的“命中绝对刻”会随 `setblock`/`tick step` 调度移动一 tick；物品生成位置和初始 Z 速度来自随机源。因此测试只断言方向/局部单刻 -1.0/确定性递推，不断言某次服务器运行的绝对坐标。用户已明确不要求随机序列逐位一致。

## 4. tick 与阶段顺序

当前 `World::tick`：

1. `gameTime++`
2. 方块计划刻（上限 65536）
3. 方块事件（`runEvents`，`handlingTick=true`）
4. `handlingTick=false`
5. `tickEntities()`：按注册顺序快照；`tickCount++`；`setOldPosAndRot()`；调用 `tick()`
6. moving_piston 方块实体刻（`pistonTickBlockEntities`，含实体推动/进度推进）
7. 外部 `blockEntityTicker`

这与 `ServerLevel.tick` 的“方块事件 → entities → blockEntities”顺序一致。请不要把 moving_piston 的实体推动或进度推进移到实体刻之前。

实体列表语义：

- 刻内新增实体进入下一份活动表，下一游戏刻才刻。
- 当前刻被移除的实体在后续快照项中跳过。
- 区块未加载的实体保留但不刻，重新加载后继续。

## 5. 构建与测试

MSVC 2026 + jemalloc：

```powershell
cd F:\source\minecraft-merged-07da9a845a-26.1-sources\redstone-cpp
cmake -S . -B build-jemalloc -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static"
cmake --build build-jemalloc --config Release -j 6
ctest --test-dir build-jemalloc -C Release --output-on-failure
```

当时最近结果：**CTest 8/8 通过**（safesave_order_parity、engine、entity、piston、storage、allocator、shapes、client_inventory）。Release 下断言仍启用。

GCC 15.2 的 system Debug 构建目录存在，但本机 `g++` 当前连空程序也返回 1（环境问题），本批未能在 GCC 下验证。

干净配置验证：本批曾用临时 `build-check` 目录完成全量配置/构建/8 组 CTest；验证后已删除该目录。

## 6. 调试客户端

```powershell
./build-jemalloc/Release/redstone_client.exe
```

- 纹理从安装目录旁的 `assets` 读取；构建后会自动复制。
- 最后一次用户请求是“运行轻量客户端”，已启动并确认窗口 `Responding=True`；随后用户关闭，退出码 0。
- 用户已明确“不用测试客户端了”，当前没有客户端进程在运行。
- 隐藏预览：

```powershell
./build-jemalloc/Release/redstone_client.exe --preview "$env:TEMP\preview.bmp"
./build-jemalloc/Release/redstone_client.exe --preview-inventory "$env:TEMP\inventory.bmp"
```

实际预览/背包截图已检查：铁轨纹理出现在创造列表底行；简单实体没有出现在默认 demo 场景，因为 demo 没有生成实体。

## 7. 历史限制 / TODO（2026-11-06）

按优先级：

1. **原版客户端直连**：当时尚未实现，见第 8 节；后续已实现可选原生协议入口。
2. 实体间碰撞、实体挤压、活塞推动玩家/生物、载具乘客、`ServerLevel.tickPassenger` 递归。
3. 玩家拾取、物品栏、漏斗、容器、生存模式、掉落经验。
4. 完整战利品表（精准采集、时运、工具条件）、方块实体掉落数据。
5. 流体刻（水/岩浆流动、液面重算、流动推挤、waterlogged 排水）；水和岩浆目前只有存档状态、空碰撞和接触判定。
6. 火焰/冰冻/粉末雪/蛛网/蜂蜜下滑等 `entityInside` 效果；目前只接入岩浆和探测铁轨。
7. 火焰爆炸（TNT `fire=false`）、方块实体爆炸钩子、爆炸 source/击杀归属、`Util.shuffle` 哈希/随机顺序。
8. 其他简单实体：箱子/漏斗/熔炉/命令方块/生成器/TNT 矿车；实验性 `NewMinecartBehavior`。
9. 下落方块的自然源方块（沙/砂砾）、铁砧伤害、混凝土遇水、方块实体数据合并。
10. 客户端实体模型/物品渲染、实体插值、铁轨完整模型；当前实体是碰撞盒调试立方体。
11. 存档实体 UUID/Owner/Thrower/Passengers/自定义组件、其他实体类型。
12. 尚未对实体/铁轨/爆炸做完整 26.1 服务端逐刻差分，不能宣称原版等价。

## 8. 历史调研：原版客户端直连（后续已实现协议入口）

> 本节记录当时的可行性调研与分阶段建议；原生协议服务端后续已实现，当前边界见[原版连接说明](protocol775.md)。

用户当时已询问“像原版服务端那样让原版客户端连进去”。当时**未实现**，只做了协议源码调研。结论是技术上可行，但这是一个独立的大子系统，不能直接塞进现有 `World::tick`；后续实现见[原版连接说明](protocol775.md)。

### 8.1 已确认事实

- 26.1 服务端 `SharedConstants.RELEASE_NETWORK_PROTOCOL_VERSION = 775`。
- 本机客户端 `D:\Release 2.7.3\Release 2.7.3\.minecraft\versions\26.1.2\26.1.2.jar` 的 `version.json` 也是 `protocol_version: 775`，可用于协议测试。
- 客户端 `26.1.2` 的 `world_version` 是 4790；本模拟器存档只接受 26.1 DataVersion 4786。协议层可连，但世界/方块状态 ID 要以 26.1 服务端/客户端实际注册表为准，需要单独验证。
- 当前项目没有网络、加密、NBT 写入、注册表同步或区块 packet 代码；AGENTS 里原本规定“无网络”，本次用户已打开这个方向，但仍应做成可选模块，不改变核心模拟语义。

### 8.2 最小连接链路

按 26.1 源码：

1. `handshake`：协议 775、NextState=1/2。
2. 状态 ping：`ClientboundStatusResponse` JSON，客户端服务器列表显示在线。
3. 登录：`ServerboundHelloPacket`（用户名）；离线模式跳过加密；`ClientboundLoginFinishedPacket`（GameProfile）。
4. 客户端 `ServerboundLoginAcknowledgedPacket` 后切到 configuration。
5. Configuration：
   - `ClientboundUpdateEnabledFeaturesPacket`
   - `ClientboundSelectKnownPacks` / `ServerboundSelectKnownPacks`
   - 由 `SynchronizeRegistriesTask` 发送 `ClientboundRegistryDataPacket` 和 `ClientboundUpdateTagsPacket`
   - `ClientboundFinishConfiguration` / `ServerboundFinishConfiguration`
6. Play：
   - `ClientboundLoginPacket`（playerId、levels、chunkRadius、CommonPlayerSpawnInfo）
   - `ClientboundSetDefaultSpawnPosition`、`ClientboundGameEvent`、`ClientboundPlayerInfoUpdate`
   - `ClientboundLevelChunkWithLightPacket`（区块 + 光照）
   - `ClientboundSetCenterChunk`、`ClientboundSetTime`、`ClientboundKeepAlive`
   - 实体/物品/矿车/TNT 需要 `ClientboundAddEntity`、`ClientboundSetEntityData`、`ClientboundSetEntityMotion`、`ClientboundTeleportEntity` 等
   - 方块变化需要 `ClientboundBlockUpdate`；活塞/移动方块需要客户端动画/方块实体数据
7. 输入回传：`ServerboundMovePlayerPacket`、方块交互、`ServerboundPlayerAction` 等；当前模拟器没有玩家实体，必须新增玩家/碰撞/物品栏模型，或限定为只读观察模式。

### 8.3 主要难点

- **注册表同步**：`RegistrySynchronization.packRegistries` 依赖 worldgen 层的动态注册表；26.1 的维度类型、生物群系、伤害类型等数据量大。可以用真实服务端捕获一次或从服务端数据生成静态数据，但要处理 `KnownPacks` 协商和版本变化。
- **方块状态 ID**：客户端 packet 用数值 block state ID，不是名称；需要 26.1 注册表顺序/ID 映射（`blocks` registry）。本模拟器的 `BlockState` 名称/属性必须映射。
- **区块 packet**：`ClientboundLevelChunkPacketData` 是现代 section palette + 区块高度、方块实体、光照数据；26.1 格式与现有 Anvil 读取不同，需另写 writer。
- **实体元数据**：物品堆叠、TNT 引信、矿车、活塞移动方块的客户端同步需要 entity data index/serializer 和 tick 插值。
- **加密/在线模式**：建议第一版强制离线模式（`online-mode=false` 语义），避免 RSA/AES 和 Mojang session 校验；正式联机必须另行处理。
- **输入**：真正“客户端能玩”需要玩家实体、碰撞、物品栏、方块交互、命令/聊天；只读观察模式会简单很多。
- **版本漂移**：26.1 vs 26.1.2 协议 775 相同，但 data/world version 不同；26.2 客户端需另行确认协议版本，不能假定可直接连 775 服务端。

### 8.4 推荐路线

优先做**独立可选模块**，例如 `protocol/` 或 `src/net/` + `redstone_vanilla_server` 可执行文件，不修改 `World` 核心语义：

- 阶段 A：状态 ping + 离线登录 + configuration。先让原版客户端进入“连接中/配置中”，用真实客户端看 disconnect 原因，逐包补齐。
- 阶段 B：静态单区块 + `ClientboundLoginPacket`，让客户端站到一个只读的平整世界。
- 阶段 C：方块更新（`ClientboundBlockUpdate`）和简单实体（物品/TNT/矿车）同步，先把模拟状态可视化。
- 阶段 D：活塞/移动方块动画、光照、区块加载边界。
- 阶段 E：客户端输入 → 方块编辑（只读观察模式可跳过）。

对于 registry/chunk 数据，建议先用真实 26.1 服务端 + 代理抓取合法配置包，保存为静态模板；不要手写完整 registry codec。要记录协议版本、客户端版本和抓包来源。

另一条更快的路线是“真实 vanilla 服务端 + 桥接”：客户端连真实服务端，C++ 通过 RCON/Fabric/命令把方块和实体镜像过去。它能让原版客户端连入，但不是直接连 C++ 模拟核心，且真实服务端自身仍在 tick，需要冻结/接管；这只与当时“单维度、无生成、只读”的实现快照冲突。后续用户已确认采用原生 C++ 协议服务端，不再需要重新选择该路线。

### 8.5 接手建议

- 先和用户确认：是要**原生 C++ 协议服务端**，还是**真实服务端 + 桥接**；是否要求玩家可操作，还是只读观察；目标客户端是 26.1/26.1.2 还是 26.2。
- 在未确认前不要直接把网络代码并入 `engine.cpp`；先建独立目录和独立可执行目标。
- 先实现状态 ping 和离线登录，再处理 configuration registry；每步用真实客户端断开原因驱动。
- 继续遵守“所有改动在本目录、上一级 Java 源码只读、未实现路径写 TODO”的约束。

## 9. 关键文件索引

| 内容 | 文件 |
|---|---|
| 方块/实体/物品注册、世界主循环 | `include/redstone/engine.hpp`、`src/engine.cpp` |
| 实体基类、物品/TNT/下落方块/矿车、爆炸 | `include/redstone/entity.hpp`、`src/entity.cpp` |
| 移动活塞、结构解析、活塞实体 | `include/redstone/piston.hpp`、`src/piston.cpp` |
| 铁轨 | `include/redstone/rail.hpp`、`src/rail.cpp` |
| 形状/碰撞盒 | `include/redstone/shapes.hpp`、`src/shapes.cpp` |
| Anvil 导入/实体 NBT | `include/redstone/storage.hpp`、`src/storage.cpp` |
| CLI/JSON 快照 | `src/main.cpp` |
| 调试客户端 | `src/client_win32.cpp`、`src/client_inventory.hpp`、`src/client_textures.hpp` |
| 流体状态、计划刻、传播与所有权 | `include/redstone/fluid.hpp`、`include/redstone/fluid_flow.hpp`、`src/fluid.cpp`、`src/fluid_flow.cpp` |
| 流体验收与测试规格 |[流体证据](fluid-acceptance.md)、[测试规格](fluid-testing.md) |
| L-1/L-2a 光照层、光学元数据、方块光传播、高度图与后台交接 |[光照底座](light-engine.md)、`include/redstone/light.hpp`、`src/light.cpp`、`include/redstone/heightmap.hpp`、`src/heightmap.cpp`、`src/light_metadata.inc`、`src/light_occlusion.inc` |
| 原生协议入口 | `include/redstone/net/`、`src/net/`、[协议说明](protocol775.md) |
| 实体测试 | `tests/entity_tests.cpp` |
| 活塞/粘液块测试 | `tests/piston_tests.cpp` |
| 简单实体说明 |[entities.md](entities.md) |
| 兼容性/剩余差异 |[compatibility.md](compatibility.md) |
| 更新顺序审查 |[update-order-audit.md](update-order-audit.md) |
| 原版物品轨迹探针 | `validation/vanilla26_1/item_motion_validation.py`、`item-motion-validation.log` |

## 10. 当前恢复命令

```powershell
cmake -S . -B build-jemalloc -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static" `
  -DREDSTONE_ALLOCATOR=jemalloc -DREDSTONE_BUILD_VANILLA_SERVER=ON
cmake --build build-jemalloc --config Release
ctest --test-dir build-jemalloc -C Release --output-on-failure
```

命令只构建当前工作区；真实 26.1 服务端差分与性能对跑有独立 runner/证据目录，不能把这里的 CTest 结果当作原版等价或同阶段性能。

如果继续协议方向，先读：

- 上一级源码：`net/minecraft/network/protocol/{login,configuration,game}/*.java`
- `net/minecraft/network/protocol/LoginProtocols.java`
- `net/minecraft/network/protocol/configuration/ConfigurationProtocols.java`
- `net/minecraft/server/network/ServerLoginPacketListenerImpl.java`
- `net/minecraft/server/network/ServerConfigurationPacketListenerImpl.java`
- `net/minecraft/server/network/config/SynchronizeRegistriesTask.java`
- `net/minecraft/network/protocol/game/ClientboundLoginPacket.java`
- `net/minecraft/network/protocol/game/ClientboundLevelChunkWithLightPacket.java`
- `net/minecraft/world/level/chunk/storage/SerializableChunkData.java`

## 11. 历史未提交快照（2026-11-06）

当时的 `git status` 包含：

- 已修改：注册/引擎/活塞/形状/存储/客户端/CMake/文档/测试等。
- 新增未跟踪：`entity.hpp`、`entity.cpp`、`rail.hpp`、`rail.cpp`、`tests/entity_tests.cpp`、`entities.md`、8 张铁轨纹理、`validation/vanilla26_1/item_motion_validation.py` 和 `item-motion-validation.log`。

这是历史快照，不代表当前工作区。当前提交与任务状态只以 Git 和 `task/TASKS.md` 末行为准。
