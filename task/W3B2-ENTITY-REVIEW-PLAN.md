# W-3b2 普通实体存盘：reviewer 后续计划

待 W-3c2 / W-3b1 落地再领取。第一小阶段支持 item；TNT、落沙、矿车分开验证；
未知实体、未实现 passengers/components 不静默舍弃。不要在本阶段扩大 World 实体模拟范围。

- 以 EntityStorage.java:46–109 和 PersistentEntitySectionManager.java 为准：
  维度本地 entities/*.mca，独立槽根含 DataVersion、Position IntArray[2]、Entities。
  校验实际槽位置和每实体坐标，加载后按 readiness 接入并保留 UUID；runtime 协议 ID
  是另一个数值，不能拿来替代持久 UUID。空槽、原版零字节 region 正常；已分配外部槽
  的 .mcc 缺失不得冒充空实体数据。非支持实体数据不覆盖，读/保存失败显式报告。
- 先检查 engine 的 item 现有导入字段与实际 Entity/ItemEntity 持久字段，做 compact record；
  维持 Pos、Motion、Rotation、OnGround、Age、PickupDelay、Health、Item id/count，
  保留/验证其余 NBT（Owner、Thrower、Fire、Air、PortalCooldown、Invulnerable 等）。
  只接受可模拟/可安全往返的状态，标明边界。不为已有实体每 tick clone 整个 NBT。
- 异步读和主线程生命周期用已有 epoch；同一 chunk 在 terrain 和 entities 都就绪后
  原子安装可见状态。存盘是两个 region 文件的独立操作，不声称跨崩溃事务；
  in-process 必须等待所有必要写确认才驱逐。跨区块移动的下一阶段要求目标先确认，
  源再删除，并用 UUID 去重/明确错误，避免部分失败使实体永远丢失。
- 沿用 autosave budget；只抓目标 chunk 的实体 compact snapshot，移动/移除保留脏标志；
  用 per-chunk 索引或已有 entity section 范围，不在网络轮询扫全实体。无其他 I/O 线程
  同时改同一个 region。实体最后移走时写入真实空槽语义（原版 IOWorker.STORE_EMPTY，
  没实现物理清槽前可明确测试兼容空根与后续优化，不把旧实体保留）。
- 真实官方 26.1 DedicatedServer 夹具母本：
  /home/zhddsj/.local/state/redstone-cpp/wsl-w4-vanilla-item-31h2fwua/interop-world 。
  [-1,-1] 强加载下冻结游戏，summon item at (-13.25,-58.2,-13.75)，石头 x3，
  Motion=(.125,0,-.125)，Age=17，PickupDelay=200，Health=5，保存并正常退出。
  原版 entities r.-1.-1.mca 目标槽真实写有一条 item，Position=[-1,-1]、DataVersion=4786；
  完整 decoded 标签见同目录父级 entity-tag.txt，官方命令输出见 server.log。
  后续用副本验证实际加载、模拟、保存、原版重启，以及删除后不再复活。

迁移后续 W-3b2b 的验收依据：PersistentEntitySectionManager.java:179–201 只有 LOADED
的实体区块才允许写；FRESH 且有 runtime 实体会先 requestChunkLoad，PENDING 不写，
避免覆盖目的槽中还没读过的旧实体。已知 UUID 集合在加载时检查重复（:63–66），正常
callback 移动 section 并改变 tracking/ticking；卸载回调才移出 knownUuids（:438）。
因此不能把“目标 terrain 已 loaded”当“目标 entities 旧内容已读完”。第一阶段安全
阻止跨 chunk 写回时需记录 source 和 destination；后续先确保两端实体存储已读，再
交接 UUID 所有权。碰撞处理和 partial save/crash 是单列兼容性边界，不可假装两 region
rename 自动成为一个事务。


## W-3b2a implementation note (2026-11-12 coder)

Implemented item-only persistence through a compact entity codec and one shared
I/O worker. Terrain and entity roots are owned by the worker; installs transfer
only compact item records to main. Write order is terrain then entity
on the same worker, with savedVersion advanced only if both writes succeed.

The official item fixture was loaded with its UUID and all supported fields,
held without entity readiness to keep Age unchanged, then advanced one tick
when the simulation/entity ring became ready. The mid entity region and final
empty list both passed official Java NBT probes. A copied official server
restarted against the completed world and confirmed no item entity remained.

Unsafe cross-chunk movement is detected from the old/current tick positions and
blocks both source and target eviction. Safe target-first migration remains the
next stage. No physical empty slot deletion is claimed; a validated empty root
is used.


### Final verification note (2026-11-12 coder)

Final direct tests passed for entity codec, real-disk lifecycle, and manager
including official item load, pause/readiness behavior, dual-region save,
failure retry, duplicate UUID rejection, cross-chunk blocking and empty reload.
Official Java NBT readback accepted both mid and empty entity files; a copied
official DedicatedServer restart reported ITEM_EMPTY_AFTER_CPP.


### W-3b2a 存档安全审查返工结果（2026-11-12 coder）

已只处理审查列出的三项，保留现有未提交改动；未改光照、未跑 CTest/Java、
未提交或推送。

1. 持久 UUID：World 新实体的零 UUID 现在由独立于 World::random 的一次性
   std::random_device 种子生成器产生 UUIDv4，已有实体/已知 UUID 不会被改写；
   冲突重试，非零重复 UUID 继续明确拒绝。同 seed、同 runtime id 的两个 World
   实例与同进程连续分配均有反例。
2. 缺失外部实体流：region 已分配 external slot 但对应 .mcc 不存在时返回
   Corrupt；覆盖写会在修改 region 表项前拒绝，测试验证原 region 字节不变。
   未分配槽、零字节 region 和空槽的 Missing 语义未改。
3. 极端数值：item entity load/save 共用归一化，先拒绝 NaN/Inf，再将 Pos
   X/Z 限为 +/-30000512、Y 限为 +/-20000000，将绝对值大于 10 的 Motion
   分量按 Entity.load 规则归零；之后才执行 floor/整数与 chunk 校验。大有限
   坐标不会进入 int 转换或碰撞，正常官方 item fixture 的 Age/PickupDelay
   仍为 17/200。

改动文件：src/entity.cpp、tests/entity_tests.cpp、src/region.cpp、
tests/region_tests.cpp、src/entity_codec.cpp、tests/entity_codec_tests.cpp。
未新增 chunk/entity DOM/光照页或全局实体列表复制；本次只新增小 UUID 记录、
窄归一化和既有拒绝路径。跨区块迁移仍阻塞，空实体根仍未改成物理删槽。

Evidence:
/home/zhddsj/.local/state/redstone-cpp/wsl-w3b2a-reviewfix-BKLsBM
build.log SHA-256 8c35b1682f73592d30612d7b0fabdb384fcfff35cb5a076cb48b85b1368aa794;
entity.log 8078ca10f75cf630d05c4a25faefdbeb5fea9d1c298836755fd6a6c79320f54a;
entity-codec.log 481293f39c8820663b46e1ecb2a12b2cb61782238381bcdee01d30a2544a20c2;
region.log 0fec8bb09ab61119d2e43789e80409ef1dd95c9db2104d802efeeea8adca8045;
manager.log 93531a1e888a2caa8cdd8fc266f1c386171d5a1124fef0adc0e1a9429b5db1c8.
redstone_vanilla_server SHA-256
c16448363238cbada0573d374d0690a4991bad90077e8520fe124e2df6e0b095.

Direct outputs include persistent_uuid same_seed_same_runtime_id=distinct,
region external_missing=corrupt overwrite=refused, entity_codec
motion_clamped=1 huge_position=rejected nan=rejected, and all four affected
tests passed. Native target rebuilt. This closes the three review blockers
pending reviewer acceptance.


## W-3b2b implementation result (2026-11-13 coder)

Implemented target-first cross-chunk migration for the supported minecraft:item
entity type. Each UUID carries a compact migration record containing its live
target, target residency epoch, the exact accepted target snapshot version, and
the source chunks whose entity lists must be cleared. A source save is gated
until the matching target snapshot (save version and epoch) completes. Failed
target writes leave source data and residency intact; failed source cleanup is
retained and retried through the existing bounded save/backoff queues. Entity
removal and return-to-source update the same ledger; a same-UUID round trip is
covered.

The target uses the normal full item-list snapshot, so an existing target item
is preserved and merged without copying a chunk or NBT DOM. The target must have
been a managed, compatible entity read before it can be written. A missing
target residency or rejected target entity slot leaves the source pinned and
reported rather than synthesizing an empty slot. Runtime entity instances keep
their persistent UUID and are not reinstalled by migration.

Validation used the focused world_chunk_manager_tests binary with injected
entity-store failures and an isolated copy of the official 26.1 item fixture:
negative-coordinate cross-region target-first write, target item merge,
return-to-source, removal before commit, target failure with source retention,
source cleanup failure followed by retry, unload/reload with one UUID, and the
existing stale-generation/duplicate rejection tests all passed. No CTest, Java
suite, lighting run, source-save write, commit, or push was performed.

Evidence: /home/zhddsj/.local/state/redstone-cpp/wsl-w3b2b-final-YqhGuN
build.log 8c35b1682f73592d30612d7b0fabdb384fcfff35cb5a076cb48b85b1368aa794;
manager.log 030e8099d02c9294de5024d2edab484dfa9c597efba7bca6aa69ad0fbf822d49.
world_chunk_manager_tests 15161bdf90554599df1af398251fbb54133ad54b12ea8311d0bc986803e06e16;
redstone_vanilla_server 6e8c3b847546d31dc6114a14009cd5416b838708d5ab39de011f4f8270c5f808.

Boundaries: target write and source cleanup remain two independent region
writes, so no cross-file or power-loss atomicity is claimed. A crash between a
confirmed target write and source cleanup can temporarily leave the same UUID
in both slots; loading rejects duplicate UUIDs rather than deleting either copy
heuristically. Simultaneous cross-migration cycles remain conservatively pinned
until the cycle clears; the ordinary same-UUID return path is supported.


Final implementation note (2026-11-13 coder): the migration ledger now keys by
persistent UUID and gates every source write behind an exact target snapshot
acknowledgement carrying the same epoch. Target failures leave source storage
and residency pinned; source-cleanup failures remain queued through the normal
bounded retry path. Returning to the original chunk cancels that source cleanup,
and removal before commit retains cleanup ownership until both sides no longer
list the UUID. Normal tick/add/remove/explosion/piston-driven movement reaches
the observer; UUID is unchanged by movement.

The final focused run covered target-before-source ordering, target merge with an
existing item, source failure retry, target failure retention, return-to-source,
removal before commit, unload/reopen with one UUID, and a negative-coordinate
cross-region cycle against an isolated copy of the official 26.1 item fixture.
The existing malformed entity-slot test continues to prove rejected target data
is not overwritten. No CTest or Java rerun was performed.

Evidence: /home/zhddsj/.local/state/redstone-cpp/wsl-w3b2b-final-YqhGuN
build.log 8c35b1682f73592d30612d7b0fabdb384fcfff35cb5a076cb48b85b1368aa794;
manager.log 030e8099d02c9294de5024d2edab484dfa9c597efba7bca6aa69ad0fbf822d49.
world_chunk_manager_tests 15161bdf90554599df1af398251fbb54133ad54b12ea8311d0bc986803e06e16;
redstone_vanilla_server 6e8c3b847546d31dc6114a14009cd5416b838708d5ab39de011f4f8270c5f808.

Limits: two independent region writes are not a cross-file atomic transaction.
A crash after target confirmation and before source cleanup can leave the same
UUID in both slots; loading rejects duplicate UUIDs rather than deleting one by
read order. A simultaneous dependency cycle involving different UUIDs remains
conservatively pinned and reported; the normal same-UUID return path completes.

Final rerun: focused world_chunk_manager_tests passed; native server target rebuilt. No CTest, Java, lighting suite, commit, or push.


### W-3b2b 严重问题审查返工（2026-11-13 coder）

已仅处理审查列出的三项阻塞。保留既有未提交改动；未改光照，未运行 CTest、
Java 或光照测试，未提交、推送或写原存档。

1. 迁移状态改为按 chunk 的正向 target 索引和反向 source 索引。确认事件从对应
   索引取 UUID 后先复制受影响集合，再完成状态更新和删除，不再在遍历 map/set
   时擦除当前元素。移除了从当前 dirty/status 推断目标 ACK 的轮询逻辑；只有
   save payload 被 controller 接受后才登记意图，且目标确认要求持久 UUID 确实
   出现在目标 payload（删除确认使用明确的 absent intent），完成时精确匹配
   epoch 与 save version。后续无关保存失败不会回滚已确认目标。源清空同样要求
   对应 epoch/version 的精确成功完成。
2. pump 不再全扫迁移表；目标安装、实体变化和保存完成只入有界事件队列，空闲
   pump 不访问长期卡住的迁移。实体位置只更新 UUID 到原区块的索引，普通 tick
   不再为源/目标构造临时 std::set。批量 8 个未就绪迁移的测试验证队列排空后
   128 次空闲 pump 的迁移访问计数不增长。
3. WorldChunkManager 持续记录实体上次索引位置，因此其它实体或运行阶段在自身
   tick 前移动、以及跨块后同 tick 移除，都能用旧索引区块识别。新增真实
   world.tickEntities() 路径和实体速度跨块测试，未用人工 observer 作为唯一
   验证。目标未驻留时保持源驻留，close 返回未完成且源记录仍在。

最终定向输出：
item_entity migration negative=1 merged=1 source_cleared=1
item_entity migration real_tick=1 target_first=1
item_entity migration velocity_crossing=1
item_entity migration stale_ack=0 multi_hop=1
item_entity migration return_to_source=1
item_entity migration removed_before_commit=1
item_entity migration_failure=1 source_retained=1
item_entity migration unready_batch=8 idle_scans=0
item_entity migration reload=1 uuid_preserved=1
item_entity duplicate_uuid_rejected=1
item_entity migration real_region=1 target_then_source=1
world chunk manager tests passed (with real World disk fixture)

Evidence:
/home/zhddsj/.local/state/redstone-cpp/wsl-w3b2b-review-final-pFNlF3
build.log SHA-256 c9d50d04b24f7b6d553f108a6134c42e3aaee7449d19f538e6ec6965ec761343;
manager.log SHA-256 4f1c15a8a5a15ceed51f19bad4e5444e0a08488105226170bc0b33508e00e852.
world_chunk_manager_tests SHA-256
8981904e8db11633c8dc64cb42f80bf9a4b999b5e15b88cafa4dbcd63eaea058.
redstone_vanilla_server SHA-256
55caed5fffdba4aee8fc4e9a82b68b736f3c24599b545a44c6568c51abb75a27.

仍保留的边界：跨文件目标写与源清理不是掉电原子事务；目标成功后、源清理前
崩溃可能留下两个槽的同一 UUID，加载路径拒绝重复 UUID，不做任意删除。不同
UUID 形成相互依赖的复杂迁移仍保守驻留；同一 UUID 的返回与多跳顺序已完成。


### W-3b2b 严重问题审查返工结果（2026-11-13 coder）

仅处理审查列出的三个阻塞，保留工作区其余改动。

- 迁移确认：仅在实际 requestSavePayload 被 controller 接受后登记目标意图。
  目标成功必须同时匹配持久 UUID 确实出现在 payload、目标 epoch 和精确保存
  版本；删除使用明确的 absent intent。源擦除也要求该源 epoch、精确版本和
  payload 不含 UUID。已移除从 dirty/status 推断 ACK 的后备路径。迟到目标
  完成不能释放新目标；已确认目标不会被之后无关保存失败回滚。
- 迭代安全：每 chunk 建立 target/source UUID 索引；保存完成和 pump 都先复制
  受影响 UUID/源位置列表，再修改或擦除 map/set，不再擦除正在遍历的元素。
- 热路径：删除每 pump 全扫 entityMigrations_。推进仅由实体变化、目标安装、
  save 完成或失败事件加入有界队列；空闲 pump 不访问长期未就绪的迁移。普通
  实体 tick 不再构造临时 std::set，仅更新上次位置和最多两个既有 chunk。
- 真实移动：Manager 跟踪每个 UUID 上一次已索引位置；活塞或其它实体在自身
  tick 前移动、以及跨块后同 tick 删除，都从旧索引位置识别。新增真实
  world.tickEntities() 和实体速度跨界验证。
- 关服：目标未加载/不可读或尚未收到精确确认时，源保持驻留，close 返回
  未完成并保留源记录；不会因源 dirty/无实体槽状态而跳过迁移。

最终定向测试全部通过：
item_entity migration negative=1 merged=1 source_cleared=1
item_entity migration real_tick=1 target_first=1
item_entity migration velocity_crossing=1
item_entity migration stale_ack=0 multi_hop=1
item_entity migration return_to_source=1
item_entity migration removed_before_commit=1
item_entity migration_failure=1 source_retained=1
item_entity migration unready_batch=8 idle_scans=0
item_entity migration reload=1 uuid_preserved=1
item_entity duplicate_uuid_rejected=1
item_entity migration real_region=1 target_then_source=1
world chunk manager tests passed (with real World disk fixture)

改动文件：include/redstone/engine.hpp、src/entity.cpp、
include/redstone/world_chunk_manager.hpp、src/world_chunk_manager.cpp、
tests/world_chunk_manager_tests.cpp。

Evidence:
/home/zhddsj/.local/state/redstone-cpp/wsl-w3b2b-review-final-w1q9B0
build.log SHA-256 51ab8d58678195d8f915e482692c779520012fec1c559c535fa7fd5d5a8b7c18;
manager.log SHA-256 4f1c15a8a5a15ceed51f19bad4e5444e0a08488105226170bc0b33508e00e852.
world_chunk_manager_tests SHA-256
d7f0b97e1fcbdd8238076573b3f440086dde2df1533699df74286b5266531f08.
redstone_vanilla_server SHA-256
809d58719928793e6f4eecc2b29eb37e7cdcd44bd2dcc4f0d1530069d404ec9c.

边界：两次 region 写仍不是跨文件/掉电原子事务。目标成功后、源清理前崩溃可能
暂时在两槽保留同一 UUID；加载拒绝重复 UUID，不做任意删除。复杂多 UUID
依赖环仍保守驻留并报告；同一 UUID 回源、多跳及迟到确认已覆盖。


### W-3b2b 最终阻塞返工结果（2026-11-13 coder）

仅修复审查列出的最终阻塞，未改光照、未运行 CTest/Java/光照测试、未提交或推送。

- 活塞在自身实体 tick 之后调用 Entity::move 时，生产移动路径立即更新实体 section
  并向 manager 发送旧/新位置；不再等待下一次实体 tick。普通实体 tick 的原顺序
  保持，每个实际位移只同步一次。
- manager 初始化并维护 UUID 到上次索引位置的映射；真实卸载成功后擦除该 chunk
  的 item UUID 位置记录。新增 ledger-size 断言确认卸载后不残留。
- 只有本次保存实际包含 itemEntities payload 时才登记 UUID present/absent 迁移
  意图；terrain-only 保存不再影响迁移确认。
- 目标确认只在对应 save 被 controller 接受且完成事件精确匹配 epoch/version 与
  UUID 意图后成立。已确认目标卸载后，迁移工作不再提前返回，源清理仍继续。
- pump 仅消费有界事件队列；target/source 使用按 chunk 索引，空闲时不扫描所有
  迁移，普通实体 tick 不构造临时 set。遍历均使用先复制的受影响集合。

窄测试最终通过：
item_entity migration negative=1 merged=1 source_cleared=1 target_unloaded=1
item_entity migration real_tick=1 target_first=1
item_entity migration velocity_crossing=1
item_entity migration piston_after_tick=1
item_entity migration stale_ack=0 multi_hop=1
item_entity migration return_to_source=1
item_entity migration removed_before_commit=1
item_entity migration unready_batch=8 idle_scans=0
item_entity migration reload=1 uuid_preserved=1
item_entity duplicate_uuid_rejected=1
item_entity migration real_region=1 target_then_source=1
world chunk manager tests passed (with real World disk fixture)

改动文件：include/redstone/engine.hpp、include/redstone/piston.hpp、
include/redstone/world_chunk_manager.hpp、src/entity.cpp、src/piston.cpp、
src/world_chunk_manager.cpp、tests/world_chunk_manager_tests.cpp。

Evidence:
/home/zhddsj/.local/state/redstone-cpp/wsl-w3b2b-final-blockers-xFymXB
build.log SHA-256 0a8ecf94fc1fba7197c7eb9c97944539cde1963f65c0501db169000c79a4928b;
manager.log SHA-256 c1c7fab9ba9587c0175e5ff4493293ae6b88f1963a35eea1a0ca086014b5d6ae.
world_chunk_manager_tests SHA-256
432c5d378b0202bebd5afd65957637ac3658cd1c5b58a3fd109b7d453050c28c.
redstone_vanilla_server SHA-256
9a0faa2e5f445058ea5ff44272ba92cfd695e1051cd26ac31c894698befa8799.

未新增 chunk、NBT DOM、光照页或全局实体列表复制；新增状态是 compact UUID 位置
账本和按 chunk 的迁移索引。既有两个 region 写的非原子崩溃窗口保持不变。
