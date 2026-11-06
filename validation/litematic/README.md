# 无沟 V3.1 双倍速防雪世吞：投影方块覆盖检查

**后续状态：**下文保留实现前的审计结果。用户随后要求实现除告示牌外的缺项，沙子、石砖墙、红石灯现已加入；正式 C++ 投影导入及协议模拟入口也已完成，见 [投影模拟](../../docs/litematic.md)。新的导入和运行证据保存在 `server-smoke-result.json`、`server-active-smoke-result.json`；旧审计 JSON 的 missing 字段是当时的注册状态。

2026-11-06，按用户要求停止 `redstone_vanilla_server` 后只读检查。没有启动客户端，没有修改原投影，也没有将结构导入或运行。

输入：`C:/Users/zhdds/Downloads/[再灰二度] 无沟V3.1双倍速防雪世吞-997.生成导出-800x83x800.litematic`。

SHA-256：`7ad76ac215039e66594a81feb45dae70896827c768328133d19b86d3e8d91e52`。

详细状态和数量保存在 [shitun-v3.1-audit.json](shitun-v3.1-audit.json)。支持状态从当前 `src/engine.cpp` 的 `blockRegistry()`、`entityRegistry()` 提取，不采用协议资产中覆盖全游戏的 ID 表作为实现依据。

## 结果

410 个子区域的包围盒没有重叠。实际解码 5,856,016 个区域内位置，其中非空气方块 70,112 个，共 18 种非空气方块。区域总体包围尺寸为 800 × 83 × 800；区域未覆盖的空间不计入解码位置数。

按用户授权将 28,764 个铁砧视为黑曜石后，未实现的方块还有以下四种，共 7,054 个：

| 方块 | ID | 实际数量 | 当前缺少的行为 |
|---|---|---:|---|
| 沙子 | `minecraft:sand` | 6,650 | 方块注册、放置及形状更新调度、2 游戏刻后检查下方并生成下落方块实体。已有 `FallingBlockEntity` 不代表沙子方块的入口已实现。 |
| 石砖墙 | `minecraft:stone_brick_wall` | 399 | 当前墙体注册及形状判断只识别 `cobblestone_wall`。需要将墙体连接、高低边、支撑及碰撞行为扩展到石砖墙。不能将它当作普通整方块。 |
| 红石灯 | `minecraft:redstone_lamp` | 4 | 注册、`lit` 状态、放置时读取电源、通电立即亮起、断电调度 4 游戏刻后再次检查电源并熄灭。 |
| 白桦木墙上告示牌 | `minecraft:birch_wall_sign` | 1 | 注册、朝向/支撑/形状、被破坏及推动反应，以及对应告示牌方块实体和文本数据。 |

其余投影方块均已注册：激活铁轨、失活火珊瑚墙扇、探测铁轨、蜂蜜块、音符盒、侦测器、活塞、活塞头、红石块、粘液块、平滑石、粘性活塞、TNT。黑曜石也已注册。

这里的“已注册”只表示对应实现存在；没有运行整个世吞，也没有据此宣称整机与原版一致。没有发现水、岩浆或 `waterlogged=true` 的已使用状态。

## 导入前还需处理的数据问题

- 投影包含 133 个 `minecraft:minecart` 实体，没有其他实体类型；矿车已在 C++ 简单实体范围内。其中一个 UUID 出现 130 次，其他三个 UUID 各出现一次。当前世界存档读取会按 UUID 去重；投影导入应为重复实例分配独立 UUID，并将子区域内实体坐标转换到放置坐标，不能直接套用存档去重语义。
- 另有一个 `minecraft:sign` 方块实体，对应缺少的白桦木墙上告示牌。没有已保存的方块计划刻或流体计划刻。
- 文件版本为 Litematic 6 / SubVersion 1，但生成器填写的 `MinecraftDataVersion` 是 `20060210`，并非本项目原版 26.1 的 `4786`。`Metadata.TotalBlocks=114514` 和 `TotalVolume=1919810` 也与实际数据不符。本报告使用实际解码结果，不使用这些元数据统计。
- 项目目前读取的是世界区域存档；尚无正式 `.litematic` 世界导入入口。此检查脚本只读统计，不创建 C++ 世界、不模拟、不改写原文件。后续导入需要显式验证状态兼容性，不能仅将 DataVersion 改为 4786 来绕过校验。
- 800 × 800 的结构若按完整水平包围范围加载，至少覆盖 50 × 50 个区块；当前协议端单次固定加载上限 64 个区块也需要另行处理。模拟核心和网络显示范围是不同限制。

## 复查

审计脚本依赖 `nbtlib` 和 `numpy`，前者安装在忽略的构建目录，不修改系统 Python 环境：

```powershell
python -m pip install --target build-jemalloc/litematic-tools --no-deps nbtlib
$env:PYTHONPATH = "$PWD/build-jemalloc/litematic-tools"
python validation/audit_litematic.py 'C:/Users/zhdds/Downloads/[再灰二度] 无沟V3.1双倍速防雪世吞-997.生成导出-800x83x800.litematic' --output validation/litematic/shitun-v3.1-audit.json
Remove-Item Env:PYTHONPATH
```

脚本按 Litematica 连续位流格式解析，处理跨 64 位边界的条目，并进行独立标量抽查。格式依据：[Litematica 作者对位打包的说明](https://github.com/maruohon/litematica/discussions/463)。本次另外以 `numpy.unpackbits` 将所有位置解码成逐位矩阵，对全部 410 区域、5,856,016 个位置的按方块类型计数进行独立复核，结果完全一致。

本次没有修改模拟核心，也没有重跑无关 CTest；验证范围是只读解码、数量复核和当前注册/行为入口的源码审查。
