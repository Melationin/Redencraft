# 任务交接

流程与状态定义见 [AGENTS.md](../AGENTS.md) 第 3 节；任务顺序见 [PLAN.md](../PLAN.md)。
本文件只记录**当前任务**。F-1…F-3 已验收，记录在 `53a5d20`…`efd54ac` 的提交内容里。

> **状态词**：`任务进行中` → `任务已完成`（coder）→ `审查中` → `审查结束` / `任务通过`（reviewer）。

---

## F-4 水与岩浆参数差异

### 目标

把水与岩浆的差异化规则从「常量落在 Fluid 上」推进到可验证的行为：传播参数、源转换开关、岩浆液面抬升的随机延迟、水接触方向、以及水岩浆反应（黑曜石/圆石/玄武岩）。方块侧的自然触发仍属 F-5，本任务把反应规则实现为可直接调用的函数并单测。

### 范围

1. `FluidTags.WATER` / `FluidTags.LAVA` 作为独立于 `isSame` 的判定面（`FluidTags.java:8-9`），`canBeReplacedWith` 改用标签。
2. 参数：水 5/1/4、岩浆 30/2/2（F-3 已落地，本任务补断言并核对 `FAST_LAVA` 的 10/1/4 接入点）。
3. 源转换开关：世界级 `waterSourceConversion` / `lavaSourceConversion`，默认开/关。
4. `getSpreadDelay`：岩浆在「新旧态都非空、都不 falling、液面抬升、`nextInt(4) != 0`」时 delay ×4（`LavaFluid.java:179-191`）。
5. `shouldSpreadLiquid`：岩浆源遇水成黑曜石、非源成圆石；下方灵魂土且邻接蓝冰成玄武岩（`LiquidBlock.java:213-235`）。
6. 注册 `cobblestone`、`basalt`、`soul_soil`、`blue_ice` 供上述反应使用。

### 源码依据

`WaterFluid.java`、`LavaFluid.java:144-191`、`LiquidBlock.java:149-235`、`FluidTags.java:8-9`、`BlockTags`/`Blocks.java` 中 `SOUL_SOIL`、`BLUE_ICE`、`BASALT` 的注册。

### 验收判据

1. 水与岩浆的 `dropOff` / `slopeFindDistance` / `tickDelay` 分别为 1/4/5 与 2/2/30。
2. 水 `canConvertToSource` 默认真、岩浆默认假；世界开关关闭后水不再转源。
3. 岩浆液面抬升且在随机条件成立时延迟为 120，否则 30；同一用例能观察到两种取值。
4. 水 `canBeReplacedWith` 只在向下且对方不在水标签时成立；岩浆只在液面高度 ≥ 0.44444445 且对方在水标签时成立。
5. 岩浆源贴水 → 黑曜石；岩浆流动贴水 → 圆石；下方灵魂土且邻接蓝冰 → 玄武岩；三者都写回方块并返回「不再扩散」。
6. 构建通过、CTest 全绿（含新增 `fluid_params` 组）。

### 计划改动

- 修改：`include/redstone/fluid.hpp`、`src/fluid.cpp`（流体标签）
- 修改：`include/redstone/fluid_flow.hpp`、`src/fluid_flow.cpp`（标签判定、源转换开关、spread delay、shouldSpreadLiquid）
- 修改：`include/redstone/block.hpp`、`src/blocks.cpp`（新增 4 个方块）
- 修改：`include/redstone/engine.hpp`（世界开关）
- 新增：`tests/fluid_params_tests.cpp`
- 修改：`CMakeLists.txt`、`docs/compatibility.md`

### 实际做了什么

（待填）

### 构建与测试

（待填）

### 提交

（待填）

任务进行中
