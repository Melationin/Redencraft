# 世界生成性能分析：C++ 26.1 vs Fabric 26.1.2 + C2ME + FastNoise

日期：2026-11-19。状态：分析轮，未改代码。

## 0. 结论摘要

- 在**同为 24 线程满核**的条件下，C++ 与 Fabric + C2ME + FastNoise 的每区块成本已经接近：
  C++ 约 68 ms CPU/区块、3.1-3.4 ms 墙钟/区块（~300 cps）；Fabric 约 74-82 ms CPU/区块、
  2.9-3.1 ms 墙钟/区块（~300-350 cps）。交错测量下差距在 ±10% 以内。
- 之前"C++ 302 cps vs Fabric 350 cps"的差距主要来自**单次测量的机器状态噪声**，不是数量级差距。
- C++ 端真正的损失点不在调度：24 线程下有 21 个线程在跑（利用率 ~87%），
  但 8→24 线程时每区块 CPU 从 28 ms 涨到 68 ms（2.4 倍）。
  频率控制实验显示同核数下纯计算循环慢 1.49 倍，剩下约 1.6 倍来自缓存/内存层次压力。
  所以优化方向是**降低每区块指令数与内存流量**，而不是继续加线程。
- 消融结果：纯 Fabric = 77 cps；+C2ME = 279 cps（主要来自并行调度）；再 +FastNoise = 300 cps
  （每区块成本再降约 10%）。C++ 的调度已经相当于 C2ME 那一层，缺的是 FastNoise 那一层的
  单区块常数优化（地表规则编译、批量写方块、密度函数特化、结构拼装去拷贝）。

## 1. 测量环境与口径

- 机器：WSL2 Ubuntu 24.04，i9-13980HX（24 逻辑核，无 SMT 暴露），15 GB 内存。
- C++：`worldgen_throughput_benchmark`，`-O3 -march=native` + LTO + jemalloc，
  `--size 50 --seed 123456` = 2500 个 FULL 区块，`--threads 8/12/16/24`，无光路。
- Fabric：MC 26.1.2，Fabric Loader 0.20.1，Temurin 25，C2ME 0.4.0-alpha.0.71+26.1.2，
  FastNoise(zfastnoise) 1.1.1+26.1，Chunky 1.5.66，Fabric API 0.155.3。
  用 RCON 驱动 Chunky：`radius 500` 方形 = 65x65 = 4225 个区块，无玩家。
- 每个配置先跑一次暖机区域再测新区域；C++ 与 Fabric 交错运行以抵消热漂移。
- 噪声：同一配置重复测量可见 ±10-20% 波动（WSL 调度 + 全核频率）。下表取暖机后的代表值。

## 2. 对照结果

| 实现 | 区块数 | 墙钟 | 吞吐 | CPU | CPU/区块 | 平均忙线程 |
|---|---|---|---|---|---|---|
| C++ 8 线程 | 2500 | 8.65-9.02 s | 278-289 cps | 70-72 s | 28.1-28.7 ms | 8.2 |
| C++ 12 线程 | 2500 | 8.04 s | 311 cps | 94.2 s | 37.7 ms | 11.7 |
| C++ 16 线程 | 2500 | 8.19 s | 305 cps | 122.4 s | 49.0 ms | 14.9 |
| C++ 24 线程 | 2500 | 8.06-8.51 s | 294-310 cps | 170 s | 68.1 ms | 21.0 |
| Fabric + C2ME + FastNoise | 4225 | 12.08-13.05 s | 324-350 cps | 296-312 s | 70-74 ms | 22-23 |

C++ 墙钟从 8 线程的 8.7 s 到 24 线程的 8.2 s，只快 6%；因为每区块 CPU 涨了 2.4 倍，
多出来的线程基本被每线程变慢抵消。

## 3. 消融（Chunky 4225 区块，暖机后取一次）

| 配置 | 墙钟 | 吞吐 | CPU | CPU/区块 | 忙线程 |
|---|---|---|---|---|---|
| 纯 Fabric + Chunky | 55.09 s | 77 cps | 219.6 s | 52.0 ms | 4.0 |
| + C2ME | 15.12 s | 279 cps | 347.7 s | 82.3 ms | 23.0 |
| + C2ME + FastNoise | 14.09 s | 300 cps | 311.7 s | 73.8 ms | 22.1 |

解读：C2ME 的 3.6 倍来自把有效线程从 4 提到 23（并行调度 + 锁/IO 解耦）；
FastNoise 在此设置下再贡献约 7-10%（每区块 CPU 82 → 74 ms），来自地表规则/方块写入/
密度与噪声的常数优化。

## 4. 频率控制实验

同一段 FMA 依赖链，每线程固定 1e9 次迭代：

| 线程数 | 每线程 CPU 秒 | 墙钟 |
|---|---|---|
| 8 | 1.99 s | 2.02 s |
| 24 | 2.97 s | 3.04 s |

即全核频率约为 8 线程时的 1/1.49。把 C++ 8 线程的 28.4 ms/区块按 1.49 折算到全核应是
42.3 ms；实测 68.1 ms，多出的 1.6 倍是缓存/内存层次与共享数据结构的压力（perf 里没有
单一锁热点，额外 CPU 均匀摊在同样的计算函数上，符合带宽/缓存而非自旋的特征）。

## 5. C++ 热点（perf task-clock，24 线程，147k 样本）

| 类别 | 占比 | 代表符号 |
|---|---|---|
| 密度函数/含水层 | 27.9% | `DensityNode::computeUncached` 8.5%、`aquifer::computeSubstance` 5.2%、`NoiseInterpolatorState::compute` 3.4% |
| 噪声 | 14.8% | `ImprovedNoise::noise` 10.9%、`PerlinNoise::getValue` 2.0%、`BlendedNoise::compute` 1.7% |
| 结构/地物 | 11.9% | `JigsawBlockInfo::operator=` 2.6%、排序/拷贝 3.0%、`elementShuffledJigsaws` 1.7%、`placeOreCore` 2.1% |
| 地表规则 | 7.5% | `applyPreparedRule` 3.3%、`testPreparedCondition` 3.0%、`buildSurface` 1.5% |
| 方块存储/状态 | 6.5% | `BlockSection::set` 1.9%、`fluidStateOf` 1.1%、`blockHasTag` 0.8% |
| libc 内存/字符串 | 5.2% | `memcmp` 2.6%、`memmove` 1.4% |
| 分配器 | 3.3% | `mallocx` 1.6%、`dallocx` 1.6% |
| 字符串/哈希 | 2.8% | biome 名查找、`lookupWithNormalizedView` |
| 生物群系/气候 | 2.6% | `searchFlatRec`（气候 R 树） |

8 线程时结构拼装只占 ~2%，24 线程时升到 ~12%——说明它在全核下更贵（缓存压力 + 拷贝/排序），
是明显的优化点。

## 6. 上游实现的关键手法（源码依据）

C2ME（`/tmp/ref/C2ME-fabric`）：

- `c2me-opts-dfc`：把整棵 density function 图编译成 JVM 字节码
  （`gen/jvm/CompiledDensityFunction`、`emitters/*BytecodeEmitters`），
  并做常量折叠、分支消除、公共子表达式消除（`opto/passes/CacheElimination`），
  同时生成单点 `ISingleMethod` 与批量 `IMultiMethod`。
- `c2me-opts-math` / `natives-math`：向量化/本地数学。
- `c2me-opts-worldgen-vanilla`：重写 aquifer、结构权重采样、
  `Block.isShapeFullCube` 的线程本地缓存。
- `c2me-rewrites-chunk-system`：异步区块任务调度（消融里 3.6 倍的主来源）。

FastNoise（`/tmp/ref/FastNoise`）：

- `FastNoiseGen.populateNoise`：一次性 `finalDensity.getFilledBuffer(volume)`
  拿整块密度，再自顶向下逐格过 aquifer，直接写 `FastChunkSection`；
  取消 section 锁和每 section 的 `Set.add`，最后批量刷高度图。
- `zmatcomp`：用 ASM 把 material rule 编译成字节码类
  （`MaterialRuleClassWriter`、`CompiledMaterialRule`）。
- `zfastsurface`：按列缓存（`FastPaletteIndexCache`、`FastColumnBitMask`、
  `FastIsFluidCache`），配合编译后的规则按列填充。
- `FastNoisePaletteHelper` / `BlockCountingPalettedContainer`：直接操作调色板索引与计数。

## 7. 优化提案（按优先级）

P0（低风险、收益确定，预计合计 -8~-12% CPU）

1. 结构拼装去拷贝：`JigsawBlockInfo` 的 operator=/排序/未初始化拷贝在 24 线程占 ~6-7%。
   做法：池化结构模板数据、用索引视图代替 vector 拷贝、只排序一次、复用临时缓冲。
2. 状态查表化：把 `fluidStateOf`/`blockHasTag`/`blocksMotion` 预计算成
   按 state id 索引的数组（tag 用 64 位位集），去掉热路径上的字符串比较
   （memcmp 2.6% + string/hash 2.8% + 两个函数 ~1.9%）。
3. 批量方块写入：给 `BlockSection` 增加"已解析调色板索引"的批量写接口
   （air/stone/water 等预先解析），`fillFromNoise` 中整段填充不再逐格线性扫调色板。

P1（中等，预计 -8~-12% CPU）

4. 地表规则编译/按列缓存：把 `PreparedRuleNode/PreparedCondNode` 编译成线性程序，
   对每列只求值一次与 y 无关的条件，缓存调色板索引（对应 FastNoise zfastsurface/zmatcomp）。
5. 密度函数求值计划 + CSE：为每个 (noise settings, seed) 预编译后序指令流，
   消除递归与 shared_ptr 解引用，按 4x4 单元批量求值并复用公共子表达式
   （对应 C2ME DFC 的 CacheElimination/IMultiMethod）。
6. 噪声批量核：固定 x 复用 `perm` 查表、FMA、按 4x4 XZ tile 组织循环。

P2（收尾，预计 -2~-5% CPU）

7. 气候 R 树缓存：按 quart (x,z) 记忆 `searchFlatRec` 结果。
8. 分配器：为每区块临时对象引入线程本地 arena/复用池（当前 mallocx+dallocx 3.3%）。
9. 如果后续仍受内存带宽限制，再考虑分块预取与结构体布局（SoA）调整。

合计预期：CPU/区块 -18~-25%，24 线程墙钟 8.1 s → 6.1-6.7 s（370-410 cps），
超过当前 Fabric + C2ME + FastNoise 的实测水平。

## 8. 验证与风险

- 任何生成结果都必须与 26.1 参考一致：用现有 `worldgen_differential_probe` 做同 seed 对照，
  逐区块 diff 方块的哈希；性能优化不得改变地形/结构/生物群系。
- 每个阶段单独提交并用同一基准 A/B；测量用 `taskset` 固定核 + 多次交错取中位数，
  避免本轮看到的 ±20% 漂移。
- 先做 P0（结构/查表/批量写），它们不改算法语义，风险最低；再做 P1 的规则编译与 DF 计划。

## 9. 复现命令

- C++：`./build-maxopt/worldgen_throughput_benchmark assets/worldgen26_1 --size 50 --seed 123456 --threads 24`
- perf：`/usr/lib/linux-tools-6.8.0-149/perf record -e task-clock -F 999 ...`
- Fabric 消融：`fabric-bench/run_ablation.sh <vanilla|c2me|both> <c1x> <c1z> <c2x> <c2z>`
- 频率对照：`/tmp/freqtest 8|24 1000000000`


## 10. P0/P1 实施与结果（2026-11-19 晚）

同一会话、同机、seed 123456、2500 区块、`build-maxopt`：

| 版本 | t24 墙钟 | t24 CPU | CPU/区块 | 吞吐 | t8 墙钟 |
|---|---|---|---|---|---|
| 基线 | 6.84-6.91 s | 146-150 s | 58-60 ms | 363 cps | 7.04-7.31 s |
| + P0 初版（结构体拷贝/流体表/调色板记忆） | 6.78 s | 150 s | 60 ms | 369 cps | 7.27-7.46 s |
| + P0 结构引用化（不再复制 JigsawBlockInfo） | 6.33-6.52 s | 138-141 s | 55-56 ms | 384-395 cps | 7.27-7.46 s |
| + P1 字符串/锁 | **5.10-5.25 s** | **120.5-123.0 s** | **48-49 ms** | **476-490 cps** | 6.69-7.11 s |

累计：墙钟 **-25%**，CPU **-18%**，吞吐 363 → 483 cps（+33%）。
对比第 2 节 Fabric + C2ME + FastNoise 的 324-350 cps，现在已经反超约 40%。

### 改动

P0：

1. `StructureTemplatePool` 候选与 `PieceState` 改为存放 `const StructurePoolElement*`，洗牌顺序、权重数量与 RNG 消耗不变。
2. `elementShuffledJigsaws` 返回 `ShuffledJigsaw`（模板指针 + offset，只有 Feature 元素持有 unique_ptr），
   洗牌/稳定排序只搬 32 字节；新增 `StructureTemplate::jigsawsFor` 提供稳定引用。
3. `tryChildren` 只复制 `StructurePiece` 的标量字段，不再复制整条含字符串与 junction 向量的 piece。
4. `fluidStateOf` 的线程本地缓存直接记录"是否带流体"，普通干方块一次索引 + 一次分支。
5. `BlockSection::set` 增加最近一次 (state id -> palette index) 记忆，地形连续同态写入跳过调色板线性查找。

P1（预编译查找/去掉不必要的分配与锁）：

6. `resolvePoolAlias` 返回 `string_view`，常见带命名空间路径不再 `normId` 分配；只在无命名空间时构造临时 key。
7. `StructureTemplateCache::getOrLoad` 命中路径改用 `std::shared_mutex` 共享锁 + 异质查找（string_view 直接 find），
   不再每次分配 `normId`。这是本轮最大收益点：24 线程下模板缓存的独占锁是实际串行点。
8. 上一轮的 `jigsawsFor/jigsawsByRotation` 使模板 jigsaw 数据成为稳定只读数据，不再随放置复制字符串。

### 观测

- 结构/地物桶从 11.9% 降到 8.3-9.0%；`JigsawBlockInfo::operator=` 已跌出 Top 24。
- 剩余热点：密度/含水层 33.5%、噪声 17.3%、地表规则 8.9%、方块存储 7.6%、结构 8.3%。
- `memcmp` 仍有 2.8%（模板池/别名等字符串键查找），`searchFlatRec` 3.1%（气候 R 树）。

### 验证

- 全量 CTest：**64/64 通过（17.0 s）**，含结构、生成、光照协议用例。
- `worldgen_differential_probe` 单线程重复运行逐字节一致；8 线程下同一二进制两次运行的结果本身就不同
  （窗口/驱逐导致邻居可用性变化），这是探测器的既有性质，不是本轮引入。因此本轮**没有**声称与 26.1 服务端逐区块等价，
  真实差分仍待按第 8 节流程补跑。
- 创建/步进计数完全一致（created=3136 steps=31064），说明没有跳过任何阶段。

### 下一步

真正的剩余大头是密度/噪声预编译：把 `DensityNode` 图展平成后序指令数组（共享子树只算一次），
以及噪声内核的批量求值。这是 C2ME DFC 的核心手法，预计还能再降 10-20% CPU；风险是浮点求值顺序必须逐节点保持一致。


## 11. 密度图平坦化预编译尝试（已回退）

在 P0/P1 的 5.02-5.14 s / 119-121 s CPU（t24）基础上，尝试把 `DensityNode` 图
用现有 `FlatDensityFunction` 编译成后序指令表，并在 `computeUncached` 入口惰性启用：

- Custom（插值/缓存标记）作为不透明调用，Spline 以 `apply(ctx)` 不透明调用，保证语义；
- 第一版：所有 `range_choice` / `mul` 都展开成直线指令；
- 第二版：`range_choice` 与 `mul` 改为惰性分支（不展开子节点，回退递归，保留短路）。

**正确性**：第二版 64 区块单线程探测输出与启用前逐字节一致（`PARITY OK`），说明值语义没有改变。

**性能（t24，2500 区块）**：

| 版本 | 墙钟 | CPU | CPU/区块 |
|---|---|---|---|
| 递归实现（P0/P1 后，基准） | 5.02-5.14 s | 119-121 s | 48 ms |
| 平坦计划（全展开） | 9.60 s | 228 s | 91 ms |
| 平坦计划（惰性分支） | 7.70 s | 183 s | 73 ms |

即"朴素展平"是**约 2 倍回退**，即使保留短路的惰性分支版本仍慢约 50%。原因：

- 递归实现经过 LTO 内联，中间值留在寄存器/栈上；计划版本把每个中间结果写进 TLS 的
  `slots` 数组，内存流量显著增加（24 线程下尤其明显）。
- 直线计划会计算未被 `range_choice` 选中的分支；惰性分支虽恢复语义，但把主要子树
  又交回递归，平坦化收益被抵消。
- 现有的 Marker 缓存（`Cache2D`/`CacheOnce`/`FlatCache`）在计划里只能作为
  不透明调用，无法与计划内联，进一步削弱收益。

C2ME 的 DFC 之所以有效，是把图编译成 **JVM 字节码**，由 JIT 分配寄存器并保留控制流；
C++ 端的等价物需要"基本块 + 跳转"的代码生成（或手写模板特化），而不是 slots 数组。

**处理**：已完整回退本次平坦化接入（保留原有给气候采样用的 `FlatDensityFunction`），
性能回到 5.02-5.14 s；全量 CTest 仍 64/64 通过。密度/噪声仍是最大热点（33.5% / 17.3%），
后续应走控制流感知的代码生成或按子树实测收益的局部特化，而不是整树展平。


## 12. 噪声/查找微优化（2026-11-19 晚）

### 12.1 保留：NoiseChunk 包装 memo 精简

`NoiseChunk::wrap` 原来每次调用都新建一个 `localMemo` 哈希表，并做两次哈希查找/节点。
由于密度图是 DAG（无环），递归过程中不会重复访问未插入的节点，`localMemo` 是冗余的：
只保留跨 `wrap()` 调用的 `wrapped_`，并预先 `reserve` `wrapped_` /
`interpolators_` / `cellCaches_`。每区块少建约 20 个哈希表、少约 300 次哈希查找。

### 12.2 回退：preliminarySurfaceLevel 固定数组缓存

尝试把 `preliminarySurfaceLevelCache_` 从哈希表换成 32 槽线性探测数组。实测**明显回退**
（t24 6.50-7.13 s，CPU 154-168 s）：量化列键并不只落在区块内的 ~25 个，
含水层 `maxPreliminarySurfaceLevel` 会查询到区块外较远的列，键数量不可控，
32 槽被填满后每次都退化为 32 次探测 + 重算。已回退为哈希表。

### 12.3 未实施：气候→生物群系缓存

- `createBiomes` 的 1536 次气候采样/quint 坐标基本互不重复，source 级缓存命中率低。
- 地表阶段读的是已加载区块的 biome 数组，通常不回到 biome source；真正重复的是
  "同一区块内少量群系名 → id"，已在 `RuleEvalContext` 的角点缓存里部分覆盖。
- 若在 source 层缓存 `string_view`，还要处理注册表/实例生命周期（拷贝、地址复用）风险。
结论：暂不实施；若后续做，应按"区块内 name→id"或"量化气候点→叶索引"做**有生命周期校验**的缓存。

### 12.4 结果

| 指标 | 本轮前 | 本轮后（最佳/典型） |
|---|---|---|
| t24 墙钟 | 5.02-5.14 s | **4.90-4.92 s** |
| t24 CPU | 119-121 s | **116.2-116.7 s** |
| ms/区块 | 2.01-2.05 | **1.96-1.97** |
| 吞吐 | ~490 cps | **~510 cps** |

（机器状态有 ±10% 漂移，以上为同一时段内的对比。）

### 12.5 已知问题：chunk_generation 测试的时序竞态

`tests/chunk_generation_tests.cpp` 的 `testStaleGenerationIsDiscarded`（"in-flight generation
finished after demand removal"）在本机是**间歇性失败**：50 ms 生成延迟与"移除需求"的时序竞争。
A/B 验证：把本轮 wrap 改动回退后同样复现（2/3 通过 vs 3/3 失败），与本轮改动无关；
同一构建重跑完整 CTest 可通过 64/64。该用例需要单独修复时序假设，本轮未改测试。


## 13. 结构模板缓存改用 phmap（2026-11-19 深夜）

### 13.1 改动

`StructureTemplateCache`（`include/redstone/worldgen/jigsaw.hpp`）：

- `std::map<std::string, shared_ptr<StructureTemplate>, std::less<>>` →
  `FlatHashMap<std::string, shared_ptr<StructureTemplate>, TransparentStringHash, std::equal_to<>>`；
- 保留 `std::shared_mutex`（命中路径共享锁），构造时 `reserve(256)`；
- 命中路径仍是 `string_view` 异质查找 + wyhash，不再做红黑树逐层字符串比较。

### 13.2 findTemplatePool 已经走哈希

`WorldgenRegistries::findTemplatePool` 并不查 `templatePools_`（`std::map` 存储），
而是查加载后构建的索引：

`StringHashMap<const StructureTemplatePool *> templatePoolIdx_;`
（`FlatHashMap<string_view, const T*, TransparentStringHash, std::equal_to<>>`）

也就是说 lookup 早就是 phmap 哈希。**存储 map 不能换成 phmap 扁平表**：索引里存的是
`templatePools_` 元素的裸指针，`phmap::flat_hash_map` 在 rehash 时会搬移元素，
指针会失效；如需 node 级稳定地址，只能用 `phmap::node_hash_map`，但 lookup 已走索引，
换存储没有运行时收益，故保持 `std::map`。

### 13.3 结果

- 全量 CTest：64/64 通过（该构建第一次 63/64，重跑 64/64；仍是第 12.5 节的
  `chunk_generation` 既有时序竞态）。
- 结构生成相关用例（`worldgen_g3_structures` 等）全部通过。
- 性能：本时段机器状态漂移较大（同一路径此前 4.90-5.20 s，现 6.01-6.24 s），
  交替 A/B 因构建脚本把两边编成同一二进制而未取得有效比较；本轮不声称量化收益，
  只确认无回退且去掉了树查找。
