# 26.1 光照底座与天空光/方块光（L-1/L-2a/L-2b）

## 当前边界

L-1 已实现天空光/方块光的数据层、官方方块光学元数据、主线程到单写者后台线程的快照交接、代际失效和完整结果发布基础。L-2a 在同一 worker 上实现真实方块光传播；L-2b 根据每列天空入口实现满光向下、侧向衰减、共享面遮挡、跨 section/chunk、负高度和无天空光配置。`blockLightComplete`、`skyLightComplete` 与完整 `propagationComplete` 分开记录；协议 775 的区块光照段仍是占位，`protocolReady` 保持 false，Anvil 光照读写也未接入。

生产服务端默认不启用该路径。`World::enableLighting()` 是 opt-in 测试/后续集成入口；核心 tick 不等待 worker，也不在方块写入时扫描区段。

## 光层数据

`include/redstone/light.hpp`：

- `LightLayerSnapshot` 区分 `Missing`、`Uniform`、`Mixed`；mixed 使用恰好 2048 bytes 的 nibble 数组。
- 索引严格为 `y << 8 | z << 4 | x`；偶数索引在低半字节，奇数索引在高半字节。
- `LightLayer::Block=0`、`LightLayer::Sky=1`，一个 `LightSectionSnapshot` 同时保存两层，两层不共享数据。光学输入的 `cells` 为空表示整个 16³ section 都使用 `palette[0]`；只有非均匀 palette 才保存 4096 个索引。
- 光 section 范围由 `minBlockSectionY - 1` 起，数量为 `blockSectionCount + 2`；输入由 `LightService` 校验连续且完整。
- 发布单元是 `std::shared_ptr<const LightChunkSnapshot>`。后续改动复制受影响的 section，旧读者持有的 chunk/section 保持不变。

## 方块光学元数据

`Block` 注册时为每个 schema variant 准备 `BlockLightProperties`，运行热路径只按状态直接读取，不再查字符串或扫描碰撞形状。字段独立保存：

- `emission`
- `dampening`
- `canOcclude`
- `useShapeForLightOcclusion`
- `propagatesSkylightDown`
- 六个方向的官方面矩形 mask；只有 `useShapeForLightOcclusion` 状态引用预构建 mask，和碰撞形状完全分开。

数据来自未修改官方 26.1 `server.jar`（SHA-1 `3872a7f07a1a595e651aef8b058dfc2bb3772f46`）的离线 Java 探针，不覆盖 `assets/protocol775`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File validation/vanilla26_1/run_light_metadata.ps1
python validation/light_metadata/generate_cpp.py `
  --tsv assets/light26_1/block_light.tsv --out src/light_metadata.inc
powershell -NoProfile -ExecutionPolicy Bypass -File validation/vanilla26_1/run_light_occlusion.ps1
python validation/light_metadata/generate_light_occlusion.py `
  --tsv assets/light26_1/block_light_faces.tsv --out src/light_occlusion.inc
```

资产包含当前注册名单的 68 个方块、2,366 个官方状态，manifest 位于 `assets/light26_1/manifest.json`。TSV SHA-256 为 `d2e60ad07a59b1ec0a1f2550c3127ee5f937a2f3b85183f9db145522bb8fac49`。C++ schema 中不存在的官方组合不会伪造；没有官方记录的 extension/后注册状态显式返回 `supported=false`。`light_metadata_asset` CTest 会核对 manifest、唯一键、行数和生成表是否陈旧。

`hasDifferentLightProperties` 与 `LightEngine.java:42-49` 一致：同一状态返回 false；emission 或 dampening 变化、任一状态使用 shape-for-light-occlusion 时返回 true。`canOcclude` 是独立元数据，不额外加入这个 Java 判定。未知元数据保守返回 true。空 section 的 false/true 转换由 `hasDifferentSectionLightProperties` 单独处理。

`block_light_faces.tsv` 由同一官方 JAR 导出 44 个 `useShapeForLightOcclusion` 状态的六方向面矩形，归并为 20 个唯一 mask；没有从碰撞形状推导，也没有按方块名猜测。传播使用 `Shapes.faceShapeOccludes` 的并集覆盖判据：分别取源状态在传播方向、目标状态在反方向的面形状，二者必须覆盖完整 16×16 面才阻断。缺失光学状态的区块不会产生 `blockLightComplete=true`。

## 后台交接

`LightService` 是每个启用世界自己的单写者 executor：

1. 主线程构造完全拥有的 `LightChunkInput`：区块坐标、连续光 section、per-section palette/cell 索引、双层光值和 `BlockLightProperties`。不传 `World`、内部 palette 或可变注册表指针。全空 section 和上下 padding 复用世界内同一组不可变输入；均匀非空 section 只保存一个 palette 值。
2. 主线程按区块合并 load、section-empty、cell 和 unload，维护每区块最新的可重试状态，不为每个格子创建线程任务。有效光照更新不会因 worker 容量而丢弃；暂停或积压时状态留在 pending map，直到 worker 真正接收。`Stats::rejected` 只记录关闭后的提交或非法 section/cell 输入。
3. worker 每次从 pending map 最多取 `min(batchSize, maxQueuedCommands)` 个区块，执行 PRE_UPDATE 类操作。方块光和天空光各自拥有 check/decrease/increase 队列并依次完全排空。天空光先按 `ChunkSkyLightSources` 的垂直相邻面判据维护每列入口，再直接填充入口以上为 15，只从入口边界和相邻低入口的侧向边入队。天空数据保持稀疏：未存储 section 不是普通 0 层，查询取最近上方已存层的底平面；只有偏离该派生值或相邻非空区段需要时才分配。顶部 padding 可由入口向下传光，底部 padding 可由相邻非空区段实际存储，超出光照 section 范围才读取为 0。未加载水平邻区块按 Java 读取屏障阻止传播。
4. 加载区块时扫描真实方块光源和天空入口；加载/卸载后从相邻已加载区块的边界格重新发起两层 increase/decrease，使跨 chunk 两侧收敛。普通方块变更只更新受影响天空列并检查对应格，不扫描整块或全区块。
5. worker 对受影响 section 分配真正可变的工作副本，仅在完整处理完成后转换为 `shared_ptr<const LightSectionSnapshot>` 发布。未变化 section 继续复用不可变快照。
6. `World` 线程调用 `drainPublished()` 才更新 latest 并触发 callback；callback 不运行在 worker。待发布结果按区块只保留最新 snapshot。生命周期 epoch 标识加载代；revision 是单写者按完成批次递增的发布序号，独立于无序 pending 批次携带的输入请求号。同代结果在完整批次后可见，卸载/重载后的旧代一律拒绝。

默认每次最多取 1000 个区块（另受 pending 容量配置约束）。每区块 pending 状态只保留最新 load、按 section 合并的 cell delta 和每 section 最新空态；已 drain 的发布结果也按区块合并，不随写入代数无限增长。unload 会立即对读者失效，并作为有序 tombstone 保留到 worker 清除旧内部代；若其后立即 reload，worker 先清除旧代再安装完整新快照。worker 错误保存在 `workerError()`。`close()` 停止接收新命令、排空 pending 并 join。`World::~World` 与 `disableLighting()` 会先关闭 worker。

`World::loadChunk`、`unloadChunk` 和成功生效的 `setBlock` 在 opt-in 时分别提交不可变初始快照、代际失效和光学属性/section emptiness 变更。两层传播都只在 worker 内进行；主线程不等待，也不读取活区块进行传播。挂钩位于既有方块状态写入之后、原更新回调之前，保持现有更新、tick 和广播顺序。`rawSetBlock` 是导入/测试的原始写入，不产生逐格光照任务；`importWorld` 完成 raw block 写入后调用 `refreshLightSnapshots()`，用当前权威区块状态替换导入前排队快照。

## 高度图

同一地基同时实现 26.1 `Heightmap` 数据。六种 `Heightmap.Types` 的 id、序列化键和 Usage 独立定义；final chunk 按 `ChunkStatus.FINAL_HEIGHTMAPS` 创建 `OCEAN_FLOOR`、`WORLD_SURFACE`、`MOTION_BLOCKING`、`MOTION_BLOCKING_NO_LEAVES` 四张 live map。世界生成阶段的 `*_WG` 类型可由类构造，但当前没有生成阶段，也不放入 final chunk。

- 每列使用 `ceil(log2(worldHeight + 1))` 位，默认世界高度 384 时为 9 bits、256 列、37 个 `uint64_t`，与 `SimpleBitStorage` 的 little-endian 字段布局一致。
- `primeHeightmaps` 先清空四张表，再从最高 section 顶向 minY 扫描，因此重复 prime 或二次导入会把已变空的列恢复到 `minY-1`。
- `LevelChunk.setBlockState` 的四表判定语义保持为 `MOTION_BLOCKING`、`MOTION_BLOCKING_NO_LEAVES`、`OCEAN_FLOOR`、`WORLD_SURFACE`。共用早退取四张 `firstAvailable` 的最小值，保证低 `OCEAN_FLOOR` 不会被高水面挡住；深写在同一列只做一次状态谓词计算。删除顶层时四张待重扫表共享一次列扫描，各表仍独立选择首个命中。
- 16 对齐世界直接复用 `World::setBlock` 已定位的 `Chunk`：从 `y-1` 起每降到一个光 section 时只取一次 `const Section&`，`hasOnlyAir()` 跳过整段，非空段只沿目标列以 256 为步长调用 `Section::get`，不经过 `World::getBlock`。非 16 对齐 `minY` 继续使用原有逐格路径，所以现有模拟布局、谓词和结果不变。诊断开关可记录扫描次数、访问/跳过 section 和直接读取格数，默认关闭。
- `World::primeHeightmaps()` 在 `importWorld` 的 raw block 写入完成后重建；正常 `setBlock` 通过一次区块定位实时更新，`rawSetBlock` 不自动更新。

高度图允许非 16 对齐的既有 `minY` 模拟输入；只有需要 `y >> 4` section 键的光照入口会显式拒绝这种世界，禁用光照的模拟契约不变。

当前没有注册 `LeavesBlock`，因此 no-leaves 类型对已注册状态使用同一 motion-blocking 谓词；未知导入方块不伪造 leaves 身份。协议仍沿用既有区块编码高度图，尚未切换到这份 live map。

## 未实现与后续

// TODO(L-3): encode only a published snapshot into protocol light masks/data and reject unloaded generations.

// TODO(W-2/W-3): read and write Anvil `SkyLight`, `BlockLight` and `isLightOn`.

方块光 benchmark 只观察正常 worker 工作量。启用天空光、方块光和发布后的 L-4 仍须做至少五组交错 A/B，并把主线程固定在同一性能核、worker 固定在另一物理核，判断主线程额外 CPU/平均刻墙钟 <10%。

## 聚焦验证

```powershell
cmake --build build-f7-nmake --target light_tests heightmap_tests
.\build-f7-nmake\light_tests.exe .\assets\light26_1
.\build-f7-nmake\heightmap_tests.exe
python validation\light_metadata\check.py --tsv assets\light26_1\block_light.tsv `
  --names assets\light26_1\block_names.txt --manifest assets\light26_1\manifest.json `
  --source src\light_metadata.inc --faces assets\light26_1/block_light_faces.tsv `
  --faces-source src/light_occlusion.inc
```

官方方块光差分需要工作区已有且 SHA-1 校验通过的隔离服务端：

```powershell
python validation\vanilla26_1\run_block_light_validation.py --promote-golden
python validation\vanilla26_1\block_light_differential_test.py `
  --probe build-f7-nmake\block_light_probe.exe `
  --golden validation\vanilla26_1\block-light-golden.json
```

runner 使用独立世界和 loopback 端口。每个阶段对四个受影响 chunk 调用 `ThreadedLevelLightEngine.waitForPendingTasks` 发出 POST_UPDATE fence；所有 future 完成后，agent 把取样任务排回服务端主线程读取 `LightLayer.BLOCK` 或 `LightLayer.SKY`，tick 本身不等待。天空夹具还同时记录指定 section 的 raw layer 是否缺席。它不使用固定睡眠或伪造亮度。原始 server log、fence 记录、候选、命令和 manifest 保存在唯一运行目录。

`light` 还覆盖天空入口、露天满光、整块屋顶、侧漏衰减、开顶/复盖、同代权威刷新、加载顺序、负 y、顶部水层衰减、底部已分配光、稀疏缺层派生、无天空光配置和双层互不串扰；`heightmap` 覆盖六类元数据、四张 live map、bit storage、最小阈值反例、反向重扫、重复 prime、二次导入和 World/import 接线；`light_metadata_asset` 检查官方 TSV、面掩码、manifest 与两份生成 C++ 表一致；`block_light_differential` 比较官方五阶段 225 个方块光观察，`sky_light_differential` 比较官方六阶段 108 个天空光值及 36 个 raw-layer 缺席标记。
