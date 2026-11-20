# 哈希表：自研实现与使用清单

本文件记录当前代码里所有哈希表实现的内部结构、扩容与删除策略，以及各处容器的访问模式（读多写少 / 读写均衡 / 写后即弃）。它描述的是**已经存在的代码**，不是目标形态；缺口见 [PLAN.md](../PLAN.md) 与 [AGENTS.md](../AGENTS.md) §1。

## 1. 为什么自研

`std::unordered_map` 每个条目一次堆分配，查找要沿桶链做一次指针追踪。这个项目里有三处热路径不能接受：

- **光照传播**：每个方块格都要查一次"该格属于哪个光学类 / 该段是否有光"。
- **计划刻**：每刻大量插入、撤销、查重，逐节点分配直接压在方块刻预算上。
- **区块键查表**：每个方块读写都可能走 `World::chunks_`（被 64 槽直映缓存挡掉大部分，未命中时仍是哈希探针）。

所以除了标准库，还有 4 套自研容器，合计 5 套实现。

## 2. 总览

| 实现 | 定义位置 | 槽位结构 | 探测 | 删除 | 扩容阈值 | 缩容 | 用在哪 |
|---|---|---|---|---|---|---|---|
| `std::unordered_map` / `unordered_set` | 标准库（链地址法） | 节点 + 桶数组 | 链 | 摘链 | 100% 负载 | 无 | 注册表、`World::chunks_`、调度器、实体、测试 |
| `redstone::FlatHashMap` | [flat_hash_map.hpp:37](../include/redstone/flat_hash_map.hpp#L37) | `hash(8B) + pair + state(1B)` | 线性 | 墓碑 | 总占用 75% | 无（`clear()` 释放） | 方块实体表、POI、世界生成 memo |
| `light_internal::FlatSet/FlatMap` | [light/common.hpp:108](../src/light/common.hpp#L108) | `states_(1B/槽) + slots_` 双数组 | 线性 + H2 标签 | 墓碑 | 总占用 70% | 仅清墓碑 | 光照引擎全部热表 |
| `FlatChunkMap<V>` | [internal.hpp:192](../src/engine/internal.hpp#L192) | `keys_(8B) + values + state(1B)` | 线性 | 墓碑 | 总占用 66.7% | 无 | 计划刻容器、下次触发时间 |
| `TickKeySet<T>` | [internal.hpp:87](../src/engine/internal.hpp#L87) | `keys(16B) + state(1B)` | 线性 | 墓碑 | 总占用 66.7% | 有，手动 | 计划刻去重、本刻待执行集 |

四套自研实现的共同前提：**键类型必须可平凡复制**（`u64`、指针、`Pos`、`SectionPos3`），值类型要么平凡要么支持默认构造 + 移动赋值。

## 3. `redstone::FlatHashMap`

对应原版 fastutil 的 `Object2ObjectOpenHashMap`，最早用于 `LevelChunk.blockEntities` 与 `tickersInLevel`（[注释](../include/redstone/flat_hash_map.hpp#L30-L35)）。

### 槽位与探测

~~~cpp
struct Slot {
  std::uint64_t hash = 0;      // 完整 64 位哈希，参与比较
  value_type entry{};          // pair<Key, T>
  State state = State::Empty;  // Empty / Used / Deleted
};
~~~

- 探测序列：`index = hash & mask`，冲突时 `(index + 1) & mask`，容量恒为 2 的幂。
- 比较时**先比 `hash` 再比 `equal_`**（[:225](../include/redstone/flat_hash_map.hpp#L225)），大多数冲突只需一次整数比较。

### 插入

1. 若总占用 `(size_ + tombstones_ + 1) * 4 > slots_.size() * 3` 触发 `rehash`（[:237](../include/redstone/flat_hash_map.hpp#L237)）。
2. 新容量从 8 起向上取 2 的幂，直到 `capacity >= 2 * (size_ + 1)`（[:238-241](../include/redstone/flat_hash_map.hpp#L238-L241)）。因此**重哈希后的有效负载约 50%**，探测链短。
3. 探测到第一个 `Empty` 时优先回填探测链上遇到的第一个 `Deleted` 槽（`firstDeleted`，[:245-263](../include/redstone/flat_hash_map.hpp#L245-L263)），顺带把墓碑计数减一。

### 删除与墓碑

- `erase(key)` 只把状态置 `Deleted`、值置 `T{}`，键与哈希留在槽里（[:190-200](../include/redstone/flat_hash_map.hpp#L190-L200)）。
- `findIndex` **只有遇到 `Empty` 才终止**（[:221-229](../include/redstone/flat_hash_map.hpp#L221-L229)），所以删除不会截断探测链。
- 墓碑没有上限，只能靠下一次 `rehash` 回收；`clear()` 直接 `slots_.clear()` **丢掉容量**。

### 与标准库不同的两个语义

- `operator[]` 对**已存在的键绝不触发重哈希**（[:179-187](../include/redstone/flat_hash_map.hpp#L179-L187)），照抄 fastutil `put` 的行为。
- 支持**透明查找**：`Hash` 与 `KeyEqual` 都带 `is_transparent` 时，可以用 `std::string_view` 查 `std::string` 键（`kTransparentLookup`，[:44-51](../include/redstone/flat_hash_map.hpp#L44-L51)）。`TransparentStringHash` 是 FNV-1a 加一次 64 位混合（[:15-28](../include/redstone/flat_hash_map.hpp#L15-L28)）。

### 使用注意

- 迭代顺序未指定；需要对外输出有序列表的调用方（区块快照）必须自行排序——头文件注释明确写了这一点。
- 任何触发扩容的插入都会让所有迭代器与槽位地址失效。

## 4. `light_internal::FlatSet` / `FlatMap`

SwissTable 风格的开放寻址表（[注释](../src/light/common.hpp#L106-L107)），光照模块专用。同一套基类 `FlatTable` 用 `FlatSetSlot` / `FlatMapSlot` 两个槽位类型特化出集合与映射。

### 双数组布局

~~~cpp
std::vector<std::uint8_t> states_; // 0 = 空, 1 = 墓碑, >= 0x80 = H2 标签
std::vector<Slot> slots_;
std::size_t size_ = 0, tombstones_ = 0;
std::uint64_t rehashEpoch_ = 1;
~~~

- H2 标签取哈希高 7 位再置最高位：`0x80 | ((hash >> 57) & 0x7F)`（[:114-116](../src/light/common.hpp#L114-L116)），与 `kEmpty = 0`、`kTombstone = 1` 天然区分。
- 探测时**先读 1 字节标签**，标签不等直接跳下一槽；只有标签相等才比较键（[:272-278](../src/light/common.hpp#L272-L278)）。一次探测几乎只碰一行状态字节，不必触碰键值数组。

### 扩容

- 阈值：`(size_ + tombstones_ + 1) * 10 >= states_.size() * 7`，即**总占用 70%**（[:237](../src/light/common.hpp#L237)）。
- 新容量：若 `size_ * 2 < states_.size()`，**保持原容量只清墓碑**；否则翻倍（[:238](../src/light/common.hpp#L238)）。空表首次插入固定 16 槽。
- `reserve(n)` 按 `bit_ceil(n * 10 / 7 + 2)` 预分配，最小 16（[:186-191](../src/light/common.hpp#L186-L191)）。

### `rehashEpoch_`

每次 `rehash` 递增（[:308](../src/light/common.hpp#L308)），对外暴露 `rehashEpoch()`。这是给**外部 L1 指针缓存**用的：光照的区段缓存直接存 `Slot*`，靠比较 epoch 判断指针是否还指向同一个槽，从而避免每格查表都做完整查找。这也是四套实现里唯一"能被外部缓存感知重哈希"的设计。

### `clear()` 与迭代器

- `clear()` 只 `memset` 状态数组，**保留容量**（[:193-206](../src/light/common.hpp#L193-L206)），适合每批复用。
- **非平凡析构**的槽位在 `clear` / `erase` 时会被显式重置为空槽（`if constexpr (!std::is_trivially_destructible_v<Slot>)`），避免泄漏持有资源的条目。
- `erase(iterator)` 返回下一个元素的迭代器（[:225-231](../src/light/common.hpp#L225-L231)），语义接近 `std::unordered_map::erase`，可以直接在遍历里删。
- 不支持透明查找；键类型必须精确匹配（所以 `noise_router.hpp` 用 `string_view` 当键，而不是 `string`）。

### 哈希函数

`UInt64Hash` 用于全部区块键，`LightPosHash` / `SectionPos3Hash` 用于坐标（[common.hpp:414-442](../src/light/common.hpp#L414-L442)），都是"乘法混合 + 右移异或"的定式，用一条乘法链打散低位。

## 5. `FlatChunkMap<V>`

对应原版 `LevelTicks` 里的 fastutil `Long2ObjectOpenHashMap` / `Long2LongOpenHashMap`（[注释](../src/engine/internal.hpp#L190-L191)）。引擎每刻都要按区块键找计划刻容器与下次触发时间，键固定是 64 位打包区块坐标。

- 三数组：`keys_(u64)` / `values_(V)` / `state_(u8)`，线性探测 + 墓碑（[:286-290](../src/engine/internal.hpp#L286-L290)）。
- 扩容：`(used_ + 1) * 3 >= keys_.size() * 2`（总占用 66.7%），新容量 `roundUp(max(64, (count_ + 1) * 4))`（[:246-250](../src/engine/internal.hpp#L246-L250)）——重哈希后负载约 25%，明显比另外两套稀疏，是在"每刻都探测"的场景下用空间换探测长度。最小容量 64。
- `set(key, value)` 对已存在的键**覆盖并返回 false**，新键返回 true（[:266-269](../src/engine/internal.hpp#L266-L269)）。注意它与 `std::unordered_map::insert` 的返回语义相反。
- 只有 `find` / `set` / `erase` / `forEach` / `clear` / `size`，没有迭代器、没有 `operator[]`、没有缩容。

## 6. `TickKeySet<Target>`

计划刻的 `(位置, 目标对象身份)` 去重集合（[注释](../src/engine/internal.hpp#L85-L86)）。`TickKey` 是 16 字节（`packedPos` + `target*`，有 `static_assert`），哈希把坐标与指针地址各乘一个常数后混合（[:72-80](../src/engine/internal.hpp#L72-L80)）。

- 三个计数器：`count_`（有效）、`used_`（有效 + 墓碑）、`slots_.size()`（容量）。
- `insert` 先 `slotOf` 查重再插入（[:92-115](../src/engine/internal.hpp#L92-L115)）：比标准库多一次探测，换来"重复插入零副作用"，因为调用方会大量重复调度同一条刻。
- 扩容：`(used_ + 1) * 3 >= slots_.size() * 2`（总占用 66.7%），新容量 `roundUp(2 * (count_ + 1))`（[:97-98](../src/engine/internal.hpp#L97-L98)）。
- **手动缩容** `compactIfNeeded()`（[:132-142](../src/engine/internal.hpp#L132-L142)）：表空时直接 `release()` 归还内存；容量 > 32 且（负载降到 1/6 以下，或墓碑达到容量一半）时重哈希。这是四套实现里唯一提供缩容的——红石电路会在一段时间内堆积上万条计划刻，然后突然清空。
- `release()` 用 `vector::swap` 立即释放两块缓冲区（[:127-131](../src/engine/internal.hpp#L127-L131)），`clear()` 则保留容量供下一刻复用（[:123-126](../src/engine/internal.hpp#L123-L126)）。

## 7. 怎么选

| 场景 | 选择 | 理由 |
|---|---|---|
| 需要在遍历中删除 | `light_internal::FlatMap` | 唯一提供 `erase(iterator)` 返回下一个 |
| 每批清空后复用 | `FlatSet/FlatMap`、`TickKeySet` | `clear()` 保留容量 |
| 峰值后要还内存 | `TickKeySet` | 唯一有自动缩容 |
| `string` 键、要用 `string_view` 查 | `FlatHashMap` + `TransparentStringHash` | 唯一支持透明查找 |
| 需要元素地址稳定（存裸指针） | `std::unordered_map` | 节点地址不随 rehash 变化 |
| `string` 键且构建后只读 | 排序数组 + 二分更合适 | 见 §9 第 1 条 |

## 8. 使用清单

### 8.1 写一次、读无数次（注册表 / 常量表）

| 容器 | 位置 | 写入 | 读取 |
|---|---|---|---|
| `Item::byName` | [items.cpp:15](../src/items.cpp#L15) | 启动注册 | 低 |
| `itemForBlock::byBlockName` | [items.cpp:237-244](../src/items.cpp#L237-L244) | 首次调用 | 高（每次破坏/掉落） |
| `axeStrippablesTable` | [item_use.cpp:236](../src/item_use.cpp#L236) | 静态初始化 | 极低 |
| `entityType::byName` | [entity_type_registry.cpp:171](../src/entity/entity_type_registry.cpp#L171) | 首次调用 | 低 |
| `itemType::byName` | [types.cpp:209](../src/engine/types.cpp#L209) | 首次调用 | 低 |
| 方块 `byName` | [state.cpp:31](../src/state.cpp#L31) | 启动注册 | 低 |
| `PropertyDefinition::valueIndex_` | [block.hpp:165](../include/redstone/block.hpp#L165) | 构建状态表 | 构建期 + 解析 |
| `noiseInstances_` / `positionalRandoms_` | [noise_router.hpp:305-310](../include/redstone/worldgen/noise_router.hpp#L305-L310) | 建路由器 | 极高（每次密度采样） |
| `kBuiltinFactories` | [noise_router.cpp:44](../src/worldgen/noise_router.cpp#L44) | `const` 全局 | 首次解析 |
| `byOptics` | [optical.cpp:182](../src/light/optical.cpp#L182) | 启动建表后丢弃 | 构建期折叠等价光学状态 |
| `randomStates_` / `structureStates_` | [noise_chunk_generator.hpp:99-103](../include/redstone/worldgen/noise_chunk_generator.hpp#L99-L103) | 每个 seed 一次 | 每区块每阶段（见 §9 第 3 条） |

### 8.2 读极多、写极少

| 容器 | 位置 | 读 | 写 |
|---|---|---|---|
| `World::chunks_` | [engine.hpp:722](../include/redstone/engine.hpp#L722) | 每次 `getBlock` 缓存未命中（[chunk_storage.cpp:161-170](../src/engine/chunk_storage.cpp#L161-L170) 的 64 槽直映缓存兜底） | load / unload，整局十余处 |
| `HolderShard::map`（64 分片） | [worldgen_scheduler.cpp:134](../src/worldgen/worldgen_scheduler.cpp#L134) | 每次 arm / 依赖扫描，`shared_lock` | 创建、取走、淘汰 |
| `containerEntities` / `containerTickerSlots` | [engine.hpp:412-413](../include/redstone/engine.hpp#L412-L413) | 漏斗/箱子每刻 | 放置 / 移除 |
| `pistonEntities` / `pistonTickerSlots` | [engine.hpp:371-373](../include/redstone/engine.hpp#L371-L373) | 活塞推进 | 活塞动作 |
| `comparatorSignals_` | [engine.hpp:815](../include/redstone/engine.hpp#L815) | 红石比较器更新 | 信号变化 |
| `hopperBlockWatchersByPos_` | [engine.hpp:421](../include/redstone/engine.hpp#L421) | 漏斗拉取 | 注册 / 注销 |
| `AttributeMap::instances_` | [attributes.hpp:368](../include/redstone/entity/attributes.hpp#L368) | 每刻（移动速度等） | 极少 |
| `LivingEntity::activeEffects_` | [living_entity.hpp:227](../include/redstone/entity/living_entity.hpp#L227) | 每刻 | 罕见 |
| 光照 `directChunks` / `versions` / `workerVersions` / `mainVersions` / `skyColumnSources` / `deferred*` / `latest*` / `lastReady*` / `persistedLightChunks` / `protocolBase*` | [service_impl.hpp:219-272](../src/light/service_impl.hpp#L219-L272) | 每次提交 / 传播 / 发布 | 仅生命周期事件 |
| 光照传播器 `workingChunks` / `lightValues` / `dirtySections` / `sources` / `storedEligible` | [block_light_engine.hpp:17-24](../src/light/block_light_engine.hpp#L17-L24)、[sky_light_engine.hpp:37-46](../src/light/sky_light_engine.hpp#L37-L46) | 极高频（每格） | 每格 |
| `BlockState::extensions` | [state.cpp:33](../src/state.cpp#L33) | 每次扩展属性查询 | 仅 intern 自定义状态 |

### 8.3 读写均衡

| 容器 | 位置 | 说明 |
|---|---|---|
| `ChunkTicksOf::ticksPerPosition`（`TickKeySet`） | [internal.hpp:346](../src/engine/internal.hpp#L346) | 计划刻插入 / 撤销 / 查重 |
| `containers` / `nextTickForContainer`（`FlatChunkMap`） | [internal.hpp:432-433](../src/engine/internal.hpp#L432-L433) | 每刻按区块检索容器 |
| `toRunThisTickSet` | [internal.hpp:436](../src/engine/internal.hpp#L436) | 每刻重建、tick 末清空 |
| 光照 `pending` | [service_impl.hpp:211](../src/light/service_impl.hpp#L211) | 每次提交写、每批读、处理完删 |
| `needsCompute` / `needsUpdate` | [service_impl.hpp:215-216](../src/light/service_impl.hpp#L215-L216) | 工作线程与主线程共享，频繁增删 |
| `PendingChunk::ownedCells` | [common.hpp:533](../src/light/common.hpp#L533) | 光照覆盖单元 |
| `PosSet`（`order` + `seen`） | [tree_feature.cpp:40-53](../src/worldgen/tree_feature.cpp#L40-L53) | 每个地物的临时去重 |
| 7 个 `queued` 集合 | [tree_feature.cpp:1519](../src/worldgen/tree_feature.cpp#L1519) | 巨型蘑菇逐层扩散去重 |
| `PoiSection::indexByLocal` | [poi.hpp:197](../include/redstone/poi.hpp#L197) | 稠密区段才启用（< 32 项走线性扫描） |

### 8.4 写多读少 / 写后即弃

| 容器 | 位置 | 形式 |
|---|---|---|
| `entityCountsByChunk_` | [engine.hpp:753](../include/redstone/engine.hpp#L753) | 每次实体增删 `++/--`，卸载时读一次（[world_entities.cpp:178](../src/entity/world_entities.cpp#L178)） |
| `containerPositionsByChunk_` / `pistonPositionsByChunk_` | [engine.hpp:418](../include/redstone/engine.hpp#L418)、[:754](../include/redstone/engine.hpp#L754) | 建时 `push_back`、拆时 `erase`，读只在卸载/存盘；把全表遍历换成 O(1) |
| `hopperItemWatchersBySection_` | [engine.hpp:419-420](../include/redstone/engine.hpp#L419-L420) | 物品移动时写，漏斗查询时读 |
| `BlockEventQueue::index` | [internal.hpp:668](../src/engine/internal.hpp#L668) | 每个方块事件 insert/erase，排空后基本为空 |
| `sweptVisited` / `visitedBlocks` | [inside_block_effects.cpp:165](../src/entity/inside_block_effects.cpp#L165)、[:510](../src/entity/inside_block_effects.cpp#L510) | 每次实体移动的临时集 |
| `piercingIgnoreIds_` | [projectiles.hpp:117](../include/redstone/entity/projectiles.hpp#L117) | 穿透箭的小集合 |
| `drainPublished` 的 `latestResult` | [service.cpp:479](../src/light/service.cpp#L479) | 每次 drain 的合并表，返回即销毁 |
| `validInput` 的 `seenYs` | [service.cpp:900](../src/light/service.cpp#L900) | 校验持久化光照段 Y 是否重复 |
| `FlatDensityCompiler::memo` / `splineMemo` | [density_function.cpp:593-594](../src/worldgen/density_function.cpp#L593-L594) | 一次编译的节点去重 |
| `DensityMemo` / `mapAllImpl` 的 memo | [density_function.cpp:1307](../src/worldgen/density_function.cpp#L1307)、[:1540](../src/worldgen/density_function.cpp#L1540) | DAG 遍历 memo，一次调用生命周期 |
| `NoiseChunk::wrapped_` | [noise_chunk.hpp:189](../include/redstone/worldgen/noise_chunk.hpp#L189) | 每区块包装一次 density 节点 |
| `preliminarySurfaceLevelCache_` | [noise_chunk.hpp:190](../include/redstone/worldgen/noise_chunk.hpp#L190) | 每列一次 |
| `MultiNoiseBiomeSource::seen` | [biome_source.cpp:727](../src/worldgen/biome_source.cpp#L727) | `possibleBiomes()` 内临时去重 |
| `carverBiomeCache_`（32 分片，各带 mutex） | [noise_chunk_generator.hpp:89](../include/redstone/worldgen/noise_chunk_generator.hpp#L89) | 雕刻时按 (carver, biome) 缓存 |

## 9. 已知问题

1. **注册表用哈希表其实是次优解。** §8.1 那些表构建后完全只读，规模几十到几千项。排序数组 + 二分（或 `string_view` 直接比较）在缓存上比哈希更好，还省掉哈希计算。当前用 `unordered_map` + `string_view` 键，代价是每次查找一次字符串哈希。

2. **`World::chunks_` 不能换成扁平表。** `TickingChunkEntry::chunk` 与 `BlockReadCache` 都存裸 `Chunk*`（[engine.hpp:715-721](../include/redstone/engine.hpp#L715-L721)），依赖 `unordered_map` **重哈希时节点地址不变**。`emplace` 期间 `Chunk` 本身也不能被移动——`Chunk` 的移动构造会重置读缓存（[:703-708](../include/redstone/engine.hpp#L703-L708)）。要换扁平表，必须先把 `Chunk` 改成 `unique_ptr<Chunk>` 或多一层间接。

3. **`randomStates_` / `structureStates_` 是真实的并发热点。** [noise_chunk_generator.cpp:78-104](../src/worldgen/noise_chunk_generator.cpp#L78-L104) 每次取 `RandomState` 都要拿全局 `mutex_`，而这两个表实际上只有 1 个条目（单 seed）。16 个生成线程每区块每阶段都会撞这把锁。可改为 `shared_mutex`、原子发布的 `shared_ptr` 快照，或按 seed 分片。

4. **光照模块内部不一致。** [service_impl.hpp](../src/light/service_impl.hpp#L211-L290) 里 `pending`、`processedGeneration`、`readyChunks`、`neighborDeferrals` 还是 `std::unordered_*`，而紧邻的 `needsCompute` / `needsUpdate` / `directChunks` 已经是 `FlatSet` / `FlatMap`。前四个在提交与批处理路径上，其中 `processedGeneration` 和 `readyChunks` 每批都要探测。[service.hpp:188](../include/redstone/light/service.hpp#L188) 那条"避免依赖 `unordered_map` 迭代顺序"的注释也是这种混用的残留。

5. **四套自研实现各自重写了扩容、墓碑与迭代。** `FlatHashMap`（线性 + 完整 hash 比较）、`FlatChunkMap`（线性 + 三数组）、`TickKeySet`（线性 + 手动缩容）、`FlatTable`（H2 标签 + `rehashEpoch_`）。H2 那套在探测上最优（只碰状态字节），但只有光照在用。合并成一套可参数化的实现是可选方向，代价是要保留 `TickKeySet` 的缩容与 `FlatHashMap` 的透明查找。

6. **几个热表其实键是稠密枚举。** `AttributeMap::instances_`（键 `AttributeId`）和 `LivingEntity::activeEffects_`（通常 0–3 项）每刻读、几乎不写，却让每个实体背一个哈希表头。前者用定长数组或位图即可，后者用小型 `vector` 更合适。

7. **`entitySections_` 用的是 `std::map` 而非哈希表**（[engine.hpp:748](../include/redstone/engine.hpp#L748)）。这是有意的：实体段遍历顺序需要确定（对应原版 `EntitySectionStorage` 的有序语义）。不要顺手换成哈希表。

8. **8 个文件包含 `unordered_map` / `unordered_set` 但实际未使用**：`src/item_tags.cpp`、`src/poi.cpp`、`src/worldgen/climate.cpp`、`src/worldgen/feature.cpp`、`src/worldgen/proto_chunk.cpp`、`include/redstone/net/wire.hpp`、`include/redstone/worldgen/worldgen_scheduler.hpp`、`include/redstone/light/service.hpp`。无害，可清理。
