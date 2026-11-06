# 原生 C++ 协议 775 服务端

后续已支持沙子、石砖墙和红石灯，以及完整投影快照导入，见 [投影模拟](litematic.md)。世界中的全部指定区块保持强加载，客户端采用周边视野及带 ACK 的分批传输，已取消旧版 64 区块总量限制。每个 tick 结束后，向玩家**聊天栏**发送 `Tick N: x.xxx ms`；计时包含世界 tick 和连接同步准备，排除休眠、tick 外 socket IO 及耗时消息本身。不是动作栏显示。

用户选择原生 C++ 直连并要求放置/破坏方块，因此新增独立可选目标 `redstone_vanilla_server`，不依赖真实服务端桥接。默认只监听 `127.0.0.1:25565`，离线模式；协议数据来自未修改的官方 26.1 jar。26.1.2 同为 775，但它的静态 ID 和实际连接尚未独立验证。26.2 不在此协议资产支持范围。

本批遵照要求没有启动真实客户端、隐藏预览或接管桌面。下列命令供用户运行；自动测试仅运行无界面 C++ 服务端和脚本 TCP 连接。

```powershell
cmake -S . -B build-jemalloc -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static" -DREDSTONE_BUILD_VANILLA_SERVER=ON
cmake --build build-jemalloc --config Release -j 6
ctest --test-dir build-jemalloc -C Release --output-on-failure
./build-jemalloc/Release/redstone_vanilla_server.exe
```

在原版客户端中连接 `127.0.0.1:25565`。默认创造模式并允许飞行，初始九格包含平滑石、粘性活塞、粘液块、白地毯、红石块、侦测器、枯管珊瑚扇、蜂蜜块、TNT。通过原版创造列表选择本项目支持的方块；滚轮/数字键切换、左右键编辑、中键选取、Shift 绕过音符盒调音、交换副手已接入。未支持的方块和自定义组件会被服务端拒绝并修正物品栏，不会假装已经实现其机制。

不传存档时提供固定九区块的平滑石测试平台；这是显式测试夹具，不生成缺失地图。运行现有只读存档可指定：

```powershell
./build-jemalloc/Release/redstone_vanilla_server.exe --world "D:/Minecraft/saves/Test" --chunk 0,0 --chunk -1,0 --spawn 8.5,66,8.5
```

`--chunk` 可重复；未指定则选择存档中已读取的全部区块。指定的区块必须存在，不会因玩家移动或放置自动加载/生成区块。`--view-distance 3` 控制网络视野半径（2–8），独立于模拟强加载；客户端移动时发送新的中心和区块、卸载旧视野。大世界每批最多 2 区块，小场景最多 9 区块，等待 `chunk_batch_received` 再继续。当前视野为方形边界，原版圆角边界和客户端距离协商仍为 TODO。协议维度目前固定原版 overworld 高度 [-64,320)，存档仍要求 DataVersion 4786；没有保存。`--port 0` 自动分配本地端口，`--ticks N` 用于有限时长自动测试，`--assets` 可选择导出资产目录。Ctrl+C 正常结束；不执行自动客户端启动。

## 连接与同步

- 实现 handshake/status ping、离线 UUID v3 登录、login acknowledgement、空 known-packs 协商、完整 registry/tags 配置及 finish acknowledgement。
- Play 发送创造玩家登录、玩家信息、能力、出生点、区块中心、chunk batch、时间、初始九格、传送确认和 keepalive。
- 区块采用 26.1 的 section 双计数（non-empty、fluid）、palette 固定 long 存储、原版状态标志计算的高度图及移动活塞 NBT（ByteTag facing、progressO）。块/物品/实体等 ID 使用完整原版导出表，而非 C++ 子集的注册顺序。
- 创造放置与破坏经过距离、范围、已加载区块、物品、支撑及当前玩家/简单实体碰撞检查；处理珊瑚地面/挂墙候选、活塞和侦测器方向、铁轨方向、草/墙/音符盒初始属性、音符盒右键调音、创造破坏活塞头关联基座、矿车坡道放置。侦测器放置不得调用会调度计划刻的通用 updateShape 查询。
- 输入预测 sequence 在修改世界之前校验，修正包和 block_changed_ack 回传；负序列不会先编辑再断开。创造丢弃限制堆叠/频率，不向未加载区块生成暂停实体。
- 物品、TNT、下落方块、矿车按原版 `ServerEntity` 跟踪：spawn 携带一次非默认元数据；之后位置是相对 `VecDeltaCodec` 基准的 1/4096 位移（`move_entity_pos` / `move_entity_pos_rot`），旋转是 1/256 圈（`move_entity_rot`），位移超出 short、着地状态变化或 400 刻未更新时改用 `entity_position_sync`，位置每 60 刻、旋转每 1 刻至少刷新一次。更新间隔取 `EntityType` 的 20（物品/下落方块）、10（TNT）、3（矿车）；矿车轨迹插值与受击速度包仍未实现。

## 广播顺序

`World::setBlock` 在原版 sendBlockUpdated 所在位置记录变化，保留 flags 判断；网络层只记录打包的脏位置（`SectionPos << 12 | 局部坐标`）。`World::tick` 的计划刻完成后、方块事件前新增观察性广播入口，对应 `ServerChunkCache.tickChunks → broadcastChangedChunks`。广播时读取当前位置，按 `ChunkHolder.broadcastChanges` 的规则发包：section 内只有一个变化时发 `block_update`，多个变化合并成一条 `section_blocks_update`（`SectionPos` long + VarInt 数量 + 每条 `(stateId << 12) | 局部坐标` 的 VarLong），同一位置的多次写入只保留一次。

成功方块事件在 `pistonBlockEvent` 等回调之后立即发包；事件、实体、方块实体阶段产生的普通块变化留至下一刻区块广播。特别是活塞源位置清空不能先于活塞事件发给客户端，否则客户端本地结构解析会丢失待移动方块。输入预测修正包独立立即回传。

世界状态的采集在主线程（方块状态、实体位置、区块 palette 只能从 `World` 读），字节编码则在独立的 `NetworkWriter` 线程完成：主线程只把“packet id + 未成帧 payload”或“区块快照”放进有序队列，写线程负责 VarInt 长度前缀、区块 palette/位打包、发送缓冲合并、写侧 `select` 和 `send`。区块发送先复制子区块的 palette + 索引（不复制 4096 个方块状态），编码完全在写线程进行。实体同步只扫描玩家视野内的 entity section（`EntitySectionStorage`），不再遍历全世界的实体表。协议注册表把 `BlockState` → 协议 ID 和协议 ID → 状态标志改成稠密数组索引，编码时不再构造名字/属性字符串。因此慢连接和积压不会阻塞 tick，输出积压仍只关闭该连接。网络 ID 映射失败不会向核心邻居更新链抛出协议异常。世界的计划刻、方块事件、实体、移动活塞方块实体顺序保持原实现。

## 验证与剩余工作

MSVC Release + jemalloc 的 CTest 11 组覆盖原有顺序比较、实体/活塞修复、编码边界/负坐标/多 palette、高度图、协议状态机、放置/破坏/物品栏、支撑/侦测器/碰撞、预测校验、方块事件广播时机和网络失败隔离。socket 测试实际启动本地 C++ 服务端，完成状态查询、离线登录、30 配置包、区块、放置、破坏和确认。

`validation/protocol775/export.ps1` 从官方 jar 重建资产并检查版本与哈希。其 `-VerifyPlayPackets` 使用官方 GameProtocols codec 解码 C++ 测试生成的所有记录，区块继续用真实 LevelChunkSection.read 解码 24 个子区块，检查无剩余字节。来源、格式和结果见 `validation/protocol775/README.md`、`verification-result.json`。这验证编码和源码约定，不能代替真实客户端实际连接、资源加载和动画验证。

仍有明确 TODO：真实 ServerPlayer 服务端物理与推挤、其他玩家模型/放置碰撞、动态区块跟踪、玩家拾取/容器与生存、任意物品组件/工具与完整 BlockPlaceContext、命令/聊天/配方/世界时钟、原版光照引擎、其他生物群系、音效粒子和完整元数据优化。当前 sky light 统一全亮、biome 统一 plains，仅用于查看；水/流体刻继续排除。网络支持是创造编辑入口，不代表完整原版多人服务端等价。
