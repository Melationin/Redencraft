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

**流体标签。** `fluid.hpp` 增加 `FluidTag` 与 `fluidHasTag`：`WATER` 含 `water`/`flowing_water`，`LAVA` 含 `lava`/`flowing_lava`，`empty` 不属于任何标签。`canBeReplacedWith` 的两条分支改用标签而不是 `isSame`——水要求 `!other` 在 WATER 标签，岩浆要求 `other` 在 WATER 标签。

**源转换开关。** `World` 增加 `waterSourceConversion`（默认 true）与 `lavaSourceConversion`（默认 false），`fluidNewLiquid` 改走 `fluidCanConvertToSource(world, self)`。

**岩浆随机延迟。** `computeSpreadDelay` 对应 `FlowingFluid.getSpreadDelay` + `LavaFluid` 覆写：仅岩浆、新旧态都非空且都不 falling、液面抬升、且 `random.nextInt(4) != 0` 时 `delay *= 4`。`fluidTick` 重新排刻时用它；对外暴露 `fluidSpreadDelay` 供测试与 F-5 共用。

**水岩浆反应。** `liquidShouldSpreadLiquid` 对应 `LiquidBlock.shouldSpreadLiquid`：岩浆贴水时源变黑曜石、流动变圆石；下方灵魂土且邻接蓝冰时变玄武岩；两者都写回并返回 false（调用方据此停止扩散）。fizz 音效留 TODO。

**新增方块。** 注册 `cobblestone`、`basalt`、`soul_soil`、`blue_ice`（追加在 `Blocks` 命名空间末尾，不移动既有注册顺序）。

**新增 `fluid_params` 测试组**（7 个场景）：参数常量、标签归属、源转换开关（水开/关、岩浆默认关再打开）、替换规则的延迟基线、岩浆随机延迟（同一条随机流里两种取值都出现；falling 与液面下降恒为 30；水不走该分支）、岩浆遇水（源→黑曜石、流动→圆石、无水保持岩浆）、灵魂土 + 蓝冰 → 玄武岩（缺灵魂土则不转换）。

**明确没有做的事**：
- 方块侧触发（`onPlace`/`neighborChanged` 调 `shouldSpreadLiquid` 与排刻）仍属 F-5；本任务把规则实现为可直接调用的函数并单测。
- `FAST_LAVA` 的 10/1/4 未接入维度环境属性——项目还没有维度，接口点记在这里，等维度落地。
- fizz 音效、粒子（levelEvent 1501）；`waterSourceConversion` 未接 gamerule 持久化（存档系统整体未做）。
- 没有跑真实 26.1 服务端差分。

### 构建与测试

`powershell
cmake -S . -B build-jemalloc -DCMAKE_PREFIX_PATH="$PWD/third_party/vcpkg-installed/x64-windows-static"
cmake --build build-jemalloc --config Release -j 8
ctest --test-dir build-jemalloc -C Release --output-on-failure
`

结果：构建通过；**CTest 19/19 通过**（原 18 组 + 新增 `fluid_params`）。

**变异验证**：

| 变异 | 结果 |
|---|---|
| 去掉岩浆的 `nextInt(4)` 乘 4 | `some rolls quadruple the delay` |
| 岩浆遇水一律变圆石 | `the lava source becomes obsidian` |
| 忽略源转换开关 | `the gap stops converting with the rule off` |

三次都被拒绝；恢复后 `git diff` 只含本次改动。

证据口径：**源码对照 + C++ 回归测试**，未跑真实 26.1 服务端差分。

### 提交

（待填）

任务已完成
