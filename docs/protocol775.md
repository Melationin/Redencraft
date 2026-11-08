# 原生 C++ 协议 775 服务端

后续已支持沙子、石砖墙和红石灯，以及完整投影快照导入，见 [投影模拟](litematic.md)。世界中的全部指定区块保持强加载，客户端采用周边视野及带 ACK 的分批传输，已取消旧版 64 区块总量限制。玩家**聊天栏**每秒更新一次 `Tick N: x.xxx ms (1s avg, M ticks)`：取报告时刻前 1 秒内完成的各 tick 耗时的算术平均，并显示样本数；边界为 `(报告时刻 − 1s, 报告时刻]`，卡顿恢复后不补发积压报告。仍逐 tick 计时，包含世界 tick 和连接同步准备，排除休眠、tick 外 socket IO 及耗时消息本身。

用户选择原生 C++ 直连并要求放置/破坏方块，因此新增独立可选目标 `redstone_vanilla_server`，不依赖真实服务端桥接。默认只监听 `127.0.0.1:25565`，离线模式；协议数据来自未修改的官方 26.1 jar。26.1.2 同为 775，但它的静态 ID 和实际连接尚未独立验证。26.2 不在此协议资产支持范围。

本批遵照要求没有启动真实客户端、隐藏预览或接管桌面。下列命令供用户运行；自动测试仅运行无界面 C++ 服务端和脚本 TCP 连接。

```powershell
cmake -S . -B build-jemalloc -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static" -DREDSTONE_BUILD_VANILLA_SERVER=ON
cmake --build build-jemalloc --config Release -j 6
ctest --test-dir build-jemalloc -C Release --output-on-failure
./build-jemalloc/Release/redstone_vanilla_server.exe
```

在原版客户端中连接 `127.0.0.1:25565`。默认创造模式并允许飞行，初始九格包含平滑石、粘性活塞、粘液块、白地毯、红石块、侦测器、枯管珊瑚扇、蜂蜜块、TNT。通过原版创造列表选择本项目支持的方块；滚轮/数字键切换、左右键编辑、中键选取、Shift 绕过音符盒调音、交换副手已接入。未支持的方块和自定义组件会被服务端拒绝并修正物品栏，不会假装已经实现其机制。

不传存档时提供固定九区块的平滑石测试平台；这是显式测试夹具，当前阶段不生成缺失地图（目标形态含世界生成，见 [AGENTS.md](../AGENTS.md) 第 1 节）。运行现有只读存档可指定：

```powershell
./build-jemalloc/Release/redstone_vanilla_server.exe --world "D:/Minecraft/saves/Test" --chunk 0,0 --chunk -1,0 --spawn 8.5,66,8.5
```

`--chunk` 可重复；未指定则选择存档中已读取的全部区块。指定的区块必须存在，当前阶段不会因玩家移动或放置自动加载/生成区块（目标形态是加载票驱动，见 [PLAN.md](../PLAN.md) 阶段 4）。`--view-distance 3` 控制网络视野半径（2–10），独立于模拟强加载；客户端移动时发送新的中心和区块、卸载旧视野。大世界每批最多 2 区块，小场景最多 9 区块，等待 `chunk_batch_received` 再继续。当前视野为方形边界，原版圆角边界和客户端距离协商仍为 TODO。协议维度目前固定原版 overworld 高度 [-64,320)，存档仍要求 DataVersion 4786；当前阶段没有保存（目标形态见 [PLAN.md](../PLAN.md) 阶段 3）。`--port 0` 自动分配本地端口，`--ticks N` 用于有限时长自动测试，`--assets` 可选择导出资产目录。Ctrl+C 正常结束；不执行自动客户端启动。

协议里的时间字段只反映当前内存 `gameTime`。26.1 World Clocks、`world_clocks`、`advance_time`、睡眠跳转和 `/time` 的时钟语义尚未实现；`/tick freeze/step/sprint` 只是测试控制入口。两者都不写回 `level.dat`。

## 连接与同步

- 实现 handshake/status ping、离线 UUID v3 登录、login acknowledgement、空 known-packs 协商、完整 registry/tags 配置及 finish acknowledgement。
- Play 发送创造玩家登录、玩家信息、能力、出生点、区块中心、chunk batch、时间、初始九格、传送确认和 keepalive。
- 区块采用 26.1 的 section 双计数（non-empty、fluid）、palette 固定 long 存储、原版状态标志计算的高度图及移动活塞 NBT（ByteTag facing、progressO）。流体状态、流体刻传播和自然方块状态更新已经参与核心模拟和区块/单格广播；协议光照段仍由占位值填充。块/物品/实体等 ID 使用完整原版导出表，而非 C++ 子集的注册顺序。
- 创造放置与破坏经过距离、范围、已加载区块、物品、支撑及当前玩家/简单实体碰撞检查；处理珊瑚地面/挂墙候选、活塞和侦测器方向、铁轨方向、草/墙/音符盒初始属性、音符盒右键调音、创造破坏活塞头关联基座、矿车坡道放置。侦测器放置不得调用会调度计划刻的通用 updateShape 查询。
- 输入预测 sequence 在修改世界之前校验，修正包和 block_changed_ack 回传；负序列不会先编辑再断开。创造丢弃限制堆叠/频率，不向未加载区块生成暂停实体。
- 物品、TNT、下落方块、矿车生成时用包组发送实体和非默认元数据。跟踪采用原版 `VecDeltaCodec` 的 1/4096 位移及 `Mth.packDegrees` 的 1/256 圈旋转。位移超出 short、着地变化或超过重同步阈值时发送 `entity_position_sync`。同一轮更新先发变化的速度，再发位置或旋转，最后发变化的 TNT 元数据。TNT 引信每刻变化，因此也会每刻进入跟踪更新。实体受推力时可以提前进入更新。矿车轨迹插值、物品后续元数据更新与原版受击专用速度同步仍待实现。

## 广播顺序

`World::setBlock` 按 flags 记录普通方块变化，`World::tick` 在计划刻之后、方块事件之前读取最新状态并合并。每个区块内从低到高处理 section，同一 section 内按原版 `ShortOpenHashSet` 的迭代顺序发送，重复位置只发一次。单个位置使用 `block_update`，多个位置使用包含完整 64 位 `SectionPos` 的 `section_blocks_update`。跨区块采用确定的坐标顺序，原版的顺序取决于 JVM 中 `ChunkHolder` 的对象身份哈希。

成功方块事件在 `pistonBlockEvent` 等回调之后立即发给距离小于 64 格的客户端。事件、实体和方块实体阶段产生的普通方块变化留至下一刻区块广播。客户端先根据事件解析活塞推动结构，下一次区块广播再接收活塞源位置的变化。输入预测修正包独立立即回传。

实体跟踪广播位于区块变化广播之后、方块事件和本刻实体运动之前，对应 `ServerChunkCache.tick` 中的 `ChunkMap.tick`。实体区段查询使用包含式上界，以覆盖区块 X 为 -1 时的最大打包键。实体注册后立即发包，下落方块落地修正先于移除。引信耗尽时先发旧 TNT 的移除，再发连锁点燃的 TNT，最后向距离小于 64 格的客户端发送爆炸效果。爆炸粒子和声音 ID 来自 26.1 导出。协议玩家的爆炸击退及其他实体在每次 `discard` 调用点立即通知仍待实现，普通清理在当前实体阶段或方块实体阶段提交移除。

世界状态和普通包 payload 仍在主线程采集/构造；独立的 `NetworkWriter` 负责 VarInt 成帧、区块高度图/palette/位打包、发送缓冲合并、写侧 `select` 和 `send`。区块先复制子区块 palette + 索引，再在主线程把本地状态 ID 映射成协议 ID、冻结移动活塞 NBT；后台编码只读该快照与不可变协议状态标志，不访问 `World` 或可能扩容的全局状态注册表。实体同步只扫描玩家视野内的 entity section；块变化缓冲复用容量，热路径 payload 移交时不再复制。

每连接有界 SPSC 队列保留 FIFO；tick 内提交不获取写线程互斥锁，也不等待 socket。队列最多 4096 批、65536 包、16 MiB 计费存储，计费包含正在编码的批次及 deferred 区块持有的容量；Session 暂存另限 16 MiB，已编码未发送数据另限 16 MiB。这些是分别实施的积压预算，不是连接总内存上限，容器保留容量和编码临时缓冲另计。达到上限只关闭该连接。写线程每轮每连接最多编码 64 包或约 2 ms，未发送数据达 1 MiB 时暂停继续编码；单个区块编码不可中途抢占。这些阈值是本项目的背压策略，不声称与原版 Netty 参数一致。诊断输出也通过有界槽交给写线程。网络映射失败不会向核心更新链抛出协议异常，各模拟更新阶段保持独立。

## 验证与剩余工作

该协议批在 MSVC Release + jemalloc 下的 CTest 为 **14/14**（2026-11-07 记录）。除既有行为用例外，新增队列 FIFO/容量/并发、冻结快照后台编码期间注册扩容、实体广播阶段/注册/落地移除顺序、块变化去重后读取最新状态、Session 积压隔离，以及 1 秒滑动窗口平均值和报告节流断言。socket 测试实际启动有限时长的本地 C++ 服务端；慢连接压测触发 1 次输出积压断开，健康连接仍通过 ping、放置、破坏并收到递增 tick 报告，服务端完成 160 tick。投影 TCP 冒烟通过 50 个区块、视野切换与右键激活。详细结果见 [发包验证记录](../validation/performance/2026-11-07-network-sync.json)。当前全量基线见[交接文档](handover.md#当前状态2026-11-09)。

`validation/protocol775/export.ps1` 从官方 jar 重建资产并检查版本与哈希。其 `-VerifyPlayPackets` 使用官方 GameProtocols codec 解码 C++ 测试生成的所有记录，区块继续用真实 LevelChunkSection.read 解码 24 个子区块，检查无剩余字节。来源、格式和结果见 `validation/protocol775/README.md`、`verification-result.json`。这验证编码和源码约定，不能代替真实客户端实际连接、资源加载和动画验证。

本轮将 wire/protocol 测试产生的记录合并，使用同一官方 codec 验证 **294 包通过**；导出写入构建目录，未覆盖固定协议资产。没有启动真实 Minecraft 客户端，也没有运行本轮真实 26.1 服务端逐 tick 差分，不能声称原版等价。

仍有明确 TODO：真实 ServerPlayer 服务端物理与推挤、其他玩家模型/放置碰撞、动态区块跟踪、玩家拾取/容器与生存、任意物品组件/工具与完整 BlockPlaceContext、聊天与配方、原版光照引擎、其他生物群系、音效粒子和完整元数据优化。当前 sky light 统一全亮、`biome` 统一为 plains，仅用于查看；流体状态和流体刻已经运行并完成 F-7 当前核心验收，剩余差分与性能补证见 [F-9a/F-9b](../PLAN.md#阶段-25--流体补充验收)及[流体验收证据](fluid-acceptance.md)。网络支持是创造编辑入口，不代表完整原版多人服务端等价。

## 命令系统与 `/tick`

命令骨架是 `CommandDispatcher` / `CommandSourceStack` / `CommandNode` 的最小可用子集，`/tick` 按 `TickCommand.java` 逐条复刻：

- `src/command.cpp`：字面量节点、参数节点（`brigadier:float` 与 `minecraft:time` 两种）、节点要求（`CommandNode.getRequirement`）和执行函数。解析失败返回 -1，与"未知或不完整命令"同一条路径。
- `Source::success/failure` 产生 `{translate, with}` 形式的反馈，客户端用自身语言文件渲染；`broadcast` 对应 `sendSuccess(..., true)`。
- `include/redstone/tick_rate.hpp`：`TickRateManager` + `ServerTickRateManager` 语义——`tick()` 决定本刻是否推进游戏元素，冻结时按 `frozenTicksToRun` 放行；`/tick sprint` 立即推进并统计；`finishTickSprint` 的报告以值返回，由服务端打印并下发。
- 子命令：`query`、`rate <1..10000>`、`step [<time>|stop]`、`sprint <time>|stop`、`freeze`、`unfreeze`；`time` 接受 `t`/`s`/`d` 后缀，1s = 20 tick、1d = 24000 tick，与 `TimeArgument` 一致。
- 客户端协议：进入 play 后发送 `clientbound/commands`（广度优先节点表，参数类型 id 1 = `brigadier:float`、43 = `minecraft:time`，管理员节点带 restricted 标记），随后发送 `ticking_state` / `ticking_step`，状态变化时重新广播。收到 `serverbound/chat_command` 后解析执行，反馈走 `system_chat`。
- 补全：`serverbound/command_suggestion` → `clientbound/command_suggestions`。按 Brigadier 的方式沿已完成的词走树，未完成的词只做前缀过滤：字面量子节点给出自身名字，参数节点给出 `ArgumentCommandNode.getCustomSuggestions` 的示例值（`rate` → `20`，`step`/`sprint` 的 `time` → `1t`/`1s`、`60s`/`1d`/`3d`）。参数节点本身也做前缀匹配，所以输入 `2` 会建议 `20`。返回的 `start`/`length` 是替换区间。
- 服务端循环按 `MinecraftServer.run/tickServer` 组织：冻结时跳过 `World::tick` 但仍处理连接；`nextTick` 按 `nanosecondsPerTick` 推进；`tick query` 使用 100 项环形样本与 `min(100, tickCount)` 平均值。

补全的坐标约定：客户端发送的字符串保留前导 `/`（`ServerGamePacketListenerImpl` 用 `StringReader.skip` 跳过它而不裁剪字符串），所以服务端解析时去斜杠、返回前再把 `start` 加 1，让区间落在客户端实际持有的文本上。手工探测结果：`/tick f` → start 6/len 1/`freeze`，`tick f` → start 5，`/` → `tick`，`/tick ` → 六个子命令，`/tick rate ` → `20`，`/bogus` → 空列表。

验证：`command_tests` 覆盖解析、权限（level 3 = admins）、`TimeArgument` 换算、冻结/步进/冲刺状态机、命令树编码与补全（前缀过滤、字面量/参数示例、空结果、权限）；`protocol_socket` 在真实 TCP 连接上断言加入时收到 `commands` 包、`/tick freeze` 触发 `ticking_state(frozen=true)`、`/tick unfreeze` 恢复，并断言 `/tick f` 的补全请求返回 `start=6, length=1, ["freeze"]`。手工运行确认冻结期间 `gameTime` 停在原值而循环继续、`/tick sprint 1s` 报告 20 tick 已完成。

TODO：没有 op 列表，本地玩家固定为 level 4；`broadcast=true` 的反馈只回显给发起者（原版广播给有权玩家）；补全只给固定示例，不按注册表/玩家名动态生成，也没有 tooltip；`chat_command_signed` 未处理；其余命令尚未移植。
