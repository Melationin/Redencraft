# T-1 票据模型与等级图交接

本轮从提交 eb5d5b2 开始。已验收的光照队列小修保持在工作区未改动；T-1 新增独立模块，没有接入 World、ChunkHolder、玩家、命令、Anvil、生成、卸载或刻循环。

## 文件与接口

- include/redstone/tickets.hpp
- src/tickets.cpp
- tests/tickets_tests.cpp
- validation/vanilla26_1/TicketLevelProbe.java
- validation/vanilla26_1/run_ticket_level_probe.sh
- CMakeLists.txt：将 src/tickets.cpp 加入 redstone_core，并增加 tickets_tests 目标。

TicketStorage 是主线程归属、无内部锁的 active/deactivated 票据容器。票身份严格是 (chunk, type, level)：

- addTicket 对同类型同等级重复来源只重置 timeout，不增加列表项，也不发等级源变化。
- 同类型不同等级可同时存在；removeTicket 只删除第一个精确匹配。
- Loading source 只取 doesLoad 票的最低等级，Simulation source 只取 doesSimulate 票的最低等级；原始的 no-ticket source 都是 45。
- 删除、批量 removeIf、关闭 deactivation 和 activateAllDeactivatedTickets 都输出每个受影响层的 source update。新增降低源使用 onlyDecreased=true；删除/批量清理输出 false。
- shouldKeepDimensionActive、hasTickets 和 forced 集合只看 active 票；persistentTickets 同时读取 active/deactivated 中带 PERSIST 的票。
- purgeTimedTickets 表示一个原版 purge tick：仅对 hasTimeout 票检查 canExpireIfUnloaded；否则 holder 不存在或 holder.readyForSaving 才符合条件。符合时 ticksLeft--，只有小于 0 才删除。holder 存在但尚未 ready 时，不允许提前过期的票不倒计时。

TicketLevelGraph 是两个独立实例：

- Loading：原始无源值 45，保存 0..44，八邻域 Chebyshev +1。
- Simulation：原始无源值同样是 45；图将值截到 33，只保存 0..32，无票查询返回 33。
- 多个来源取最小“源等级 + 图上距离”。
- source batch 没有实际变化时不重算、不输出 change；有变化时保留旧图并输出精确的 oldLevel/newLevel，去源可产生等级上升。
- recomputeCount 供测试证明无变化 purge tick 没有扫描等级图。

ChunkDemandSnapshot 把四个常用概念分开：

- loadingLevel / simulationLevel 是两套独立图结果。
- schedulingScope 等价于 ChunkLevel.isLoaded(level <= 44)，只表示 holder 调度范围。
- holderFullStatus 由 loading level 按 33/32/31 边界投影为 FULL/BLOCK_TICKING/ENTITY_TICKING。
- blockTickingDemand / entityTickingDemand 由 simulation level 派生。
- dataResidency 在本阶段固定为 NotTracked；没有把 loaded、FULL 或 ticking demand 当成区块数据已经 resident。

## 原版依据

- ../net/minecraft/server/level/TicketType.java:10-25：flags 与九个注册类型。
- ../net/minecraft/server/level/Ticket.java:23-67：timeout 初始化、续期、递减及 ticksLeft < 0 判定。
- ../net/minecraft/world/level/TicketStorage.java:150-216：同类型同等级去重/续期、两套最低 source。
- 同文件 :227-365：精确 remove、purge predicate、deactivate/removeIf 输出。
- 同文件 :367-409：等级替换、forced 和持久票遍历。
- ../net/minecraft/server/level/ChunkTracker.java:17-31,64-73：八邻居和 +1 成本。
- ../net/minecraft/server/level/LoadingChunkTracker.java:6-31：加载图内部 no-ticket 45。
- ../net/minecraft/server/level/SimulationChunkTracker.java:9-37：levelCount=34、默认 33、仅保存 <33。
- ../net/minecraft/server/level/ChunkLevel.java:10-69：FULL/33、BLOCK_TICKING/32、ENTITY_TICKING/31 和 MAX_LEVEL。
- ../net/minecraft/world/level/chunk/status/ChunkPyramid.java:9-40 与 ChunkStatus.java:16-32：FULL accumulated dependencies。

## 官方 26.1 探针

使用 Zulu Java 26.0.2 和本地 26.1 server classes jar；探针没有启动服务器或客户端。命令：

    validation/vanilla26_1/run_ticket_level_probe.sh

原始输出：/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t1-20261112/ticket-level-probe.log，SHA-256 a0cff0a1590bc70c1c40bd8c15e53ed55d0dec2d238cdec54faa85d79f82c4f2。26.1 server classes jar SHA-256：a7fed6f7d88379349e35ae0c6e9881d4484605132f6f620376a3868eea6cce52；Java 26.0.2 Zulu26.32+13-CA。探针源码 SHA-256：0a18e005f1fd4bda4989664b9b784bb212536ca9504f7e870282f0ba3200e95b；脚本：cf9013320b4c6f6404e42bad55e183687c1574914fa8aee91b7b379fded51997。

探针确认：

- ChunkLevel.MAX_LEVEL=44，RADIUS_AROUND_FULL_CHUNK=11。
- FULL distance 0..11：full, initialize_light, carvers, biomes, structure_starts x8。
- byStatus 边界：empty/structure_starts=44，structure_references/biomes=36，noise/surface/carvers=35，features/initialize_light=34，light/spawn/full=33。
- loading/simulation no-ticket TicketStorage source 都是 45；Simulation tracker 默认 33。
- 负坐标及对角线 source 31/30 传播得到 31/30/31；删除 30 后两处上升到 32；simulation 队列随后为空。
- Ticket timeout 从 1 到 0 不超时，到 -1 才超时。
- duplicate PORTAL 7 返回 false，并把已有票重置到 300 ticks。
- 原始事件次序：新增较低 source 先 simulation 后 loading，均 onlyDecreased=true；删除 PORTAL 后先 simulation=10 再 loading=20，均 false。

holder-ready/未加载的 purge 私有 predicate 没有在探针中构造完整 ChunkMap；这部分由源码逐行对照和 C++ 注入 holder lookup 的行为测试覆盖，不能宣称为真实服务器 tick 差分。

## C++ 直接验证

构建并直接运行：

    cmake --build /home/zhddsj/.local/state/redstone-cpp/wsl-perf-light-split-20261111/build --target tickets_tests --parallel 6
    /home/zhddsj/.local/state/redstone-cpp/wsl-perf-light-split-20261111/build/tickets_tests

结果：ticket tests passed。覆盖重叠来源删除、负坐标对角线、loading/simulation 分离、重复续期与等级替换、精确删除、timeout 0/-1 边界、holder absent/not-ready/ready、canExpireIfUnloaded、deactivated/activate，以及无变化 tick 不触发等级图重算。

日志：

- Build：/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t1-20261112-build-final3.log
- Tests：/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t1-20261112-tickets-tests-final3.log
- Build SHA-256：c4c6bc3534a3c7ac325cc8214c87a5c860a170c14db2dedcdee066a811ed97dd；tests SHA-256：8ccc0af72f8368e09724a798635ee4b38e6a30e641ceb5ec60028e897ccb5677。
- headers/source/test SHA-256：7910082f8800c52b894415541ffdf32096a068f94a7659d6046cd9d3c9810c27、f37e93ff1158f43d7d55e93af03b4b5f697a8c4cf8a8646f316f74a99e94d590、279808ebc84dd1d37b449f1675d635a6268c6b57de7368999cb788bd6411c3fb。

未运行 CTest、未做 World 接线、未提交或推送。下一阶段接口是 TicketLevelSystem 的 TicketSystemUpdate/ChunkDemandSnapshot：T-2 只需提供按原版来源产生票据操作；T-3 可消费 simulation 等级和 ticking demand，再由后续 holder/I/O 层单独提供 data residency。


## T-1b 批量票图更新

新增接口：

- TicketOperation::add/remove，以及 TicketStorage::applyBatch、TicketLevelSystem::applyBatch。操作按输入顺序执行，批量结束后才按 chunk 比较最终 loading/simulation 原始 source。
- 批量中途加入又删除、或等级往返后回到原值时 sourceChanges 为空；两个等级图都不提交。TicketStorage 的 changed/dirty 仍如实表示票对象曾被修改。
- raw source 的最小 level 通过 optional 计算，存在参与票时即使 level>45 也返回真实值；只有完全无参与票才返回45。图仍把 >=storedBelow 的来源截断为无影响。
- removeTicketIf 改为在每 chunk 的 vector 内原地压缩，不再为每个有票 chunk 创建 kept vector；purge 无票移除时不做该分配。

ChunkLevelGraph 的局部重算：

- 对每个有效 source 的 old/new 等级计算半径 storedBelow-1-level，取 old/new 半径并集。
- 先按 z 行合并 x 区间，再生成唯一的受影响格子，避免大视距批次重复插入重叠方阵。
- 在这些格子内，以 new source、以及紧邻 affected 区域外一格的旧等级+1 作为稳定边界种子，做有界八邻域 bucket BFS。
- 只删除并写回 affected 范围内的 levels_；区域外等级与源不重建。
- 每个 applySourceChanges 批次最多提交一次图重算；无有效 source 差时提交0次。lastRecomputedCells/ChangedCells/recomputeCount 可供 T-2/T-3 观测。

性能夹具结果：

- 远端已有源，随后一次加入 65x65（4225）个视距32风格 loading source：storage 合并为4225条最终 raw source update；loading 图提交1次，重算8281格；simulation 图提交0次。
- 单格移动为同一批 remove+add：2条最终 source update；loading 图提交1次，重算756格；远端区域保持不变。
- 240轮随机增删，分别对 loading/simulation 的 -10..10 全部441个位置逐点与全量 Chebyshev 多源参考比较，共211,680次图值断言通过。

T-1b 最终直接验证：

    cmake --build /home/zhddsj/.local/state/redstone-cpp/wsl-perf-light-split-20261111/build --target tickets_tests --parallel 6
    /home/zhddsj/.local/state/redstone-cpp/wsl-perf-light-split-20261111/build/tickets_tests

输出：

    ticket view32 sources=4225 loading_recomputes=1 loading_cells=8281 simulation_recomputes=0
    ticket one_move source_changes=2 loading_recomputes=1 loading_cells=756
    ticket tests passed

日志：

- Build：/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t1b-20261112-build-final3.log，SHA-256 34b20fef55e59d78dace541dd8aef59b17394143e2051048143ded5a460bdaf7。
- Tests：/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t1b-20261112-tickets-tests-final3.log，SHA-256 a8b977b0e452af2b3f621c4f4a43f290025bb8618b51bb6b43852b0a171d050e。
- 当前 headers/source/test SHA-256：60d78e377a50ac183e3f460dcba121fe7fa79d067227588fd40c561e811a0a75、741bddf2e0b4c9391760016f616812d53e9bf8cd765931f98f5ad9a9699b0718、33dfa1d857e4e2137518920a6a53447df790f3a043723ef169b4f6ca100967e2。

未运行 CTest、光照性能、World/玩家接线、磁盘或额外性能活动；未提交、未推送。保留此前所有光照收尾和其他未提交修改。


## T-2a 基础票来源

新增 TicketSourceManager（include/redstone/tickets.hpp、src/tickets.cpp）：

- viewDistance 和 simulationDistance 独立，范围均为 2..32，越界明确抛 invalid_argument；同值设置是 no-op。
- 玩家以调用方提供的稳定 PlayerTicketId 识别。addPlayer 对同玩家同 chunk 为 no-op，movePlayer 同样不刷新；同一玩家出现在新 chunk 时按旧移出+新移入处理。
- PLAYER_LOADING 覆盖为以玩家中心 chunk 为心的 Chebyshev 正方 viewDistance，每覆盖 chunk 一个 level31 票；coverage map 记录每 chunk 的来源数，重叠玩家共享票，最后一个覆盖来源离开才 remove。
- PLAYER_SIMULATION 只在玩家中心 chunk 一个票，level=max(0,31-simulationDistance)；同 chunk 玩家共享，最后一个离开才 remove。
- 每个玩家 add/move/remove 只生成一个最终 TicketOperation 批，调用 TicketLevelSystem::applyBatch；没有逐票全图重算。
- viewDistance 更新重建玩家覆盖并只发非重叠差异票；simulationDistance 更新给每中心原子替换 simulation 票，二者不会互相更新另一层的图。
- 返回 TicketSourceUpdate 带 stateChanged、loadingPresenceChanges、simulationPresenceChanges 和实际 TicketSystemUpdate，便于 T-3/观察者只处理真实变化。
- FORCED API 固定 level31且重复 enable/disable 幂等；PlayerSpawn 请求为 radius3/level30，SpawnSearch 为 radius0/level33。本阶段未造 chunk、未建 future、未实现找点。
- tick 只调用现有 purge，holder 状态由调用者传入；离线空 manager 不创建票。

修复 T-1b 局部 seed 的小开销：现在只遍历 affected 格子，逐格查 sources_.find；不再遍历全维度 sources_ 找 intersects。

Session/服务端最小接线：

- ServerOptions 增加独立 ticketViewDistance；--view-distance 仍保持协议原上限 10，--ticket-view-distance 为 2..32，--simulation-distance 为 2..32。客户端可见范围和票来源范围分开。
- 真实连接创建时以该连接稳定 playerId 调 addPlayer；连接消失时 removePlayer；存活连接的位置/playerId 在主循环中调用 movePlayer。没有改变离线 benchmark 的固定加载、模拟更新、发包内容或 tick gate。
- 当前 Session 已把玩家位置作为 creative-mode 位移状态；这里只把它作为需求来源。没有把发包视距当来源，也没有在来源层实际加载/卸载 chunk、改 World 或启动 spawn search。
- server_main 的 movePlayer 循环只处理非 closed/dead 连接；若连接先关闭，remove 先于位置 moved，避免残留覆盖。Session 不拥有任何票状态。

原版依据：

- ../net/minecraft/server/level/DistanceManager.java:113-160：玩家中心 simulation 来源、同 chunk 最后离开 remove、view tick 与 simulationDistance 替换。
- 同文件 :247-324：PLAYER_LOADING level31 覆盖与视距变化。
- ../net/minecraft/server/level/ChunkMap.java:1031-1090：玩家加入、移动、移除在位置变化时更新 status。
- ../net/minecraft/server/network/config/PrepareSpawnTask.java:34,135-155,173-179：spawn radius3 与 keepAlive 续期。
- ../net/minecraft/server/level/PlayerSpawnFinder.java:41-65,115-142：spawn search radius0 候选 ticket；本阶段只请求票，不实现搜索生成。

验证（仅 tickets_tests，以及实际构建/现有协议回归）：

    ticket view32 sources=4225 loading_recomputes=1 loading_cells=8281 simulation_recomputes=0
    ticket one_move source_changes=2 loading_recomputes=1 loading_cells=756
    ticket view32_player loading_presence=4225 simulation_presence=1 move_loading_presence=130 move_simulation_presence=2 move_loading_recomputes=1 move_loading_cells=4914 move_simulation_recomputes=1 move_simulation_cells=552
    ticket tests passed

T-2a 夹具另外覆盖：同 chunk 多玩家/相交覆盖与最后离开、单格移动窄边带、view/simulation 独立变化、FORCED 幂等、PlayerSpawn/SpawnSearch expiry，以及空来源离线 tick 零重算。已有 protocol_tests 和 light_protocol_tests（206 packets）直接通过，确认 Session 加入 playerId 访问与原网络行为没有回归。没有真实原版服务端逐刻差分；也没有启动服务器/客户端，未跑 CTest、光照性能或正式 benchmark。

日志：

- Build：/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t2-20261112-build-final4.log，SHA-256 c9d50d04b24f7b6d553f108a6134c42e3aaee7449d19f538e6ec6965ec761343。
- Ticket tests：/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t2-20261112-tickets-tests-final4.log，SHA-256 4b34b208967a1a6650afa5a51edcea8cecbe7c62c837752fcdd3aa9825a1e85e。
- Protocol tests：/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t2-20261112-protocol-tests-final2.log，SHA-256 439a920da85feb47170f0195ffd17b536ec54b3acd56c77f6d6a5ad5ca49b960。
- Light protocol tests：/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t2-20261112-light-protocol-tests-final2.log，SHA-256 d15646170c5d13e0b748eead5b6a03c0adc08d60aeea7391f4f37dc62438f8bc。

当前 tickets-related files：headers 495be514c653e63c446a45cd056951e0cab413a713f3d3c585ecade707a665c9、source b13fb97a047cb5dadb87ac8b629ad35c9cff1223a55200432f2cee8e6ea3ff2f、tests fd2b13529b78594759fe766ff6ed229ebe17c4650d0b94e51b8046f77a6a8e05、session header 99bf115c48a88ae176116a20206b17dba9a0cd61a716454782c5f69b754a176f、server main 4b8fa6fed06a21e1bd72bc7a8d27e998aa05b70f9b353267870e1419b6593b17。

未提交、未推送；保留工作区光照收尾和其他已有修改。下一阶段 T-2b 才接完整生成/加载与 spawn 搜索生命周期；T-3 再使用 demand/actual-ready 两个条件替换可刻判定。

### T-2a reviewer correction

- Restored the original unconditional erase_if(connections, dead || (closing && flushed)). Connections cannot accumulate after this patch.
- Removed both ticket-only std::set passes. One existing connection loop now calls idempotent TicketSourceManager::setPlayer(playerId, present, center) per connection. present is true only when the Session is in Phase::Play and is not dead/closing/closed. Status, Login, Configuration and rejected connections therefore create no tickets; leaving Play removes them in the next loop.
- The erase predicate is the original recycle condition, except it also calls exact removePlayer for each retired playerId before erasing. This defers cleanup safely if a connection remains until successful flush. No second pass and no dead/erased path is reconciled.
- ticketPlayerDemandActive gives a direct state policy assertion for non-Play, dead, closing and closed connections.
- setPlayer is the single idempotent reconciliation API: add absent, move existing, remove absent. Repeated same chunk/absent calls are no-ops. Direct fixtures cover add/same/move/remove and connection policy.
- Fixed unspecified evaluation order in setViewDistance and setSimulationDistance by capturing operation count before move. Tests assert both returned count changes. setViewDistance compares through a const reference to the existing coverage map before replacement, avoiding the previous full-table copy.

Review correction validation:

    ticket tests passed

Build: /home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t2a-review-build-final5.log, SHA-256 b4841a948505371929d470d476412bc6f6716af51b91a337a50780e1f3e500ce.
Tests: /home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t2a-review-tests-final5.log, SHA-256 4b34b208967a1a6650afa5a51edcea8cecbe7c62c837752fcdd3aa9825a1e85e.
Files: headers 867dca08fcf285cf37b4a5fc85f76e2dde0a9731a18ea4660ec4ba5995972ab3; source 92a49ac78ede5b4eaf640e663703ea83b5bf410377eb648fec49a09d345ba585; tests bbdc589abe4e937728ca40bb1f24e8486d63db47b48d926c120140b7a52bf0af; session header 99bf115c48a88ae176116a20206b17dba9a0cd61a716454782c5f69b754a176f; server main 812122f76fb6d88eac7b95e94dafc509137eb4a14a09c0186edee2ec010c3ea5.

No CTest, no prior unrelated test rerun, no light performance run, no commit/push. Waiting for reviewer.
