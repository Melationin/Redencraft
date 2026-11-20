# 区块生成调度：需求与现状设计说明

**用途**：给独立分析者一份不需要通读全部源码就能评估的结构说明。
**描述对象**：当前**工作区**代码（src/world_chunk_manager/、src/worldgen/worldgen_scheduler.cpp、src/chunk_map.cpp、src/chunk_lifecycle.cpp、src/engine/chunk_storage.cpp）。工作区包含未提交改动，本文按工作区实际内容描述，与 HEAD 不完全一致。
**不在本文范围**：光照传播算法、地表规则、结构拼接、存档格式细节（仅在契约处提及）。

---

## 0. 一句话概括

加载票 → 生命周期控制器 → **生成层（WorldChunkManager 内的第二套队列）** → **调度器（WorldGenScheduler，真正的依赖 DAG 调度）** → 回装。生成层与调度器之间存在"两个调度者、三套数据结构、一次静默丢弃"的结构性重叠，本文的全部差距都源自这个重叠。

---

## 1. 术语与常量

### 1.1 加载等级（include/redstone/tickets.hpp:245-253）

| 常量 | 值 | 含义 |
|---|---|---|
| 实体刻（SimulationTrackerMax） | 31 | 可刻实体 |
| 方块刻 | 32 | 可刻方块 |
| FullChunkLevel | 33 | 完整区块（安装进 World） |
| MaxLevel | 44 | 外围生成缓冲最外层 |
| NoTicketSource | 45 | 无需求 |
| RadiusAroundFullChunk | 11 | MaxLevel = 33 + 11 |

数值越小优先级越高。票向 8 邻域扩散每格 +1，自然形成依赖缓冲环。

**驻留需求** = 等级 <= 33（生成到 Full 后安装进 World）。
**缓冲需求** = 等级 34..44（只推进到对应阶段，留在生成缓存里供邻居复用）。

### 1.2 生成阶段（include/redstone/tickets.hpp:229-242，kChunkStatusCount = 12）

| 索引 | 阶段 | 写入半径 | 备注 |
|---|---|---|---|
| 0 | Empty | 0 | |
| 1 | StructureStarts | 0 | 累积依赖半径 8 |
| 2 | StructureReferences | 0 | |
| 3 | Biomes | 0 | |
| 4 | Noise | 0 | fillFromNoise，依赖邻居 Biomes（半径 1） |
| 5 | Surface | 0 | 依赖 Biomes（半径 1） |
| 6 | Carvers | 0 | |
| 7 | Features | **1** | 唯一写入半径 > 0 的阶段，进九色队列 |
| 8 | InitializeLight | 0 | |
| 9 | Light | 0 | 依赖 InitializeLight（半径 1） |
| 10 | Spawn | 0 | 依赖 Biomes（半径 1） |
| 11 | Full | 0 | 终态 |

### 1.3 等级与阶段映射（src/worldgen/chunk_status.cpp:362-371）

generationStatusForLevel(level)：level <= 33 返回 Full；34..44 返回按 Full 累积依赖半径反推的外围阶段；> 44 返回 nullopt。

**重要**：34..44 覆盖一直到 Noise 的阶段，因此**缓冲需求同样会调用 fillFromNoise**。这一点在 §4.1 的错误判据里是决定性的。

---

## 2. 需求清单

| 编号 | 需求 | 现状 |
|---|---|---|
| **R1** | 等级驱动分流：<=33 生成到 Full 并安装；34..44 只推进缓存；>44 无需求 | 满足 |
| **R2** | 安装仲裁：只有"当前生命周期代际 + 状态仍为 Reading + 仍有 demand"才安装；否则丢弃且不泄漏 | 满足 |
| **R3** | 邻居依赖：阶段开始前邻居必须达到规定状态；不满足则等待并被唤醒 | 满足 |
| **R4** | 写入互斥：Features 阶段必须 3x3 坐标互斥（九色波前），其余阶段可并行 | 满足 |
| **R5** | 持久化：安装前序列化完整 NBT 作为后续增量存盘模板；裁剪时把非空 Proto 写盘 | 满足 |
| **R6** | 资源上界：同时在飞的生成目标有上限，生成缓存不随票环线性膨胀 | 满足（靠批次 32+32） |
| **R7** | 优先级：近处优先、阶段深的优先、且跨批次单调 | **部分**（两套优先级并存，见 D3） |
| **R8** | 撤销：需求消失后停止推进并丢弃结果，不得安装 | **部分**（见 D2） |
| **R9** | 可观测：统计能回答"请求多少、派出多少、完成/失败/取消多少" | **不满足**（见 D1） |
| **R10** | 线程契约：调度 worker 只做只读执行；安装、光照提交、保存入队只在主线程 | 满足 |
| **R11** | 关停：可排空在飞工作并报告未落盘区块 | 满足 |

---

## 3. 现状结构（as-is）

### 3.1 分层图

    TicketSourceManager(玩家视距/强制加载)
            | ChunkLevelChange
            v
    WorldChunkManager::applyTicketUpdate            <- 唯一需求入口
            |-- controller_->applyLoadingChanges()   -> 生命周期状态机
            |-- cancelGeneration() / requestBufferGeneration()
            v
    生成层 generationLoop()（1 线程，manager 内）
            |  <=32 驻留 + <=32 缓冲 --> ChunkBatchRequest
            v  scheduler->ensureStatusBatch(...)   【阻塞到整批空闲】
    WorldGenScheduler（N worker）
            |  ChunkHolder 依赖 DAG + 分桶优先队列 + 9 色波前
            v
    CompletedGeneration --> 主线程 pump(): processCompletedGeneration()
            v
    controller_->completeGeneratedChunk() -> installGenerated 命令 -> I/O worker
            v
    GenerationReady -> 主线程 drain() -> installLoaded()
            v
    World::installGeneratedProtoChunk() -> ChunkHolder::promoteProtoToWorld() -> World

### 3.2 各层职责

| 层 | 类型 | 线程 | 职责 | 代码位置 |
|---|---|---|---|---|
| 需求源 | TicketSourceManager | 主线程 | 等级计算、8 邻域扩散、玩家中心 | include/redstone/tickets.hpp |
| 需求分发 | WorldChunkManager::applyTicketUpdate | 主线程 | 等级边 -> 控制器 + 生成层 | lifecycle.cpp:291-337 |
| 生命周期 | ChunkLifecycleController | 主线程 + 1 I/O worker | 每坐标 Entry{epoch,state,demand} 状态机；读盘/生成/保存编排；安装仲裁 | src/chunk_lifecycle.cpp |
| 生成层 | WorldChunkManager | 1 专用线程 | 需求排队、批次组装、优先级、完成记账、NBT 模板编码、缓存裁剪 | world_chunk_manager/lifecycle.cpp:1218-1579 |
| 调度器 | WorldGenScheduler | N worker（默认 hardware/2，夹 1..8） | holder 状态机、依赖展开、分桶优先队列、九色波前、完成通知 | src/worldgen/worldgen_scheduler.cpp |
| 区块表 | ChunkMap | 共享 | holders_（phmap::parallel_flat_hash_map，6 分片 shared_mutex）+ 主线程 worldIndex_ | src/chunk_map.cpp |
| 安装 | World::installGeneratedProtoChunk | 主线程 | promoteProtoToWorld、区段移交、高度图、索引登记 | engine/chunk_storage.cpp:438- |

### 3.3 数据结构

**生成层（manager，generationMutex_ 保护）**

| 名字 | 类型 | 语义 | 更新点 |
|---|---|---|---|
| residencyGenerationJobs_ | deque<GenerationJob> | 待派发驻留需求（FIFO）；同坐标可有多条（不同 epoch） | 入队 requestChunkGeneration，出队 generationLoop |
| bufferGenerationJobs_ | map<pos,int> | 待派发缓冲需求（pos->等级） | 入队 requestBufferGeneration，删除 cancelGeneration/出队 |
| residencyGenerationQueued_ | map<pos,epoch> | 驻留需求的"仍然有效"交叉校验表 | 入队写入，cancelGeneration(true) 删除，出队时校验 |
| completedGeneration_ | deque<CompletedGeneration> | 已完成待安装（含 NBT 模板与 LifecycleLoadedChunk） | generationLoop 写，processCompletedGeneration 换出 |
| generationStats_ | ChunkGenerationStats | 见 §3.8 | |
| generationKeepCenters_/Radius_ | vector/int | 裁剪中心（玩家位置）与半径 viewDistance+11+2 | publishGenerationKeepCenters，每次 pump() |
| evictedGeneratedChunks_ | vector<pos> | 被调度器裁剪掉的坐标，主线程据此注销光照登记 | generationLoop 写，unloadEvictedGeneratedLight 换出 |

GenerationJob{ position, loadingLevel, epoch, residency, gameTime }；
CompletedGeneration{ position, epoch, residency, ok, detail, loaded, root }。

**调度器**

| 名字 | 类型 | 语义 |
|---|---|---|
| ChunkHolder::GenTaskState | 每 holder 一份 | currentStage、explicitTarget、dependentTarget（effectiveTarget=max）、execState、waitersByStage[12]、pendingDeps、generationId、distBucket、stagePinnedDeps |
| parallelQueue_ | PriorityBucketQueue | 396 桶 = distBucket(0..32) x 12 阶段，bitset 7 words 取最高优先级 |
| coloredQueues_[9] | PriorityBucketQueue[9] | 同上，按 holder 颜色（3x3 奇偶）分 9 条 |
| activeHolders_ | atomic<size_t> | 已被武装且未结算的 holder 数；waitUntilIdle() 的唯一判据 |
| chunksCreated_ / stepsExecuted_ / inlineSteps_ / parallelDispatches_ / coloredDispatches_ / holdersEvicted_ | atomic | 调度器统计 |

ChunkHolder 同时持有 ProtoChunk 或 WorldChunk（shared_mutex chunkMtx_ 保护），generationRefCount 表示被邻居 pin 的次数。

### 3.4 正向时序（磁盘缺失 -> 安装）

| # | 执行者 | 动作 |
|---|---|---|
| 1 | 主线程 | addPlayer -> applyTicketUpdate -> controller_->applyLoadingChanges |
| 2 | 主线程 | 控制器 updateDemand：新建 Entry{demand=true,state=Reading} -> beginRead 入命令队列 |
| 3 | I/O worker | 读盘 -> 缺失 -> Result{GenerationNeeded} |
| 4 | 主线程 | pump() -> controller_->drain() -> requestGeneration 回调 -> requestChunkGeneration 入队 + ++requested |
| 5 | 生成线程 | generationLoop 出队 <=32 驻留（先过滤+按距离排序）+ <=32 缓冲（按等级） |
| 6 | 生成线程 | 组装 targets（驻留=Full，缓冲=等级映射阶段，取 max）-> ensureStatusBatch(requests, anchor) |
| 7 | 调度器 | getOrCreateHolder + explicitTarget 提升 + execState=WaitingDeps + activeHolders_++ |
| 8 | 调度器 | armHoldersIterative：展开依赖、pin 邻居、注册 waitersByStage、必要时武装邻居 |
| 9 | worker | 按 (distBucket, stage) 取 ReadyEntry -> 校验 generationId -> 连续内联无等待阶段 -> runSingleStage |
| 10 | worker | Features 走对应颜色队列（3x3 互斥）；完成后 notifyWaiters 递减 pendingDeps |
| 11 | worker | currentStage == effectiveTarget -> execState=Idle -> releaseActiveHolder() |
| 12 | 生成线程 | waitUntilIdle() 返回 -> 逐驻留作业 findHolder -> protoShared -> encodeProtoChunkToNbt -> completedGeneration_，++completed/++failed |
| 13 | 主线程 | pump() -> processCompletedGeneration -> controller_->completeGeneratedChunk -> installGenerated 命令 |
| 14 | I/O worker -> 主线程 | GenerationReady -> drain() -> handleLoadReady -> installLoaded -> promoteProtoToWorld -> World |

同时进行的生成期光照交接：worker 投递 GeneratedLightWork 到 GeneratedLightQueue；主线程 pumpGeneratedLightWork 提交给 LightService 并把 {submit,compute} 记入 generatedLightRequests_；安装时把 lightSubmissionGeneration 塞进载荷以跳过重传。

### 3.5 撤销时序（需求消失）

| # | 执行者 | 动作 | 备注 |
|---|---|---|---|
| 1 | 主线程 | removePlayer -> applyTicketUpdate -> controller_->applyLoadingChanges | |
| 2 | 主线程 | 控制器 updateDemand：Reading 条目 -> epoch++、state=Absent、invalidate+drop 命令、**从 entries_ 删除** | 删除后任何迟到结果都装不上（R2 满足） |
| 3 | 主线程 | cancelGeneration(pos, true)：删 bufferGenerationJobs_[pos] 与 residencyGenerationQueued_[pos] | **不删 residencyGenerationJobs_ 里的条目**（不一致点 1） |
| 4 | 生成线程 | 出队时查 residencyGenerationQueued_，查不到 -> continue | **静默丢弃，无任何计数**（不一致点 2） |
| 5 | 生成线程 | 若该批已在飞：跑完整批 -> ++completed，随后安装被拒 | 这条路径本身正确，但只在"已派发"时成立 |

另有一处同型过滤器：驻留队列超过 32 时按 residencyGenerationQueued_ 预过滤（lifecycle.cpp:1333-1359），同样静默丢弃。

### 3.6 调度器内部

- **队列键**：idx = distBucket*12 + (11 - stage)，distBucket 由 clamp(chebyshevDist(pos, anchor), 0, 32) 得到。pop() 用 countr_zero 找最小索引 = 最近 + 最深阶段。
- **九色波前**：scheduleSpecForStage 只有 Features 返回 writeRadius=1；其余全部进 parallelQueue_。彩色队列按 activeColor_ 轮转，colorTurnDispatchLimit=64 防单色饿死；activeColorInFlight_==0 时才允许换色。
- **状态机**：Idle -> WaitingDeps -> Queued -> Running -> (Idle | Queued)。generationId 每次入队自增，worker 出队时校验，防止旧条目执行新任务。
- **armHoldersIterative 的五个出口**：非 WaitingDeps（continue）、dependencyFailed（结算+释放）、已达标（结算+释放）、依赖不足（回 WaitingDeps 重武装）、依赖满足（入队）。
- **activeHolders_ 不变量**：每武装一个 holder +1，结算时 -1；waitUntilIdle() 只认 ==0。**任何漏释放都会造成永久挂起**，因此这是最需要复核的契约（见 Q2）。
- **explicitTarget 单调**：只有 if (gen.explicitTarget < tgt) gen.explicitTarget = tgt;，**从不下降**。调度器自身没有"取消"的表达能力，撤销只能由外部队列近似。
- **裁剪**：evictIdleOutsideRadius(centers, keepRadius) 只淘汰满足 evicted==false && kind==Proto && execState==Idle && pendingDeps==0 && currentStage>=effectiveTarget && generationRefCount==0 && generationUserCount==0 的 holder，淘汰前把非空 Proto 写盘（worldgen_scheduler.cpp:940-992）。裁剪由生成线程在批次边界调用（每 4 批或收到测试触发的 trim 标志）。

### 3.7 线程与锁

| 锁/原语 | 保护对象 | 持有者 |
|---|---|---|
| generationMutex_ / generationReady_ | 生成层三套队列、completedGeneration_、evictedGeneratedChunks_、generationStats_、keep 中心 | 主线程 + 生成线程 |
| queueMutex_ / queueCv_ | 调度器两条队列与颜色轮转 | 所有 worker + 生成线程 |
| idleMutex_ / idleCv_ | activeHolders_ 归零通知 | worker + 生成线程 |
| ChunkHolder::gen.mtx | GenTaskState 全字段 | worker + 生成线程 + 主线程（promoteProtoToWorld） |
| ChunkHolder::chunkMtx_ | 持有的 Proto/World 与区段 resize | 同上 |
| ChunkMap::holders_ | 6 分片 shared_mutex | 全体 |

**主线程绝不直接触碰调度器内部**；唯一接触点是 scheduler() 返回的指针，且只用于 ensureStatusBatch（生成线程调用）与 evictIdleOutsideRadius（生成线程调用）。

### 3.8 统计语义（ChunkGenerationStats）

| 字段 | 含义 | 现状问题 |
|---|---|---|
| requested | 驻留请求入队次数 | **按"入队"计数，同坐标可重复计**（新 epoch 会再 push 一条） |
| completed | 驻留作业随批次成功返回 | 只在批次返回后加 |
| failed | 驻留/缓冲作业失败 | 批次级 failed += bufferJobs.size() |
| bufferRequests / bufferCompleted | 缓冲需求请求/批次完成 | 缓冲作业数，非坐标数 |
| cachedChunks / evicted | 调度器 heldChunkCount() / 裁掉的数量 | 采样值 |

**没有 dispatched、没有 cancelled**：走 §3.5 第 4 步丢弃的请求不留任何痕迹，requested == completed + failed 不成立。

---

## 4. 现状与需求的差距

### D1 — R9 不成立：请求会被静默吞掉（本次已实测）

requested 只加不减；丢弃路径（lifecycle.cpp:1336-1342、1364-1367）无计数。实测一次失败运行的终态：

    requested=81  completed=0  failed=0  buffer=880/64  cached=212  managed=0
    sched: created=212 steps=942 held=212 err=<空>
    center(0,0) filled=0   distinctFilled=102

81 个驻留请求全部消失，completed 与 failed 都是 0，从外部无法区分"还在排队""被丢弃""正在生成"。

### D2 — R8 只部分成立：丢弃点与登记点分离

条目留在 residencyGenerationJobs_，有效性存在 residencyGenerationQueued_，出队时才比对；比对失败就 continue。后果：

1. 需求撤销后，已排队的工作仍占用队列与去重判断（同坐标新 epoch 会再入队一条，旧条目变成垃圾）；
2. 撤销只能按"是否已派发"分成两种行为，且两种行为都不计数；
3. 无法表达"正在生成但已无人需要 -> 尽快停"：调度器 explicitTarget 单调，"停"只能靠"跑完整条链再丢结果"。

### D3 — R7 部分成立：优先级实现了两遍

生成层按"到玩家中心距离"排序 deque；调度器再按 distBucket 排序。而 distBucket 来自**本批 anchor**（residencyJobs.front()），批次之间不保证单调。两套优先级互相覆盖。

### D4 — R6 与 R8 耦合：批次阻塞是撤销粒度

ensureStatusBatch 内部 waitUntilIdle()，必须等该批所有被武装 holder 归零才返回。实测一批 32 个目标会膨胀到 212 个 holder，因此撤销/安装的有效粒度都是"批次"，最粗可达数百个 holder 的完成时间。

### D5 — 同一事实三处表达

驻留需求同时存在于 residencyGenerationJobs_（条目）、residencyGenerationQueued_（有效性与 epoch）、bufferGenerationJobs_（互斥删除）。任何一处忘记更新都会产生"幽灵需求"或"丢失需求"。

### D6 — 缓冲队列 O(n)

bufferGenerationJobs_ 每批线性扫最小值；实测该表有 880 条，每批取 32 -> 每批全扫。

### D7 — 等级到阶段转换两次

GenerationJob 存 loadingLevel，组批时才 generationStatusForLevel；而 requestBufferGeneration 的调用点已经知道等级。驻留侧又固定硬编码 Full。

### D8 — 失败重试污染批次记账

batchOk==false && requests.size()>1 时逐驻留作业单独 ensureStatus 重试；同时批次级 failed += bufferJobs.size()。单个作业失败会同时影响"批次失败计数"和"逐作业重试"两条语义。

### D9 — 清理路径双份

close() 与 stopGenerationThread() 各自清理队列与 disableWorldGen()。

---

## 5. 不得改变的行为（修改约束）

| 编号 | 约束 | 原因 |
|---|---|---|
| **C1** | 安装仲裁必须仍由 ChunkLifecycleController（epoch + Reading + demand）裁决 | R2；也是"迟到结果不安装"的唯一保证 |
| **C2** | 安装前必须序列化完整 NBT 作为模板；裁剪时必须把非空 Proto 写盘 | R5；后续增量保存依赖该模板 |
| **C3** | 阶段顺序、heightmapsAfter 掩码、Carvers 前后高度图切换点不变 | 26.1 对齐 |
| **C4** | Features 的 3x3 九色互斥不变 | R4；否则结构写入会串位 |
| **C5** | 裁剪判定条件（Idle、pendingDeps==0、达标、无 pin）不变 | 防止淘汰在飞或仍被依赖的 holder |
| **C6** | ensureStatusBatch / ensureStatus 的阻塞语义不变 | benchmark 与既有测试依赖 |
| **C7** | 主线程不得直接驱动调度器内部；安装/光照提交/保存入队仍在主线程 | R10 |
| **C8** | 生成期光照的"worker 投递 -> 主线程提交"两段式不变 | LightService 的主线程契约 |

---

## 6. 供分析者回答的问题

- **Q1** 生成层的排队职责能否整体并入调度器？调度器已有 explicitTarget/dependentTarget/优先级队列，生成层实际只提供"准入窗口 + 批次阻塞 + 完成队列"。
- **Q2** activeHolders_ 的释放点是否完备？executeHolder 的早退分支（execState != Queued || generationId 不符）与 armHoldersIterative 的非 WaitingDeps 分支都不调用 releaseActiveHolder()。请判定这两处是否构成漏计数（若成立，waitUntilIdle() 会永久挂起）。
- **Q3** 若让 explicitTarget 可下降（真正的取消），需要哪些配套？已 Running 的阶段是否允许跑完（建议允许），currentStage 已超过新目标时如何结算，waitersByStage 里等待该 holder 的邻居如何通知。
- **Q4** 准入窗口若不靠批次，应该用什么表达？候选：dispatched 集合大小上限、distBucket 窗口（只武装距中心 <=R 的目标）、令牌桶。
- **Q5** 完成通知怎么做最不容易漏？候选：调度器维护 completedHolders_ 队列由主/生成线程 drain；或 worker 在 holder 达标时回调。需处理"已完成但未 drain 就被裁剪"的窗口。
- **Q6** 优先级归一：以玩家中心为全局 anchor（而不是每批第一个作业）是否足够？多玩家时取最近中心还是按票权加权？
- **Q7** 统计应如何定义才能自洽？建议 requested == completed + failed + cancelled，并明确 dispatched 的语义（进入调度器即计，还是开始执行才计）。
- **Q8** 缓冲需求是否值得保留？它当前的真实用途是"邻居依赖的预热缓存"；若调度器已按依赖半径自动展开邻居，则缓冲需求可能只应作为"预取提示"而不是独立目标。
- **Q9** 如果要一次做完，删除哪些结构收益最大？当前候选：residencyGenerationQueued_（可被统一需求表取代）、GenerationJob.loadingLevel（可存阶段）、批次过滤器两处。

---

## 7. 复现与观测

### 7.1 已知失败现象

build/chunk_generation_tests 的 testStaleGenerationIsDiscarded 间歇失败，失败断言恒为 "in-flight generation finished after demand removal"（tests/chunk_generation_tests.cpp:389-391）。本机连续运行实测失败率约 7/8、11/12。

### 7.2 判据（已验证）

- 失败时**不是死锁**：gdb 栈显示调度器全部 worker 停在 workerLoop 的队列等待、生成线程停在 generationReady_.wait、生命周期与 I/O worker 空闲；stepsExecuted/created/held 二次采样 1.5 s 内不变。
- 根因判据：失败运行中**中心区块从未进入 fillFromNoise**（center(0,0) filled=0，另有 102 个区块填充，全部来自缓冲等级），说明驻留作业在派发前就被丢弃。
- 门槛缺陷：测试等到 generator->started > 0 才撤票，而该计数器统计任意 fillFromNoise——§1.3 的等级映射使**缓冲需求也会触发它**，因此该门槛不能证明"驻留生成在飞"。

### 7.3 命令

    cmake --build build --target chunk_generation_tests -j 8
    for i in $(seq 1 8); do ./build/chunk_generation_tests >/dev/null 2>&1 && echo ok || echo FAIL; done
    gdb -batch -ex run -ex "thread apply all bt 18" ./build/chunk_generation_tests

---

## 8. 代码位置索引

| 主题 | 位置 |
|---|---|
| 等级常量 | include/redstone/tickets.hpp:229-253 |
| 阶段枚举与元数据 | include/redstone/worldgen/chunk_status.hpp:20-60、src/worldgen/chunk_status.cpp:50-81 |
| 等级与阶段映射 | src/worldgen/chunk_status.cpp:362-378 |
| 需求入口 | src/world_chunk_manager/lifecycle.cpp:291-337 |
| 生命周期状态机 | src/chunk_lifecycle.cpp:393-465（updateDemand）、573-607（生成完成/失败仲裁） |
| 生成层 | src/world_chunk_manager/lifecycle.cpp:1218-1579 |
| 统计结构 | include/redstone/world_chunk_manager.hpp:42-50 |
| 生成层私有成员 | include/redstone/world_chunk_manager.hpp:490-503 |
| 调度器接口 | include/redstone/worldgen/worldgen_scheduler.hpp:41-155 |
| 调度器实现 | src/worldgen/worldgen_scheduler.cpp（队列 79-121、arm 336-478、enqueue 480-497、worker 499-590、execute 590-734、notify/wait 816-876、裁剪 900-992） |
| 安装 | src/world_chunk_manager/residency_persistence.cpp:77-、src/engine/chunk_storage.cpp:438- |
| 主线程泵 | src/world_chunk_manager/lifecycle.cpp:383-406 |
| 失败测试 | tests/chunk_generation_tests.cpp:376-397 |
