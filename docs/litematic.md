# 投影模拟

2026-11-06 已补入沙子、石砖墙、红石灯，并支持用户的 800 × 83 × 800 无沟 V3.1 投影。告示牌和其方块实体按用户要求跳过；28,764 个铁砧替换为黑曜石。原文件保持只读。

```powershell
./build-jemalloc/Release/redstone_vanilla_server.exe --schematic 'C:/Users/zhdds/Downloads/[再灰二度] 无沟V3.1双倍速防雪世吞-997.生成导出-800x83x800.litematic' --spawn 2.5,102,-1.5
```

26.1 原版客户端连接 `127.0.0.1:25565`。投影默认偏移 (0,64,0)，前端音符盒在 (2,101,3)。服务器持续 tick；初始状态保留投影快照，右键音符盒可以触发结构更新。没有自动启动客户端或接管桌面。每个 tick 完成后向聊天栏发送编号和毫秒耗时，包括世界模拟与连接同步准备，排除等待、tick 外 socket IO 和消息自身发送准备。

新增参数：`--schematic file`、`--offset x,y,z`、`--force-margin 2`、`--view-distance 3`。`--world` 与 `--schematic` 互斥；投影路径支持 Windows Unicode。默认完整加载 50 × 50 区块并向四周扩展两区块空白边缘，共 54 × 54 = 2,916 个强加载区块，没有生成地形。移动出该范围后仍遵循原有未加载区块暂停语义；无限跟随机器加载不在此功能范围。

只将实际非空气状态存入现有 Chunk/Section 存储。完整解码 410 区域，导入 70,111 个方块、133 辆矿车，跳过一个告示牌及一个告示牌方块实体。所有投影实体分配独立 UUID，防止生成器重复模板身份被存档 UUID 去重过滤。块/方块实体坐标按区域最小角落转换；实体 Pos 按区域 Position 转换，负 Size 时二者不同。格式依据见 [Litematica 的区域粘贴/实体保存实现](https://raw.githubusercontent.com/maruohon/litematica/pre-rewrite/fabric/1.20.1/src/main/java/fi/dy/masa/litematica/schematic/LitematicaSchematic.java)。

生成器声明的 DataVersion 20060210 不作为原版存档版本接受，而由专门的 Litematic 6/SubVersion 1 读取路径处理：每个已使用状态通过当前核心注册表及官方 26.1 状态/属性表检查。拒绝不支持的机制方块、实体、乘客、非支持方块实体、流体计划刻及重叠区域；没有修改 DataVersion 来绕过 Anvil 校验。一般历史版本 DataFixer、旋转/镜像及明确重叠粘贴顺序仍待实现。

导入使用原始快照写入，不逐块执行放置、邻居或形状更新，也不凭空添加初始沙子计划刻。保存的计划刻会恢复；当前输入没有保存计划刻。这不是特定客户端粘贴时的回调顺序复现。后续正常放置/更新中的沙子按原版任意方向 shape update 调度两刻重力，移动活塞到位后也会通过原有路径调度。红石灯保留四刻熄灭及已有计划刻去重期限；石砖墙复用墙连接/形状路径，硬度 1.5、抗性 6。

下落方块成功落地后，立即向跟踪该实体的连接发送当前位置方块状态，然后移除实体；普通区块变更仍按既有广播阶段合并。沙子和铁轨的移动/实体 tick 顺序没有合并。未实现的 ENTITY_DROPS 非默认规则、门户和其他 Fallable 特性继续在源码保留 TODO。

验证：MSVC Release + jemalloc CTest **12/12**；真实输入只读导入测试断言 70,111 块、133 矿车、2,500 原始区块。`validation/litematic/server_smoke.py` 实际启动临时 C++ 服务端，验证 30 个配置包、视野分批及 ACK、跨至另一角落的区块加载/旧区块卸载、逐 tick 聊天消息，完成后退出；另一次 `--activate-control` 右键前端音符盒后运行至 100 tick，日志记录 401 计划刻、266 移动活塞。结果分别在 `validation/litematic/server-smoke-result.json`、`server-active-smoke-result.json`。新增协议测试的 255 个数据包已由原版 Java GameProtocols codec 严格解码。没有进行真实客户端显示或原版整机差分，不能据此宣称整机与原版等价。
