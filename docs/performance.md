# 性能优化记录（2026-11-06 / 11-07）

本文件记录红石模拟核心与网络发送的优化过程、测量方法、当前开销分布和验证状态。
历史 ETW/WPR 采样见 [litematic/performance-profile.md](../validation/litematic/performance-profile.md)；
本文件给出的是带 PDB 的进程内符号化采样和可重复的无剖析基准。

2026-11-07 后续的纯服务端分配优化、已纳入仓库的基准工具和验证结果见第 10 节。
优化后的 CPU 采样与下一轮热点判断见第 11 节。
第 1–7 节保留上一轮测量，不能把不同机器负载下的耗时直接拼成性能提升比例。

## 1. 结论摘要

| 口径 | 优化前 | 上一轮结果 |
| --- | ---: | ---: |
| 投影 3000 tick，无剖析（`bench_tick600`，O2） | 129,507 ms / 173,004 ms（历史两次，波动大） | **26,921 / 27,124 ms**（两次，相差 0.8%） |
| 每 tick 平均 | ≈ 43–58 ms | **8.97–9.04 ms** |
| 每 tick 中位数 | — | **8.28 ms** |
| 投影 3000 tick，带 2 ms 采样器 | 110,392 ms，median 30.06 ms | 54,583 ms，median 12.25 ms（同一二进制另一次 68,752 ms） |
| 真实服务端 + 客户端，机器激活 | 旧记录 50–775 ms/tick | 约 10–30 ms/tick（5,187 个移动活塞、1,197 实体） |

- 采样器本身有明显开销（`SuspendThread` + `StackWalk64`），同一二进制的两次带采样运行相差可达 25%；**比较时以无剖析的中位数/平均值为准**。
- 无剖析两次终态计数一致：`entities=1197 scheduled=1197 moving_pistons=1995`；计数一致不构成完整状态等价证明。

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
- 后续增加了仓库内的 `benchmarks/tick_benchmark.cpp`，优先用第 10 节的 CMake 目标复现纯服务端测量。

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
- 活塞 ticker：按注册顺序快照与 weak_ptr 槽推进。当前推进前仍通过 `entityAt` 检查 map 身份；`live` 字段尚未由注册/移除路径维护以替代这次查询。此前本项把该优化写成已完成不准确，见第 11 节的实际采样与源码核对。
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

## 6. 上一轮开销分布（exclusive，历史带采样运行）

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
- 爆炸的 `interactWithBlocks` 仍是逐块 `setBlock` + 掉落，射线本身已按 lithium 优化；后续可以复用临时缓冲，但不能合并或重排更新/掉落回调。
- `getBlock`/`chunkAtCached` 合计约 18%：可把区块表换成开放寻址（无桶取模），或让热点路径直接持有子区块指针。
- 网络：实体跟踪仍是“视野 section 列 + sentChunks 过滤”，可精确到 `ChunkMap` 的 per-chunk tracking；同 tick 多包可合并为 bundle；多连接写侧可合并为单次 `select`。
- 模拟范围外仍未实现：随机刻、光照、流体刻、天气、完整实体/方块实体；见 [兼容性说明](compatibility.md)。

## 10. 纯服务端：减少分配与临时构造（2026-11-07）

### 改动与行为边界

- `BlockState::with`、方向解析和铁轨形状解析接受 `string_view`；核心比较属性时使用 `getView`。`has_block_entity` 这样的 16 字符属性名在 MSVC 下原本会为 `get` 的 `std::string` 入参分配堆内存，现在正常 schema 路径不再构造该字符串。属性值索引使用透明哈希查找，避免 `Property::indexOf` 重建字符串；活塞推动检查借用 `BlockMetadata`，避免复制其中的字符串。
- `PistonStructureResolver` 的待推动列表按 Java `PUSH_LIMIT=12` 使用 `array`，只暴露有效区间的 `span`。`moveBlocks` 借用解析结果，在栈上保存原始方块、待清空位置和推动通知状态。销毁列表仍动态分配，不假设粘性分支的销毁数也受 12 格限制；保持原有逆序销毁、逆序移动、清空源位置和邻居通知阶段。
- 移动活塞推动实体时借用缓存形状，直接将当前盒转换为 AABB，避免复制形状及构造整份 AABB 列表。形状缓存改为 `deque`，保证注册扩展状态、缓存增长时已经返回的引用仍有效。
- 普通支撑面、单盒铁轨支撑直接读取缓存形状并判断覆盖，空形状/单矩形免去划分坐标缓冲；复杂多盒仍走原来的覆盖算法，移动活塞仍读取当时的进度与 NOCLIP 状态。
- `Entity::move` 复用实体私有碰撞盒缓冲，每次查询先清空；在没有回调的收集和解析阶段使用。实体删除从反复 `vector::erase` 改为稳定压缩，保留存活实体注册顺序。实体刻与移动活塞刻的局部快照仍保留，以保护刻内注册、删除、重绑定及回调生命周期。

没有合并 shape update、neighborChanged、放置/移除回调、计划刻、方块事件或方块实体阶段。本轮未修改网络发送路径。

### 同批前后测量

基线是修改前 `c459cbb52b846f5f46d0b546657af92d10a37709` 的 Release 核心库和头文件；前后链接同一份新基准。手工链接前已核验旧头与该提交匹配（仅补充计数声明），避免派生实体字段的 ABI 偏移错误。机器为 i9-13980HX / Windows / MSVC Release `/O2 /MT` / jemalloc。

默认调度下交替测得：旧版 **58,672 / 44,707 ms**，新版 **45,039 / 55,251 ms**；波动太大，不能据此宣布稳定提速。随后只对各个新创建的基准进程设置 `High` 优先级、`ProcessorAffinity=4`（逻辑 CPU 2），交替跑旧→新→旧→新；没有改变系统全局电源/调度设置，也没有其他基准同时运行：

| 3000 tick，无计数/无指纹 | 修改前两次 | 修改后两次 |
| --- | ---: | ---: |
| tick 总墙钟时间 (ms) | 30,903.533 / 30,148.961 | 28,216.102 / 29,623.981 |
| 每 tick 中位数 (ms) | 9.413 / 9.225 | 8.381 / 8.787 |
| 每 tick p95 (ms) | 43.303 / 42.545 | 42.458 / 44.873 |
| 循环线程 CPU 时间 (ms) | 30,593.750 / 29,906.250 | 28,062.500 / 28,156.250 |

两次平均墙钟时间下降 **5.3%**，线程 CPU 时间下降 **7.1%**；p95 没有改善。CPU 时间通过 Windows `GetThreadTimes` 在循环前后读取，包含循环管理工作；带 `--fingerprint` 时也会包含诊断工作，不能拿其 CPU 时间与纯计时比较。这些数值只描述该投影与该配置，不代表所有结构或联网服务端的提升。

另用同一基准的计数版各跑一次 3000 tick（不带指纹），仅累计 tick 内的分配请求：

| 全局 C++ `new/new[]` | 修改前 | 修改后 | 减少 |
| --- | ---: | ---: | ---: |
| 成功分配次数 | 56,365,745 | 22,446,198 | **60.2%** |
| 累计请求字节 | 3,844,659,796 | 1,906,838,108 | **50.4%** |

原始数据、源码文件 SHA256 与检查点指纹见 [机器可读记录](../validation/performance/2026-11-07-server-allocations.json)。分配次数末尾会随哈希表探测/清理波动，不能把少量尾数差当作行为变化。

累计请求字节不能解释为进程内存下降；实体私有缓冲会保留自身的历史容量直到实体释放。这一轮计数版耗时不用于计算提速比例。

### 可复现基准

```powershell
# 使用现有 jemalloc 构建目录；不需要构建或启动客户端
cmake -S . -B build-symbols -DREDSTONE_BUILD_BENCHMARKS=ON
cmake --build build-symbols --config Release --target redstone_tick_benchmark
build-symbols/Release/redstone_tick_benchmark.exe '<litematic>' 3000

# 状态回归诊断（每 250 tick 及最终状态）
build-symbols/Release/redstone_tick_benchmark.exe '<litematic>' 3000 --fingerprint

# 分配计数使用独立构建目录；正常服务端没有计数开销
cmake -S . -B build-counted -G 'Visual Studio 18 2026' -A x64 `
  -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static" `
  -DREDSTONE_BUILD_CLIENT=OFF -DBUILD_TESTING=OFF `
  -DREDSTONE_BUILD_BENCHMARKS=ON -DREDSTONE_BENCHMARK_COUNT_ALLOCATIONS=ON
cmake --build build-counted --config Release --target redstone_tick_benchmark
build-counted/Release/redstone_tick_benchmark.exe '<litematic>' 3000
```

默认两个基准选项都关闭。输入必须在 `(2,101,3)` 有音符盒，否则报错；无网络、无实时节流。测量范围为 `World::tick()` 加 `world.log.clear()`，不含导入、强加载或输出。计数仅统计此范围内成功的全局 C++ `new/new[]` 请求次数与请求字节数，不是存活内存、峰值 RSS 或所有 C 库的 `malloc`。计数版使用独立 allocator 对象，仍通过 `allocator.hpp` 选择相同后端。

需复现同核口径时，可用 `System.Diagnostics.Process` 启动上述命令，设置 `UseShellExecute=false`、`CreateNoWindow=true`，随后对返回进程设置 `PriorityClass='High'`、`ProcessorAffinity=[IntPtr]4`。逐个等待退出后再跑下一版本；不要同时构建或采样。默认不设置优先级/亲和性的结果也应保留，以展示测量噪声。

指纹记录强加载区域方块状态、计划刻、实体的部分运行字段、移动活塞及待处理事件，另累计成功事件的 tick/顺序。指纹计算在 tick 计时之外，但事件观察会增加少量刻内工作；因此计时和分配计数都在不带 `--fingerprint` 时比较。指纹只用于定位回归，不覆盖完整世界状态，也不替代原版服务端差分。

### 验证

- 源码对照：本地 Java 26.1 `PistonStructureResolver` 的 12 格上限、`PistonBaseBlock.moveBlocks` 的状态快照/阶段、`PistonMovingBlockEntity` 的碰撞盒顺序。
- C++ 行为断言新增：全部注册 schema 状态的属性转移、非零偏移字符串视图和扩展属性保留；12 格推动与解析器复用、每个移动状态的预先快照；连续移动清除旧碰撞盒、刻内移除和存活顺序；六方向支撑面、多盒活塞头及缓存扩容后的引用稳定性。既有轨道支撑边界用例继续执行。
- Release/jemalloc 构建及 CTest **12/12 通过**，包括七个既有原版录制场景的 `safesave_order_parity`。这次重放的是已保存的录制数据。
- 与修改前 C++ 核心回放同一投影：每 250 tick 的 12 个检查点，方块/计划刻/实体/移动活塞/成功及待处理事件指纹全部一致；11,571 条实体记录逐行一致，包含位置、速度的浮点位模式和已采集的派生字段。终态均为 `entities=1197 scheduled=1197 moving_pistons=1995 events=0`。这仍只是该负载的抽样状态回归。
- 本轮没有运行真实 26.1 服务端逐刻差分，不能声称与原版等价。

## 11. 优化后的 CPU 采样（2026-11-07）

### 方法与数据质量

使用与第 10 节相同的 3000 tick 投影工作负载，只采样模拟循环；导入、强加载、激活和符号解析不在采样循环内。主线程固定逻辑 CPU 2，采样线程固定逻辑 CPU 3，进程优先级 `High`；未启动客户端或联网服务端。核心源码 SHA256 与第 10 节的优化后版本一致。

先尝试 WPR/ETW 的 `CPU -filemode`，被本机策略拒绝：`0xc5585011 Failed to enable the policy to profile system performance`。没有更改系统策略，未获得有效 ETW 采样；随后使用本地进程内采样器 `build-symbols/bench_profiled3.cpp`（`build_profiled3.bat` / `run-profile-after.ps1`）。

采样器每约 2 ms 执行 `SuspendThread`、`GetThreadContext` 和最多 24 帧 `StackWalk64`，结束后通过当前本地 PDB 解析函数与源码行。相对旧 `profiled2` 做了两项统计修正：

- exclusive 使用实际 `CONTEXT.Rip`，避免把首次展开得到的调用者当作叶子；inclusive 对每条栈中的函数去重，分母固定为栈数，而非所有帧出现次数。
- 采样地址暂存在栈上，恢复主线程后才写入堆容器；停止标志使用 atomic。DbgHelp 展开仍然需要挂起线程，仍有测量扰动。

运行两次，每次均为当前 Release 核心库：

| 口径 | 当前符号构建对应的增量链接 | 关闭增量链接的诊断复核 |
| --- | ---: | ---: |
| 栈样本数 | 13,044 | 13,287 |
| 带采样 tick 总时间 | 29,677 ms | 29,602 ms |
| 带采样 median / p95 | 8.83 / 43.69 ms | 8.70 / 44.47 ms |
| `World::tick` inclusive | 99.78% | 99.80% |

本地 `build-symbols` 的 Release 链接参数是 `/DEBUG:FULL`，生成的 VS 工程启用了增量链接，因此首轮仍有 `ILT+...` 跳板；第二轮显式使用 `/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF`，用于检查跳板对归属的影响，没有改动工程或服务端链接配置。两次随后各跑一次当前未插桩基准，分别为 **28,481 / 28,637 ms**。这些时间仅作测量扰动参考，不据此计算优化收益；也不把第二轮不同链接配置与控制组当作严格的性能对比。

### 热点

| 函数 | 首轮 self | 复核 self | 复核 inclusive |
| --- | ---: | ---: | ---: |
| `World::explode` | 20.38% | **21.10%** | 24.89% |
| `World::getBlock` | 11.46% | **12.09%** | 22.64% |
| `World::chunkAtCached` | 7.87% | **8.18%** | 8.18% |
| `World::setBlock` | 6.44% | **6.64%** | 26.32% |
| `World::Section::get` | 2.65% | 3.33% | 3.33% |
| `StateDefinition::value` | 3.53% | 3.24% | 5.95% |
| `pistonTickBlockEntities` | 3.08% | 3.24% | 31.15% |
| `StateDefinition::find` | 1.32% | 1.52% | 3.35% |
| `collectCollisionBoxes` | 1.50% | 1.56% | 7.55% |

self 各行互斥；inclusive 包含子调用、相互重叠，不能相加。编译器内联、尾调用和函数合并会影响符号归属，源码行也可能落在函数收尾位置，因此不能把一个高采样的 `return` 行本身解释为高成本操作。由于统计方法已修正，不把本表与第 6 节直接作逐项性能差分。

### 下一轮优先级（分析推断，尚未实现）

1. **方块访问和区块定位。** `getBlock + chunkAtCached + Section::get` 在复核中 self 合计 **23.61%**。`getBlock` 栈的主要直接调用者是 `getSignal`（28.26%）、更新队列 `runNext`（25.76%）、实体碰撞（17.29%），爆炸只占 7.11%；不能只优化爆炸内部的方块读取。优先补充区块缓存命中/未命中统计，评估访问局部性和子区块访问，不能仅凭 CPU 样本推定缓存未命中率。
2. **爆炸射线循环。** self 约 21%，采样集中在 `entity.cpp` 的逐步取整、同方块检查、缓存键与阻力访问（当前约 1553–1563 行）。后续应检查这些常数项；不合并或重排破坏、掉落、随机调用和邻居更新。此次未逐 tick 导出热点分类，因此没有证明它就是 p95 的唯一来源。
3. **活塞实体索引。** `entityAt` 与两种活塞关联表的 `find/_Eqrange` self 合计约 **6.9%**，插入/清理另计。当前 `pistonTickBlockEntities` 仍有 `entityAt(w, e->pos) != e` 的身份检查，而 `setPistonEntity/removePistonEntity` 未维护 `live`。可评估可靠的存活/代际标志或索引结构，但必须保留同槽重绑定、移除后重建延后一轮、注册顺序与快照生命周期。
4. **属性读取。** `StateDefinition::value/find` self 合计 **4.76%**；`value` inclusive 为 **5.95%**。`value` 的主要调用者为 `getSignal`（34.94%）、爆炸（12.28%）、活塞朝向（10.51%）。可评估预解析属性或属性句柄，同时保留导入扩展状态与缺失属性 fallback。`memcmp` 另有 2.13% self，但不能全部归到属性比较。

分配次数已显著降低；本次 `je_mallocx` inclusive 约 1.63%，其 self 约 0.57%，说明该入口不是最大的 CPU 热点。这个比例不能当作全部分配/释放成本，也不能用来推断剩余分配可忽略。

原始函数/调用者/源码行表见 [文本报告](../validation/performance/2026-11-07-server-profile.txt)，完整测量参数与机器可读结果见 [JSON](../validation/performance/2026-11-07-server-profile.json)。本次仅修改分析文档与记录；构建及 CTest **12/12 通过**。采样运行的终态计数均为 `entities=1197 scheduled=1197 moving_pistons=1995 events=0`，计数一致不是完整状态等价证明；未运行本轮真实 26.1 服务端差分。

## 12. 剩余优化空间核查（2026-11-07，仅分析）

本节保留核查时的候选状态；随后快速实现的子集见第 13 节。

### 查询计数补充

在忽略目录中复制当前 `engine.cpp`，只为 `chunkAtCached/getBlock` 添加计数，用这个临时对象覆盖静态库中的同名实现；生产源码与核心库未修改。导入、强加载和激活后清零计数，再跑相同投影 3000 tick，不启用指纹观察器。该运行只用于计数，不用插桩耗时计算提速。

| 指标 | 结果 |
| --- | ---: |
| `chunkAtCached` 调用 | 829,823,066 |
| 64 项直接映射缓存命中 | 827,465,430（**99.7159%**） |
| 进入区块哈希表 | 2,357,636（**0.2841%**） |
| `getBlock` 调用 | 723,717,136 |
| 连续请求落在同一 XZ 区块 | **91.6616%** |
| 连续请求落在同一子区块 | **87.9751%** |

局部性按连续两次 `getBlock` 请求的位置计算，包含其后可能被高度/加载检查拒绝的请求，因此不是某个尚未实现的子区块缓存的实测命中率。区块缓存统计还包含 `isLoaded` 等其他调用者。原始计数、实验方法和临时源码 SHA256 见 [访问探针记录](../validation/performance/2026-11-07-access-probe.json)。终态计数仍一致，但本次没有重新做完整状态差分。

这些结果收窄了第 11 节的优先级：哈希表只处理约千分之三的区块查询，更值得先检查缓存命中路径本身的重复工作，暂时不能把替换全局哈希表排在首位。

### 已定位的候选改动（均尚未落入核心）

1. **区块/子区块访问。** 可评估 `getBlock` 的上次子区块缓存或局部读取器，复用 `Chunk/Section` 指针，减少重复的 chunk key 哈希、section 定位与函数调用。必须读取当时的 section 内容，保留高度检查、空 section 创建、加载/卸载以及 World 移动后的指针有效性。不能跨更新回调缓存旧方块状态。另一个较小的候选是将 `chunkIndex` 改为 C++20 的算术 `v >> 4`；MSVC 的独立 noinline 实验中，当前表达式仍产生符号分支与负数修正，而移位只需 `sar`。这不等于已验证实际调用点的内联汇编或整体收益。
2. **爆炸缓存与固定射线方向。** 局部 `CachedBlock.state` 只有赋值没有读取。MSVC 布局实验显示，去掉它后条目由 24 B 缩至 16 B，512 项缓存由 **12 KiB 缩至 8 KiB**。此外，每次爆炸都重新遍历 16³ 外壳并计算同样的 **1,352** 个归一化步进向量，可按原 xx/yy/zz 顺序一次生成并复用。需保持浮点运算结果、逐射线随机调用次数与顺序、逐步阻力扣减；不能把整个爆炸约 21% 的 self 都视为可消除开销。缓存未命中时的 `getBlock(pos)` 与 `isLoaded(pos)` 还可评估共用一次区块查询，继续区分未加载位置和已加载空气。
3. **活塞生命周期与索引。** 在完整维护注册、替换、移除、清空后的存活/代际状态后，可尝试省掉 ticker 的每次 map 身份查询。两张同位置的关联表还存在重复查找和节点分配，可评估共用记录或直接关联 ticker 槽。现有同槽重绑定、移除后重建、刻内新增与完成顺序断言必须保持。约 6.9% 是整条相关查询链的 self 合计，不是只替换一个检查就能获得的提速。
4. **属性预解析。** 当前每次 `StateDefinition::value` 仍按名字查找，再通过 stride 和取模计算属性值。可让正常 schema 状态的朝向、powered、extended、waterlogged 等走按状态 ID 索引的小表，或使用已解析属性句柄；导入扩展状态和缺失值继续原路径。优先改善热点读取，保持 `BlockState` 本身的轻量布局。需额外验证未知属性值与不同调用者 fallback 的语义。
5. **移动碰撞形状临时容器。** `collectCollisionBoxes` 虽已复用最终盒缓冲，但移动活塞分支仍按方块调用返回 vector 的 `collisionShape(World&, ...)`。可增加填入调用方缓冲的接口复用中间形状容量；只做赋值/移动返回值不会避免被调用方最初的分配。该路径的优先级低于前几项。

建议先验证区块命中路径与爆炸缓存的局部改动，再处理活塞索引和属性访问。这里确认的是具体优化机会；真实提速仍需逐项修改后跑无剖析前后对照、行为断言和状态回归，不能由采样占比直接相加估算。

## 13. 快速实现与最小验证（2026-11-07）

按用户要求控制改动与验证范围，本轮实现区块读取和爆炸的局部优化；未同时重构活塞索引或属性布局。

- `chunkIndex` 使用 C++20 算术右移，保留负坐标向下划分。
- `getBlock` 缓存上次子区块的指针（包括空指针），命中时跳过 64 项区块缓存探测和 section 定位；仍读取最新 palette/索引和非空计数，不缓存旧方块状态。加载/卸载及新建 section 时使子区块缓存失效。World 移动构造/赋值时清空双方的读取缓存，避免跨世界悬垂或串读；原有 64 项区块缓存也一起清空。
- 爆炸删除未读取的 `CachedBlock.state`，用编译期断言固定条目为 16 B，512 项栈缓存从 12 KiB 降到 8 KiB。
- 按原 xx/yy/zz 顺序和同一浮点计算顺序，一次生成 1,352 个射线步进向量（约 31.7 KiB 静态表，无堆容器），后续爆炸复用；每条射线仍在原位置消耗一个随机 float，保留逐步阻力扣减、破坏与回调顺序。

仅运行一次 Release 构建、一次完整 CTest 和前后各一次同核 3000 tick benchmark；未跑采样器、分配计数或重复测量。构建关闭可选客户端，保留协议服务端及其回归测试。新增断言覆盖：缺失区块/section 后创建、原地 palette 修改、清空与恢复、卸载/重载、跨 section、负坐标与 int 极值、高度边界、World 移动及赋值、零强度爆炸的初次射线表生成和再次复用时的随机消耗。

| 3000 tick，同核 CPU 2 / High / jemalloc | 本轮前 | 本轮后 |
| --- | ---: | ---: |
| tick 总时间 | 25,382.202 ms | 24,954.298 ms |
| 每 tick 中位数 | 7.629 ms | 7.413 ms |
| 每 tick p95 | 38.741 ms | 37.439 ms |

本次总时间下降 **1.69%**；只有一组前后对照，差异较小，可能包含运行噪声，不宣称已经证实稳定提速。两次都启用相同的成功事件观察与每 250 tick 指纹；指纹计算在 tick 墙钟计时之外，循环 CPU 时间包含诊断工作，因此此处只比较 tick 墙钟时间。

CTest **12/12 通过**（4.85 s），前后 **12 个检查点的全部指纹一致**，终态均为 `entities=1197 scheduled=1197 moving_pistons=1995 events=0`。这属于 C++ 回归和该投影的抽样状态对照，未运行真实 26.1 服务端差分。源码依据仍为原有 Java 26.1 行为对照；本轮没有合并更新阶段。

数据与修改后源码 SHA256 见 [快速验证记录](../validation/performance/2026-11-07-quick-pass.json)。

## 14. 发包隔离、广播阶段与每秒耗时显示（2026-11-07）

本轮针对网络同步引起的主线程等待和复制，未重复运行纯模拟 3000 tick 基准或 CPU 采样。协议和验证详情见 [协议文档](protocol775.md)，机器可读结果见 [发包验证记录](../validation/performance/2026-11-07-network-sync.json)。

- 每连接使用有界 SPSC 批队列，主线程提交不获取写线程锁，不执行发送或延迟区块编码；后台按 FIFO 成帧并发送。连接注册锁只在 tick 外使用。慢连接的预算独立，积压只关闭该连接。
- 队列计费覆盖 payload 容量、deferred 快照及已经取出但尚未编码完成的批次，限制为 16 MiB、65536 包、4096 批。Session 暂存和已编码未发送数据各另限 16 MiB；这不是进程或每连接总内存上限，保留容量和编码临时内存另计。
- 写线程按连接轮转，每轮最多 64 包或约 2 ms，1 MiB 未发送水位暂停继续编码；单个区块编码不可中途抢占。已发送前缀达到条件后再压缩，诊断 IO 也移到写线程。阈值属于本地背压策略，未宣称复刻原版 Netty 配置。
- 主线程将区块快照中的 palette 映射为协议 ID、移动活塞 NBT 冻结为自有字节；后台只读不可变快照和协议状态标志。消除后台 `BlockState::fromId` 与主线程扩展状态注册之间的竞争。世界快照采集和普通包 payload 构造仍在主线程，并非零开销。
- 高频块变化、实体变化 payload 移交采用移动；块变化缓冲复用容量，section 发包去掉中间 entries vector，实体跟踪使用 `try_emplace`。
- 按 26.1 源码将实体跟踪广播放在区块变化广播之后、方块事件和实体运动之前；注册后提交 spawn，落地修正先于移除。普通块变化仍在广播阶段去重并读取当前状态，成功方块事件立即提交，网络异常不截断核心更新链。
- 聊天栏从每 tick 一条改为每秒最多一条 `Tick N: x.xxx ms (1s avg, M ticks)`，显示前 1 秒完成的 tick 平均耗时及样本数。卡顿恢复后不补发报告。测量仍覆盖 `World::tick` 和连接同步准备，排除显示自身、休眠及 tick 外 socket IO。

### 最小测量与验证

构建为 MSVC Release / jemalloc，关闭可选客户端。CTest **14/14 通过**，覆盖队列边界/并发、后台冻结快照、广播阶段/实体生命周期、网络失败隔离和每秒报告平均值。官方 26.1 `GameProtocols` codec 与真实 `LevelChunkSection.read` 验证合并后的 **294 个包通过**；这是编码验证，不是实际原版服务器运行差分。

无界面 TCP 背压测试：慢连接登录后停止读取并累计发送 89,088 条 ping 请求，触发 1 次输出队列积压断开；健康连接仍通过 ping、放置、破坏，收到 tick 20/40/61 报告；服务端正常完成 160 tick。队列满载和取出后仍计费另有确定性单元断言，集成测试的请求数会随 TCP/调度变化。

同一完整投影（410 region、70,111 blocks、133 entities），前后各一次 100 tick TCP 冒烟，均在 tick ≥20 时右键激活控制、tick ≥40 时移动视野；各收到 50 区块、卸载 25 区块，收到远端目标区块与预测 ACK：

| 服务端日志的 tick 内耗时 | 本轮前 | 本轮后 |
| --- | ---: | ---: |
| 100 tick 平均 | 2.687 ms | 2.588 ms |
| 最大值 | 13.198 ms | 12.207 ms |
| 收到的 play 包（截至退出条件） | 535 | 481 |

仅一组实时 TCP 冒烟，显示频率、广播阶段和结束时刻存在差异，不能把这组耗时差认定为稳定提速；包数也包含每秒报告替代逐 tick 报告的影响。未重新测量网络分配次数或做高分位延迟统计。本轮确认的是移除 tick 对写队列锁和 socket 的等待、背压隔离及已覆盖的顺序行为。

源码对照为本地 Java 26.1 `ServerChunkCache.tick/tickChunks/broadcastChangedChunks`、`ChunkMap.tick`、`ServerEntity.sendChanges` 和 `Entity.setRemoved`。TODO：当前实体移除观察点在实体清理/方块实体刻末；原版在 `discard/setRemoved` 内立即执行生命周期回调，同阶段内更精细的交错顺序尚未完全复刻。既有追踪距离、速度/元数据和其他协议 TODO 仍见协议文档。没有启动真实 Minecraft 客户端或运行本轮真实 26.1 服务端逐刻差分，不能声称原版等价。

## 15. 方块破坏射线遍历与爆炸工作计数（2026-11-08）

参考 Lithium 26.1 的 `world.explosions` 系列后，本轮只做两件低风险的事：把 `World::clip` 换成原版
`BlockGetter.traverseBlocks` 的方块步进，并给爆炸射线路径加只增不改的计数。两者都不改变模拟语义：
改前改后 1000 tick 的世界指纹完全一致（`blocks=144615 hash=a193750ee04acc4a`，`entities=1`）。

`clip` 是 `ServerExplosion.getSeenPercent` 唯一的方块查询入口（爆炸曝光度采样，每个受伤实体最多 27 条短射线）。
旧实现取线段包围盒后三重循环逐格判定，访问量是 O(体积)；新实现按 Amanatides-Woo 步进，只访问线段真正穿过的
格子，并沿用原版 `Mth.lerp(-1e-7, …)` 把两个端点向外扩 1e-7，使落在格子边界上的端点所在格子仍被访问。
`rayBox` 判定未改，因此每次调用的结论仍与包围盒扫描一致（新增测试对比 20,000 条随机线段）。

同一负载（1000 tick，26,386 次爆炸）：

| 项 | 改前（包围盒扫描） | 改后（步进） |
| --- | ---: | ---: |
| `clip` 调用 | 726,601 | 726,601 |
| 访问格子总数 | 25,748,122 | 7,017,967 |
| 每次调用平均格子 | 35.4 | 9.65 |

`clip` 内部少 3.67 倍探测，但总访问量只占每 tick 约 7,000 格，端到端不可测：交替各跑 3 次 1000 tick，
平均每 tick **21.35 ms（改前）与 21.29 ms（改后）**，差异小于该机器 ±5% 的漂移。保留这项改动是因为它更贴近
原版行为（包围盒扫描本身是偏差）且无额外代价；不宣称带来提速。

爆炸射线计数（`World::explosionStats`，同上负载）：

| 项 | 合计 | 每次爆炸 |
| --- | ---: | ---: |
| 爆炸次数 | 26,386 | 1 |
| 直接映射缓存未命中（读方块） | 34,924,667 | 1,324 |
| 破坏列表原始条目 | 7,371 | 0.28 |
| `sort` + `unique` 后 | 4,181 | 0.16 |

结论：这份负载里 TNT 在空气中起爆，破坏列表平均不到 1 项，`sort`/`unique` 的开销可以忽略；把去重前移到射线
循环（Lithium 的 `LongOpenHashSet`）没有收益，本轮不做。每次爆炸真正花钱的是 1,324 次方块探测，比原版
逐射线逐步读取低一个数量级（射线步数约为探测数的十几倍），继续下降需要跳过整块空气 section 一类结构性改动。

未照搬 Lithium 的两项及原因：`skip_air` 会把空气移出破坏列表，从而改变 `Util.shuffle` 抽取随机数的次数，
让后续随机流与原版分叉，本轮不采用；`no_air_counting` 依赖“附近是否有玩家决定发包里的 blockCount”，
本模拟器没有玩家池与发包，不适用。

验证：CTest 13/14，新增 `entity_tests.clipTraversal` 覆盖薄方块、部分高度方块和端点落在格边界的情形，并用
20,000 条随机线段对比包围盒穷举扫描与步进实现。失败项 `client_inventory` 属于物品映射表断言，与本轮改动
无交集，本轮未处理。以上是源码对照与 C++ 回归，未运行真实 26.1 服务端差分。

## 16. 爆炸空气过滤器替代方块缓存（2026-11-08）

第 15 节测得每次爆炸 1,324 次方块探测，其中绝大多数落在空气上，而空气的结论只有一条：不扣阻力、不产生流体
效果、不进破坏列表。本轮据此把 512 项、每项 24 B 的方块缓存换成"只记空气"的过滤器。

**先做的纯位图版本不成立。** 512 位、无 tag 的直接映射位图（64 B，一条缓存行）跑同一负载 1000 tick 后世界
指纹就变了：

```
改前 blocks=144615 blocks_hash=a193750ee04acc4a entities=1
纯位图 blocks=145029 blocks_hash=5dbf34b1457593c2 entities=174
```

原因是一位过滤器无法区分"这个格子是空气"和"另一个位置哈希到了同一个槽"。一次半径 4 的 TNT 会覆盖约 1,000
个不同格子而只有 512 个槽，碰撞必然发生：实心方块被当成空气，射线直接穿过去，TNT 没被炸掉，于是后续刻的
实体数和方块数全变。**这个版本没有提交。**

**提交的版本**保留位图，但给每个槽加 8 B 的打包位置 tag，命中要求 `bit && tag == key`，共 4,160 B（旧表
12,288 B）。tag 不需要每爆炸清零：值位才是"槽有效"的标志，而写位时总是和 tag 一起写。

| 项 | 旧方块缓存 | 空气过滤器 |
| --- | ---: | ---: |
| 成员内存 | 12,288 B | 4,160 B |
| 1000 tick 探测次数 | 34,924,667 | 34,926,530 |
| 空气命中/1000 tick | — | 243,673,966 |
| 世界指纹 | `a193750ee04acc4a` | `a193750ee04acc4a`（一致） |

探测次数没有下降：命中率由槽数（512）与每次爆炸不同格子数（约 1,000）决定，换表示法不改变这一点。交替
各跑 3 次 1000 tick，每 tick 平均 21.71 ms（旧）与 21.93 ms（新），差异在噪声内。真正省下来的是 8 KB
成员内存和每槽的 12 B 状态。

射线循环改为自己维护单条 chunk/section 缓存（Lithium 的 `prevChunkX/prevChunkZ/prevChunk` 做法），
不再走 `getBlock` 里那条被所有调用方共享的可变 section 缓存。

**代价**：非空气格子不再缓存阻力，每次进入都要重读方块状态。这份负载里实心格子几乎不重复出现，代价为零；
但爆炸完全埋在低阻力方块里时（`entity_tests.denseExplosion`：17³ 沙子填满、射线球覆盖约 1,400 个格子，
其中 180 个被炸掉），同一个实心格子会被射线反复进入，每次都要重读状态，而旧表只需第一次读。该测试用破坏
数量的区间断言守住过滤器的正确性：位图一旦把实心误判成空气，破坏数会掉到个位数。

综上：本轮拿到的收益是内存与状态简化，不是耗时。若后续要降耗时，瓶颈不在探测次数——每 tick 约 27.9 万次
射线-格子进入（`airHits + probes`）的循环体本身才是 `World::explode` 的主要开销。
