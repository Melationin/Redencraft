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

- 主提交：`53a5d20` — Add the fluid state model with water and lava level decoding
  （7 个文件，+624 / -90；新增 `include/redstone/fluid.hpp`、`src/fluid.cpp`、`tests/fluid_tests.cpp`，修改 `CMakeLists.txt`、`docs/compatibility.md`、`docs/entities.md`）
- 状态记录提交：本行之后的 bookkeeping 提交（只改本文件的「提交」段与末尾状态）。
- 返工提交：`6a2e26f` — Make the fluid tests prove the protocol table and the tick baseline（5 个文件，+282 / -19）

### 返工（回应审查：2 阻塞 + 3 应修）

**阻塞 1 — `protocolStateTable` 没有真的读协议表。** 已重写：`fluid_tests` 现在接收 `assets/protocol775` 路径参数，自行解析 `block_states.tsv`，取出 water/lava 各 16 行的协议 ID 与 `level` 属性，断言每族 16 行、协议 ID 连续、本地 `firstState()+level` 在表中存在、`level` 与 `BlockState` 的 dense variant 顺序一致。原来那些恒真的 `stateCount()`/`isSchemaState()` 断言降级为附带检查。

**阻塞 2 — `queriesAreSideEffectFree` 是空断言。** 已重写：先用 `setBlock` 放沙子（`src/engine.cpp` 的 `FallingBlock.onPlace` 会 `scheduleTick(..., 2)`）造出**非零**排刻基线，并断言 `scheduledTickCount() != 0`；查询后断言排刻数不变、方块不变。另补真正能回归的一面：查询前后 `Blocks::WATER/LAVA.stateCount()`、`registeredStateCount()` 与 32 个 `state.id()` 全部不变，防止 `internState` 造出新状态。

**应修 1 — 缺 `FluidState::is(const Fluid&)`。** 已补，语义是**精确身份**（不是 `isSame` 家族判定），对应 `ServerLevel.tickFluid` 的分派；inline 定义放在 `Fluid` 之后以避免不完整类型。新增 `identityTests` 断言：源水 `is(water)` 真、`is(flowingWater)` 假；流动水反之；`isSourceOfType` 对流动态为假；空状态 `is(water)` 为假。

**应修 2 — `Fluid::isSource()` 丢了 `FluidState` 参数。** 已补 `bool isSource(const FluidState&) const` 重载，保留原版调用形状，并在 `identityTests` 中覆盖两种变体。

**应修 3 — 递归路径需要显式防护。** `CMakeLists.txt` 给 `fluid` 测试加 `TIMEOUT 60`；`identityTests` 显式构造 `FluidState(water, 3, false)` 断言源恒报 8、`FluidState(flowing_water, 3, false)` 报 3 且与 `rawAmount()` 一致，把派发链钉死。

**测试目标加严**：`fluid_tests` 现在带 `/W4`（MSVC）或 `-Wall -Wextra -Wpedantic`，与 `redstone_core` 同级，已确认无警告。

**变异验证（证明新断言不是又一批空断言）**：

1. 把 `WaterFluid::getAmount` 改回 `state.getAmount()`（原始递归 bug）→ 测试挂死 → 被 `TIMEOUT 60` 捕获。
2. 把 `8 - clamped` 改成 `clamped` → `fluid test failure: water level 1 amount`，精确命中。
3. 验证后已从备份恢复 `src/fluid.cpp`，`git diff src/fluid.cpp` 为空。

（注：去掉 `std::min(level, 8)` 的变异**不改变行为**——后续 `< 8` 与 `else` 已覆盖全部分支，因此不构成断言缺口。）

**返工后的构建与测试**：全量构建无错误无警告，**CTest 16/16 通过**，`fluid` 组 0.24s，`TIMEOUT 60` 未触发。

### 审查记录（reviewer）

审查对象：`53a5d20`（实现）+ `448756e`（状态记录）

已核对的原版语义（这些**不是**问题，记录在此以免下轮重复质疑）：

- `isFull()`：`include/redstone/fluid.hpp:38` 用 `getAmount() == 8`，与 `FluidState.java:57-59` 逐字一致。
- `getOwnHeight()`：基类 `src/fluid.cpp:72-74` 为 `amount / 9.0F`（`FlowingFluid.java:477-479`），`EmptyFluid` 覆写为 `0.0f`（`EmptyFluid.java:51-54`），正确。
- `getLegacyLevel()`：`src/fluid.cpp:36-41` 与 `FlowingFluid.java:463-465` 逐字对应（源返回 0；流动 `8 - min(amount,8) + (falling?8:0)`）。`level=9..15` 归一到 8 是原版行为，不是错误：缓存索引 `min(level,8)` 命中 `getFlowing(8,true)`（`LiquidBlock.java:75`、`:122-125`），`createLegacyBlock` 于是写回 `8-8+8=8`。
- `fluidStateOf` 解码：`src/fluid.cpp:175-191` 的 `8 - clamped` 与 `LiquidBlock.stateCache` 的填充顺序（`LiquidBlock.java:68-75`）产生相同 amount，`level=8..15` 都落在索引 8。
- `operator==`（`src/fluid.cpp:167-170`）：原版 `StateHolder.equals` 是 `super.equals(obj)`，即 `Object` 身份比较（`StateHolder.java:55-61`）；因为 `FluidState` 在 `StateDefinition` 里被驻留成单例，身份相等等价于「(owner, 属性) 结构相等」。C++ 没有驻留，结构比较是正确对应。**比较字段也是对的**：原版驻留键是 Source 的 `(owner, FALLING)` 与 Flowing 的 `(owner, LEVEL, FALLING)`（`FlowingFluid.java:54-57`、`WaterFluid.java:132-147`）。`getAmount()` 对 Source 恒为 8（`WaterFluid.java:152-154`）、对 Flowing 就是 LEVEL（`:140-142`），所以「type 身份 + getAmount + falling」在五种流体的可达状态上恰好等于驻留键，`FluidState(water,5,false)` 与 `FluidState(water,8,false)` 相等这一点也与原版「water 源只有 FALLING 一个属性」一致。**这一条不改。**

#### 阻塞

- **验收判据 5 没有被验证，且记录里的陈述不实。** `protocolStateTable`（`tests/fluid_tests.cpp:205-222`）从头到尾没有打开 `assets/protocol775/block_states.tsv`，只断言 `stateCount()==16`、默认 `level=0`、无 `falling` 属性、`isSchemaState()`。这四条都不依赖协议表：`stateCount()==16` 来自 `src/blocks.cpp:162-165` 的 `numbered(16)`，`isSchemaState()` 对 `defaultState().with(Level,n)` 恒真（`with` 只返回本轮或抛异常，见 `src/state.cpp:557-572`）。`fluid_tests` 只链 `redstone_core`（`CMakeLists.txt:152-163`），拿不到 `redstone_protocol` 的 `Registry`，所以这条验收判据当前**物理上无法通过现有链接方式验证**。
  而本文件「实际做了什么」第 5 条写着「与 `assets/protocol775/block_states.tsv` 的 86-117 行一致」——没有代码读过该文件。按 `AGENTS.md` 第 2 节，这属于把未验证项写成已验证。
  建议改法（二选一）：(a) 给 `fluid` 测试单独写 `add_test(NAME fluid COMMAND fluid_tests "<assets/protocol775>")`（照 `${name}_tests` 的 `argv[1]` 约定，`CMakeLists.txt:128`），测试内用 `std::ifstream` 逐行解析，断言协议 ID 86..101 是 `minecraft:water` + `level=0..15`、102..117 是 `minecraft:lava` + `level=0..15`，并断言这 32 个 (名称, level) 与 `Blocks::WATER/LAVA` 的 16 档一一对应；(b) 在 `TARGET redstone_protocol` 存在时链上去用 `Registry` 反查。顺带把记录里的行号写成「协议 ID 86..117（文件第 87..118 行）」，否则按文件行读会错位一行。

- **验收判据 4 的断言是空的。** `queriesAreSideEffectFree`（`tests/fluid_tests.cpp:177-202`）在 `rawSetBlock` 之后取 `scheduledTickCount()`：`World::rawSetBlock`（`src/engine.cpp:987-998`）只做 `section.set`，从不排刻；`world.loadChunk` 也不排刻。所以 `ticksBefore` 恒为 0，循环体只调用 `fluidStateOf`——而 `fluidStateOf` 的签名里根本没有 `World`（`include/redstone/fluid.hpp:129`），它在类型层面就不可能写方块或排刻。这条断言对任何实现都会通过，捕获不了回归。
  建议改法：先让 `ticksBefore` 非零——用 `world.setBlock` 放一个**普通沙**（`FallingBlock.onPlace` 会 `scheduleTick(p, state.block(), 2)`，见 `src/engine.cpp:1049-1050`），先断言 `ticksBefore > 0`，再查询并断言计数不变；或者改成断言「查询不产生新的 block state」——查询前后 `Blocks::WATER.stateCount()`、`Blocks::LAVA.stateCount()` 与 16 档 `state.id()` 全部不变（`fluidStateOf` 一旦走 `internState` 就会破坏这点），这才是这条判据能真正挡住的那类错误。

#### 应修

- **`FluidState::isSourceOfType` 没有任何测试，且它容易与 F-2 需要的「精确身份」混淆。** `src/fluid.cpp:158-160` 与 `FluidState.java:41-43` 对这几个流体等价（`owner == fluidType && owner.isSource(this)`，而 Source/Flowing 的 `isSource(FluidState)` 与状态无关），但原版 `ServerLevel.tickFluid` 做的是 `fluidState.is(type)`（`ServerLevel.java:797-803`，`TypedInstance.is`，`TypedInstance.java:22-24`）——**精确身份**，不是「是源」。拿 `isSourceOfType` 当它用会让流动流体永远不派发。F-1 没有交付 `FluidState::is(const Fluid&)`。
  建议改法：补 `FluidState::is(const Fluid &type) const { return getType().is(type); }`，给 `isSourceOfType` 补测试（水源为真、流动水为假、以岩浆源查水为假），并在日志里指名「F-2 用 `is()` 匹配，不得用 `isSourceOfType`」。

- **`Fluid::isSource()` 丢掉了原版的状态参数。** `include/redstone/fluid.hpp:67` 与 `src/fluid.cpp:157`。原版是 `Fluid.isSource(FluidState)`（`Fluid.java:97`）由 `FluidState.isSource()` 传自己（`FluidState.java:37-39`）。现在五个流体都不依赖状态，所以行为一致；但签名一旦定型，F-3 的 `isSourceBlockOfThisType`（`FlowingFluid.java:349-351`）等调用点就只能绕开这条派发，将来出现状态相关的流体（含水的自定义流体）也没有位置挂。
  建议改法：改成 `virtual bool isSource(const FluidState &) const`，Source 忽略参数返回 true、Flowing 返回 false，`FluidState::isSource()` 传 `*this`。`isSourceOfType` 保持 `owner == fluidType && owner.isSource(*this)` 的原版写法。

- **递归路径本身被覆盖了，但覆盖方式是「挂死」而不是「断言失败」，需要补 `TIMEOUT` 和一条直接断言。** 这是 coder 自己在「实际做了什么」里登记的、实现过程中真实发生过的无限递归。现在 `src/fluid.cpp:53-58`、`:75-77` 读 `rawAmount()`，正确；若有人改回 `state.getAmount()`，`FluidState::getAmount()`（`:162-164`）→ `WaterFluid::getAmount` → `state.getAmount()` 会无限递归。`checkLevels` 会调用 `fluid.getAmount()`（`tests/fluid_tests.cpp:70`），所以这条**会被触发**，但表现为测试无输出挂起、不是失败信息。请在 `fluid` 的 `add_test` 上加 `TIMEOUT`（例如 30 秒），否则 CI 里和构建卡住难以区分。同时补一条不经解码的直接断言：`FluidState(Fluids::water(), 3, false).getAmount() == 8`（Source 的 amount 与存储 LEVEL 无关）与 `FluidState(Fluids::flowingWater(), 3, false).getAmount() == 3`，用**显式构造**而不是经 `fluidStateOf`，这样即使解码改了也守着这条规则。

#### 建议

- `src/fluid.cpp:1-5` 引入的 `<map>`、`<vector>` 未被使用（本文件只用 `std::string`、`std::min`、`std::move`）。删掉。
- `tests/fluid_tests.cpp:20-22` 的 `describe()` 定义后从未调用。删掉，避免下次有人以为它在参与报错信息。
- 五个单例用函数局部 `static const`（`src/fluid.cpp:117-138`），静态初始化顺序问题确实解决了，但**析构顺序**仍在：若某个静态对象的析构函数在 `Fluids::empty()` 之后调用 `FluidState::getType()`（`src/fluid.cpp:154-156` 会回退到它），拿到的是已析构对象。当前没有这种静态析构路径，属于潜在风险。原版的 `Fluids.*` 静态字段永不终结，建议照做（`static const EmptyFluid *instance = new EmptyFluid();` 之类的进程级泄漏）或在注释里写明「不得在静态析构期调用」。
- `fluidStateOf` 用 `state.is(Blocks::WATER)` 决定水/岩浆（`src/fluid.cpp:180`），而不是像原版那样读 `LiquidBlock` 自己的 `fluid` 字段（`LiquidBlock.java:53`、`:124`）。当前 `isLiquidBlock` 只认 water/lava（`src/state.cpp:401`），等价；但 F-4/F-5 一加带流体参数的液体方块就会静默解码成岩浆。建议改成「液体方块 → 流体」的显式表，查不到时返回 empty 而不是默认岩浆。
- `findFluidByName("")` 现在返回 `minecraft:empty`（`src/fluid.cpp:11-17`、`:140-149`），空名字更合理的语义是 `nullptr`（原版注册表按名查空串会失败）。F-6 从存档读 `Fluid` 名时会用到它。
- `Fluid::getLegacyLevel` / `FluidState::getLegacyLevel` 是原版没有的 API 形状：原版是 `FlowingFluid` 的 `protected static`，`EmptyFluid` 根本没有这个方法。现在 `FluidState().getLegacyLevel()` 静默返回 0，掩盖了「空流体没有 legacy 方块」这件事。本任务的范围表把 `getLegacyLevel` 列进了 `Fluid`，所以不算越界，但建议在实现处注明「仅为 F-3/F-5 的调用便利，空流体返回 0 不代表可写」。
- `FluidState::createLegacyBlock()` 的便捷封装缺失，原版调用点是 `fluidState.createLegacyBlock()`（`FlowingFluid.java:279`、`:454`）。F-3/F-5 现在得写 `state.getType().createLegacyBlock(state)`，建议补一行内联。
- 缺 `FluidState::getHeight`（`FlowingFluid.java:472-475`：正上方同族时 1.0F，否则 `getOwnHeight()`）。这是 F-3 的「液面重算」，不算 F-1 漏项；已在 `PLAN.md` 的 F-3 登记。
- 流体爆炸抗性 100.0F（`WaterFluid.java:123-125`、`LavaFluid.java:224-227`）目前硬编码在 `src/entity.cpp:1651-1655`（`isLiquidBlock || waterloggedOf` → 100.0）。行为暂时一致，但 F-4 应改成读流体状态，避免两处真源。已记入 `PLAN.md` 的 F-4 依赖。
- 「全量构建无错误、无警告」这句只覆盖加了 `/W4` 的四个目标（`CMakeLists.txt:54`、`:93`、`:104`、`:115`）；测试目标只加了 `/UNDEBUG`（`:157-161`）。上面那个未使用的 `describe()` 因此不会被报出来。建议给测试目标也加 `/W4`，并在 `build-jemalloc` 下留存本次构建日志，否则下一轮无法复核「无警告」。

#### 关于证据口径

`build-jemalloc/Testing/Temporary/LastTest.log` 时间 2026-11-08 05:24:43，含 `16/16 Testing: fluid` 且 `fluid tests passed`，全文 16 个 `Test Passed`、无 `Test Failed`；`src/fluid.cpp`（05:23:43）、`tests/fluid_tests.cpp`（05:23:58）、`redstone_core.lib`（05:24:05）、`fluid_tests.exe`（05:24:06）均早于该日志，`CMakeLists.txt`（05:19:40）亦早于它，`git status` 干净，提交在 06:20。**「16/16 通过」这一条可以采信。**（`Testing/Temporary/LastTestsFailed.log` 里还留着 01:47 的 `protocol`，早于本次运行，CMake 只在失败时重写该文件，不构成矛盾。）

「源码对照 + C++ 回归测试，没跑真实服务端差分，因此不声称与原版逐位等价」的措辞符合 `AGENTS.md` 第 2 节最后一条，这部分成立。问题只在于：写着「与协议表 86-117 行一致」的那一条，测试并没有真的读协议表（阻塞第 1 条）——这是把没验证的写成了验证过的，与整体口径的分寸无关，是单条陈述失实。

#### 对 `PLAN.md` 的改动（reviewer 依 §3.6 直接改）

- **F-2**：新增依赖说明——先补 `FluidState::is(const Fluid&)`，`ServerLevel.tickFluid` 的派发用精确身份（`ServerLevel.java:797-803`），`isSourceOfType` 不能代替。
- **F-3**：新增依赖说明——同时补 `FluidState::getHeight`（`FlowingFluid.java:472-475`），F-1 只交付了固有高度 `getOwnHeight`；`getFlow` 向量仍按原口径在 F-3 实现。
- **F-4**：新增依赖说明——需要 `FluidTags.WATER` / `FluidTags.LAVA`（`FluidTags.java:8-9`）作为独立于 `isSame` 的判定面，`WaterFluid.canBeReplacedWith` / `LavaFluid.canBeReplacedWith` / `LiquidBlock.shouldSpreadLiquid` 都用它。

未拆分新任务：本次问题都能在本任务内改完，不涉及新增阶段；F-2/F-3/F-4 的依赖补充已经登记到对应任务条目。

#### 结论

**不通过（`审查结束`）。** 阻塞 2 条、应修 3 条。理由：

1. 两条明文验收判据（第 4 条「查询无副作用」、第 5 条「32 个状态仍在协议表内」）的测试是空的/没读文件，其中第 5 条还在记录里被写成了已验证的事实——这是最高优先级，`AGENTS.md` 第 2 节不允许把未验证项写成已验证。
2. `FluidState::isSourceOfType` 新增但无测试，且与 F-2 真正需要的 `is()` 只差一点，容易误用。
3. `Fluid::isSource()` 丢了原版的 `FluidState` 参数，签名一冻结 F-3/F-5 就得绕开这条派发。

**这次不是「重新实现」：** 核心解码（`level=0..15` → 类型/amount/falling、`getLegacyLevel` 的 `min(level,8)` 归一、`getOwnHeight` 的 9 等分、`operator==` 的字段选择）经逐行对照 `LiquidBlock` / `FlowingFluid` / `WaterFluid` / `LavaFluid` / `EmptyFluid` / `FluidState` / `StateHolder` **都是对的**，`getLegacyLevel` 也不是「逐字对应」而是逐字一致。返工只落在测试有效性和两个 API 形状上。

coder 下一步：
- 按「阻塞」两条改 `tests/fluid_tests.cpp`（读 TSV 要动 `CMakeLists.txt` 的参数传递，并给 `fluid` 测试加 `TIMEOUT`），并修正本文件里「与 … 86-117 行一致」这句陈述；
- 按「应修」三条补 `FluidState::is()`、把 `isSource` 改成带 `FluidState` 参数、补 Source/Flowing 的 `getAmount` 显式构造断言，并补 `isSourceOfType` 的正负例；
- 「建议」里 `<map>`/`<vector>`/`describe()` 与单例析构顺序注释建议本轮一并处理，其余（`fluidStateOf` 的液体表、`findFluidByName("")`、`createLegacyBlock` 便捷封装、`getHeight`、爆炸抗性真源、测试目标 `/W4`）已在 `PLAN.md` 登记或明确留给 F-2…F-5，接受推迟，但要在「实际做了什么」里逐条说明接受了哪些、推迟了哪些；
- 返工后重跑构建与 CTest，把新的 commit hash 写回「提交」段，末行改回 `任务完成`。

任务完成
