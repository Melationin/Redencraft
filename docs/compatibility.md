# 26.1 行为基线与验收边界

## 必须保留的阶段与顺序

`ServerLevel.tick` 中本项目关注的顺序为：推进 gameTime → block scheduled ticks（每刻上限 65536）→ fluid scheduled ticks → chunkSource.tick → block events → entities → block entities → entity management。天气、睡眠、袭击等省略的系统需要在实现处标记 TODO；实体阶段暂时不模拟实体，但不能将移动活塞方块实体提前到方块事件之前。

计划刻不是简单的全局按时间排序队列：区块内使用 triggerTick、priority、subTickOrder；到期区块之间用 priority、subTickOrder 决定 drain 次序。先收集本刻待执行项，再执行回调；执行期间新加入的到期刻不能自动混入这次收集。唯一性依据为位置与类型。区块不 tick 时保留待执行刻。

方块事件按首次插入顺序去重，处理过程中产生的新事件允许继续执行；不能处理的区块事件保留到后续。事件必须再次检查当前位置方块类型；活塞事件再次检查电源，不能仅相信入队时状态。

## 更新不是一种通知

- `Level.setBlock` / `LevelChunk.setBlockState`：flags、回调、方块实体移除/创建、递归深度。
- `CollectingNeighborUpdater`：嵌套更新由 addedThisLayer 与栈驱动；多邻居通知可以暂停以处理新层。
- `NeighborUpdater.UPDATE_ORDER` 与 `Block.UPDATE_SHAPE_ORDER`：分别对照，不能混用。
- `neighborChanged`：用于准连接、电源变化等，不等于 shape update。
- `updateShape` 与间接 shape update：侦测器触发、墙体连接、珊瑚支撑依赖这些路径。
- 放置/移除及移动标志：尤其避免活塞移动中错误提前通知或漏通知。

不要求哈希表跨区块迭代顺序一致；局部方向顺序、事件顺序、subTickOrder 仍属于兼容目标。默认以非实验性红石规则为基线，实验 redstone_experiments 另行移植。

## 源码索引（相对上一级）

| 内容 | Java 源码 |
|---|---|
| 世界阶段 | `net/minecraft/server/level/ServerLevel.java` |
| 更新标志与回调 | `net/minecraft/world/level/Level.java`、`chunk/LevelChunk.java`、`block/Block.java` |
| 连锁更新 | `net/minecraft/world/level/redstone/{NeighborUpdater,CollectingNeighborUpdater}.java` |
| 计划刻 | `net/minecraft/world/ticks/{LevelTicks,LevelChunkTicks,ScheduledTick,SavedTick}.java` |
| 活塞 | `net/minecraft/world/level/block/piston/{PistonBaseBlock,PistonHeadBlock,MovingPistonBlock,PistonStructureResolver}.java` |
| 移动方块实体 | `net/minecraft/world/level/block/piston/PistonMovingBlockEntity.java` |
| 侦测器/音符盒/TNT | `net/minecraft/world/level/block/{ObserverBlock,NoteBlock,TntBlock}.java` |
| 区块读取 | `net/minecraft/world/level/chunk/storage/{RegionFile,SerializableChunkData}.java` |
| 压缩索引 | `net/minecraft/world/level/chunk/storage/RegionFileVersion.java` |

## 分阶段验收

1. 编译与 CTest：存储边界、脉冲延迟、更新模式、事件时序、活塞推动/拉回/限制。
2. 固定实际存档回放：导入状态、计划刻和移动中的活塞，逐刻导出比较结果（本版不写回 Minecraft 存档）。
3. 26.1 服务端差分：先用原版服务端命令控制冻结/单步并检查状态与物品；更细的逐回调轨迹需要 Fabric 测试夹具。完整覆盖目标包括准连接、短脉冲丢块、粘液蜂蜜分离、12 方块上限、跨区块与负坐标、侦测器移除/放置、更新抑制。

尚未做第 3 项时，不能宣称已验证“几乎全部原版特性”。具体当前限制在实现完成后追加。

2026-11-06 已完成用户截图中的地毯复制结构的实际原版 26.1 服务端比较；原版与 C++ 均保留移动后的地毯并产生一个额外地毯物品（C++ 为实体请求日志）。证据见 `validation/vanilla26_1`。其他结构仍主要依赖源码审查与回归测试；更新顺序审查记录见 `docs/update-order-audit.md`。

## 当前实现与剩余差异

本批按用户后续要求排除水/流体模拟，保留其存档数据和属性。默认关闭实验性红石，不模拟玩家、碰撞实体、掉落实体和 TNT 实体；需要创建实体的路径记录日志。移动活塞作为方块实体实际执行，并非仅日志占位。

后续已添加 Windows 调试客户端（见 client.md）与 16 色羊毛地毯。地毯参考 `CarpetBlock.java`、`WoolCarpetBlock.java` 与 `Blocks.java`：下方非空气即可支撑，任意方向的形状更新均检查存活，厚度 1/16，推动反应 NORMAL，失去支撑时按颜色记录物品实体请求。

已经区分 Simple（执行时读取状态）与 Full（使用捕获状态）的 neighbor update、可被嵌套层暂停的 Multi update、Shape update、间接形状更新入口、移除/放置回调。更新标志、递归限制、更新总数限制分别处理。已实现本批方块的 shape 路径，包括侦测器调度、音符盒乐器属性、草方块 snowy、墙与珊瑚支撑、活塞头存活。当前支持的方块没有间接形状更新的覆写；接口保留以便未来接入红石线等。

活塞包括 12 方块限制、粘液/蜂蜜分离与分支、准连接、伸出/回缩/丢块事件、每刻 0.5 的移动进度、强制 finalTick、移动实体移除副作用、源/目标通知顺序及结束时重新计算形状。移动方块实体按注册顺序推进，执行过程中新增的实体留到下一次方块实体阶段；区块卸载时暂停，重新加载后恢复。`handlingTick` 在方块事件后、方块实体前清除。

以下仍须明确区分：

- 区块和子区块已经分层，但子区块内部当前是稀疏状态表；不是原版完整的 PalettedContainer 压缩存储。状态也未做全局驻留/ID 化，尚不适合作性能对比结论。
- 未实现随机刻、光照、高度图、天气、睡眠、世界边界动画、袭击、命令等系统。草方块随机蔓延/退化不在当前循环中。
- 注册表覆盖本批方块与日志用实体类型，不包含全游戏内容。未支持的方块保留状态，碰撞形状保守按整方块处理；其回调和硬度/推动反应不保证原版一致。因此不能用含其他机制方块的存档推断本批逻辑错误或正确。
- 墙及活塞的盒形状实现覆盖本批方块；与未实现方块标签、特殊面的互动仍待扩充。实体碰撞推动、粘液弹跳、蜂蜜携带只保留日志与 TODO。
- 音效、粒子、客户端通知、游戏事件、完整掉落表和其他方块实体尚未实现。音符盒只保证本批范围内的更新路径；不模拟头颅自定义声音。
- 区块控制是显式加载开关，无加载票，也无原版加载阶段/相邻区块等级计算。卸载的数据保留在内存以便重新加载，未实现磁盘按需驱逐。
- 计划刻已按区块头部比较器收集，执行阶段与收集阶段分开；公开存储暂为线性容器，性能优化待做。存档导入不含原版 DataFixer；只接受 26.1 DataVersion 4786、完整生成的区块。
- 存档中的 gamerules、实体、持久化强加载列表与自定义维度元数据未自动导入；API/CLI 显式设置高度与加载区块，TNT 游戏规则默认开启。

后续验收优先补真实服务端逐刻差分，再做状态驻留、调度索引和 palette 存储优化。不得用性能优化改变更新语义。
