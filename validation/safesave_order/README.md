# SafeSave2 顺序场景与 C++ / 原版 26.1 对比

2026-11-06，读取 `D:/Java/SafeSave2/tests/server`，未修改或运行该工作目录。取其中与当前单维度、无水模型相关的运行时场景，分别执行 C++ 探针和原版 26.1 Java 代码。不是运行完整 SafeSave 保存/重启矩阵，也没有把安装了 SafeSave 的 `reference` 当成无模组原版。

**最终：7 项对比一致。初始发现 1 处实际错误，已修复。** [机器可读结果](comparison.json)；[修复前结果](comparison.before_signed_fix.json)。C++ 客户端与 CLI 已重新链接修复后的核心。

| 对比项 | 原版证据 | 最终结果 |
|---|---|---|
| 三个边界的 12 个计划刻，优先级与 subTickOrder 交错 | 实际 LevelTicks / LevelChunkTicks | 一致 |
| 运行时重复计划刻保留先入队项，保留无关位置 | 实际调度容器 | 一致；不是 SafeSave 的恢复合并 API |
| 不同到期时间的区块队首竞争 | 实际调度容器 | 一致 |
| 负恢复序号与新刻序号竞争 | 实际调度容器 | 初始不一致，修复后一致 |
| 12 个音符盒事件跨区块交错、每个重复入队两次 | 实际服务端 doBlockEvent 入口 | 一致，实际执行 12 个 |
| 6 个半程移动实体连续三刻的 progress / progressO / lastTicked / 最终方块 | 实际服务端 | 一致 |
| 6 个移动实体到位调用顺序 | 实际 PistonMovingBlockEntity.tick 入口 | 一致：x = 1, -1, 17, 15, 8193, 8191 |

坐标依据 SafeSave 的 `b={0,16,8192}` 边界场景。移动实体场景结合其半程输入与相邻区块反向注册的测试意图，使用 `(b+1,84,-3)`、`(b-1,84,-3)` 两个位置。调度器类型使用无计划刻行为的 smooth_stone 替代原测试的 stone，只比较调度而非完整 stone 行为。不执行流体，但保留其造成的 block subTickOrder 间隔。

## 发现与修复

C++ 原先把 ScheduledTick.order 存为 uint64_t；Java 使用有符号 long。原版 LevelChunkTicks.unpack 给区块保存列表分配 `-N..-1` 的子顺序，新刻一般从 0 开始。同优先级时，原版将负序号恢复刻排在新刻之前，C++ 却把负数解释为巨大正数，反转执行顺序。

现已改用有符号 64 位，并修复导入：每个区块分别按保存列表分配 `-N..-1`，保留原版 scheduleUnchecked 留在队列中的重复保存项，不与流体列表混计。新增负序号与导入回归。`cpp_cases.before_signed_fix.json` 保留修复前实际输出。

## 证据与限制

- 调度器运行 Mojang 26.1 JAR 中的原类；事件与移动实体运行在实际原版专用服务端，没有 SafeSave、Lithium、C2ME 或 Fabric。记录代理只注入场景与记录调用，不重写模拟逻辑。
- 所有参与区块同时达到 BLOCK_TICKING 且 areEntitiesLoaded 后才建立场景。lastTicked 以建立时的游戏时间为 0；其他数组次序不排序，无浮点容差。
- [原版探针说明](vanilla/README.md)、vanilla-order.json、vanilla-server-order.json 和 exact-server-trace.log 保存源码和原始结果。早期未等远端就绪的 server-trace.log 仅为诊断，不作为通过证据。
- observer_reschedule 只有 C++ 输出，比较器明确列为尚未经原版验证，没有算入七项。
- 漏斗、实体、流体、多维度、加载票，以及 SafeSave 的保存/重启恢复、序号合并、时间重基尚未实现或不在当前目标内，不标为通过，也不标为顺序失败。这不代表通过整个 SafeSave 测试集。
- Java 哈希表迭代顺序仍按原约定豁免；本次记录到的序列均逐项比较。

## 重跑

在 redstone-cpp 下：

```powershell
cmake --build build-jemalloc --config Release --target safesave_order_probe -j 6
python validation/safesave_order/compare.py --native build-jemalloc/Release/safesave_order_probe.exe
ctest --test-dir build-jemalloc -C Release --output-on-failure
```

比较器要求七项原版记录齐全，缺失或不同即失败，报告首个不同的数组位置和值。safesave_order_parity 已接入 CTest。原版重跑见 vanilla/README.md；服务端与世界在独立验证目录，不访问用户存档。
