# 26.1 区块票、生命周期与 Anvil 接线

本交接基于本地 Java 26.1 源码和项目 PLAN 阶段 3–4。按 T-1 → T-2/T-3 → 按坐标 Anvil I/O → 生命周期接线/原版重启差分推进。先锁定行为边界，不改已收尾的光照传播。

## T-1：票与等级

- 票是 (区块坐标, 类型, 等级)，同类型同等级重复提交续期；active 和 deactivated 分开。以原版 TicketType 注册表为准，不用旧版本的 ticket key 或 timeout 表。
- TicketStorage 计算两套独立源等级，LoadingChunkTracker 和 SimulationChunkTracker 沿八邻域 +1 传播，缺省和截断不同。区分票需求、FULL / BLOCK_TICKING / ENTITY_TICKING 目标与实际数据/邻居 future 就绪。用原版 ChunkPyramid 的 FULL 依赖核实加载上限。
- purge 条件照 TicketStorage.canTicketExpire：限时票且 (类型允许，或 updating holder 不存在，或其 isReadyForSaving)。持有者存在而 saveSync 未完成时，对不允许提前过期的类型暂停倒计时。ticks_left 降到负数才删除。
- 去源后上调等级，多个源取最低；没有变更时不遍历等级图。给生命周期层一份明确的最终等级变化记录。独立实现和测量；不在 T-1 接 World 或磁盘。
- 接玩家前的性能门槛：一次进服、跨区块或视距改变的一组票更新只提交一次。按最终变更源的旧/新影响范围增量更新等级，不逐票重建全图。用视距 32 的加入和单格移动、相距很远的双玩家域验证处理范围。
- 证据优先用同版本官方 jar 探针，覆盖负坐标、对角线、重叠源去除、续期、两个 tracker 的边界和 no-ticket 值。没有实际运行原版的部分单列。

## T-2/T-3：票来源和模拟门槛

- 依次接玩家进入/离开/移动、视距与模拟距离、显式强加载、出生/出生搜索路径。按 DistanceManager，服务端视距覆盖区逐区块 PLAYER_LOADING level 31；同一位置多玩家覆盖按引用计数。PLAYER_SIMULATION 仅在各玩家所在区块，等级 max(0,31−simulationDistance)，同 chunk 最后一名离开才移除。FORCED 等级为 31。票需求与客户端网络发送范围区分。
- 方块/流体计划刻在原版共享 isPositionTickingWithEntitiesLoaded：实体数据已加载、模拟图满足 block ticking、visible holder 的 ticking future 已成功。实体位置判定则为 entity manager canPositionTick 与模拟图 entity ticking。保留 tick 阶段顺序和未就绪的待执行刻。
- 原版邻居依赖：prepareAccessibleChunk 范围 1（生成状态随距离）；prepareTickingChunk 范围 1 FULL；prepareEntityTickingChunk 范围 2 FULL。不要把票等级直接当 future 已完成。T-3 可与 T-4 一起接实际就绪，避免先造虚假的 ready 状态。
- 离线世吞与夹具继续有明确的固定加载输入；未接 generation 的位置不造空块来满足 FULL。

## W-2/W-3：按坐标读写真实 Anvil

- 现有 loadAnvilWorld 是扫整个目录、解码为导入投影的只读接口。在线路径用按 (维度, chunkX, chunkZ) 的接口；同一个 region 的写入串行，禁止重写无关 chunk。复用有验证的 region/NBT 解析逻辑，不能把全世界导入当在线加载。
- 以 RegionFile/RegionFileVersion 为准：8 KiB 双表头、4 KiB 扇区、big-endian location/length/timestamp、负坐标 slot、.mcc 外部流、高位标志、默认 zlib/deflate。读取现有 gzip、deflate、none、LZ4Block；无法理解的压缩或非法字段显式错误。先验证读回真实文件，再启用写；区分缺失、不可读、版本不支持。
- W-2a 先实现 region 单槽原始 NBT 读写，与 chunk 对象序列化分开验收；测试覆盖新 region、跨负坐标、改一个槽不动其他槽、扇区扩大/缩小和外部流。不把现有一次读完整 region 文件的实现搬到在线路径。
- 现有 NBT 模块只有读，且 Tag 解析不保留根名和空列表元素类型。写入层必须验证未知 NBT 的往返边界；不能只重建当前 LoadedWorld 投影后覆盖原区块。
- 按 SerializableChunkData 和同版本原版世界核实 DataVersion、坐标、Status、section palette/packed longs、biomes、heightmaps、计划刻、方块实体和光照。实体走对应维度 entities 区域文件；原版持久票走维度内的 data/minecraft/chunk_tickets.dat；SavedDataStorage 使用 gzip NBT，外层 DataVersion 和 data，内层 tickets。按 TicketStorage/Ticket Codec 核对 chunk_pos/type/level/ticks_left，不能写自定义格式。
- 原样保留未支持/未编辑的 NBT（尤其生成结构、实体/方块实体、状态或 biome）；无法无损覆盖时先拒绝写入该区块。没有可证明完整的光照时不要声称 isLightOn。持久化一致性要求与网络弱一致读取不同：光页稳定或由 worker 交接后才标完整。

## T-4/W-4：在线生命周期与验收

- 每区块区分需求等级、请求代号、数据驻留、读写中、待卸载和完成状态。I/O 在后台，主线程只领完成并校验代号；旧读、旧保存确认、旧光照结果不能回填新一代。
- 保存携带脏版本；旧保存成功不能清掉新修改。卸载在保存成功且仍无需求后才释放，失败保留驻留和重试路径；重新获得票时正确取消待卸载。主线程不等待磁盘，关闭时明确排空策略。
- 光照加载/卸载通知必须可靠。在线安装应先建好区块数据、tick 容器和所需实体状态，再发布驻留与一次正确的光学快照；不能调用全世界 importWorld 改时间或刷新全部光照。现有 World::unloadChunk 只是标记 loaded；实际擦除区块节点前须使 World 的 chunk/section 快速读缓存失效，并分离待存的刻和实体。
- 第一版无世界生成：仅能推进原版存档中已满足需要的 chunk status；缺区块/欠生成不能偷偷补空气并标 FULL。
- 双向用隔离世界副本验收：C++ 读原版、C++ 写后原版 26.1 能重启读取并保持已支持内容。负坐标、跨 region、缺失/损坏、失败重试、过时代际均覆盖。禁止用原存档做写入验证。
- coder 做相关直接测试，reviewer 查代码、证据、严重问题；不重复跑 CTest。未完成阶段、未运行官方差分和性能限制分别报告。

## 保存数据交接

现有 World section 在主线程原地修改，不能交其可变存储指针给 I/O。在线第一版应把主线程保存开销限在有预算的单区块交接：卸载路径可以在撤下访问后移交独占数据并保留失败恢复；仍驻留且可写的 autosave 路径只复制保存确实需要的脏 section/刻/实体快照，压缩和磁盘操作在后台。避免每刻/每次保存构造整个 World 的 LoadedWorld 投影。凡必须复制数组，记录每 section 字节与触发次数，避免和裸读并发修改或隐式大拷贝互换。

## Native 模式接线待办（不在独立 codec 阶段抢接）

当前 Session.availableChunks_ 来自启动时 ServerOptions.chunks；光照生命周期回调也会检查这个静态集合。因此线上动态磁盘模式必须给Session显式resident available/unavailable入口，装入区块可入发送队列，卸载失效generation token；不能只接world.load/unload而保留静态可见集合。初始chunk等两层所需光值，增量仍分层发送。

磁盘模式应显式开启，保持旧--world导入/投影模式语义；启动不扫完整region。metadata只读一次level.dat时间/seed等，票源逐位置驱动load，运行中每循环非阻塞pump，ticket timeout仅server tick。close先排空托管dirty保存，遇未支持实体/BE/坏chunk清楚报告，无静默数据替换。实体region应先检查空/缺失，在未有安全实体往返前非空明确拒绝，而非当作空实体ready。

## W-3c 持久票与世界时间的后续验收

- 26.1 的 ServerChunkCache 构造函数从 levelStorage.getDimensionPath(...)/data 创建 SavedDataStorage；DimensionType.getStorageFolder 解析到 dimensions/<namespace>/<path>。因此现代 overworld 的票在 dimensions/minecraft/overworld/data/minecraft/chunk_tickets.dat；全局 world_gen_settings.dat 则在世界根的 data/minecraft。以实际选择的维度目录定位票文件，不从两个路径混合读取。
- 文件结构以 TicketStorage.TICKET_ENTRY、Ticket.CODEC、ChunkPos.CODEC 与 SavedDataStorage.encodeUnchecked 为准：gzip NBT，根 DataVersion 和 data，内层可省略的 tickets list；每项 chunk_pos 是两个 int 的 NBT int array [x,z]，type 为资源标识符，level 为非负 int，ticks_left 为可省略的 long（默认 0）。写出只包含原版 persist 类型 forced/portal，包含 active 和 deactivated；读出后先完整验证，再一次批量激活并更新等级图。恢复 portal 剩余时长而非续满 300；恢复 forced 后 setForced(false) 应能移除已有票。不支持的版本/结构不得被静默当空覆盖。没变化时不要重写。
- level.dat 的 Data.Time 采用 PrimaryLevelData 的 long；更新时只补当前真正持有的字段，保留未知 NBT 与原有 metadata，不生成或重置 world_clocks。LevelStorageSource.saveLevelData 先写同目录 gzip 临时文件，再替换，并留旧的 level.dat_old。元数据保存错误与区块保存分别报告；与 terrain 都持有原版兼容的 session.lock 直至关服完成。注意这仍不等于实现 26.1 World Clocks。
- 保留 I/O 线程职责与主线程小开销。实际持久票从原版 Codec 编码读入、C++ 写出再由原版 Codec 解析，随后用隔离世界做原生两次启动，检查第二次无玩家时仅持久票仍能驱动驻留，负坐标、写失败和禁写非持久票都应有证据。不要为这部分重跑 CTest。

## 在线保存预算（进入长跑前）

- 当前版本只在撤票或关服时写脏 terrain。原版 ChunkMap.saveAllChunks(false)/saveChunkIfNeeded 会保存仍有票且可访问的脏区块。单独补一个有限预算的 dirty-queue autosave，按完成事件更新状态；不在每一轮 socket poll 为每一名玩家重复扫全部驻留区块。
- 对持久化暂时不支持的运行时实体/方块实体，阻塞只应影响实际涉及区块。不能因短暂活塞动画或其他区块一个物品，把全部区块永久标记为不可存。保留 dirty 信息，动态阻塞在条件消失后可重试；持久未知数据继续明确拒绝。
- 检查保存期间有新编辑、保存失败后新旧编辑合并、多个 held tickets、budget 用尽时留待后续 tick，以及同一坐标失效代结果。只需要一份有界 patch/tick 快照和 worker 自有 NBT；若有大拷贝，交接前报告来源和字节数。

## W-4 真实官方服务端夹具

- 官方 26.1 DedicatedServer 实际生成的隔离纯石头平坦世界（不是手工拼 NBT）：/home/zhddsj/.local/state/redstone-cpp/wsl-w4-official-stone-ha8xrh9n/interop-world 。服务器日志同目录 ../server.log，read-only C++ 探针结果 ../cpp-readonly.log。
- Overworld 平坦层 minecraft:stone 高度 5，minecraft:the_void，seed 2614786，不生成结构/地物。官方命令 forceload add -16 -16 保存了 [-1,-1] forced；setblock -13 -59 -16 minecraft:stone 提供基准，保存时 Time=25。C++ 解码 status=minecraft:full、24 sections、block_entities=0、目标石头，目标 entities region 文件为官方生成的零字节 .mca；原版视作没有任何槽，C++ 旧实现错误地返回 Corrupt。
- 保留夹具作为只读母本。复制到新的 scratch 目录后再跑 C++/Java；验收实际的 Java → C++ 写盘 → Java 重启，询问目标方块、强加载票和 Time。另一个官方默认类地表夹具 /home/zhddsj/.local/state/redstone-cpp/wsl-w4-official-flat-ujjge37k/interop-world 当前因 minecraft:dirt 不在持久化白名单明确拒绝，可用于负例，不应因夹具而放宽 state 保护。

## W-3c1 implementation handoff (2026-11-12 coder)

Implemented startup restore and explicit-close persistence for the opt-in
--ticketed-world path. Loading remains read-only until the world and persistent
ticket files validate successfully; malformed DataVersion, ticket shape,
unknown persistent type, or corrupt gzip is rejected before demand is installed
or a writable world is accepted.

Persistent tickets use <region parent>/data/minecraft/chunk_tickets.dat. Missing
input creates no file. The loader validates DataVersion=4786, data, optional
tickets, chunk_pos IntArray[2], supported forced/portal, nonnegative level and
optional Long ticks_left (default 0). Startup applies all records in one batch
and records forced-source ownership without adding a second ticket.

level.dat is read once into WorldMetadata.levelRoot. Only Data.Time is mutated.
Saving writes a same-directory gzip temporary, copies the current level.dat to
level.dat_old, and renames the temporary into place. A failed backup or rename
leaves the current file readable and removes the temporary. Shutdown skips
metadata when Time is unchanged and skips tickets when missing+empty or content
is unchanged. Terrain close precedes metadata/ticket save under the same held
session lock. There is no cross-file crash transaction claim.

Copies/allocation: the existing level root is retained once in metadata; each
save adds one NBT encoding and gzip temporary. Ticket save adds one encoding
and temporary. No terrain, light page or region copy exists in this path. No
per-tick I/O or reserialization was introduced.

Official Java 26.1 TicketStorage.CODEC produced negative-coordinate forced +
nondefault-remaining portal records and parsed C++ output. Final output was
JAVA_TICKETS_READ_OK count=2, forced=-13,-16 loading=31 simulation=31,
portal=-14,-16 loading=33 simulation=33.

Two isolated native launches restored the Java file, exited 0, logged
TICKETED_TICKETS_LOADED 2 and TICKETED_WORLD_CLOSED complete=1 unsaved=0.
First run saved Time 52 -> 62 and level.dat_old retained Time 52; second run
restored tickets and continued Time. A manager fixture proves restored level-31
forced demand installs an existing FULL chunk and setForced(false) releases it.
A separate missing+empty run created no ticket file.

Logs: wsl-w3c1-final2-run1.log SHA-256
9e741ab0138147a1eae8b004509fb3e41b20993e47f5731684fa2476970b5ddd;
wsl-w3c1-final2-run2.log
47333d48793b2fe6b70a213ea271d35a8d111ba9206e183be1a824e86e6afdc3;
wsl-w3c1-java/java-write-final.log
28c0074123a1723d4dd506ebdff1f2d0c59c96b76a00343a0b4149d5d9a0245b;
wsl-w3c1-java/java-read-final2.log
66abdd0ea3609d88d69b7a5dce053dae268e17a526c49a24fe307fa8e14b9a27.

Final binary SHA-256 78784c986ff981483cb9d19038f8bee26dd0184da5e5d71b799b28ff7d9e6e71.
Storage, ticket and World manager fixtures exited 0. No CTest, client, commit,
push or original-save write was performed.

Remaining boundaries: only forced/portal persistent tickets are supported;
nonpersistent player/spawn tickets are never serialized. World Clocks,
gamerules, autosave budgets, generation, entities/block entities, UI metadata
and full vanilla restart equivalence remain future work.


## W-3c1 reviewer rework (2026-11-12 coder)

The native startup now calls WorldChunkManager::restorePersistentTickets, which
performs the single validated raw-ticket restore and submits its resulting
loading changes to the lifecycle controller. The prior native path no longer
drops the update returned by TicketSourceManager.

Tests preserve the full supported ticket identity rather than only a position:
restored forced and portal records at separate chunks retain exact levels and
timer values, including duplicate portal records with ticks_left 257 and 123.
setForced(false) removes forced records at that chunk and leaves portal demand.
The first focused failure was diagnosed as correct propagation from an adjacent
level-9 portal to a level-31 forced position (31 -> 10), not a lost restore; the
fixture now places the records outside each other's influence.

Input acceptance matches the supported world extent: chunk coordinates are
checked against the asymmetric floor range [-1875000, 1874999]. Negative
ticks_left values, including INT64_MIN, are rejected before countdown use.
Secure temporary-file writes create new exclusive names, never truncate or
follow an existing temporary symlink, check flush/close, and clean through RAII.
Canonical ticket comparison prevents spelling/order-only rewrites while keeping
duplicates and exact ticks. Shared level.dat root is mutated only at Data.Time,
and level.dat_old is replaced through its own temporary.

Region EOF compatibility is fixed in both readState/write paths. A zero-byte
region reads as Missing and its first write creates the 8 KiB empty double
header. Sizes 1..8191 remain Corrupt; focused cases use 1, 4095, 4096 and 8191.
The official copied stone world with zero-byte entities .mca regions was accepted
by the native opt-in path. A Java 26.1 codec file containing only forced (-1,-1)
level 31 caused 23 terrain installations without a client; the final process
exited 0, logged saved=0 unsaved=0 and complete=1.

Evidence root:
/home/zhddsj/.local/state/redstone-cpp/wsl-w3c1-reviewfix-0tJYxI
Final log: native-forced-only-zero-entities-final.log, SHA-256
fc50178fc0088c92a318751a2949d740f3487a71cbabf974728ef0669726f1b9.
Focused test logs and hashes are recorded in task/TASKS.md. Binary SHA-256:
d8336249103e6114d691cb70e26f1badb532351df57501a2ef40552aeb0927f4.
No CTest, client, original-save write, commit or push was performed.
### W-4 真实 Java → C++ → Java 地形/持久票/时间验证

母本为真实 26.1 DedicatedServer 生成的纯石头世界，未写母本。副本：
/home/zhddsj/.local/state/redstone-cpp/wsl-w4-restart-q5u0fnl4/interop-world 。
原生候选 d8336249103e6114d691cb70e26f1badb532351df57501a2ef40552aeb0927f4。
两轮 native TCP 正常退出，实盘单元格 (-13,-59,-16) 由 stone 改 air、再改 smooth_stone；
Time 25→105→185，每次 level.dat_old 字节等于前一轮 level.dat，NBT 其他字段等值，
原版强加载票文件字节不变。之后真实 Java 26.1 服务端重启，加载 25 个持久需求
区块，forceload query 确认 [-1,-1]，execute if block 确认 smooth_stone，
time query gametime 报 187。服务器最后打印保存所有维度。
日志在该副本父目录的 cpp-1.log、cpp-2.log、java-restart.log、review-evidence.log；
脚本最初用旧版时间提示字符串等待超时，实际日志已记录新提示和正确值，已独立解析
原日志确认，不为修正字符串重跑服务器。只证明该地形/票/Time 场景，不是任意存档兼容。
