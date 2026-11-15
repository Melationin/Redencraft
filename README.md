# Redencraft — Minecraft 26.1 C++20 服务端模拟器

以 **C++20** 实现、行为对齐 **Minecraft Java Edition 26.1**（协议 **775** / DataVersion **4786**）的服务端与红石核心模拟器。  
所有机制以仓库上一级只读 26.1 Java 源码与原版导出数据为行为依据，持续逼近原版；完整长期目标见 [AGENTS.md](AGENTS.md) 与 [路线图](PLAN.md)。

> **状态边界提示**：本项目仍在分阶段演进中，**当前并非“全部原版等价”**。文档严格区分「目标形态」与「当前已实现边界」，未运行真实 26.1 服务端逐刻差分的系统不声称等价；已确认边界与待移植项见 [兼容性说明](docs/compatibility.md) 与 [BLOCK_TODO.md](BLOCK_TODO.md)。

---

## 核心能力与当前边界

| 模块 | 当前已实现边界 | 尚未完成（目标阶段） |
|---|---|---|
| **世界与维度** | 支持 26.1 维度类型注册与 `world_gen_settings.dat` / `dimensions/<ns>/<path>` 解析（`D-1`）、`ServerWorldSet` 同服多维度与共享时钟推进、维度 `waterEvaporates` / `fastLava`（`D-2`），以及 `--dimension` 选择目标维度 | 维度传送门切换触发与跨维位置换算（`D-3`）；世界生成（`G-1 ~ G-4`） |
| **区块加载与存档** | `redstone_cli` 与 `--world` / `--schematic` 为只读导入；`redstone_vanilla_server --ticketed-world <dir>` 支持基于加载票的异步加载/卸载与 Anvil 写回（26.1 FULL 区块、计划刻、移动活塞、简单实体、`level.dat` `Time`、内置 `world_clocks` 与持久票）；不支持内容在写回路径显式拒绝 | 缺失区块不会自动生成；不提供跨文件崩溃事务原子性 |
| **方块、红石与刻顺序** | 按 `ServerLevel.tick` 阶段分离推进；`NeighborUpdater` / `CollectingNeighborUpdater`、`updateShape`、`neighborChanged`、方块/流体计划刻、方块事件、方块实体（移动活塞等）、容器、随机刻、26.1 方块标签与 16 色羊毛地毯 | 原版全部方块/实验性红石规则持续补齐中，进度见 [BLOCK_TODO.md](BLOCK_TODO.md) |
| **流体** | 水与岩浆状态、传播/坡度/反应、含水宿主与 `fluid_ticks` 调度（[流体文档](docs/fluid-testing.md)） | 桶交互、实体推流及与真服逐刻全量差分（`F-9a / F-9b`） |
| **光照** | 双光层（BlockLight + SkyLight）独立 4-bit 压缩快照、后台 worker + SPSC 无锁交接、Anvil 光照读写及协议 775 初始/增量发送（[光照文档](docs/light-engine.md)） | 独立多维度下的逐维度光学实例与全量真服差分 |
| **实体** | 简单实体（物品、TNT、下落方块、空矿车）、4 种铁轨（`OldMinecartBehavior`）、活塞扫掠推动与粘液/蜂蜜块相互作用（[实体文档](docs/entities.md)） | 生物/玩家 AI 之外的大多数复杂实体仍仅作注册或日志占位 |
| **网络协议（775）** | 可选原生 TCP 服务端 `redstone_vanilla_server`，支持离线登录、区块/光照分发、放置/破坏方块、容器基础交互、`/tick`、`/time` 与服务端 `ClientboundRespawnPacket` 会话切维接口 | 传送门触发的真实客户端跨维全链路验证（`D-3`） |
| **调试客户端** | 可选 Windows 极轻量 3D OpenGL 调试客户端 `redstone_client`（[客户端说明](docs/client.md)） | 非完整游戏客户端 |
| **内存与分配** | 默认通过 `allocator.hpp` 使用 `jemalloc`；`--low-memory` 开启光影/几何直读共享，计划刻使用 32 B `QueuedTick` / 16 B `TickKey` 紧凑布局并支持排空回收与周期压缩 | 光照快照页进一步压缩与跨页去重 |

---

## 快速构建与测试

### 1. WSL / Linux（Ubuntu 24.04 推荐配置）

依赖工具与库：`build-essential`、`cmake`（≥ 3.24）、`ninja-build`、`zlib1g-dev`、`libjemalloc-dev`。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DREDSTONE_BUILD_VANILLA_SERVER=ON \
  -DREDSTONE_BUILD_BENCHMARKS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

- 默认要求 `jemalloc`；若要对比系统分配器，请使用独立构建目录并显式传入 `-DREDSTONE_ALLOCATOR=system`。
- 若 `jemalloc` 安装在自定义前缀（如 `$HOME/.local`），附加 `-DCMAKE_PREFIX_PATH="$HOME/.local"`。

### 2. Windows（MSVC + 静态 jemalloc）

仓库 `third_party/vcpkg-installed/x64-windows-static` 已包含静态 `jemalloc`（也可自行通过 vcpkg 安装 `jemalloc:x64-windows-static` 并指向对应前缀）：

```powershell
cmake -S . -B build-jemalloc -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static" `
  -DREDSTONE_BUILD_VANILLA_SERVER=ON
cmake --build build-jemalloc --config Release -j 6
ctest --test-dir build-jemalloc -C Release --output-on-failure
```

### 主要 CMake 选项

| 选项 | 默认值 | 说明 |
|---|---|---|
| `REDSTONE_ALLOCATOR` | `jemalloc` | 分配后端（`jemalloc` 或 `system`） |
| `REDSTONE_BUILD_VANILLA_SERVER` | `OFF` | 构建可选协议 775 服务端 `redstone_vanilla_server` 与协议测试 |
| `REDSTONE_BUILD_BENCHMARKS` | `OFF` | 构建离线性能基准（`redstone_tick_benchmark` 等） |
| `REDSTONE_BUILD_CLIENT` | `ON` | 在 Windows 上构建轻量调试客户端 `redstone_client` |
| `REDSTONE_FETCH_ZLIB` | `ON` | 系统缺少 `zlib` 时自动拉取固定版本 1.3.1 |

---

## 常用运行示例

### CLI 离线模拟（`redstone_cli`）

```bash
# 运行内置示例 6 tick 并导出 JSON 快照
./build/redstone_cli --demo --ticks 6 --dump

# 只读导入指定 Anvil 存档的两个区块并推进 20 tick
./build/redstone_cli --world /path/to/save --force 0 0 --force -1 0 --ticks 20 --dump
```

- `--region <dir>` 可显式挂载单一维度目录（配合 `--min-y` / `--max-y`，默认 `-64..320`）。

### 原生协议 775 服务端（`redstone_vanilla_server`）

```bash
# 仅内存只读模式加载 Anvil 存档（低内存模式，关闭控制台输入）
./build/redstone_vanilla_server --world /path/to/save --skip-non-full-chunks --low-memory --port 25565 --no-console

# 显式启用加载票、异步加载/卸载与 Anvil 写回
./build/redstone_vanilla_server --ticketed-world /path/to/save_copy --port 25565
```

- 常用选项：`--schematic <file>`、`--ticks <n>`（默认持续运行，传 `0` 仅加载校验）、`--view-distance <n>`、`--ticket-view-distance <n>`、`--simulation-distance <n>`、`--light-workers <n>`、`--allow-unsupported`。

---

## 代码结构

| 目录 / 文件 | 职责 |
|---|---|
| `include/redstone/engine.hpp`、`src/engine.cpp` | 世界状态、区块与区段存储、邻居/形状更新回调、计划刻与主循环 |
| `src/redstone/`、`include/redstone/redstone.hpp` | 红石信号、线、火把、中继/比较器、输入控件与用电器 |
| `src/blocks.cpp`、`src/state.cpp`、`src/block_tags.cpp`、`src/shapes.cpp` | 26.1 方块/状态注册表、方块标签与碰撞/遮挡形状 |
| `src/fluid.cpp`、`src/fluid_flow.cpp`、`src/random_tick.cpp` | 流体状态/传播与随机刻系统 |
| `src/piston.cpp`、`src/rail.cpp`、`src/containers.cpp`、`src/entity.cpp` | 活塞与移动方块实体、铁轨、容器与简单实体 |
| `include/redstone/light/`、`src/light/` | 双光层光学表、块光/天空光传播器、异步光照服务与不可变快照 |
| `src/storage.cpp`、`src/region.cpp`、`src/terrain_codec.cpp`、`src/entity_codec.cpp`、`src/nbt.cpp` | Anvil `.mca`、压缩 NBT、地形/实体编解码与 `level.dat` / `world_clocks` / `chunk_tickets` 持久化 |
| `src/tickets.cpp`、`src/chunk_lifecycle.cpp`、`src/world_chunk_manager.cpp` | 加载票（`TicketStorage`）、区块生命周期状态机与异步区块管理器 |
| `src/net/`（`wire.cpp`、`session.cpp`、`server_main.cpp`） | 协议 775 编解码、连接状态机、命令与服务端主入口 |
| `src/litematic.cpp`、`src/command.cpp`、`src/allocator.cpp` | `.litematic` 投影导入、指令解析与统一分配器封装 |
| `validation/`、`benchmarks/`、`tests/` | 26.1 Java 离线/真服差分探针、性能基准与 CTest 回归套件 |

---

## 延伸阅读

- **总体设计**：[项目约束与目标形态](AGENTS.md) · [开发路线图](PLAN.md) · [兼容性边界](docs/compatibility.md)
- **子系统文档**：[光照引擎](docs/light-engine.md) · [协议 775 说明](docs/protocol775.md) · [流体测试规格](docs/fluid-testing.md) · [简单实体、铁轨与物品](docs/entities.md) · [存档与投影](docs/storage.md) · [性能优化记录](docs/performance.md)
- **调试客户端**：[Windows 调试客户端](docs/client.md)
