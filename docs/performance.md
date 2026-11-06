# 性能优化记录（2026-11-06 / 11-07）

本文件记录红石模拟核心与网络发送的优化过程、测量方法、当前开销分布和验证状态。
历史 ETW/WPR 采样见 [litematic/performance-profile.md](../validation/litematic/performance-profile.md)；
本文件给出的是带 PDB 的进程内符号化采样和可重复的无剖析基准。

## 1. 结论摘要

| 口径 | 优化前 | 现在 |
| --- | ---: | ---: |
| 投影 3000 tick，无剖析（`bench_tick600`，O2） | 129,507 ms / 173,004 ms（历史两次，波动大） | **26,921 / 27,124 ms**（两次，相差 0.8%） |
| 每 tick 平均 | ≈ 43–58 ms | **8.97–9.04 ms** |
| 每 tick 中位数 | — | **8.28 ms** |
| 投影 3000 tick，带 2 ms 采样器 | 110,392 ms，median 30.06 ms | 54,583 ms，median 12.25 ms（同一二进制另一次 68,752 ms） |
| 真实服务端 + 客户端，机器激活 | 旧记录 50–775 ms/tick | 约 10–30 ms/tick（5,187 个移动活塞、1,197 实体） |

- 采样器本身有明显开销（`SuspendThread` + `StackWalk64`），同一二进制的两次带采样运行相差可达 25%；**比较时以无剖析的中位数/平均值为准**。
- 无剖析两次终态一致：`entities=1197 scheduled=1197 moving_pistons=1995`，说明优化没有改变模拟结果。

## 2. 参考源码目录

| 用途 | 路径 |
| --- | --- |
| 原版 26.1 Java 源码（只读的行为依据） | `F:\source\minecraft-merged-07da9a845a-26.1-sources` |
| Lithium 26.1.x（爆炸/实体/区块优化参考） | `F:\source\lithium-26.1.x` |
| 本工程 | `F:\source\minecraft-merged-07da9a845a-26.1-sources\redstone-cpp` |
| 协议 775 导出数据 | `assets/protocol775/`（packet_ids / block_states / state_flags / registry_ids 等） |
| 原版校验材料 | `validation/vanilla26_1/`、`validation/safesave_order/`、`validation/protocol775/` |

原版关键文件：

- 方块与状态：`net/minecraft/world/level/block/Block.java`、`Blocks.java`、`state/StateDefinition.java`、`state/StateHolder.java`、`state/BlockBehaviour.java`
- 区块与更新：`server/level/ChunkHolder.java`、`server/level/ChunkMap.java`、`server/level/ServerChunkCache.java`、`world/level/Level.java`
- 计划刻：`server/level/LevelTicks.java`、`LevelChunkTicks.java`、`world/ticks/ScheduledTick.java`
- 实体跟踪：`server/level/ServerEntity.java`、`network/protocol/game/ClientboundMoveEntityPacket.java`、`ClientboundEntityPositionSyncPacket.java`、`VecDeltaCodec.java`
- 方块批包：`network/protocol/game/ClientboundSectionBlocksUpdatePacket.java`、`core/SectionPos.java`
- 爆炸：`world/level/ServerExplosion.java`（lithium 侧见 `common/src/main/java/net/caffeinemc/mods/lithium/mixin/world/explosions/block_raycast/ServerExplosionMixin.java`）

## 3. 工作负载与测量方法

统一工作负载：投影 `[再灰二度] 无沟V3.1双倍速防雪世吞-997.生成导出-800x83x800.litematic`，
导入 70,111 方块、133 实体、2,916 个强加载区块；在第一个 tick 之前点击一次音符盒（`note 0 -> 1`，无方块事件），
然后全速跑 3,000 个 `World::tick()`（无网络、无实时节流）。

- 无剖析：`build-symbols/bench_tick600.cpp` → `build_tick600.bat` → `tickcost.exe`（`/O2 /MT`，链接 Release 核心库）。
- 带采样：`build-symbols/bench_profiled2.cpp` → `build_profiled2.bat` → `profiled2.exe`，每 2 ms `SuspendThread` 主线程、`StackWalk64` 取 24 帧，输出 exclusive/inclusive 热点与每 tick 时间分布。
- 这两个 harness 位于被忽略的 `build-symbols/`，是本地测量工具，不随仓库提交；复现方式见第 8 节。

机器：Windows / MSVC Release / jemalloc。同一二进制重复运行的中位数波动见上表。

## 4. 优化阶段与结果（带采样、3000 tick）

| 阶段 | wall (ms) | mean | median | p95 | 采样栈数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 起点 | 110,392 | 36.80 | 30.06 | 114.69 | 46,568 |
| Block/Blocks 单例 + 指针身份 | 74,921 | 24.97 | 22.23 | 100.50 | 30,635 |
| `named()` 全改指针比较 + 常数项 | 69,118 | 23.04 | 21.38 | 94.63 | 28,208 |
| 64 项区块查找缓存 | 62,341 | 20.78 | 12.14 | 74.41 | 26,229 |
| Lithium 式爆炸 | **54,583** | **18.19** | **12.25** | 56.07 | 23,279 |

同二进制第二次带采样运行：68,752 ms（mean 22.92 / median 20.14）——采样器与机器负载的噪声约占 25%。

无剖析最终值：**27,124 / 26,921 ms**，mean 9.04 / 8.97 ms，median 8.32 / 8.28 ms，
p90 12.27 / 12.23 ms，p95 37.78 / 37.67 ms，p99 39.62 / 39.59 ms，max 74.35 / 67.60 ms。

## 5. 逐项改动

### 5.1 方块状态：`Block` 单例与指针身份

对应原版 `Blocks` 的静态 Block 字段 + `BlockStateBase.getBlock()`：

- 新增 [include/redstone/block.hpp](../include/redstone/block.hpp)：`Block`、`Blocks::*`、`BlockState`（`const Block *block_` + 32 位注册 ID）、`StateDefinition`/`Property`、`BlockMetadata`。
- 新增 [src/blocks.cpp](../src/blocks.cpp)：59 个进程级单例；53 个注册方块 + 6 个仅保留身份的原版方块（snow/snow_block/powder_snow/crying_obsidian/respawn_anchor/reinforced_deepslate）。声明顺序即注册顺序，`minecraft:air` 为状态 ID 0。
- [src/state.cpp](../src/state.cpp)：注册表改为稠密状态表（`gStateData/gStateCount`），`BlockState::fromId` 是一次 clamp 索引 load；扩展状态（导入的越界属性）仍单独登记。
- 108 处 `named(state, "xxx")` 改为 `state.is(Blocks::Xxx)`（一次指针比较）；空气三元判断改 `isAir()`；`sameRailType` 改 `sameBlock()`；客户端/协议里的方块名字比较同步替换。
- `ScheduledTick.block`、`BlockEvent.block`、`Update::source` 改为 `BlockRef`（8 字节 Block 指针），`TickKey` 直接哈希块指针，计划刻路径不再有名字。
- `blockMetadata` 返回 `const BlockMetadata&`；扩展活塞基座的导体例外由 `isRedstoneConductor` 承担。

### 5.2 区块存储与查询

- 子区块 palette + 索引（0/8/16 位），区块表哈希；读方块 = 一次区块查找 + 一次数组取 palette。
- 修复过的点：`airState()` 现在绑定注册的空气状态（`block_` 非空），避免与 `fromId(0)` 身份不一致。
- 64 项直接映射区块缓存，按 `chunkKey` 哈希取槽；`loadChunk` 主动发布节点，`unload` 只翻 loaded 位，因此缓存指针稳定。
- `World::snapshotChunk` 复制子区块 palette + 索引与移动活塞快照，供网络线程编码。

### 5.3 更新队列、计划刻、方块事件

这些在更早一轮完成，当前结构仍按原版：`LevelTicks`/`LevelChunkTicks` 的每区块堆 + `TickKeySet` 去重、`nextTickForContainer` 索引、`containersToTick` 堆、先收后跑；方块事件是 `deque` + 插入序哈希集合；`subTickOrder` 来自 `Level.nextSubTickCount`。详见 [兼容性说明](compatibility.md)。

### 5.4 实体

- `EntitySectionStorage`：16³ section 桶，查询按 XZ +2、Y −4 的 CHONKY 搜索余量，结果按 entity id 排序。
- 活塞 ticker：`PistonMovingBlockEntity.live` 标志，ticker 清理不再对每个实体做一次 map 查找。
- 网络侧 `entitiesInSectionRange` 只扫玩家视野内的 section 列。

### 5.5 爆炸（按 Lithium）

[src/entity.cpp](../src/entity.cpp) 的 `World::explode`：

- 512 项直接映射缓存（打包位置 → 状态、空气/流体标志、每步阻力 `(阻力+0.3)*0.3`、是否加载）；
- 同一方块步复用（`prevX/prevY/prevZ`），跳过缓存探测；
- 每条射线先算好 0.3 步进向量，循环内只做加法；
- 强度只降不升，同一方块只在首次进入时记录（与之前 sort+unique 的集合等价）；
- 空气不进入破坏列表（无火焰/计数需求）；
- 内联 `floorToInt` 取代 libm `floor`；保留 0.225 常量衰减与每步先扣阻力再判定破坏的顺序。

### 5.6 网络发送（服务端 → 客户端）

| 内容 | 原版依据 | 实现 |
| --- | --- | --- |
| 方块变化批处理 | `ChunkHolder.broadcastChanges` | section 内 1 个变化发 `block_update`；多个合并为 `section_blocks_update`（`SectionPos` long + VarInt 数量 + 每条 `stateId << 12` 按位或 12 位局部坐标的 VarLong） |
| 实体增量 | `ServerEntity.sendChanges` | `VecDeltaCodec` 1/4096 位移、1/256 旋转，超 short/着地变化/400 刻 → `entity_position_sync`；元数据只在 spawn |
| 更新间隔 | `EntityType.updateInterval` | 物品/下落方块 20、TNT 10、矿车 3 |
| 发送线程 | Netty event loop | 主线程只采集世界状态并入队；`NetworkWriter` 线程成帧、编码区块、合并缓冲、写侧 `select`、`send` |
| 查表 | `Block.BLOCK_STATE_REGISTRY` | `BlockState::Id` → 协议 ID、协议 ID → 状态标志都是稠密数组 |

细节与协议字段见 [协议说明](protocol775.md)。

## 6. 当前开销分布（exclusive，最后一次带采样运行）

| 函数 | 占比 | 说明 |
| --- | ---: | --- |
| `World::explode` | 18.35% | 射线循环本身；破坏阶段（逐块 setBlock/掉落）另计 |
| `World::getBlock` | 11.53% | 区块查找 + 子区块 palette |
| `World::chunkAtCached` | 6.62% | 64 项缓存未命中时仍需哈希探测 |
| `World::getSignal` | ≈3.8% | 活塞/导轨每刻的多方向信号探测 |
| `pistonTickBlockEntities` | ≈2.4% | 移动活塞推进 |
| `StateDefinition::value/find` + `memcmp` | ≈6% | 属性名二分查找与字符串比较（`facing`/`shape`/`type` 等） |
| `Section::get` / 碰撞盒收集 / `shapeUpdate` | 各 ≈2% | 方块读取与形状更新 |

已经消失的热点：`stateNamed`（原 10.26%）、`registry()` 守卫（5.13%）、`BlockState::fromId`（3.61%）、
`blockMetadata`（2.36%）、`findBlockByName`（2.27%）、每刻 2,400 条实体包。

## 7. 验证

- `ctest -C Release` **12/12**：wire、protocol、protocol_socket、safesave_order_parity、engine、entity、piston、storage、litematic、allocator、shapes、client_inventory。
- `safesave_order_parity`：7 个原版录制用例逐字段一致（计划刻/去重/过期/负 order/方块事件/移动实体/完成顺序）。
- `wire`：区块字节与 Java 26.1 codec 导出的 `empty_section.bin` 逐字节一致；移动活塞 NBT 解析比对；palette/高度图/光照编码边界。
- `protocol`：新的 `section_blocks_update` 与实体增量断言；`protocol_socket` 启动真实 C++ 服务端，完成状态、离线登录、30 个配置包、区块、放置、破坏、预测 ACK。
- 投影真实服务端：加载 2,916 区块、200 tick 冒烟通过；客户端连接后的实测 tick 约 10–30 ms（5,187 移动活塞、1,197 实体），空闲阶段 0.2–0.6 ms。
- **未做**：本轮的 26.1 服务端逐刻差分。因此只能声称 C++ 回归测试与编码验证通过，不能声称与原版逐位等价。

## 8. 复现命令

```powershell
# 构建 + 全部测试
cmake -S . -B build-symbols -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static" -DREDSTONE_BUILD_VANILLA_SERVER=ON
cmake --build build-symbols --config Release
ctest --test-dir build-symbols -C Release --output-on-failure

# 无剖析 3000 tick（本地 harness）
cmd /c "build-symbols\build_tick600.bat"
build-symbols\tickcost.exe "<litematic>" 3000

# 带 2 ms 采样（本地 harness，需 PDB）
cmd /c "build-symbols\build_profiled2.bat"
build-symbols\profiled2.exe "<litematic>" 3000

# 真实服务端 + 投影
build-symbols\Release\redstone_vanilla_server.exe --schematic "<litematic>"
```

## 9. 已知限制与后续

- 属性读取仍按属性名二分查找（`StateDefinition::find/value`）并做字符串比较，约占 6%；可缓存每个状态的解码值或引入属性句柄。
- 爆炸的 `interactWithBlocks` 仍是逐块 `setBlock` + 掉落，射线本身已按 lithium 优化；下一步可合并该阶段的更新/掉落。
- `getBlock`/`chunkAtCached` 合计约 18%：可把区块表换成开放寻址（无桶取模），或让热点路径直接持有子区块指针。
- 网络：实体跟踪仍是“视野 section 列 + sentChunks 过滤”，可精确到 `ChunkMap` 的 per-chunk tracking；同 tick 多包可合并为 bundle；多连接写侧可合并为单次 `select`。
- 模拟范围外仍未实现：随机刻、光照、流体刻、天气、完整实体/方块实体；见 [兼容性说明](compatibility.md)。
