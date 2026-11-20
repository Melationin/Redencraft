# 生物生成测试编写规划（Testing Plan，不变量 + 统计收敛口径）

> **定位**：与 [mob_spawning_plan.md](file:///C:/Users/zhdds/.gemini/antigravity/brain/d56070f8-3344-4c33-b10a-94fbf7859474/mob_spawning_plan.md)（S-0 ~ S-5）一一对应的测试编写指南。
> **核心口径（按你的要求调整）**：
> - **不要求与原版 26.1 逐格/逐随机步完全一致**，也**不固定随机序列**；
> - 测试重点验证两类属性：**硬不变量（绝不能违反的规则与边界）** + **宏观行为收敛（该刷的能刷出来、该停的能停住、并发/存盘/协议不炸）**。
> - **状态**：仅规划，未修改任何源码。

---

## 1. 测试设计三原则（不依赖固定 RNG 序列）

1. **硬不变量 100% 精确断言（零随机性）**
   - 直接测纯判定函数与硬边界，无论随机数怎么滚都必须 100% 成立：
     - 禁刷条件（`PEACEFUL`、`SPAWN_MOBS=false`、`blockLight > limit`、距玩家/出生点 `<= 24`、满上限、超势能预算、方块碰撞/危险方块、古城/试炼密室禁刷等）→ 跑 N 轮后**恒为 0 只**；
     - 必清条件（`PEACEFUL`、`> 128` 格 / `WATER_AMBIENT > 64` 格）→ 单 tick **100% 清除**；
     - 豁免条件（`PersistenceRequired`、骑乘、拴绳、`< 32` 格）→ **恒不被清除**且不计上限；
     - 空间与类型不变量（生成期坐标严格落在中心区块内、碰撞盒不与地形重叠、实体类型必属于该群系/结构的合法刷怪表）。
2. **随机行为用“大样本统计收敛”测，不赌单次、不锁种子**
   - 例如：暗室平台跑足够刻数（如 200~400 tick）或生成足够多草原区块（如 64~100 块），断言**有刷出（`> 0`）**、**未超上限（`<= cap`）**、**物种均来自合法表**；
   - 随机清除（`> 32` 格且 `noActionTime > 600`）同时放 200 只怪跑 1000 tick，断言至少清除了一部分（失败概率 $< 10^{-50}$），而 `< 32` 格对照组 0 只清除。
3. **微型隔离夹具 + 快速执行**
   - 单元测试使用内存构造的微型世界（石板暗室、特殊方块矩阵、人工注入结构包围盒、多玩家坐标夹具），整个刷怪测试集控制在 **几秒内跑完**。

---

## 2. 编号测试用例矩阵（对应 S-0 ~ S-5）

### 2.1 元数据与配置（`M-01 ~ M-04`，对应 S-0）

| 编号 | 测试点 | 核心断言 |
|---|---|---|
| **M-01** | `MobCategory` 8 分类表 | 核对 8 个类别的 `max` (`70/10/15/5/5/5/20/-1`)、`isFriendly`、`isPersistent`、`despawnDistance` (`WATER_AMBIENT=64`, 其余 `128`)、`noDespawnDistance=32` |
| **M-02** | 实体类型与群系/结构刷怪表解析 | 核对核心生物 `EntityType` 的 `category`、`canSpawnFarFromPlayer`、`allowedInPeaceful`、`fireImmune`；核对 `assets/worldgen26_1` 的 `plains`、`soul_sand_valley`（含 `spawn_costs`）、`mushroom_fields`、`swamp_hut`、`fortress`、`monument`、`ancient_city` 解析结果非空且结构/权重正确 |
| **M-03** | `GameRules` 与维度 `MonsterSettings` | 核对 `spawn_mobs` / `spawn_monsters` 读写与 `level.dat` 持久化；核对主世界 / 下界 / 末地的光照门槛参数 |
| **M-04** | 候选区块与玩家过滤 | 核对 `spawnableChunkCount`（Chebyshev `<=8` 含旁观者）与 `collectSpawningChunks`（XZ 欧氏 `<128` 排除旁观者，127.9 纳入 / 128.1 排除） |

### 2.2 `Mob` 基类、清除、持久化与协议（`E-01 ~ E-05`，对应 S-1）

| 编号 | 测试点 | 核心断言 |
|---|---|---|
| **E-01** | `checkDespawn` 硬规则与统计清除 | `PEACEFUL` 立即清敌对留和平；`>128`（`WATER_AMBIENT >64`）单 tick 必清；`<=32` 格 `noActionTime` 归零且 0 清除；200 只 `>32` 格且 `noActionTime>600` 跑 1000 tick 后有明显清除 |
| **E-02** | 持久化/骑乘/拴绳豁免 | `persistenceRequired` / `isPassenger` / `isLeashed` 在 `>128` 格绝不清除，且不计入刷怪上限 |
| **E-03** | `Mob` NBT 编解码往返 | 核心生物（动物/怪物/水生/下界/末地）编解码后坐标、朝向、生命值、`PersistenceRequired`、`NoAI`、UUID 完整一致；未支持生物返回显式状态不崩溃 |
| **E-04** | 含生物区块存盘-卸载-重载 | 生成 `Mob` → 撤票卸载写 `.mca`（确认不被 `managedChunkPersistenceState` 拒卸）→ 重载恢复数量与属性一致 |
| **E-05** | 协议 775 同步解帧 | 生成代表性动物与怪物并接入 `Session`：严格解帧验证 `add_entity`、`set_entity_data`（零 `ProtocolError`）、`rotate_head`、`remove_entities` |

### 2.3 `SpawnPlacements` 与 `mobsAt` 判定（`P-01 ~ P-07`，对应 S-2）

| 编号 | 测试点 | 核心断言（均为确定性纯判定，正例 + 负例） |
|---|---|---|
| **P-01** | 4 种 `PlacementType` | `ON_GROUND`（实心顶面/倒半砖通过；下半砖/玻璃/基岩/屏障/树叶拒绝；头顶非空拒绝）；`IN_WATER`（水中+上方非红石导体）；`IN_LAVA`（岩浆中）；`NO_RESTRICTIONS`；世界边界外拒刷 |
| **P-02** | 空位与危险方块 | 满碰撞、红石信号源、流体、铁轨/按钮等一律拒刷；火/岩浆块对普通怪拒刷、对 `fireImmune` 放行；仙人掌/甜浆果/粉雪/凋灵玫瑰拒刷 |
| **P-03** | 光照规则（主世界/下界/末地） | 主世界暗室 `blockLight=0, skyLight=0` 允许、`blockLight>=1` 恒拒、白天露天高天空光拒绝/雷暴允许；下界允许 `blockLight<=7`（按维度设定）；末地 `blockLight=0` 允许、`>=1` 拒刷 |
| **P-04** | 动物与水生规则 | `Animal`/`MushroomCow` 校验脚下方块标签与 `rawBrightness > 8`（`8` 拒、`9` 通）；`WaterAnimal`（`seaLevel-13..seaLevel`）、`Axolotl`、`GlowSquid`（深水且亮度 0）、`Guardian`、`Drowned` 边界正负例 |
| **P-05** | `Bat` 与 `Slime` | `Bat` 地表下低亮度通过、地表上拒绝；`Slime` 史莱姆区块 `y<40` 通过 / `y>=40` 拒绝、沼泽 `50<y<70` 边界检查 |
| **P-06** | 下界与末地生物 | `Ghast`（簇上限 1）、`Strider`（岩浆面上空气）、`Piglin`/`Hoglin`/`ZombifiedPiglin` 禁下界疣块、`Enderman` |
| **P-07** | `mobsAt` 结构覆盖优先级 | 下界要塞 `PIECE` 内或大盒+`nether_bricks` 返回要塞怪表；女巫小屋返回女巫/猫；海底神殿返回守卫者并清空美西螈；`ancient_city`/`trial_chambers` 全分类为空表禁刷；无结构覆盖时回退群系表 |

### 2.4 区块生成期生物生成 `SPAWN` 阶段（`G-01 ~ G-05`，对应 S-3）

| 编号 | 测试点 | 核心断言 |
|---|---|---|
| **G-01** | 开关门控 | `disableMobGeneration=true` 或 `SPAWN_MOBS=false` 时，`ProtoChunk::entities()` 恒为空 |
| **G-02** | 群系合法性与统计产出 | 生成一批主世界草原/森林区块（如 64 块）：至少产出若干 `CREATURE`（`> 0`），所有产出实体类型均属于对应群系 `CREATURE` 刷怪表 |
| **G-03** | 几何合法性与中心夹紧 | 每个生成期实体的 `(fx, fz)` 严格落在中心区块 `[xo + w, xo + 16 - w]` 内；实体 AABB 与周围地形无碰撞；`hasCeiling=true` 维度不刷在天花板顶面 |
| **G-04** | `WorldGenScheduler` 8 线程并发压测 | 8 线程并发生成 + 取消/重提压测（连续 10×）：0 崩溃、0 数据竞争、0 越界写邻居，所有产出实体均合法 |
| **G-05** | 物化与存盘往返 | 经 `World::installGeneratedProtoChunk` 与 `WorldChunkManager::installLoaded` 安装含初始生物的区块，验证实体进入 `World`、可被 `Session` 同步，卸载重开后不丢失 |

### 2.5 运行期自然刷怪 `NaturalSpawner`（`N-01 ~ N-07`，对应 S-4）

| 编号 | 测试点 | 核心断言 |
|---|---|---|
| **N-01** | 全局上限不越界 | 暗室平板持续 tick：怪物数量增长到 `max * chunks / 289` 后稳定停刷（**绝不超过单批合理余量**）；预置 `cap` 只普通怪后 0 新增；额外放持久化怪不影响上限 |
| **N-02** | 多人局域上限 `LocalMobCapCalculator` | 相距 200 格的两名玩家 A、B：A 周围满 70 只后 A 旁停刷，B 旁照常刷怪；仅旁观者所在区域 0 刷怪 |
| **N-03** | 势能电荷 `PotentialCalculator` | 灵魂沙峡谷参数下同点 `+INF` 拒绝、近距高密度压制新刷怪、拉开距离后恢复 |
| **N-04** | 400 tick 周期与难度/规则门控 | `CREATURE` 仅在 `gameTime % 400 == 0` 触发；`PEACEFUL` 或 `SPAWN_MONSTERS=false` 时 0 怪物刷出；`SPAWN_MOBS=false` 时全类别 0 刷出 |
| **N-05** | 24 格禁刷圈与边界 | 玩家 24 格内与世界出生点 24 格内放暗室平台跑 300 tick → **恒为 0 只**；移至 28 格处 → 正常刷出；非 `EntityTicking` 区块不刷怪 |
| **N-06** | `World::tick` 阶段序与红石/流体零回归 | 开启刷怪后既有红石与流体回归测试全部通过，且无玩家场景下刷怪零开销 |
| **N-07** | 与默认异步光照模式协作 | `asyncWorldGenLight=true` 下新生成区块未出光照前刷怪不崩溃；光照就绪后暗室刷怪、插火把后立刻停刷 |

---

## 3. CTest 目标拆分与性能门槛

| CTest 目标 | 文件 | 覆盖范围 | 预期耗时 |
|---|---|---|---|
| `mob_spawning_metadata` | `tests/mob_spawning_metadata_tests.cpp` | `M-01 ~ M-04` | `< 0.5 s` |
| `mob_spawning` | `tests/mob_spawning_tests.cpp` | `E-01..02`、`P-01..07`、`N-01..07` | `< 2.0 s` |
| `worldgen_spawn_stage` | `tests/worldgen_spawn_stage_tests.cpp` | `G-01 ~ G-05` | `< 4.0 s` |
| 既有目标扩展 | `entity_codec`、`world_chunk_manager`、`chunk_generation`、`light_protocol` | `E-03..05`、`G-05` | 增量 `< 1 s` |

**性能与稳定性门槛**：
- **无玩家离线场景**：直接跳过刷怪，**0 堆分配**，不拖慢红石/世吞基准。
- **有玩家在线场景（1 玩家 `vd=10`）**：复用候选区块列表与 `SpawnState` 容器，稳态单刻刷怪开销目标 **< 0.3 ms**。
- **世界生成吞吐**：`worldgen_throughput_benchmark --size 30 --threads 24` 开启 `SPAWN` 后吞吐降幅 **< 5%**。
- **并发压测**：`worldgen_spawn_stage` + `worldgen_scheduler` + `chunk_generation` 连续 **10×** 通过。
- **静态表校验（可选，免起服）**：可从本地 `server-26.1.jar` 一次性导出 `EntityType` 分类与 `SpawnPlacements` 静态表做对拍，避免手抄漏项；**不强求**跑官方服务端动态随机差分。
