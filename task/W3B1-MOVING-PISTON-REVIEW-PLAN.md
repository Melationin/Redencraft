# W-3b1 移动活塞方块实体：reviewer 验收计划

在 W-3c2 autosave 验收后派发。只覆盖已经能实际模拟的 minecraft:piston 方块实体；
其它 BE、普通实体、客户端额外渲染与生成不加入本阶段。

- 原版 PistonMovingBlockEntity.java:342–359：blockState 使用 BlockState.CODEC，facing 为
  Direction.LEGACY_ID_CODEC 的 TAG_Byte，progress 写 progressO，恢复 progress=progressO，
  extending/source 为 Byte boolean。不保存 lastTicked。注意 26.1 原版活塞方块实体 id
  是 minecraft:piston，方块是 minecraft:moving_piston。LevelChunk.java:472–485 另写
  keepPacked；只支持 active（false 或默认），keepPacked=true 若未实现 lazy promotion
  则明确拒绝，不静默提前激活。
- worker 从 terrain NBT 解析成紧凑 PistonSnapshot；验证坐标、唯一位置、合法 movedState
  和实际 palette 中匹配 moving_piston（双向）。只传 compact records 给主线程，原 root
  仍由 I/O worker 持有。现有 DecodedChunk.blockEntities 的 DOM 深拷贝不要扩到在线负载；
  可保留旧测试读取接口，在线只解紧凑记录或安全 move。未知字段要么随同位置原记录保留，
  要么在不能无损表示时拒绝；原版生成的空 components 应可接受。禁止以空气或默认值
  填补未知 movedState/缺失 BE。
- 原子安装 terrain、计划刻、BE，之后发布 loaded/光照；失败回滚位置索引和 ticker。
  完成、卸载、恢复之间避免调用会模拟 finalTick/邻居更新的移除路径。原版
  Level.tickBlockEntities 依 shouldTickBlocksAt（模拟 tracker）与 runsNormally；
  保存/restoration 不改现有注册/待处理顺序，保持离线语义。
- 运行态 set/remove/改变持久表示（progressO）记脏。一个有预算的保存版本一起携带
  terrain edits、tick lists、该区块的 BE 记录，不能先写 moving_piston state 再另一次补 BE。
  后台更新 block_entities 后重编码一次。分清“列表未改”和“清掉旧 BE”；保存失败仍保留
  在世界和候选队列。只枚举本区块活塞索引，不调用 World::snapshotChunk 复制 24 个 section。
- 真实 Java 26.1 DedicatedServer 夹具（只读母本）：
  /home/zhddsj/.local/state/redstone-cpp/wsl-w4-vanilla-piston-t6dz5m34/interop-world 。
  官方平坦世界保留 [-1,-1] 强加载票，`tick freeze` 后放朝东普通活塞 (-14,-58,-14)，
  前方石头 (-13,-58,-14)，背后红石块 (-15,-58,-14)，`tick step 1`，保存关服。
  实盘 block_entities 已读得两个 active 记录：(-13,-58,-14) moved=piston_head,
  facing=5, progress=0.0, source=1；(-12,-58,-14) moved=stone, source=0。
  两者 extending=1、keepPacked=0、components={}。旁边 server.log 与 piston-tags.txt
  是官方生成与检查证据。后续只跑副本；做 C++ load/暂停/单步或完成/保存，
  让官方服务端读出匹配的活塞头和石头，同时覆盖中途快照 + 再恢复。
- 定向回归：同区块运动中从盘装入、无模拟票不推进、进入模拟范围顺序正确、
  运行时保存中途后重启恢复、完成后旧 BE 移除、保存失败与非支持 BE 拒绝。
  不重跑 CTest，不重跑此前无关票/region/光照矩阵。


## W-3b1 implementation note (2026-11-12 coder)

The online codec retains compact records only. A C++-side strict schema and
terrain/BE bidirectional check reject missing, duplicate, cross-chunk, unknown
field, keepPacked, components and non-piston cases before the manager installs
anything. The World owns a per-chunk piston position index, installs compact
entities and tickers before the loaded lifecycle/light publication, and rolls
the whole install back on failure.

Ticker progress is gated by blockTickPositionReady. Progress and create/remove
events dirty the owning chunk. Saves snapshot only the target chunk's compact
piston list and apply block edits, exact block_entities, and tick lists to the
worker-owned root before one encode/write. Completion writes an engaged empty
BE list, clearing stale records atomically with final terrain.

This stage was validated against a copied official 26.1 paused-piston world:
C++ loaded both active records, enforced no-simulation pause, advanced one
half-step, saved/reloaded progressO, completed after the restored steps, and
removed both records. The official Java NBT probes accepted mid and completed
files and the original DedicatedServer restart script exited 0.


### Review-fix note (2026-11-12 coder)

Both manager constructors now share installWorldCallbacks/detachWorldCallbacks,
so RawChunkStore injection cannot miss the piston mutation observer. A BE-only
FakeStore regression proves progressO changes alone dirty and persist the chunk.

Chunk coordinates are range-checked immediately after decode and in every
editable-root validation; moving-piston pair scanning uses int64 arithmetic and
checked int conversion. Extreme xPos/zPos tests fail explicitly before scaling.
pistonForgetWorld clears the per-chunk piston position index as well.
