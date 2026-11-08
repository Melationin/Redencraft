# 26.1 光照底座（L-1）

## 当前边界

L-1 已实现天空光/方块光的数据层、官方方块光学元数据、主线程到单写者后台线程的快照交接、代际失效和完整结果发布基础。它**没有实现光照传播**：新增或更新的 snapshot 仍保持 `propagationComplete=false`、`protocolReady=false`，不能作为真实亮度发布。协议 775 的区块光照段仍是占位，Anvil 光照读写也未接入。

生产服务端默认不启用该路径。`World::enableLighting()` 是 opt-in 测试/后续集成入口；核心 tick 不等待 worker，也不在方块写入时扫描区段。

## 光层数据

`include/redstone/light.hpp`：

- `LightLayerSnapshot` 区分 `Missing`、`Uniform`、`Mixed`；mixed 使用恰好 2048 bytes 的 nibble 数组。
- 索引严格为 `y << 8 | z << 4 | x`；偶数索引在低半字节，奇数索引在高半字节。
- `LightLayer::Block=0`、`LightLayer::Sky=1`，一个 `LightSectionSnapshot` 同时保存两层，两层不共享数据。
- 光 section 范围由 `minBlockSectionY - 1` 起，数量为 `blockSectionCount + 2`；输入由 `LightService` 校验连续且完整。
- 发布单元是 `std::shared_ptr<const LightChunkSnapshot>`。后续改动复制受影响的 section，旧读者持有的 chunk/section 保持不变。

## 方块光学元数据

`Block` 注册时为每个 schema variant 准备 `BlockLightProperties`，运行热路径只按状态直接读取，不再查字符串或扫描碰撞形状。字段独立保存：

- `emission`
- `dampening`
- `canOcclude`
- `useShapeForLightOcclusion`
- `propagatesSkylightDown`
- 六个方向的 16×16 face mask 与 validity mask（结构已保留；L-1 未生成遮蔽形状，实际 validity 为 0）

数据来自未修改官方 26.1 `server.jar`（SHA-1 `3872a7f07a1a595e651aef8b058dfc2bb3772f46`）的离线 Java 探针，不覆盖 `assets/protocol775`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File validation/vanilla26_1/run_light_metadata.ps1
python validation/light_metadata/generate_cpp.py `
  --tsv assets/light26_1/block_light.tsv --out src/light_metadata.inc
```

资产包含当前注册名单的 68 个方块、2,366 个官方状态，manifest 位于 `assets/light26_1/manifest.json`。TSV SHA-256 为 `d2e60ad07a59b1ec0a1f2550c3127ee5f937a2f3b85183f9db145522bb8fac49`。C++ schema 中不存在的官方组合不会伪造；没有官方记录的 extension/后注册状态显式返回 `supported=false`。`light_metadata_asset` CTest 会核对 manifest、唯一键、行数和生成表是否陈旧。

`hasDifferentLightProperties` 与 `LightEngine.java:42-49` 一致：同一状态返回 false；emission 或 dampening 变化、任一状态使用 shape-for-light-occlusion 时返回 true。`canOcclude` 是独立元数据，不额外加入这个 Java 判定。未知元数据保守返回 true。空 section 的 false/true 转换由 `hasDifferentSectionLightProperties` 单独处理。

## 后台交接

`LightService` 是每个启用世界自己的单写者 executor：

1. 主线程构造完全拥有的 `LightChunkInput`：区块坐标、连续光 section、per-section palette/cell 索引、双层光值和 `BlockLightProperties`。不传 `World`、内部 palette 或可变注册表指针。
2. 主线程提交 load、section-empty、block-change 和 unload；连续方块变更合并成一个 owned batch，不为每个格子创建线程任务。
3. worker 每次最多取 `batchSize=1000` 个命令，按到达顺序执行 PRE_UPDATE 类操作；传播入口尚未实现。
4. 受影响 section 每批至多复制一次；未变化 section 由 shared immutable snapshot 复用，随后发布完整 chunk snapshot。
5. `World` 线程调用 `drainPublished()` 才更新 latest 并触发 callback；callback 不运行在 worker。旧代结果不会覆盖新代。

默认队列上限为 4096 个命令、每个变更批最多 4096 个 block change。队列满时提交返回 false 并增加 `Stats::rejected`，不会静默成功；worker 错误保存在 `workerError()`。`close()` 停止接收新命令、排空已有命令并 join。`World::~World` 与 `disableLighting()` 会先关闭 worker。

`World::loadChunk`、`unloadChunk` 和成功生效的 `setBlock` 在 opt-in 时分别提交不可变初始快照、代际失效和光学属性/section emptiness 变更。挂钩位于既有方块状态写入之后、原更新回调之前，保持现有更新、tick 和广播顺序。`rawSetBlock` 是导入/测试的原始写入，不产生光照任务；批量导入完成后应重新启用或显式重建初始快照。

## 高度图

同一地基同时实现 26.1 `Heightmap` 数据。六种 `Heightmap.Types` 的 id、序列化键和 Usage 独立定义；final chunk 按 `ChunkStatus.FINAL_HEIGHTMAPS` 创建 `OCEAN_FLOOR`、`WORLD_SURFACE`、`MOTION_BLOCKING`、`MOTION_BLOCKING_NO_LEAVES` 四张 live map。世界生成阶段的 `*_WG` 类型可由类构造，但当前没有生成阶段，也不放入 final chunk。

- 每列使用 `ceil(log2(worldHeight + 1))` 位，默认世界高度 384 时为 9 bits、256 列、37 个 `uint64_t`，与 `SimpleBitStorage` 的 little-endian 字段布局一致。
- `primeHeightmaps` 从最高 section 顶向 minY 扫描；`update` 严格保留 Java 的 `localY <= firstAvailable - 2` 短路、向上抬升、以及删除顶层后向下重扫条件。
- `LevelChunk.setBlockState` 的更新顺序保持为 `MOTION_BLOCKING`、`MOTION_BLOCKING_NO_LEAVES`、`OCEAN_FLOOR`、`WORLD_SURFACE`。
- `World::primeHeightmaps()` 在 `importWorld` 的 raw block 写入完成后重建；正常 `setBlock` 实时更新，`rawSetBlock` 不自动更新。

当前没有注册 `LeavesBlock`，因此 no-leaves 类型对已注册状态使用同一 motion-blocking 谓词；未知导入方块不伪造 leaves 身份。协议仍沿用既有区块编码高度图，尚未切换到这份 live map。

## 未实现与后续

// TODO(L-2a/L-2b): implement block/sky increase-decrease propagation, retained queues, section/neighbor constraints, exact face masks and published light values.

// TODO(L-3): encode only a published snapshot into protocol light masks/data and reject unloaded generations.

// TODO(W-2/W-3): read and write Anvil `SkyLight`, `BlockLight` and `isLightOn`.

L-1 不包含任何性能门槛结论。固定世吞的 1000 tick 基线已保存在工作区 `build-l1-baseline/`，后续 L-4 才会在启用真实传播和发布的同一阶段边界上做至少五组交错 A/B，并判断主线程额外 CPU/平均刻墙钟 <10%。当前数据不能用于该判断。

## 聚焦验证

```powershell
cmake --build build-f7-nmake --target light_tests heightmap_tests
.\build-f7-nmake\light_tests.exe .\assets\light26_1
.\build-f7-nmake\heightmap_tests.exe
python validation\light_metadata\check.py --tsv assets\light26_1\block_light.tsv `
  --names assets\light26_1\block_names.txt --manifest assets\light26_1\manifest.json `
  --source src\light_metadata.inc
```

`light` 覆盖双层 nibble、世界高度 padding、光学状态与线程/代际交接；`heightmap` 覆盖六类元数据、四张 live map、bit storage、prime/update 和 World/import 接线；`light_metadata_asset` 检查官方 TSV/manifest 与生成 C++ 表一致。
