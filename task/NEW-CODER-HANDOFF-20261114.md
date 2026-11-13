# 新 coder 交接：区块加载票、实际卸载与 Anvil

2026-11-14 · reviewer 交接 · 当前领取项：**W-4c 区块生命周期集成收口**。

## 接手先看这里

- 在 Ubuntu-24.04 WSL 的 `/home/zhddsj/cpp/redstone-cpp` 工作。Windows 浏览路径是 `\\wsl.localhost\Ubuntu-24.04\home\zhddsj\cpp\redstone-cpp`；旧 `F:\source\...\redstone-cpp` 不是工作副本。上一级 `../net/minecraft` 是本地 **Java 26.1 原版源码，只读**。
- 先看 [AGENTS.md](../AGENTS.md) 的角色约定和 [TASKS.md](TASKS.md) 末尾的 W-4c。此前 [CODER-HANDOVER.md](CODER-HANDOVER.md) 是 F-5 时期的历史文档。此交接写入时，唯一裸末行是 `任务通过`，它对应上一项 W-3c3，**不代表 W-4c 通过**。新 coder 领取后替换末行为 `任务进行中`，完成时再替换为 `任务已完成`；正文里的旧状态必须带反引号或说明。完成后通知 reviewer，reviewer 在编码期间只等交付。
- 当前是共享的脏工作树，`main` 的 HEAD 为 `eb5d5b2`。此后的票、存档及时间改动大多还在工作树中，包含未跟踪源码和夹具。保留已有改动，不 reset/clean、不按 HEAD 重建旧实现、不提交或推送。
- 用户要求**不要重复跑 CTest**。沿用已通过的日志，只跑本阶段缺失的定向验证。只写独立 scratch 存档，原档和作为夹具来源的存档保持只读；构建、日志、世界副本放在仓库外，例如 `/home/zhddsj/.local/state/redstone-cpp/`。如需要复制整区块、整区段、光照页或完整 NBT 根，先告知 reviewer。W-4c 不应需要新增此类拷贝。

## W-4c 剩余验收

用当前 native 产物和同一隔离世界，串起：**玩家产生加载需求 → 区块实际装入 → 真实 775 会话编辑 → 撤去最后的 FULL 需求 → 编辑落盘、World 节点实际移除 → 同坐标以新代重读并看到编辑 → 原版 26.1 读回结果**。优先复用下列证据和脚本，只补未连通的步骤。

1. 明确指定并记录目标 chunk、同一个世界副本、初态和写入的方块。确保没有 persistent force、出生区或第二个连接继续持有目标的 FULL 需求。用已有 [775 smoke 脚本](../validation/protocol775/managed_world_smoke.py) 的握手、Play、区块批次确认与编辑 ACK。选一个明确不同于初态、会留在最终存档的方块，并验证坐标对应的区块。
2. `forget_chunk` 和再次收到区块包只能证明视野变化，不能证明服务器释放了区块。用已有 `WorldChunkManager::managedChunkRemoved` / `managedChunkInstalled`、`World::isChunkLoaded`、lifecycle 状态/epoch 或等效可观察信号，记录目标确实移除且随后新代安装。尽量在同一 native 运行中关联编辑、移除和重装；若用分开的夹具，分别标明覆盖范围，不把结果拼成一次端到端验证。若需要额外探针，用测试专用或临时构建，不加每刻生产 trace。
3. 在关服前后区分**在线保存并卸载**与**关闭时保存**。关闭摘要的 `saved=0` 不意味着没保存，也不能独立证明此前已经保存。最有用的证据是移除之后从 region 读回、同坐标重读看到修改，以及 `complete=1, unsaved=0`。官方验证优先用真正重启的 26.1 服务端在同一次写出的隔离副本上查询目标；仅官方 `RegionFile/NbtIo` 探针时明确标注为离线读回。
4. 更新 [README](../README.md) 和 [兼容性说明](../docs/compatibility.md) 的现状与边界。按代码区分 `--ticketed-world` 与旧 `--world` / 投影入口；保留其他人的未提交文档。发现生产问题时先在 TASKS 记录最小复现和代码位置，不借收口重写生命周期、光照或协议。

### 上一位 coder 留下的草稿证据

证据目录：`/home/zhddsj/.local/state/redstone-cpp/wsl-w4c-integration-closeout-20261113`。**TASKS.md 中没有 W-4c 完成交付，以下结果都还未做本阶段验收。**

| 文件 | 已有信号及限制 |
| --- | --- |
| `run_lifecycle.py`、`events.json`、`native.log` | 真实 775 TCP：目标区块送达、编辑 ACK 1/2、视野 forget/resend、正常关服。没有同一次运行的 World 节点删除与新 epoch 证据。 |
| `manager_reload.cpp`、`manager-reload.log` | 独立 manager 夹具：撤去强制需求后 `removes=1`，同位置再安装，`(-29,-58,-20)` 读到 glass。使用单独的 `probe-world`，不是上面的 TCP 运行。 |
| `run_probe_readback.py`、`W4cTerrainReadback.java`、`probe-readback.log` | 官方 26.1 `RegionFile/NbtIo` 对 manager 的 `probe-world` 读到 glass；这是离线探针。另有 `official-readonly.log`，目前只看到启动和冻结，没有最终目标方块查询结果。 |

现有脚本包含硬编码路径和重建 scratch 目录的操作。先读脚本，把输出指向新的、已核实的 scratch 路径；不要直接运行会覆盖旧证据或源夹具的版本。现有原版地形夹具来源是证据根目录下 `wsl-w4-official-stone-ha8xrh9n/interop-world`。官方 jar/libraries 与 JDK 的取址可沿用上述 Python 探针（旧盘上原版构建依赖只读，C++ 工程仍在 WSL）。当前已核对 `wsl-w3b2c2-review-20261113/build/redstone_vanilla_server` 的 SHA-256 是 W-3c3 最终记录的 `e320d6aa69e683112f4c86507621b873790e0b0bb94f1a2b81c49e59c010b808`；目录名是历史名字，不等于旧版本。重建时记录实际 binary 哈希，勿覆盖封存产物。

## 已验收的工作与实现边界

| 范围 | 已验收、后续须保留的约束 |
| --- | --- |
| 光照 | 已收尾。天空光、方块光分存分发，使用 4 bit 紧凑数据，运行中由后台处理。此次不重开光照重构或重跑旧光照/世吞矩阵。 |
| 加载票与生命周期 | T-1～T-4、T-4b3：加载和模拟等级分别传播，需求、数据驻留、方块/流体/实体可刻分别判定；后台读写、脏版本确认、真实卸载、同位置实例代号和旧回报隔离。失败在仍有需求时可诊断，撤票后清理无保全义务的记录。 |
| 地形与计划刻 | 显式 `--ticketed-world` 模式写入同版本 Anvil；支持已有且可安全处理的 FULL 区块、方块/流体计划刻、移动活塞方块实体、`level.dat` Time 与支持的持久票。坏/缺/不支持数据与合法空数据区分；无生成，不把缺失当可用。 |
| 实体 | 保持有序 compact variant 和 UUID；范围为受支持掉落物、默认已点燃 TNT、默认下落沙、普通空矿车。跨区块迁移按精确版本确认目标保存再清源；两个 region 不具备跨文件崩溃原子性，重复 UUID 明确拒绝。W-4b 对三种后续实体已完成真正的 Java → C++ → Java 隔离存档重启。 |
| 游戏规则与时钟 | 原版规则文件读取已支持 TNT 爆炸、水源/熔岩源转换与 `advance_time`。两个原版内置世界时钟独立于 `gameTime`，支持原版存读盘、775 同步、核心 `/time`、内置时间标记和 timeline 查询。没有在线 gamerule 写入、玩家睡眠、自定义时钟/维度的完整行为。 |
| W-3c3 保存快照 | 脏编辑 8 B/格，失败合并保留最新值并消除临时去重树。整区块输出 2.25→0.75 MiB；普通构建耗时接近，失败合并在独立微基准快约 9–10 倍，不能外推整服或视为 2 ms 硬上限。 |

当前只承诺已核实的 26.1（协议 775 / DataVersion 4786）子集。未知内容必须原样保留或明确拒绝写入。没有通用 DataFixer、完整方块/实体支持、世界生成或通用跨文件事务保证。原版源码对照、C++ 定向回归、官方 codec/离线读盘、官方真服务端重启是四种不同证据。

## 查代码与复现入口

- 票和可刻：`src/tickets.cpp`、`src/world_chunk_manager.cpp`，对照原版 `server/level/{TicketStorage,ChunkMap,ServerChunkCache}.java`。I/O、世代和回收：`src/chunk_lifecycle.cpp`。实际世界节点安装和删除：`src/engine.cpp`。
- 格式和 native 接线：`src/{region,terrain_codec,entity_codec,storage}.cpp`、`src/net/server_main.cpp`、`src/net/session.cpp`。相关历史设计见 [区块/Anvil](CHUNK-TICKETS-ANVIL-HANDOFF.md)、[实体](W3B2-ENTITY-REVIEW-PLAN.md)、[自动保存](W3C2-AUTOSAVE-REVIEW-PLAN.md)。细节或审查结论以 TASKS 为准。
- 需要重建时，在 WSL 用 Release/Ninja 和现有 jemalloc，构建目录置于仓库外。当前机器可用的 CMake 前缀是 `/home/zhddsj/.local/state/redstone-cpp/wsl-perf-oJwCYo/deps/prefix/usr`；native 目标为 `redstone_vanilla_server`，开关 `-DREDSTONE_BUILD_VANILLA_SERVER=ON`。首选相关探针目标而非重新跑整套；做性能对照时用相同分配器、相同编译配置，先核对允许的 CPU 与物理核心再绑核。
- 证据根目录 `/home/zhddsj/.local/state/redstone-cpp`，可直接引用 `wsl-w4b-three-entities-20261113/final-combined/`（三种实体真重启）、`wsl-t4b3-lifecycle-20261113/`（回收定向）、`wsl-w3c3-compact-edits-20261113/`（快照验证与微基准）、`wsl-w1a-world-clocks-20261113/`（官方时钟）。
- 交付时在 TASKS 记录改动文件、实际命令、日志路径与哈希、目标 chunk/方块、是否使用同一世界和 native 进程、尚未覆盖的边界，然后替换唯一末行并通知 reviewer。不要只报「通过」。
