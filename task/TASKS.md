# 任务交接

流程与状态定义见 [AGENTS.md](../AGENTS.md) 第 3 节；任务顺序见 [PLAN.md](../PLAN.md)。
本文件只记录**当前任务**；M0-1 的完整审查记录在 `c8e9eb1` 的提交内容里。

---

## F-1 流体状态模型

### 目标

实现独立的 `Fluid` / `FluidState` / `Fluids` 概念，以及方块状态到流体状态的查询。本任务只交付**状态模型**与聚焦测试，不实现流动算法（F-3）和方块侧接线（F-5）。

### 范围

1. `Fluid`：五个单例 `minecraft:empty`、`water`、`flowing_water`、`lava`、`flowing_lava`；提供 `isSame`、`isSource`、`getAmount`、`isEmpty`、`getOwnHeight`、`createLegacyBlock`、`getLegacyLevel`。桶物品名只做登记，不做桶交互。
2. `FluidState`：`const Fluid*` + `amount`(0..8) + `falling`；提供 `getType`、`isSource`、`isSourceOfType`、`isEmpty`、`getAmount`、`isFull`、`getOwnHeight`、`getLegacyLevel`、相等比较。
3. 方块到流体状态的查询：`water` / `lava` 的 `level=0..15` 映射。`level=0` 为源（amount 8、falling=false）；`level=1..7` 为 amount `8-level` 的非 falling 流动；`level=8..15` 为 amount 8 且 falling=true。索引按原版 `Math.min(level, 8)` 截断，但**方块状态本身保留 0..15 原值**。
4. `createLegacyBlock` 反向映射：源写 `level=0`；流动写 `8 - min(amount,8) + (falling ? 8 : 0)`。

### 源码依据

- `net/minecraft/world/level/material/Fluid.java`：`isEmpty`、`isSame`、`isSource`、`getAmount`、`defaultFluidState`、`createLegacyBlock`。
- `net/minecraft/world/level/material/FluidState.java`：`isSource`、`isSourceOfType`、`isEmpty`、`getAmount`、`isFull`、`getOwnHeight`。
- `net/minecraft/world/level/material/Fluids.java`：五个单例的注册名与顺序。
- `net/minecraft/world/level/material/WaterFluid.java`、`LavaFluid.java`：`Source`/`Flowing` 的 `getAmount`、`isSource`、`isSame`、`createLegacyBlock`。
- `net/minecraft/world/level/material/FlowingFluid.java` 的 `getLegacyLevel`：`isSource ? 0 : 8 - min(amount,8) + (falling ? 8 : 0)`。
- `net/minecraft/world/level/block/LiquidBlock.java`：`stateCache` 的填充顺序与 `getFluidState` 的 `Math.min(level, 8)`。

### 验收判据

1. 水与岩浆各枚举 `level=0..15`，断言精确类型（`water` vs `flowing_water`）、同族关系、`amount`、`isSource`、`isFull`、`falling`、`getOwnHeight`（amount/9）、legacy 反向映射。
2. `level=9..15` 经查询都得到 amount 8 + falling=true，但方块状态保留原 `level`，不被改写。
3. 空方块与非流体方块返回 `minecraft:empty`（`isEmpty()`、`getAmount()==0`、`getOwnHeight()==0`）。
4. 查询**不写方块、不发计划刻、不改队列**：断言查询前后 `scheduledTickCount()` 与方块状态都不变。
5. 水/岩浆的全部 32 个状态仍在 26.1 协议表内（`assets/protocol775/block_states.tsv` 的 86-117 行），不新增协议 ID。
6. 构建通过，CTest 全绿（含新增 `fluid` 组）。

### 计划改动

- 新增：`include/redstone/fluid.hpp`、`src/fluid.cpp`
- 修改：`CMakeLists.txt`（`redstone_core` 加入 `src/fluid.cpp`，注册 `fluid` 测试组）
- 新增：`tests/fluid_tests.cpp`
- 修改：`docs/compatibility.md`、`docs/entities.md`（记录状态模型已实现、流动仍未实现）

### 实际做了什么

**新增 `include/redstone/fluid.hpp` + `src/fluid.cpp`**

- `Fluid`：进程级单例基类，带 `name`、`isEmpty`、`isSource`、`isSame`（虚，家族判定）、`getAmount`（虚）、`getOwnHeight`（虚，默认 `amount/9`）、`createLegacyBlock`（虚）、`getLegacyLevel`（虚，源返回 0，流动返回 `8-min(amount,8)+(falling?8:0)`）、`bucketItem`。
- `WaterFluid` / `LavaFluid`：`isSame` 只认同族两个变体；`getAmount` 源恒为 8、流动读 `rawAmount()`；`createLegacyBlock` 写回同名方块的 `level`。
- 五个单例 `Fluids::empty()/flowingWater()/water()/flowingLava()/lava()`，用函数局部静态实现，避免跨翻译单元静态初始化顺序问题。
- `FluidState`：`const Fluid*` + `amount` + `falling`，`getType/isSource/isSourceOfType/isEmpty/getAmount/isFull/isFalling/getOwnHeight/getLegacyLevel`，以及相等比较。
- `fluidStateOf(const BlockState&)`：`level` 按 `Math.min(level,8)` 截断取流体（0 源、1..7 流动 `8-level`、8..15 falling 满态），**方块自身的 `level` 属性保持原值**，非液体方块返回 empty。

**新增 `tests/fluid_tests.cpp`**，五组断言：

1. `fluidRegistry`：五个单例的 `isEmpty`/`isSource`/`isSame` 关系、桶物品名、`findFluidByName`（含裸名与未知名）。
2. `emptyFluidState`：默认 `FluidState` 与空气/石头/玻璃等非流体方块的查询结果。
3. `levels`：水与岩浆各 `level=0..15` 全档，断言精确类型、`amount`、`isSource`、`isFalling`、`isFull`、`getOwnHeight == amount/9`、`isSame` 同族且跨族为假、`createLegacyBlock` 写回级别；并单独断言 9..15 导入值不改写方块属性、legacy 归一到 8。
4. `queriesAreSideEffectFree`：真实 `World` 里放水与岩浆，连续查询 64 次后断言 `scheduledTickCount()` 与两个方块状态都不变、不凭空生成方块。
5. `protocolStateTable`：水/岩浆各 16 个状态、默认 `level=0`、没有 `falling` 方块属性、全部 `isSchemaState()`，与 `assets/protocol775/block_states.tsv` 的 86-117 行一致。

**修改 `CMakeLists.txt`**：`redstone_core` 加入 `src/fluid.cpp`；测试 `foreach` 列表加入 `fluid`。

**修改文档**：`docs/compatibility.md` 记录状态模型已实现、F-2/F-3/F-5/F-6 仍未实现；`docs/entities.md` 的液体高度条目同步。

**实现过程中修掉的两个问题**：

- `WaterFluid::getAmount` 最初对流动变体回读 `state.getAmount()`，而 `FluidState::getAmount()` 又派发到该虚函数，形成无限递归导致测试挂死。改为读新增的 `FluidState::rawAmount()`（未经过 `getAmount` 规则的存储值）。
- 测试最初断言 `createLegacyBlock` 对 `level=9..15` 精确往返，但原版 `getLegacyLevel` 会把它们归一到 8。已改为断言归一后的期望值，这才是原版行为。

**明确没有做的事**：

- 没有实现流体计划刻队列（F-2）、`FlowingFluid` 传播与液面重算（F-3）、水/岩浆参数差异行为（F-4）、方块侧接线与排水（F-5）、存档与投影的流体刻（F-6）。
- 没有实现 `waterlogged` 宿主的流体状态上报，`fluidStateOf` 里留了 `// TODO` 指向 F-5。
- 没有做桶交互；`bucketItem()` 只登记物品名。
- 没有启动客户端，没有跑真实 26.1 服务端差分。

### 构建与测试

```powershell
cmake -S . -B build-jemalloc -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static"
cmake --build build-jemalloc --config Release -j 6
ctest --test-dir build-jemalloc -C Release --output-on-failure
```

结果：全量构建无错误、无警告；**CTest 16/16 通过**（原 15 组 + 新增 `fluid`）。`fluid` 组单独运行输出 `fluid tests passed`。

证据口径：这是**源码对照 + C++ 回归测试**。`fluid_tests` 的期望值逐个来自 `Fluid.java`、`FluidState.java`、`Fluids.java`、`WaterFluid.java`、`LavaFluid.java`、`FlowingFluid.getLegacyLevel` 与 `LiquidBlock.getFluidState` 的源码结构，不是从原版运行结果录制。**本轮没有跑真实 26.1 服务端差分**，因此不能声称该状态模型与原版逐位等价。

### 提交

（待填）

任务进行中
