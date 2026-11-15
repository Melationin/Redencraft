# 简单实体、铁轨与物品

本批按 26.1 Java 源码补入非生物简单实体、铁轨方块和物品实体。**当前实现范围**是服务器端、单维度、只读世界的调试模拟；项目目标形态（多维度、加载票、世界生成、保存、时间、光照）见 [AGENTS.md](../AGENTS.md) 第 1 节，分期见 [PLAN.md](../PLAN.md)。玩家物理、生物、一般实体间碰撞推动、乘客和容器仍不实现。独立可选协议服务端现已提供创造编辑及简单实体同步，见 [协议说明](protocol775.md)，不代表已实现完整玩家模型。

## 阶段与顺序

`ServerLevel.tick` 的实体阶段位于方块事件之后、方块实体之前。当前顺序为：

1. `gameTime++`
2. 方块计划刻（每刻上限 65536）
3. 方块事件
4. `handlingTick = false`
5. 实体刻
6. 移动活塞方块实体刻
7. 外部方块实体 tick 回调

实体刻使用 `EntityTickList` 语义：按注册顺序建立本刻快照；刻内新增实体进入下一份活动表，下一游戏刻才刻；本刻内被移除的实体在后续快照项中被跳过。区块未加载的实体保留但不刻，重新加载后继续。这个顺序不能与移动活塞方块实体刻合并，也不能把 TNT 爆炸或铁轨邻居更新推迟到方块实体之后。

## 实体基础

源码依据：

- `net/minecraft/world/entity/Entity.java`
- `net/minecraft/world/entity/EntityDimensions.java`
- `net/minecraft/world/level/entity/EntityTickList.java`
- `net/minecraft/server/level/ServerLevel.java`（`tickNonPassenger`、`tickPassenger`、`tickBlockEntities`）

已实现：

- AABB 尺寸与位置语义：`Pos` 为脚底中心，宽度左右对称，高度向上。
- 基于方块碰撞盒的逐轴移动。轴顺序按 `Direction.axisStepOrder`：先 Y，再按水平位移绝对值选择 X 或 Z；`collideAxis` 的容差为 1e-7，碰撞标记容差为 1e-5。
- 垂直碰撞后的 `onGround`/`verticalCollision`/`verticalCollisionBelow` 语义，水平碰撞清零对应速度分量。
- 方块速度因子、落地后默认清零 Y 速度、史莱姆块落地反弹（非生物倍率 0.8）与 `stepOn` 水平减速。
- 液体高度检测按 `FlowingFluid.getHeight` 的 8/9 源流体高度近似；流体状态模型（`fluid.hpp` 的 `fluidStateOf`）已可用，但流体刻、流动、液面重算和流体推挤仍未实现（[路线图](../PLAN.md) 阶段 1，测试规格见 [水与岩浆对照测试设计](fluid-testing.md)）。
- 岩浆接触调用 `lavaIgnite` + `lavaHurt`（伤害 4）；火焰刻每 20 刻伤害 1。水和岩浆的存档状态保留，且碰撞为空。
- `moveTowardsClosestSpace`：物品卡在方块内时按原版方向顺序选择最近空位并随机推出。
- 世界底部 `minY - 64` 以下移除。

未实现并保留 TODO：

- 实体间碰撞（`getEntityCollisions`）和实体挤压；目前只实现矿车对矿车的推动分支。
- 世界边界、光照、雨、冰冻、末地传送门/玩家进门延迟、载具/乘客、游戏事件、音效和粒子（下界传送门 `entityInside`、冷却 `300` 刻与已加载目的地的跨维传送已接入；目的地未生成/未加载时按 `TODO` 不建门也不传送）。
- 其余未覆盖方块的 `entityInside` 效果（如细雪、蜘蛛网、蜂蜜块滑动等）；目前已接入岩浆、火焰/灵魂火、下界传送门、探测铁轨与漏斗。

## 移动活塞与实体

源码：`PistonMovingBlockEntity.moveCollidedEntities/moveStuckEntities/tick`、`PistonMath`、`Entity.move(MoverType.PISTON)`。

- 移动活塞方块实体刻（位于实体刻之后）先计算密度形状、由当前位置和 `extendedProgress` 移动后的 AABB，再用 `PistonMath.getMovementArea` 与当前 AABB 求并集搜索实体，之后才推进 `progress`。
- 对每个可推动实体按扫掠 AABB 计算所需位移，限制在 `deltaProgress` 内并加 0.01；通过 `MoverType.PISTON` 移动。该类型按轴累计到每刻 0.51 的上限，并在移动期间设置 NOCLIP：被推动方向相同的 moving_piston 不再用房块自身的移动状态碰撞盒阻挡实体，回缩源活塞仍保留伸出的活塞头碰撞盒。
- 粘液块被移动时（`movedState == slime_block`）执行 `causeBounce`：非玩家实体沿活塞移动轴的 `deltaMovement` 分量被直接设为 ±1，然后仍执行上述扫掠位移。因此下一次实体刻会以 1.0 的初速沿活塞方向飞出；方块破坏在同一刻生成的物品实体也能在这一方块实体阶段被击中。
- 蜂蜜块被水平移动时执行 `moveStuckEntities`：搜索上方 1.5 格、脚下满足条件且 `onGround` 的实体，按 `deltaProgress` 随蜂蜜块移动。
- 回缩源活塞还执行 `fixEntityWithinPistonBase`，把卡在活塞基座内的实体沿反方向挤出。
- 尚未实现实体间碰撞挤压、玩家/生物的特殊推送、矿车乘客和完整 `ServerLevel.tickPassenger`。

## 物品实体

源码：`net/minecraft/world/entity/item/ItemEntity.java`。

- 尺寸 0.25×0.25，重力 0.04。
- 在水（液体高度 > 0.1）中按 0.99 水平阻尼并施加 0.0005 的上浮增量；在岩浆中按 0.95；否则施加重力。
- 移动后水平摩擦为 `方块摩擦 × 0.98`，垂直乘 0.98；落地且 Y 速度为负时反转一半。默认方块摩擦 0.6，史莱姆块 0.8。
- 静止在地面且水平速度不超过 1e-5 时，仅每 4 刻执行一次 `move` 分支；这与原版 `ItemEntity.tick` 的 `(tickCount + id) % 4` 分支一致。
- 合并：每 2 刻（跨方块时）或每 40 刻检查 0.5 格水平膨胀盒内的同类可合并物品；合并上限受物品最大堆叠数和单次 64 限制；合并后取较大 `pickupDelay`、较小 `age`，源物品为空时移除。
- `age` 每刻加 1；`age >= 6000` 移除；`age == -32768` 无限寿命；`pickupDelay == 32767` 不可拾取。
- `Health` 默认 5，爆炸/火焰伤害按原版扣减；归零移除。没有玩家拾取，因此 `playerTouch` 路径不实现。

## TNT 实体与爆炸

源码：`net/minecraft/world/entity/item/PrimedTnt.java`、`net/minecraft/world/level/ServerExplosion.java`、`ExplosionDamageCalculator.java`、`TntBlock.java`。

- 可被红石信号点燃的 TNT 方块：`TntBlock.onPlace`/`neighborChanged` 生成实体并 `removeBlock(pos, false)`；默认引信 80。
- `PrimedTnt.tick`：重力 0.04、移动、方块效果、整体乘 0.98、落地乘 `(0.7, -0.5, 0.7)`；引信归零后先移除实体再以 `y + 0.0625` 为中心爆炸，威力 4，`fire=false`。
- 世界开关 `tntExplodes` 对应原版 `TNT_EXPLODES` 游戏规则；关闭时方块不点燃、实体不爆炸。
- 爆炸射线：16³ 边界方向、每方向 `radius × (0.7 + random × 0.6)`、步长 0.3、每步损耗 `(resistance + 0.3) × 0.3`。空气/空流体不损耗；水/岩浆按 100 抗性；方块抗性来自 `BlockMetadata.explosionResistance`。抗性按 `BlockBehaviour.Properties.strength(x)` 的 `explosionResistance = x` 规则设置；`moving_piston` 为 0，黑曜石 1200，基岩 3,600,000。
- 爆炸实体伤害：AABB 搜索、`getSeenPercent` 采样遮挡、伤害与击退。简单实体没有 `onExplosionHit` 覆写；物品按生命值销毁；矿车按 `VehicleEntity.hurtServer` 累计 `damage += damage × 10`，超过 40 时移除并掉落矿车物品；TNT 和下落方块不可伤害。
- 方块破坏：每个目标方块先按空工具掉落表生成物品实体，再 `setBlock(air, 3)`；TNT 方块不掉落自身，而是在移除后生成随机引信 `nextInt(20) + 10` 的 TNT 实体，形成链式爆炸。
- 掉落堆叠按原版 `StackCollector` 在目标方块间合并，单堆最多 16，再以方块中心附近随机偏移生成物品实体。
- `fire=true` 的火焰生成尚未实现（TNT 固定为 false）。

已知差异：原版 `calculateExplodedPositions` 使用 `HashSet`，`Util.shuffle` 使用世界随机源；本项目按用户约定不要求哈希迭代顺序，也不要求随机序列逐位一致。因此多方块爆炸的破坏/掉落顺序与随机位置可能不同。射线数量、抗性公式、伤害公式、TNT 引信和链式生成逻辑保持源码结构。实体查询（物品合并、爆炸受伤、矿车推动、探测铁轨搜索）按实体注册顺序遍历，原版按区块实体列表顺序；这属于尚未消除的迭代顺序差异，但用户已明确不要求保持哈希表顺序。实体 ID 由本项目从 1 顺序分配，而 `ItemEntity.tick` 的 `(tickCount + id) % 4` 静止分支相位取决于全局实体 ID，故该相位不保证与真实服务器相同。

## 下落方块实体

源码：`net/minecraft/world/entity/item/FallingBlockEntity.java`、`FallingBlock.java`。

- 已实现实体刻、重力、落地判定、`DropItem`、`Time > 100/600` 丢弃、`moving_piston` 不下落放置、落地后写回方块并移除。
- 当前注册表没有沙子/砂砾等 `FallingBlock` 方块，因此没有自然触发源；只能通过 `World::spawnFallingBlock` 或读取存档中的 `minecraft:falling_block` 创建。铁砧伤害、混凝土遇水、方块实体数据合并仍是 TODO。

## 矿车与铁轨

源码：

- `net/minecraft/world/entity/vehicle/minecart/{AbstractMinecart,Minecart,OldMinecartBehavior}.java`
- `net/minecraft/world/level/block/{BaseRailBlock,RailBlock,PoweredRailBlock,DetectorRailBlock,RailState}.java`
- `net/minecraft/world/entity/vehicle/VehicleEntity.java`

默认按非实验性 `minecart_improvements` 关闭处理，即 `OldMinecartBehavior`：

- 矿车尺寸 0.98×0.7；重力水面 0.005、其他 0.04；水中最高速度 0.2，陆地 0.4；自然阻尼 0.96（无乘客）。
- 每刻查找脚下或当前方块是否为铁轨：有轨进入 `moveAlongTrack`，无轨进入 `comeOffTrack`。轨道移动包括 0.0078125 的坡度滑速、1/16 像素级轨道中心校正、朝向修正、坡度出口高度修正、速度投影和动力铁轨加速/静止启动。
- 动力铁轨按 8 格链式搜索供能，并检查下方/相邻的红石导体；激活铁轨按动力状态调用 `activateMinecart`（空矿车受伤/弹射逻辑，乘客系统未实现）。
- 探测铁轨检测 0.2..0.8 搜索盒内的矿车，变化时更新 `powered`、通知连接铁轨、更新相邻方块信号、每 20 刻复查；`getSignal` 输出 15，并按原版 `getDirectSignal` 强充能其下方方块。
- 矿车对矿车的推动按 `AbstractMinecart.push` 与 `OldMinecartBehavior` 的旧版分支实现；玩家/生物/物品的乘客和推动不实现。
- 爆炸对矿车按 `VehicleEntity.hurtServer` 处理，超限时移除并生成矿车物品实体。

铁轨方块：

- 已注册 `rail`、`powered_rail`、`detector_rail`、`activator_rail`，状态包含 `shape`、`powered`（动力/探测/激活）和 `waterlogged`。
- 碰撞形状为空（原版 `noCollision`）；放置、支撑检查、直角/坡道自动连接、邻居变化、支撑移除掉落、动力铁轨更新、探测铁轨计划刻均已实现。
- `RailState` 的连接判定、软连接移除、直角选择、坡道选择和放置后邻居重连按源码移植。玩家放置朝向由客户端编辑器提供初始 `shape`，随后 `onPlace` 自动连接。
- `waterlogged` 已接通语义（F-5）：源水可经 `LiquidBlockContainer.placeLiquid` 注入干宿主，失去支撑或玩家破坏时经 `Level.removeBlock`/`destroyBlock` 保水，含水时由铁轨自己的 `updateShape` 排水刻。

未实现：实验性矿车改进（`NewMinecartBehavior`）、箱子/漏斗/熔炉/命令方块/生成器/TNT 矿车、矿车乘客与碰撞伤害、矿车与粘液块/蜂蜜块的实体互动、铁轨被水流冲毁的掉落路径（`BaseRailBlock` 之外的冲毁入口）。

## 物品与掉落

- `itemRegistry()` 覆盖本批方块物品和 `minecraft:minecart`（最大堆叠 1）。未登记物品仍可保留在物品实体中，但最大堆叠按 64 处理。
- `blockDrops()` 使用空工具评估本批方块：草方块掉泥土，玻璃/枯珊瑚扇需要精准采集因此为空，活塞头/移动活塞/基岩为空，不稳定 TNT 为空，其余支持方块掉自身。完整战利品表、精准采集、时运、工具耐久和经验仍是 TODO。
- 活塞破坏、形状更新破坏、`updateOrDestroy` 和爆炸都生成真实 `ItemEntity`，不再只写日志。
- 拾取、物品栏、漏斗、掉落物消失动画、物品合并上限之外的容器逻辑不实现。

## 存档

`LoadedWorld` 读取选中维度独立 `entities/r.x.z.mca` 的 `Entities`，以及地形中遗留的 lowercase `entities`；导入 `minecraft:item`、`minecraft:tnt`、`minecraft:falling_block`、`minecraft:minecart`。支持 `Pos`、`Motion`、`Rotation`、物品堆栈、`Age`、`PickupDelay`、`Health`、`fuse`、`block_state`、`explosion_power`、`BlockState`、`Time`、`DropItem`、`HurtEntities`、`FallHurtAmount`、`FallHurtMax`、`CancelDrop`、`FlippedRotation`、`HasTicked`。其他实体保留原始标签，不要求其 Pos 来创建实体；重复 UUID 导入去重，未知实体不模拟。数值转换前按原版限制位置/速度，拒绝 NaN；`HasTicked` 直接赋给 `firstTick`，保留原版反直觉读写。种子来自 26.1 的 `data/minecraft/world_gen_settings.dat` / `data.seed`，兼容旧嵌套 `WorldGenSettings.seed`，不是废弃的 `RandomSeed`。随机序列不承诺与真实服务器一致。具体存档检查和实际存档验证见 [读取说明](storage.md)。

## 验证边界

- 现有 `entity` CTest 覆盖：物品落地/地毯高度/水平碰撞、合并、物品与方块掉落表、水/岩浆接触、史莱姆反弹、寿命、TNT 方块点燃、TNT 爆炸与黑曜石抗性、链式 TNT、爆炸伤害、下落方块、矿车坠落、直轨/动力轨/动力链/坡道/探测轨、探测轨断电、铁轨支撑移除掉落、实体刻顺序、物品实体 NBT 导入。
- `piston` CTest 额外断言地毯复制产生真实物品实体、回缩粘液块把物品向西弹出（局部单刻速度为 -1.0），以及动态活塞扫掠位移。
- 已实际运行 26.1 服务端的地毯复制结构并读取物品 `Pos`/`Motion`：`validation/vanilla26_1/item_motion_validation.py` 与 `item-motion-validation.log`。原版被粘液块击中后的 X 速度序列为 `-1.0, -0.98, -0.9604, ...`，C++ 的对应序列一致；原版命中的绝对刻会随命令/`tick step` 调度偏移一 tick，Y/Z 还含随机初速，因此测试只断言方向、局部单刻 -1.0 和确定性递推，不断言某次服务器运行的绝对坐标。
- 其余实体/铁轨/爆炸路径仍主要是 C++ 回归测试，尚未做完整服务端差分；不能声称与 26.1 完全等价。
