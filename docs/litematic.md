# 投影模拟

2026-11-06 已补入沙子、石砖墙、红石灯，并支持用户的 800 × 83 × 800 无沟 V3.1 投影。告示牌和其方块实体按用户要求跳过；28,764 个铁砧替换为黑曜石。原文件保持只读。

```powershell
./build-jemalloc/Release/redstone_vanilla_server.exe --schematic 'C:/Users/zhdds/Downloads/[再灰二度] 无沟V3.1双倍速防雪世吞-997.生成导出-800x83x800.litematic' --spawn 2.5,102,-1.5
```

26.1 原版客户端连接 `127.0.0.1:25565`。投影默认偏移 (0,64,0)，前端音符盒在 (2,101,3)。服务器持续 tick；初始状态保留投影快照，右键音符盒可以触发结构更新。没有自动启动客户端或接管桌面。聊天栏每秒更新一次，显示前 1 秒完成的 tick 平均毫秒耗时与样本数；包括世界模拟与连接同步准备，排除等待、tick 外 socket IO 和消息自身发送准备。

新增参数：`--schematic file`、`--offset x,y,z`、`--force-margin 2`、`--view-distance 3`。`--world` 与 `--schematic` 互斥；投影路径支持 Windows Unicode。默认完整加载 50 × 50 区块并向四周扩展两区块空白边缘，共 54 × 54 = 2,916 个强加载区块，没有生成地形。移动出该范围后仍遵循原有未加载区块暂停语义；无限跟随机器加载不在此功能范围。

只将实际非空气状态存入现有 Chunk/Section 存储。完整解码 410 区域，导入 70,111 个方块、133 辆矿车，跳过一个告示牌及一个告示牌方块实体。所有投影实体分配独立 UUID，防止生成器重复模板身份被存档 UUID 去重过滤。块/方块实体坐标按区域最小角落转换；实体 Pos 按区域 Position 转换，负 Size 时二者不同。格式依据见 [Litematica 的区域粘贴/实体保存实现](https://raw.githubusercontent.com/maruohon/litematica/pre-rewrite/fabric/1.20.1/src/main/java/fi/dy/masa/litematica/schematic/LitematicaSchematic.java)。

生成器声明的 DataVersion 20060210 不作为原版存档版本接受，而由专门的 Litematic 6/7、SubVersion 1 读取路径处理：每个已使用状态通过当前核心注册表及官方 26.1 状态/属性表检查。拒绝不支持的机制方块、实体、乘客、非支持方块实体、流体计划刻及重叠区域；没有修改 DataVersion 来绕过 Anvil 校验。一般历史版本 DataFixer、旋转/镜像及明确重叠粘贴顺序仍待实现。

导入使用原始快照写入，不逐块执行放置、邻居或形状更新，也不凭空添加初始沙子计划刻。保存的计划刻会恢复；当前输入没有保存计划刻。这不是特定客户端粘贴时的回调顺序复现。后续正常放置/更新中的沙子按原版任意方向 shape update 调度两刻重力，移动活塞到位后也会通过原有路径调度。红石灯保留四刻熄灭及已有计划刻去重期限；石砖墙复用墙连接/形状路径，硬度 1.5、抗性 6。

下落方块成功落地后，立即向跟踪该实体的连接发送当前位置方块状态，然后移除实体；普通区块变更仍按既有广播阶段合并。沙子和铁轨的移动/实体 tick 顺序没有合并。未实现的 ENTITY_DROPS 非默认规则、门户和其他 Fallable 特性继续在源码保留 TODO。

验证：MSVC Release + jemalloc CTest **12/12**；真实输入只读导入测试断言 70,111 块、133 矿车、2,500 原始区块。`validation/litematic/server_smoke.py` 实际启动临时 C++ 服务端，验证 30 个配置包、视野分批及 ACK、跨至另一角落的区块加载/旧区块卸载、逐 tick 聊天消息，完成后退出；另一次 `--activate-control` 右键前端音符盒后运行至 100 tick，日志记录 401 计划刻、266 移动活塞。结果分别在 `validation/litematic/server-smoke-result.json`、`server-active-smoke-result.json`。新增协议测试的 255 个数据包已由原版 Java GameProtocols codec 严格解码。没有进行真实客户端显示或原版整机差分，不能据此宣称整机与原版等价。

## 小宽 1600 × 80 × 1600 投影（2026-11-07）

用户后续指定 `[小宽]双倍速无沟世吞V3.1.litematic`，实际为 Version 7 / SubVersion 1、DataVersion 4435、4262 个互不重叠区域；不依据不准确的 Metadata.TotalBlocks 导入。按 [Litematica 原始读写实现](https://raw.githubusercontent.com/maruohon/litematica/pre-rewrite/fabric/1.21.1-masa/src/main/java/fi/dy/masa/litematica/schematic/LitematicaSchematic.java) 核对 v6/v7 共用的区域坐标、palette 与跨 long 位打包，读取区域上限提高到 8192，其他体积/区块/机制校验保持。未增加通用实体组件 DataFixer；此输入无实体、无计划刻，仅有两个被既有规则跳过的告示牌方块实体。

沿用铁砧替换黑曜石、跳过告示牌的导入规则，预计保留 124006 个非空气方块，其中替换 25568 个铁砧，跳过 2 个告示牌。默认偏移 `(0,64,0)` 后范围为 `[0,1600) × [-15,65) × [-9,1591)`。两个橡木栅栏门控制点为 `(3,3,-7)` 与 `(3,4,1585)`；此投影没有音符盒控制点。

补入橡木栅栏门的 32 个状态、碰撞/轮廓、墙连接、in_wall 更新、红石开关、右键开关及放置初始化，对照本地只读 26.1 `FenceGateBlock`/`WallBlock`/`Blocks`。TODO：声音与 BLOCK_OPEN/CLOSE 游戏事件尚未接入；风弹等 TRIGGER_BLOCK 爆炸模式尚未实现。本项目可选调试客户端尚未核实使用新增轮廓查询接口，原版客户端按自身形状选取。

本次人工测试服使用独立 `build-performance` 的 MSVC x64 Release：`/O2 /Ob2 /DNDEBUG`、IPO（`/GL` / `/LTCG`）、`/INCREMENTAL:NO /OPT:REF /OPT:ICF`、静态运行库及 jemalloc，无调试信息/分配计数/剖析器，关闭可选调试客户端；保持现有浮点计算选项。此配置以速度优化为目的，没有对各编译参数进行逐项性能排名。运行参数为视距 10、出生点 `(3.5,4,-10.5)`，在第一处栅栏门附近；读取原始文件，不自动激活机器。用户要求尽快启动，本轮只执行必要的构建、针对性 CTest 与本地监听检查，不重复完整性能测试。

无车防雪 2006 × 81 × 100 版另外使用苔藓块、石砖和白桦栅栏门。按 26.1 `Blocks` 注册原始方块身份与状态。苔藓为完整导体方块，活塞推动时破坏，骨粉生成苔藓斑块依赖世界特性，仍为 TODO。石砖正常移动，音符盒识别其底鼓音色。白桦栅栏门共用橡木栅栏门的更新和交互路径，保留独立的协议方块 ID，既有门音效与游戏事件 TODO 同样适用。默认偏移下，两个控制门位于 `(4,86,6)` 和 `(4,87,93)`。
