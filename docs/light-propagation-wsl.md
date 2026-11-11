# 当前光照传播与 WSL 重采样

这里描述天空光区段索引优化之后、双 worker 拆分之前的实现。重采样用优化后二进制 `168ba8b933cff1b06233f7dd921f5c7bfcd3d0109b8ca2694cfa19b9b98d2661` 和同一份世吞投影，模拟/光照线程分别绑 WSL CPU 2/4，三组关光/双光，各 1000 刻。perf 用 397 Hz 用户态 CPU 时钟和 DWARF 调用栈，仅在初照完成后的计时窗口附近采样。采样轮包括明显的耗时异常值，故性能达标判断继续使用上次五组无采样 A/B；本次只用样本定位热点。

## 模拟线程开销

开双光的 6,030 个模拟线程 CPU 样本中，提交函数调用栈在三轮分别占 0.90%、1.02%、0.53%；接收发布约 0.07%–0.21%。显式光学比较、取属性、提交和接收合计按轮中位约 1.3%。这只是能归到相应符号的用户态样本，不能用于否定锁等待或给所有额外耗时定因。

另在仓库外副本加计时，正式窗口内每 127 次成功改块抽样一次，减去时钟读取的估计开销后，按实际改块和提交数外推。每轮 2,680,149 次有效写入，1,637,501 次提交。三轮中位数如下，单位都是每 1000 刻：

| 阶段 | 墙钟 | 说明 |
|---|---:|---|
| 比较光学属性与区段空态 | 49.7 ms | 抽样估计，三轮 43–67 ms |
| 组包和入环 | 66.3 ms | 抽样估计，三轮 64–71 ms；无满环回退 |
| 主线程接收 worker 发布 | 91.8 ms | 实测，三轮 91–115 ms；CPU 中位 6.3 ms |
| 其中获取共享锁 | 85.1 ms | 包含在上一行；三轮合计 84–108 ms，单次最大 8.7–12.5 ms |

稳态在主线程有明确可削减的等锁时间。`drainPublished` 与 worker 的取批/合并、发布共用一个锁；本次计时只定位了等待点，未分辨哪一段持锁造成每次等待。以上三类也不能解释全部开光/关光差额：主线程读方块、活塞和邻居更新等路径仍占大头，硬件缓存竞争与调度影响未定量。离线世吞没有连接，不含会话和出包成本。

## 后台热点

三轮开光共 6,084 个 worker 样本。下列百分比为每轮调用栈覆盖率的中位数，同一条栈可以同时落入多个项目，不能相加。

| 路径 | 样本占比 |
|---|---:|
| 天空光整体 | 90.0% |
| 天空增光 / 撤光 | 44.3% / 14.2% |
| 天空读光 `storedLevel` / 写光 `setStoredLevel` | 28.2% / 10.5% |
| 区块快照查询 `stateFor` / 工作区块查询 `workingLayers` | 7.2% / 5.9% |
| 快照区段查询 `LightChunkSnapshot::section`（两层） | 11.5% |
| 天空列入口重算 `computeSource` | 6.0% |
| 缺层基准 `inheritedBase` | 1.0% |
| 方块光整体 / 环记录归并 | 4.1% / 4.1% |

之前 `storedLevel` 在独立采样中占 54.5%；本次约 28%，这是样本占比变化，不是函数绝对耗时降幅。热点已移到增光循环和重复寻址：一次目标格传播中会查目标区段、读旧光、查光学状态，写入时部分查找再做一次。第二个 worker 可隔离方块光并与天空光重叠，但天空光本身仍是主要工作量，不能期待分线程单独消除门槛差距。

## 当前传播链路

1. **输入与所有权。** `World::setBlock` 写入区段和四张高度图后，比较发光强度、光衰减、面遮挡条件和区段空态。只有需要重查才提交自有光学记录：区块键、加载代、区段、格内编号、新光学属性和空态。正常写入环形队列；满环用受锁的 pending 保住最新记录。加载/权威刷新发送区块光学快照，卸载使旧代失效。worker 不访问活 World。
2. **后台取批。** 默认每个 LightService 一个 worker。它按区块/代际/区段/格合并输入，较新写入覆盖较旧写入，权威快照屏障排除其前面的旧记录。批次应用到 worker 的光学区段副本后，先跑方块光，再跑天空光；每层都是检查、撤光队列、增光队列。未加载区块是传播边界，加载或卸载时重查接触面。
3. **方块光。** 加载时从发光方块播种。改动点按当前光值与新自发光入队。增光沿六邻格传播：`候选 = 来源光值 − max(1, 目标衰减)`，来源队列值已经过时、两方块共享面的形状并集封满、或候选不比目标亮，均跳过。撤光从失效的旧值向外清除可能依赖它的较弱值；仍有效的发光源和较亮邻居进增光队列补回。多光源取最大可达值，不相加。
4. **天空入口。** 不使用四张普通高度图。每列从顶向下找第一条不透的垂直边：下方衰减非零，或上方下表面与下方上表面共同封住边界。入口及其上方是直射 15；无遮挡列可一直到下方光照边界。初照批量填各列，动态改动只对脏列重算入口（当前仍从顶向下扫）。入口上移撤掉失去的直射 15，下移新增直射 15；最低入口及相邻列入口有差异的位置引出传播。离开直射源后用普通至少减 1 的六邻格传播，受目标衰减和共享面遮挡限制。
5. **天空光层存储。** 光照范围比方块上下各多一段。已有方块/邻区 3×3×3 区段状态决定天空 raw 层是否应存储。工作层按区块内区段号直接索引，物化层 4096 格；缺层查询用同一区块最近上方已存层的 `y=0` 底面，找不到则露天为 15。缓存的是上方区段编号，底面随最新工作值读；分配、移除或重建使指向失效。跨区块边缘若源侧下方连续缺层，已衰减的水平值还会补到目标侧下方对应的已存层；撤光对称处理。无天空光维度跳过天空层传播。方块光缺层读 0，和天空缺层不同。
6. **发布与协议。** 变化的工作层变成不可变的缺席/均匀值/2048 字节 nibble 层。worker 对整批生成同一 revision，保留分层的变化区段与就绪状态。模拟线程在普通方块广播前接收已完成发布，过滤旧加载代；本身不跑传播，也不调用等待传播完成的接口，但当前接收要获取共享锁。原生服务端只使用已就绪的同代快照，初始区块包和后续更新分别取对应层，writer 负责异步编码。旧版或缺输入不能伪装成已照明；Anvil 光照读写不是这里的发布过程。

原始 perf 栈、归因、分段计时和日志：`~/.local/state/redstone-cpp/wsl-skyopt-resample-S2jBzN/`（`attribution.json`、`main-phase-timing.json`）。生产源码未因诊断改动，未运行 CTest。


## 双 worker 与分层保存（2026-11-11）

当前实现把 block 与 sky 从批次执行到持久结果都拆开：

- `LightWorkerOptions::workerCount` 默认 1。1 沿用原 light worker 顺序执行 block、sky；2 让原 worker 专责 block，并创建一个持久 sky worker。没有额外协调线程，协调者是原 light worker。无天空光维度保持有效 worker 数为 1，sky 结果缺席。
- 协调者只准备一次 owned batch 输入：光学属性、区段空态、epoch、变更位置和固定影响域。两个层各自从同一 immutable 基线浅拷贝 COW 工作区；只有本层 section 被写入，因此不共享 `WorkingChunk`、`mutableSections` 或可写 `LightSectionSnapshot`。
- `SkyLightPropagator` 的 raw 资格域由批次几何输入一次确定，不再从 block 阶段扩充后的 working 推导。这样线程调度不会改变 sky raw Missing、最近上方底面、空段补撤光资格。
- 两结果到齐后生成同 batch revision、同 lifecycle epoch 的 `LightChunkSnapshot`。信封内另有独立 `blockLayerSnapshot` 和 `skyLayerSnapshot`；主线程可用 `latestLayerChunk` 取单层 section，协议编码也按层读取。两层 section 对象不通过写同一个信封更新。
- 环境/时间造成的天空亮度变化是读取端衰减，不改 raw sky 层，也不提交传播任务。没有方块几何变化时不会周期性重算天空光。
- 满环回退、owned 输入、旧 epoch 拒绝、同代权威输入屏障、不可变发布和关停 drain 保持原路径。若一层抛错或阻塞，协调者不会发布已完成层的半批；sky 失败写 worker error，关闭时先排空并 join 原 worker，再停止 sky worker。

命令行：

- native server：`--light-workers 1|2`。
- tick benchmark：`--light-workers 1|2`，单层可选 `--block-affinity`、`--sky-affinity`；`--light-affinity` 保留为单 worker/block worker 的兼容别名。
- 双 worker 的 process mask 必须同时覆盖 main、block、sky 三个 CPU。本机实测 main/block/sky 为 WSL CPU 2/4/6，mask 分别为 `0x4`、`0x10`、`0x40`；`lscpu` 显示三者 core 不同且均在线。

固定世吞五组 × 1000 刻四臂交错（历史基线、候选关光、候选单、候选双）结果：

| 相对历史基线 | CPU 配对中位 | 平均刻配对中位 | P95 配对中位 |
|---|---:|---:|---:|
| 候选单 worker | +10.247% | +12.664% | +10.031% |
| 候选双 worker | +10.521% | +12.769% | +11.123% |

双相对单为 CPU **+0.492%**、平均刻 **+0.197%**、P95 +0.423%；稳态天空光仍占主要 worker CPU，因此并行没有在 1000 刻端到端中带来稳定增益。双 worker 初照为 5.717–6.169 s，单 worker 为 6.701–7.102 s。双模式每轮实际接收 sky/block sections，rejected 0，排空后 pending 为 0；所有正式组均无 >50 ms 刻。

正式证据：`/home/zhddsj/.local/state/redstone-cpp/wsl-light-split-measure-20261111/measure5/`。summary SHA-256 `31f29ec3c6dc251092861354153a647ecf1e9d7c9cc6d2b13ec611429941903c`；候选 benchmark SHA-256 `f89e172ff92eae7073f4dcb3ab61e73041bd2c0f2c0cbea4c4a3e19028041978`。


## 增量天空直射高度图（2026-11-11）

`skyColumnSources` 仍是 sky worker 独占的每 chunk 256 列“最低直射 Y”。初照、同代完整刷新、缺失缓存初始化继续从顶向下做完整列扫描；普通 block 光学变化不再每列从世界顶部扫描。

增量更新先按最终 batch 快照把脏位置按列分组并排序，再统一更新高度，最后才对所有源变化列执行 `seedColumn`。因此同列同批多改、多列同批和邻列高度依赖都读取最终几何，不会在高度尚未稳定时 seed。每个变化格只重查两个候选边：

- `y`：`y+1` 与 `y` 之间；
- `y-1`：`y` 与 `y-1` 之间。

新增阻挡若高于旧直射起点则直接上移。旧最高边仍阻挡时保留；旧最高边失效时从该边下一条边向下回扫。回扫会包含同一批次刚加入的较低阻挡，避免漏掉“低处新增 + 原最高移除”的组合。无阻挡 sentinel 保持 `minBlockY-1`。

回扫按 section 从高到低读取。只有 `section.empty`、uniform palette、supported、dampening 0 且无遮挡 shape 同时成立时才跳过整段；进入空段的上边界和离开空段的下边界仍通过前一个状态检查，不丢跨 section 的上下面对。unsupported 仍作为不可证明的遮挡，和原 `computeSource` 一致。

验证夹具用独立逐格整列参考算法核对全部 256 列，覆盖同列同批多改、多列同批、非最高阻挡、跨 section 的 `y`/`y-1` 候选、最高阻挡移除回退、同批低处新增加高处移除、sentinel、顶/底边、同代刷新、卸载重载、负 y 与源缓存缺失。协议测试、官方 243 sky + 72 raw-layer 和 225 block golden 均未改 golden 并通过。

使用修改前封存 binaries：

- before：`/home/zhddsj/.local/state/redstone-cpp/wsl-sky-height-incremental-20261111/before/redstone_tick_benchmark`，SHA-256 `f89e172ff92eae7073f4dcb3ab61e73041bd2c0f2c0cbea4c4a3e19028041978`。
- after SHA-256 `47cb2d6b90f0a42a3ca18977b30dd9f978cca2a2877328f25b826c4a1e070832`。
- 五组交错、每组 1000 tick，另 250 tick warmup 和独立指纹轮。main/block/sky 实际 CPU 2/4/6，mask `0x4`/`0x10`/`0x40`；单 worker process mask `0x14`，双 worker `0x54`。

| 同 worker 相对修改前 | CPU 配对中位 | mean tick 配对中位 | sky worker CPU 配对中位 |
|---|---:|---:|---:|
| 单 worker | +4.758% | +4.277% | −0.542% |
| 双 worker | +3.128% | +7.488% | +5.624% |

The measure3 numbers predate the review fix and compare only the already-lit before/after binaries with the same worker mode. They do not establish the original no-light <10% goal, and host causality is not confirmed. Initial wall changed -23.747%/-21.463%; initial sky-worker CPU changed -26.662%/-23.817%; steady double-worker sky CPU was +5.624% and is not an improvement claim. All formal runs had real sky/block reception, rejected=0 and final pending=0; one or two isolated >50 ms ticks occurred and were retained.

证据目录：`/home/zhddsj/.local/state/redstone-cpp/wsl-sky-height-incremental-20261111/measure3/`，summary SHA-256 `9a6adde67ec6900c43220651f6f58ab8b4e9403cd74d9d995f387e54be6cb9d4`。

### Review-fix correction

- findSourceBelow keeps topState at the upper cell of the invalidated edge, but starts the first bottom cell at topY-1 and derives its section/local coordinates from that cell. The lower cell can no longer form a fabricated self-edge.
- Added the official extended piston facing up case (dampening 0, full DOWN face, empty UP face) at y15/y16. Removing y16 must fall back to source 15 instead of the fabricated 16.
- sourceEmptySection now requires actual DOWN and UP face rectangles to be absent; the emptyShape flag alone is not accepted as proof.
- The performance table is retained as a pre-review-fix record. No new formal benchmark was run for this correctness fix, and the original no-light <10% goal remains unestablished.


## Final stage 1/2 layout (2026-11-12)

The opening sections above are historical. The current implementation uses independent block/sky envelopes and mutable atomic nibble pages:

- World::setBlock submits the 16-byte {x,y,z,stateId} record. The coordinator owns the only compact optical geometry.
- One persistent light worker runs block and then sky sequentially. Each layer has its own persistent result view and publishes as soon as its own propagation completes; there is no shared batch revision or combined-section publication gate.
- A layer descriptor is a one-byte atomic state plus a page pointer. Uniform uses state 0x10 | value and allocates no page. Mixed owns one shared atomic<uint8_t>[2048]; even cells use the low nibble and odd cells the high nibble. Missing remains represented by an absent layer, separate from stored uniform zero.
- Worker and main thread each own their chunk/section headers. Publication copies only these small headers and descriptors; both retain the same atomic page. Once a descriptor is published, its topology and page pointer are not changed. Uniform/Mixed conversion installs a new descriptor, while Mixed-to-Mixed batches update the same page in place.
- Propagation work layers share the same descriptor/page. LightValues moves only shared_ptr; there is no 4096-byte unpacked workspace.
- changedSections comes from actual worker writes. A mutable-page revision with the same descriptor still carries its dirty sections. Network state stores only per-layer epoch, revision and dimensions; it never compares old light bytes.
- Initial chunks wait for the required block and sky views. Incremental packets may contain only one layer. Main current views and callback enqueue only retain shared pointers.
- World::getBrightness composes max(block, sky - skyDarken) at query time and stores no combined array.

Remaining large copies:

1. Chunk load and authoritative refresh convert live World::Chunk::Section directly into GeometrySection on the world thread. There is no LightRawSection copy and no queued second geometry representation. A nonuniform section allocates one unique 4096-byte uint8 optical-class array; a uniform section allocates none.
2. Conversion creates only a temporary palette-sized uint8 map from World palette indices to optical classes. It is released after the section has been written.
3. Uniform-to-Mixed propagation allocates its 2048-byte atomic page once. Mixed batches do not recopy it. Wire encoding necessarily reads the 2048 atomic bytes into the outgoing packet.

Validation for the final tree is in /home/zhddsj/.local/state/redstone-cpp/wsl-stage1-final-20261112/. No CTest was run. The official golden files were not changed.


## Single worker and SPSC result handoff (2026-11-12)

- The selectable second sky worker has been removed. One persistent worker executes block propagation and publishes it, then executes sky propagation and publishes that independently.
- Geometry input is a 10 MiB fixed SPSC ring. A full ring drops the newest record and increments droppedGeometryTasks; the former overflow map and full-ring mutex fallback are gone.
- Layer results use a fixed 4 MiB SPSC ring. The worker publishes the worker-owned header shared pointer; the main thread pops, constructs its own small header, updates latestBlock/latestSky, and invokes callbacks. Result consumption does not take the input-state mutex that was responsible for the long tick stalls.
- Main cache lookups and result draining are main-thread-only. Per-layer lifecycle validation uses the main-thread lifecycle mirror, so a worker input merge cannot block publication consumption.
- Native server and benchmark accept --light-workers 1 only; 2 is rejected. Sky shares the sole worker affinity.


## Section-local propagation priority (2026-11-12)

- Block and sky propagators now maintain separate FIFO queues for the active SectionPos3 and for work entering other sections. Within each decrease/increase phase, queued work for the active section runs to exhaustion before another section is selected. Same-section tasks are appended to the active FIFO, so this remains breadth-first inside the section.
- Geometry lookups distinguish uniform GeometrySection: stateAt, seedChunk, sourcePropertyAt, and sky propagation use the section's single class directly instead of calculating a local light index and reading a cell array.
- Functional validation passed: light_tests, light_protocol_tests (206 packets), sky golden (243 + 72 raw flags), block golden (225 observations).
- One diagnostic 1000-tick run with masks 0x4 main and 0x10 worker measured mean 3.407 ms, median 2.451 ms, P95 7.221 ms, max 41.340 ms, over50=0. It accepted 2,379,870 tasks and explicitly dropped 31,746 on the 10 MiB ring. The same-run unlit control was mean 2.899 ms, median 2.113 ms, P95 6.292 ms. This is one diagnostic pair, not the formal five-group result.


Final focused performance pairs for the BFS-cache/uniform build: five interleaved 1000-tick lit/unlit pairs yielded paired median deltas of CPU +10.664%, mean +10.670%, median +11.550%, P95 +7.376% and max +10.982%. All runs had no tick over 50 ms. Four lit runs dropped zero geometry tasks and one dropped 751. This is still slightly outside the CPU/mean <10% target.
