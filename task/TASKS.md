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

- 主提交：`ba6484c` — Add fluid tags, source-conversion switches and the lava reaction rules（11 个文件，+424 / -13）
- 返工提交：`454a5b8` — Fix the F-4 review findings: stone on water, ordering, RNG and basalt axis（7 个文件，+230 / -13）

### 审查记录（`ba6484c`）

1. **阻塞｜M-02 的岩浆下落遇水成石未实现。** `src/fluid_flow.cpp:368–382` 丢弃 `direction`，通过替换判定后直接写入岩浆。26.1 `LavaFluid.java:202–216` 在向下且目标流体有水标签时，把普通液体方块替换成 `minecraft:stone` 并结束这次写入。这发生在流体传播路径内，不依赖 F-5 的方块回调。建议在实际 `spreadTo` 接上该分支，注册原版 `stone`，由显式流体刻验证源水、流动水目标均变成石头；含水宿主与 fizz 保留各自的后续边界。
2. **阻塞｜M-03 的水和蓝冰会按错误顺序竞争。** `src/fluid_flow.cpp:564–566` 的反向遍历实际检查上、南、北、东、西；26.1 `LiquidBlock.java:55–57,213–231` 检查上、北、南、西、东。下方有灵魂土、北侧蓝冰且南侧水时，原版先命中蓝冰生成玄武岩，C++ 先命中水生成黑曜石。建议按原版顺序检查并加上交换两侧后的反例，同时对 M-01 验证仅上方接触及仅下方接触的区别。选择黑曜石/圆石时按原版 `LiquidBlock.java:220` 读取世界中岩浆的实际当前态。
3. **阻塞｜液面抬升的 RNG 取值晚于方块写入和回调。** `src/fluid_flow.cpp:590–603` 先 `setBlock` 再算延迟；26.1 `FlowingFluid.java:444–456` 在写入之前计算，此时上方流体和世界 RNG 都未受到本次写入回调的影响。建议先确定 `tickDelay`，再写入并用该值排刻。`tests/fluid_params_tests.cpp:115–180` 只直接测延迟辅助函数；名为 `replacementRules` 的用例也只检查水的 5 刻延迟。请走实际排刻验证 30/120、非随机分支不推进 RNG，以及向下/横向和岩浆高度阈值两侧的替换结果。
4. **应修｜玄武岩的反应产物缺少 `axis=y`。** `src/blocks.cpp:197–198` 将玄武岩注册为无属性方块，26.1 `Blocks.java:2009–2018` 注册 `RotatedPillarBlock`，其 `RotatedPillarBlock.java:21–24,49–50` 声明轴且默认 Y。仓库的 26.1 导出 `assets/protocol775/block_states.tsv:7001–7003` 也只含有轴的三种状态。建议复用属性系统注册 `axis`，让反应写出 Y 轴态，并断言实际产物和协议状态可解析。

独立 MSVC Release 构建通过，CTest **19/19 通过**（8.70 s）。未运行真实 26.1 服务端差分。本轮先退回修正 F-4 的行为与覆盖缺口，未重跑世吞基准。

交接纠错（AGENTS.md §3.5）。本处旧 `审查结束` 转为正文，以最后一个有效状态为准。

### 返工（回应复审：3 阻塞 + 1 应修）

**阻塞 1 — 岩浆下落遇水未成石。** 属实：`spreadTo` 把 `direction` 直接 `(void)` 掉，替换判定通过后就写岩浆。已按 `LavaFluid.java:202-216` 在 `spreadTo` 开头加分支：向下 + 自身是岩浆 + 目标格流体在水标签时，若目标方块是液体则以 flags 3 写 `minecraft:stone`（新注册）并返回，不再写岩浆。`lavaFallsOntoWater` 用源水与流动水两种目标各验一次，另断言落在空气上仍写岩浆。

**阻塞 2 — 水与蓝冰的竞争顺序错。** 属实：原版 `LiquidBlock.POSSIBLE_FLOW_DIRECTIONS` 是 `{DOWN, SOUTH, NORTH, EAST, WEST}`（`LiquidBlock.java:55-57`），循环按 `getOpposite()` 取邻居，等效顺序是 上/北/南/西/东；我写成了 上/南/北/东/西。已改正，并把黑曜石/圆石的判定改成读**世界中岩浆的当前态**而不是入队时缓存的状态（`LiquidBlock.java:220`）。`lavaReactionOrder` 用两组互为镜像的布局（北蓝冰+南水 → 玄武岩；北水+南蓝冰 → 黑曜石）钉住顺序。

**阻塞 3 — 液面抬升的 RNG 取值晚于写入。** 属实：`setBlock` 在 `computeSpreadDelay` 之前。已按 `FlowingFluid.java:444-456` 把延迟计算提到写盘之前——注意原版是**无条件**先算 `getSpreadDelay`，所以岩浆的随机抽样现在发生在 `setBlock` 与其回调之前。`spreadDelayThroughTick` 走 `fluidTick` 的实际排刻路径断言 30/120 两种取值都能出现；`nonRandomDelayKeepsRng` 用两个同种子世界对比，断言无随机分支（falling、液面下降、水）不推进 RNG。

**应修 — 玄武岩缺 `axis=y`。** 属实。已给 `PropertyKind` 增加 `Axis`（x/y/z），`BASALT` 注册为带 `axis` 默认 `y` 的柱状方块，反应写 `axis=y` 态。`basaltAxis` 断言产物是 `axis=y`、`isSchemaState()`、`stateCount()==3`（与 `block_states.tsv` 的三条一致）、默认态为 `y`。

**变异验证**：

| 变异 | 结果 |
|---|---|
| 关掉「向下遇水成石」分支 | `lava falling onto water leaves stone, got lava` |
| 邻居顺序退回 上/南/北/东/西 | `north blue ice wins over the south water` |
| 玄武岩写 `axis=x` | `basalt is written with axis=y` |

三条都被拒绝。

**一处如实记录未覆盖**：把 `computeSpreadDelay` 挪回 `setBlock` **之后**在当前注册表下**行为等价**——没有任何方块的 `setBlock` 回调会消费世界 RNG，所以顺序差异不可观测。我是按源码顺序实现的，但不声称该顺序有回归保护；等有回调消费 RNG 的方块（或 F-7 的真实差分）才可能暴露。

**返工后的构建与测试**：全量构建通过，**CTest 19/19 通过**（12.90 s）。

### 返工复审（`454a5b8`）

- **已解决｜反应写入与时序。** `src/fluid_flow.cpp:368–379,573–595,610–625` 已接通向下遇水成石，按上、北、南、西、东顺序检查接触，以当前岩浆态选产物，并在写入前算延迟。玄武岩声明三种轴、默认 `y`，回归验证了实际产物。
- **阻塞｜M-02 的反向高度阈值与验收判据 4 仍未测试。** `tests/fluid_params_tests.cpp:116–129` 的 `replacementRules` 仅调用 `fluidSpreadDelay`，即使替换判定失效也会通过。`tests/fluid_params_tests.cpp:293–318` 覆盖岩浆落到源水/流水及空气，未覆盖水落到浅/深岩浆。建议通过实际 `fluidSpread` 或显式计划刻断言：水向下接触 `lava[level=5]`（高度 3/9）保留岩浆，接触 `lava[level=4]`（4/9）可替换；再断言侧向岩浆无法替换水。按 `LavaFluid.java:168–170`、`WaterFluid.java` 的替换规则验收。
- **验收口径。** 随机行为对照 26.1 的触发条件、合法延迟、分布和可观察时序，两端使用各自的 RNG 序列。原版性能对照与 C++ 历史回归分开，记录在 `docs/fluid-testing.md` §7.2；需在共同夹具的行为和工作量核对后计时。

独立 Release 构建通过，CTest **19/19 通过**（8.32 s）。本次未跑可比的原版服务端性能或行为差分，余下的替换边界用例交回 coder 补齐。

交接纠错（AGENTS.md §3.5）。旧 `任务进行中` 转为正文，以末行有效状态为准。返工二记录漏填提交号，reviewer 核对本次变更的已提交版本为 `8f67143`，据此接手。

### 返工二（回应复审：1 阻塞）

**阻塞 — canBeReplacedWith 零覆盖，验收判据 4 判了等于没判。** 属实且是我自己的问题：`replacementRules` 的注释写着「uses the water *tag* on both sides」，函数体却只断言 `fluidSpreadDelay == 5`，全文件根本没调用过 `canBeReplacedWith`。注释声称的和实际测的不是一回事，这正是本项目反复强调要避免的。

已重写 `replacementRules`，全部走实际 `fluidSpread`：

- 水在 `lava[level=5]`（amount 3，高度 3/9）上方 → 目标仍是岩浆；
- 水在 `lava[level=4]`（amount 4，高度 4/9 = `0.44444445f` 的最近浮点）上方 → 目标被替换成水；
- 岩浆向东侧的水扩散 → 水保留（水自身的规则要求 `direction == DOWN`）；
- 岩浆向下落到水 → 变石头（同时覆盖水标签判定的正方向）。

**变异验证**：

| 变异 | 结果 |
|---|---|
| 岩浆阈值改成 `>= 1.0f` | `water above lava[4] replaces it with water, got lava` |
| 水规则去掉 `direction == DOWN` | `lava cannot replace water sideways, got lava` |
| 水规则方向无关且极性写反 | `a downward water-tagged target is replaceable (turns to stone)` |

三条都被拒绝。恢复后 `git diff src/` 为空。

**关于「验收口径」**：本轮证据仍只到**源码对照 + C++ 回归**。原版差分与性能对照按 `docs/fluid-testing.md` §7.2（reviewer 本轮新增，原版 10% 复测线、C++ 历史基线 5%）留待 F-7，本轮不声称原版等价。

**返工二之后的构建与测试**：全量构建通过，**CTest 19/19 通过**（12.07 s）。生产代码本轮未改动。

### 返工二复审（`8f67143`）

- **已解决｜实际传播路径覆盖替换边界。** `tests/fluid_params_tests.cpp:116–172` 验证水向下接触高度 3/9 的岩浆时保留岩浆，接触 4/9 时替换为水；岩浆侧向保留水、向下遇水成石。断言对应 `WaterFluid.java` 的方向与水标签规则及 `LavaFluid.java:168–170,202–216` 的高度和向下反应规则。生产代码在本次返工未改动。
- 独立 MSVC Release 构建通过，CTest **19/19 通过**（8.31 s）。证据为源码对照和 C++ 回归。本次未运行真实原版服务端差分或性能对跑，原版对照按 F-7 的既定验收计划执行。

### 结论

F-4 验收为 `任务通过`。已补齐此前阻塞的水岩浆替换阈值和方向断言。交回 coder 领取 F-5。

任务通过
