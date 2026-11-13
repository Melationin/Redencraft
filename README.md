# Minecraft 26.1 服务端模拟器（C++20）

已新增带原版纹理的 Windows 极轻量 3D 调试客户端及 16 色羊毛地毯；使用方式见 [客户端说明](docs/client.md)。

新增可选原生 C++ 协议 775 服务端，支持离线登录、固定区块、创造物品选择和放置/破坏方块、活塞事件、简单实体同步；构建和启动命令见 [原版连接说明](docs/protocol775.md)。已用无界面 TCP 测试和原版 Java codec 验证；没有启动真实客户端，实际连接和渲染仍待验证。

本批补入简单实体（物品、TNT、下落方块、空矿车）、四种铁轨方块、物品注册与空工具掉落表、TNT 爆炸和实体存档导入；行为依据和剩余差异见 [简单实体、铁轨与物品](docs/entities.md)。当前未提交状态、构建/验证结果、剩余 TODO 与原版客户端直连调研见 [交接文档](docs/handover.md)。

SafeSave2 顺序场景对比已完成：[七项原版 26.1 对比与修复记录](validation/safesave_order/README.md)。已修复负 subTickOrder 及存档刻解包顺序，新增 CTest 对比项。

项目目标是 **C++20 实现的 Minecraft Java Edition 26.1 服务端**：多维度、加载票、世界生成、世界保存、时间与光照，分期见 [路线图](PLAN.md)，目标形态见 [项目约束](AGENTS.md)。**目前代码处在它之前的子集阶段**：单维度（主世界）、无世界生成、无完整原版存档/自定义维度/玩家睡眠，用这个子集逐项对照原版。当前已实现 L-1…L-3 方块/天空光计算与 775 协议分发（尚无 Anvil 光照读写）、流体计划刻与传播（F-1…F-8）、内置 World Clocks 与 `/time`（无睡眠跳转或自定义时钟）；原生协议 775 服务端的显式 `--ticketed-world` 入口已支持加载票、真实区块卸载与 Anvil 写回，但仅限于 26.1（协议 775 / DataVersion 4786）已有且可安全处理的 FULL 区块与受支持子集，旧 `--world` 与 `--schematic` 入口仍为只读导入。性能优化的阶段结果、测量方法和开销分布见 [性能优化记录](docs/performance.md)。行为依据是本目录上一级的 26.1 Java 源码，不以社区机制概述替代源码。**当前不是“全部原版红石已经等价”的承诺**；未移植依赖和验证边界见 [兼容性说明](docs/compatibility.md)。

## 本批范围

- 基础方块/实体/物品类型注册与方块状态属性；未支持的方块名称和属性在内存和只读路径保留，未知/不支持内容在 `--ticketed-world` 写回路径明确拒绝而非降级抹除。
- 内存按 Chunk → 16×16×16 Section 组织；负坐标必须按向下取整划分。
- `--world` / `--schematic` 与 `redstone_cli` 仍是只读导入（`level.dat`、Anvil `.mca`、区块调度信息、移动活塞方块实体和简单实体），不生成或写回地图；显式 `redstone_vanilla_server --ticketed-world <dir>` 额外支持同版本 Anvil 写回（已有 FULL 区块、方块/流体计划刻、移动活塞方块实体、受支持简单实体、核心 `level.dat` Time、内置 `world_clocks` 与支持的持久票），不生成缺失区块，也不提供跨文件崩溃原子性。
- 可直接控制区块的加载/强加载（`--ticketed-world` 模式使用加载票）；方块计划刻、流体计划刻（水与岩浆状态、传播、参数/反应及含水宿主方块；桶交互、实体推流与 `fluid_ticks` 写回边界见 [兼容性说明](docs/compatibility.md)）、方块事件、实体刻和移动活塞方块实体刻；原版邻居更新栈及不同更新标志。
- 空气、草方块、基岩、粘液块、蜂蜜块、平滑石、白色陶瓦、干枯珊瑚扇、圆石墙、黑曜石、玻璃、普通/粘性活塞、活塞头与 moving_piston（通常称 b36）、侦测器、音符盒、红石块、TNT、四种铁轨、水、岩浆。
- 简单实体：物品、TNT、下落方块、空矿车。TNT 方块点燃、默认引信 80、威力 4 爆炸、方块破坏掉落与链式 TNT 已接入；物品会下落、合并、进液体和到期消失；矿车按非实验性 `OldMinecartBehavior` 在铁轨/离轨状态下移动。移动活塞按原版扫掠区域推动实体（含每刻 0.51 限制、NOCLIP、回缩基座挤出），粘液块会把简单实体沿移动轴弹到 ±1 初速，蜂蜜块水平粘性搬运实体。
- 物品：方块物品注册、最大堆叠、空工具掉落表（草方块→泥土，玻璃/枯珊瑚扇/活塞头/移动活塞/基岩不掉落，不稳定 TNT 不掉落，其余掉自身）。
- 后续新增：全部 16 色羊毛地毯（支撑更新、1/16 厚度、推动/拉回与真实掉落物）。

## 开发约定

全部后续工作放在本目录。源码路径索引和 tick/更新约束见 [兼容性说明](docs/compatibility.md)。默认通过独立的 `include/redstone/allocator.hpp` 选择 jemalloc；切换分配器用于基准比较时应使用独立构建目录，并记录配置。

## 构建与运行

### WSL / Linux

Ubuntu 24.04 下使用 C++20 编译器、CMake、Ninja、zlib 和 jemalloc 开发头文件（Ubuntu 包名分别为 `build-essential`、`cmake`、`ninja-build`、`zlib1g-dev`、`libjemalloc-dev`）。在 WSL 仓库目录执行，构建和测试产物放在仓库外：

```bash
build="$HOME/.cache/redstone-cpp/release"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DREDSTONE_BUILD_VANILLA_SERVER=ON -DREDSTONE_BUILD_BENCHMARKS=ON
cmake --build "$build" --parallel 6
PYTHONDONTWRITEBYTECODE=1 ctest --test-dir "$build" --output-on-failure
```

世吞基准支持 Linux 线程 CPU 计时和绑核。下例仅适用于可用且不同核心的 CPU 2、CPU 4；换机器前用 `lscpu -e=CPU,CORE,SOCKET` 与当前允许的 CPU 列表核对。

```bash
"$build/redstone_tick_benchmark" /path/to/workload.litematic 1000 --lighting \
  --process-affinity 0x14 --main-affinity 0x4 --light-affinity 0x10
```

`--no-lighting` 可用于同版本对照。正式计时不要带 `--fingerprint`；指纹对照单独跑。未安装 jemalloc 开发包时，只有明确指定 `-DREDSTONE_ALLOCATOR=system` 才会使用系统分配器；性能对照双方使用同一分配器。

### Windows（历史验证）

已在 Windows 上验证 MSVC 2026 + jemalloc 5.3.0（静态 CRT），以及 GCC 15.2 + system 分配器。CMake 默认要求 jemalloc，找不到时会报错，不会静默切换。zlib 缺失时自动下载固定版本 1.3.1 并验证 SHA256；LZ4 1.10.0 的 C 源码和许可证在 `vendor/lz4`。

当前机器的 jemalloc 已安装在本项目 `third_party/vcpkg-installed/x64-windows-static` 中。以本目录作为工作目录：

```powershell
cmake -S . -B build-jemalloc -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static"
cmake --build build-jemalloc --config Release -j 6
ctest --test-dir build-jemalloc -C Release --output-on-failure
./build-jemalloc/Release/redstone_cli.exe --demo --ticks 6 --dump
```

新机器可用 vcpkg 安装 `jemalloc:x64-windows-static`，再把 `CMAKE_PREFIX_PATH` 指向对应安装前缀。使用 MinGW 时，必须匹配 MinGW 构建的依赖，不能混用 MSVC 的静态 jemalloc 库。比较系统分配器时，单独配置 `-DREDSTONE_ALLOCATOR=system`。

读取存档、只让指定的两个区块参与运行：

```powershell
./build-jemalloc/Release/redstone_cli.exe --world "D:/Minecraft/saves/Test" --force 0 0 --force -1 0 --ticks 20 --dump
```

`--force` 可以重复；未指定时加载 region 目录中找到的所有区块。`--region` 可指定单一其他维度的 region 路径；`--min-y` / `--max-y` 指定其建造高度，默认 [-64, 320)。当前读取器先读取所有 region 再筛选，尚未优化为仅访问指定区块。不会生成缺失区块或写回存档。`--dump` 把逐刻方块与简单实体快照写入 stdout；日志和不支持项统计写入 stderr（含 `simple_entities_imported` 和 `other_entities_not_simulated`）。

## 当前验证结果

2026-11-06：默认 jemalloc 的 MSVC Release 构建及启用协议模块的 CTest **11/11 通过**，Release 下断言仍启用；关闭协议模块仍为原有 8 组。新增 wire、protocol、protocol_socket 验证编码、状态机、交互及真实 localhost TCP。新增内容本批只在 MSVC 下验证；本机 GCC 15.2 对空程序也返回 1，未作为本批验证依据。实际原版解码验证及限制见 [协议说明](docs/protocol775.md)。

这仍是持续逼近原版的实现，不是“全部原版等价”的承诺。后续已实际运行原版 26.1 服务端，确认用户截图中的地毯复制结构与 C++ 的地毯位置和额外物品结果一致；证据见 [原版验证](validation/vanilla26_1/README.md)。简单实体的当前 8 组 CTest 全部通过。地毯复制结构已额外用真实 26.1 服务端读取物品 `Pos`/`Motion`：击中后的 X 速度序列与 C++ 一致（`-1.0, -0.98, -0.9604, ...`），证据见 [原版验证](validation/vanilla26_1/README.md)；但其余实体/铁轨/爆炸路径**尚未完成逐刻差分**，不能称为服务端等价。更新顺序审查见 [审查记录](docs/update-order-audit.md)，剩余差异见 [兼容性说明](docs/compatibility.md)，存档限制见 [读取说明](docs/storage.md)。

## 代码入口

| 文件 | 职责 |
|---|---|
| `include/redstone/engine.hpp` / `src/engine.cpp` | 注册、世界/区块、更新回调、调度与主循环 |
| `include/redstone/entity.hpp` / `src/entity.cpp` | 实体基类与碰撞、物品/TNT/下落方块/矿车、掉落与爆炸 |
| `include/redstone/rail.hpp` / `src/rail.cpp` | 四种铁轨的状态、支撑、连接、动力与探测 |
| `include/redstone/piston.hpp` / `src/piston.cpp` | 结构解析、活塞事件、moving_piston 方块实体 |
| `include/redstone/shapes.hpp` / `src/shapes.cpp` | 方块盒形状、面覆盖、墙连接和珊瑚支撑 |
| `include/redstone/storage.hpp` / `src/storage.cpp` | Anvil 导入、调度与移动实体恢复 |
| `include/redstone/nbt.hpp` / `src/nbt.cpp` | 有边界检查的只读 NBT 解析 |
| `include/redstone/allocator.hpp` / `src/allocator.cpp` | 分配器选择、完整 new/delete 配对 |
| `src/main.cpp` | 命令行演示、存档读取、快照 |

`rawSetBlock` 只用于无回调导入或测试布置；正常修改使用 `setBlock`。移动方块实体通过 `setPistonEntity` 注册，以保留 ticker 顺序。实体通过 `World::addEntity` 注册并参与实体刻；`spawnItem` 等辅助函数生成真实实体，同时写一条 `entity_spawn` 日志便于 CLI/客户端观察。`traceUpdates=true` 才记录每条邻居/形状/计划刻调试轨迹，避免默认运行时无限堆积更新日志。
