# Minecraft 26.1 红石模拟器（C++20）

已新增带原版纹理的 Windows 极轻量 3D 调试客户端及 16 色羊毛地毯；使用方式见 [客户端说明](docs/client.md)。

SafeSave2 顺序场景对比已完成：[七项原版 26.1 对比与修复记录](validation/safesave_order/README.md)。已修复负 subTickOrder 及存档刻解包顺序，新增 CTest 对比项。

第一版目标是在单维度内建立可以继续逐项对照原版的红石运行核心。行为依据是本目录上一级的 26.1 Java 源码，不以社区机制概述替代源码。**第一版不是“全部原版红石已经等价”的承诺**；未移植依赖和验证边界见 [兼容性说明](docs/compatibility.md)。

## 本批范围

- 基础方块/实体类型注册与方块状态属性；未支持的方块名称和属性应保留，不能直接改为空气。
- 内存按 Chunk → 16×16×16 Section 组织；负坐标必须按向下取整划分。
- 只读 `level.dat`、Anvil `.mca`、区块调度信息和移动活塞方块实体；读取维度由 region 目录显式选择，不生成或保存地图。
- 可直接控制区块的加载/强加载；方块计划刻、方块事件和移动活塞方块实体刻；原版邻居更新栈及不同更新标志。按后续约定，本批不模拟水和流体刻。
- 空气、草方块、基岩、粘液块、蜂蜜块、平滑石、白色陶瓦、干枯珊瑚扇、圆石墙、黑曜石、玻璃、普通/粘性活塞、活塞头与 moving_piston（通常称 b36）、侦测器、音符盒、红石块、TNT。
- 音符盒仅处理更新行为；实体生成请求（包括 TNT、掉落物等）先记录日志。
- 后续新增：全部 16 色羊毛地毯（支撑更新、1/16 厚度、推动/拉回与掉落日志）。

## 开发约定

全部后续工作放在本目录。源码路径索引和 tick/更新约束见 [兼容性说明](docs/compatibility.md)。默认通过独立的 `include/redstone/allocator.hpp` 选择 jemalloc；切换分配器用于基准比较时应使用独立构建目录，并记录配置。

## 构建与运行

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

`--force` 可以重复；未指定时加载 region 目录中找到的所有区块。`--region` 可指定单一其他维度的 region 路径；`--min-y` / `--max-y` 指定其建造高度，默认 [-64, 320)。当前读取器先读取所有 region 再筛选，尚未优化为仅访问指定区块。不会生成缺失区块或写回存档。`--dump` 把逐刻方块状态快照写入 stdout；日志和不支持项统计写入 stderr。

## 当前验证结果

2026-11-06：默认 jemalloc 的 MSVC Release 构建完成，CTest **5/5 通过**，Release 下断言仍启用。GCC 15.2 的 system Debug 构建也通过五组测试。测试分组为 engine、piston、storage、allocator、shapes。演示场景验证了侦测器第 2 刻开启、第 4 刻关闭，强充能方块驱动活塞，随后完成回缩。

这仍是第一版实现。后续已实际运行原版 26.1 服务端，确认用户截图中的地毯复制结构与 C++ 的地毯位置和额外物品结果一致；证据见 [原版验证](validation/vanilla26_1/README.md)。最新六组 CTest 全部通过，包含移动方块实体到位顺序与快捷栏测试。尚未完成全机制差分或用户实际存档回放，不能解释为“几乎全部原版行为已验证”。更新顺序审查见 [审查记录](docs/update-order-audit.md)，剩余差异见 [兼容性说明](docs/compatibility.md)，存档限制见 [读取说明](docs/storage.md)。

## 代码入口

| 文件 | 职责 |
|---|---|
| `include/redstone/engine.hpp` / `src/engine.cpp` | 注册、世界/区块、更新回调、调度与主循环 |
| `include/redstone/piston.hpp` / `src/piston.cpp` | 结构解析、活塞事件、moving_piston 方块实体 |
| `include/redstone/shapes.hpp` / `src/shapes.cpp` | 方块盒形状、面覆盖、墙连接和珊瑚支撑 |
| `include/redstone/storage.hpp` / `src/storage.cpp` | Anvil 导入、调度与移动实体恢复 |
| `include/redstone/nbt.hpp` / `src/nbt.cpp` | 有边界检查的只读 NBT 解析 |
| `include/redstone/allocator.hpp` / `src/allocator.cpp` | 分配器选择、完整 new/delete 配对 |
| `src/main.cpp` | 命令行演示、存档读取、快照 |

`rawSetBlock` 只用于无回调导入或测试布置；正常修改使用 `setBlock`。移动方块实体通过 `setPistonEntity` 注册，以保留 ticker 顺序。`traceUpdates=true` 才记录每条邻居/形状/计划刻调试轨迹，避免默认运行时无限堆积更新日志。实体创建请求仍记录日志。
