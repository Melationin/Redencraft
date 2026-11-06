> 后续优化（Block 单例、指针身份、lithium 爆炸、网络发送）的记录与最新数据见 [性能优化记录](../../docs/performance.md)。

# 原生服务器 CPU 采样

2026-11-06 使用 Windows Performance Recorder `CPU.Verbose.File` 对正在运行的投影模拟服务器采样。PID 40120 全程保持监听，没有重启或暂停。管理员 ETW 采集时长 2 分 27 秒，记录 **0 lost events**；`xperf` 完成后处理。

聚合结果显示 `redstone_vanilla_server.exe` 对应 **3.63% 的全机采样权重**（147,903,690 weight）；这是多核全机归一化占比，不是进程内部函数占比。采样日志中可见投影在正常平台阶段约 5–6 ms/tick，但结构进入工作循环后达到约 147–359 ms/tick，短时峰值 775.663 ms。同期活塞数最高采样点为 3,325；之后物品/TNT实体由 133 升至 1,197 并保持增长。详细采样时间点见运行日志 `logs/schematic-20261106-192904.stdout.log`。

这是可用的进程/模块级采样，但本次构建为 Release 且没有保留 `redstone_vanilla_server.exe` 的 PDB。Windows ETW 因此无法把该模块里的地址还原成 C++ 函数；不能根据这次结果断言具体函数是热点。`xperf` 的 call-stack 报告也以未解析的系统帧为主。要得到函数级热点，需要使用与服务器二进制完全匹配的 PDB；当前运行进程没有这样的 PDB。真实客户端没有连接，采样中的压力来自服务端自身运行投影和 TNT/实体扩增。

采样原文件和 `xperf` 派生报告在忽略的本地目录 `build-jemalloc/`：`performance-cpu.etl`（约 2.32 GB）、`performance-cpu-samples.txt`、`performance-cpu-stacks.txt`。WPR 已停止；服务端仍在 127.0.0.1:25565 运行。

**历史结论（旧构建）：**投影在动作密集期超过 50 ms/tick 预算较多，客户端移动时编码邻近区块也会偶尔产生约 225 ms 的 tick 样本。旧样本无法定位 C++ 函数热点；后续带符号采样见下节。

## 30 秒符号化采样（PID 26748）

2026-11-06 对带 PDB 的 `build-symbols/Release/redstone_vanilla_server.exe` 采集 WPR CPU ETW。进程 PID `26748` 全程运行。ETL 时间跨度为 51.775 秒（19:56:44.886–19:57:36.661，UTC+8）；WPR 启动配置阶段计入 ETL，因此按 ETL 尾部截取连续 30 秒窗口（19:57:06.661–19:57:36.661）生成下列函数报告。窗口内 **0 lost buffers、0 lost events**。分析结果通过匹配 PDB 解出了服务端 C++ 函数名。

30 秒窗口的调用栈热点：

| 调用路径/函数 | 进程采样占比 | 说明 |
| --- | ---: | --- |
| `World::tick` | 99.50% inclusive | 几乎全部服务器样本都在世界 tick 调用树中 |
| `World::tickEntities` | 71.87% inclusive | 占 `World::tick` 子树约 72.23% |
| `PrimedTnt::tick` | 70.94% inclusive | 占实体 tick 子树约 98.70% |
| `World::explode` | 58.85% inclusive | 占 `PrimedTnt::tick` 子树约 82.96% |
| `pistonTickBlockEntities` | 12.72% inclusive | 占 `World::tick` 子树约 12.79% |
| `World::runEvents` | 10.40% inclusive | 占 `World::tick` 子树约 10.45% |
| `World::runScheduled` | 3.01% inclusive | 占 `World::tick` 子树约 3.02% |

Exclusive 样本热点主要是内存分配/释放和状态/区块查询：`je_mallocx` 8.88%、`World::isLoaded` 8.60%、状态名 `canonical` 8.12%、`je_dallocx` 7.99%、jemalloc `arena_palloc` 6.40%、状态查找 `named` 3.98%、`World::getBlock` 1.89%、`World::entitiesIn` 1.72%。Inclusive 与 exclusive 是不同统计口径，inclusive 百分比会重叠，不能相加。

**判断：**本次机器负载主要由 TNT 实体 tick 引发的爆炸链驱动；活塞方块实体 tick 约占 12.8%，不是当前最大热点。次级成本集中在 jemalloc 分配/释放、`isLoaded` 检查和状态字符串查询。服务端之后的日志（tick 1500–2400）记录 1,064 个实体、每 tick 532 个计划刻、约 2,128 个移动活塞，采样 tick 耗时约 4.3–4.5 秒；这是 ETW 窗口之后的运行状态，不能当作采样窗口内的精确 tick 统计。

ETL 与派生报告保存在忽略目录 `build-symbols/`：`server-30s.etl`、`server-30s-window-profile-detail.txt`、`server-30s-window-stack.html`、`server-30s-trace-stats.txt`。本次采样未停止服务端。
