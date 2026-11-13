# W-3c2 有预算的在线保存：reviewer 验收计划

在 W-3c1 通过后领取；当前文档仅供准备，不与 W-3c1 coder 同时修改共享模块。

- Java 26.1 `../net/minecraft/server/level/ChunkMap.java:214` 在 setChunkUnsaved 时向有序集合加坐标，:497 的 eager 路径每次最多 20 次保存、active writes <128、尚有可用时间；:714 检查可访问性、ready-for-saving、FULL 和 unsaved，成功安排后给该 chunk 10000ms cooldown。保留这些可观察语义，单 I/O worker 按实测可用更紧的预算，并写明不同处。绝不在 main 等待压缩或磁盘。
- 当前 `WorldChunkManager::pump` 每轮/每连接扫描全部 managed，且每个 chunk 会查询状态、扫描 runtime 持久化 blocker。用脏版本、需求边和明确的 I/O 完成通知组织局部候选。避免无变化的 setPlayer/ticket purge 重入全量 pump。空闲且无结果时零驻留遍历；处理预算同时限制候选检查、提交数和在途量（失败或未来 cooldown 的一长串不能把一刻扫描用满）。卸载和 close 仍优先收尾，关服不受在线预算限制。
- 可靠保存：对同一 chunk 至多一个在途版本；最新编辑在 pending；失败的旧 patch 与后续 patch 按位置合并，后写优先。不要依赖失败结果“已经 patch 了 worker root”的隐式副作用：失败也可能发生在 patch 之前。接受失败和 I/O 失败不丢 delta，不让同一失败每次 socket poll 重试。保持读写 epoch，拒绝迟到 ack。保存成功后根据 dirtyVersion 决定是否排下一版，不提前驱逐。
- 已实现计划刻的相对延迟语义。避免每轮网络 pump 为所有 chunk 重新遍历 tick 队列：按 tick 容器是否非空，或在候选保存/卸载时刷新相对延迟；现有延迟、撤票/重载、due 执行顺序不可变。若对“仅时间经过是否使 chunk 再脏”做语义修改，先拿 Java 源码和定向反例说明，单列评审。
- 当前 World::managedChunkPersistenceState 会因任意区块有实体而阻塞所有区块，短暂 piston ticker 也会把 blocker 永久保存。必须改为按被检查 chunk 查局部运行时状态（复用已有 entity section 范围或局部计数，避免创建实体全量快照）；活塞/实体仍活跃时保留该 chunk 脏状态，结束后可以重试。未知 NBT/不支持状态仍是明确持久阻塞。此阶段不实现实体/BE 编解码。
- 主线程只抓受影响 chunk 的 patch 和计划刻快照；payload 移动传给同一 I/O worker，worker 保持 NBT 模板。记录实际复制的 patch、tick 字节，避免一份新 edits 向量再拷贝两次做 retry；不复制整个 chunk/DOM/世界，不改光照存储。
- 验收：fake store 故意挂起写盘时连续 pump 返回；held ticket 的脏块按预算自然落盘并保持 resident；多块负载及 idle 路径观察候选数；在 v1 写失败前编辑 v2，重试时不能丢 v1 独有位置或覆盖 v2；候选暂不可保存或处于 cooldown 时不会饿死其他坐标；一个区块有实体时不影响相邻脏区块保存；活塞动画结束后恢复。可复用现有实盘 fixture，其他阶段用例和 CTest 不重复跑。

后续 W-3b1 优先 moving piston block entity，因为模拟核心已经有可直接继续推进的状态，不适配的 BE 类型继续拒绝。Java `PistonMovingBlockEntity.java:342–359` 保存 progressO 为 float，加载后 progress 与 progressO 同值；facing 用 legacy id，包含 blockState/extending/source，保留 keepPacked/未知字段边界。避免调用完整 ChunkSnapshot 只为几个 BE；按坐标取小快照并随同一个 terrain 保存版本。普通实体区域稍后按 `EntityStorage.java` 单独处理（同维度 entities/*.mca，DataVersion/Position/Entities），不假设两份 region 能跨崩溃原子提交。

另一个待对齐点：原版 `ChunkHolder.isReadyForSaving()` 是 `saveSync.isDone()`，其依赖来自 FULL/ticking/entity-ticking/generation futures（ChunkHolder.java:111, :283, :296, :313，GenerationChunkHolder.java:241）。它不是 `dirtyVersion == savedDirtyVersion`，也不等价于 RegionFile 写完成（ChunkMap.java:714–749 另做保存）。当前 WorldChunkManager 把脏 resident 判不 ready，会让 portal 的倒计时在 held 且 dirty 时无限暂停，直到关服才写。把到期门槛按 holder 是否有未完成安装/依赖判断，至少保证 dirty resident 也能递减；现有 testTimeoutPausesDuringInFlightSave 测的是 C++ 的额外保守规则，不能作为原版依据。如保留在途保存的暂停，明确记录为与原版不同且不阻塞已装载但待保存的 holder；如同步修正，调整测试验证 I/O 已挂起时也能正常到期、但驱逐仍要等落盘。

W-3b1 tick gate: Java Level.java:537–555 gates the block-entity ticker by shouldTickBlocksAt and runsNormally; ServerLevel.java:461–463 implements that query from the simulation tracker. This is separate from isPositionTickingWithEntitiesLoaded at :1886 for scheduled block/fluid ticks. Managed piston tickers must obey the former while retaining registration/pending order. Same save version must include the moving_piston block state and the block_entities entry. A loaded moving piston with no valid matching BE is a failed install, not a permanently stuck air/placeholder substitution.

独立生命周期内存收尾（可在 W-3c2 后单列 T-4b3）：ChunkLifecycleController::updateDemand 当前对任何 loading 等级先 entries_[position]，即使旧/新都 >FULL；所有远行足迹都留在 entries_/workerEpochs_/workerWrittenVersions_，manager lightEpochs_/errors_ 也需审计。先过滤从未 FULL 的等级边，再用全局单调请求号和匹配代 Drop/Invalidate 回收不再 demand/resident/在途的项；勿用清表后从 0 的每坐标代号导致旧结果复活。与既有空缺/损坏诊断、网络/光照生命周期 tombstone 的语义分别验证，不把无关光照清理塞进 autosave。


## W-3c2 implementation note (2026-11-12 coder)

Implemented the bounded autosave stage and local dynamic blocker without changing region format,
light, persistent-ticket I/O, entity/BE codecs, or tick ordering. The online
queue is event-driven: dirty writes enqueue one position, ticket removal queues
only changed positions, and I/O completion visits only the explicit in-flight
set. WorldChunkManager::pump no longer traverses all managed chunks.

Defaults per real WorldChunkManager::serverTick that processes chunks:
64 candidate checks, 4 submissions, 32 active saves, a 2 ms candidate window,
10 s success cooldown and 1 s failure backoff. These are stricter than Java
ChunkMap's 20-save/128-write eager limits. Held dirty chunks autosave on this
path. A blocked-position or cooldown entry is removed from the immediate queue;
dynamic blockers are re-appended after the next tick's eligible candidates, so
a blocked prefix cannot starve chunks behind it. Demand-edge unload work and
completed-result follow-up remain bounded by the same active/submission limits,
but may be initiated from a nonblocking pump because eviction cannot proceed
without its pending save.

The save payload now carries shared_ptr<const vector<BlockEdit>>. The exact v1
patch remains shared across the lifecycle command and retry bookkeeping. New
edits stay in the position map; on v2 the retained v1 patch is copied once and
pending positions are appended with v2 last. A successful ack drops the
retained patch, while a failed write does not mutate or clear it.

readyForSaving now represents an installed holder (Resident, Saving, or
EvictPending) and no longer compares dirty and saved versions. Dirty resident
portal/spawn timeout tickets therefore age while I/O is pending. Eviction still
requires savedDirtyVersion to cover the dirty version.

World persistence blockers now use O(1) per-chunk counts maintained by entity
section add/remove and moving-piston entity/ticker add/remove. One blocked chunk
does not inspect or block another. Removing the runtime entity/piston clears the
reason and the next tick's autosave retries the retained dirty patch. Unsupported
saved NBT remains a separate fatal install boundary.

Direct evidence is recorded in task/TASKS.md. No CTest was run.


### W-3c2 review-rework implementation (2026-11-12 coder)

The hot-path budget now starts before cooldown promotion, not after it. Each
autosave iteration extracts exactly one entry from either the earliest due
cooldown or the ordinary FIFO, increments candidateChecks once, and then checks
the entry. Missing epochs, in-flight residents, clean entries, temporary
blockers and deferred retries therefore all consume the bounded check. The
loop cannot extract another entry after checks/submissions/in-flight/time are
exhausted.

Demand and eviction use epoch-tagged FIFO deques with queued dedup. A socket
pump processes at most four, so it never swaps and rescans the whole set while
unloading or while an I/O slot is full. Reacquiring demand requeues the position
on the held-autosave path. Empty loading/simulation updates skip readiness and
pump, and tick-time observation is memoized per gameTime.

Retained failed edits are merged in a temporary cell map keyed by section and
local index. Pending edits overwrite retained values before one shared immutable
vector is published. Repeated failures of the same cell therefore retain one
entry, while distinct cells are preserved. The native socket loop also calls one manager pump after reconciling all
connections, so large removal queues can progress while the game tick is frozen
without multiplying work by the connection count.


### Final cooldown/dynamic-queue closure (2026-11-12 coder)

Cooldown entries are partitioned by current demand. Reclassification is a
point operation on the relevant maps. The no-demand map is drained only when
its earliest wall-clock deadline is due, by the bounded pump; it remains active
without World::tick. The held map remains server-tick-only.

Dynamic retries use epoch-tagged held/no-demand FIFOs and queued membership
sets. A demand transition moves only the current entry marker; old deque entries
are skipped without a scan. One held dynamic retry is attempted alternately
with normal autosave work. Pump-side no-demand retry is bounded by the common
four-entry pump ledger. This removes the previous whole-set move and keeps a
large blocked prefix from starving eligible chunks.

The normal edit snapshot path now emits pendingEdits directly into its one
immutable vector. Cell dedup and its temporary map exist only when a failed
retained patch must be merged with newer pending edits.

### 快照内存上限（reviewer 实测 sizeof，非频率测量）

WSL 当前 ABI：terrain::BlockEdit 24 B，重试去重 std::_Rb_tree_node 64 B（未计分配器额外开销）。
主世界单区块上限 24*4096=98304 个 distinct edits，普通不可变 vector 上限 2,359,296 B
（2.25 MiB）；失败保留 patch 加新修改时临时树上限 6,291,456 B（6 MiB），并存有
旧/新 vector 与 pending map。真实普通保存只处理实际编辑，不每 tick 构建此快照。
证据：/home/zhddsj/.local/state/redstone-cpp/wsl-w3c2-final-contract-closure-VbeXQ7/edit-size.log。
后续优化建议：payload 使用局部 section/cell 编号 + BlockState::Id（worker 还原），统一
按 cell 排序，失败重试用双指针合并旧/新连续数组，避免额外树。若需每刻硬时限，应
把单区块极端快照也分预算，不将 2 ms 候选时窗当作硬上限。不要与 W-3b1 并发改保存结构。
