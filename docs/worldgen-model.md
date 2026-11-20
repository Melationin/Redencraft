# 世界生成的数学模型与查找/分配清单

日期：2026-11-19。配套：[worldgen-perf-analysis.md](worldgen-perf-analysis.md)。
数据来源：`assets/worldgen26_1`（overworld）、源码对照、perf 采样与本机基准。

## 1. 数据流

`noise_router`（15 个通道）→ `NoiseChunk` 按 `marker` 决定采样粒度 →
逐方块密度 → 含水层/矿脉 → 地表规则 → 结构/地物。

- 顶层 `final_density` 被包成 `CacheAllInCell`：**每个方块**求值一次（16×16×384 = 98,304/区块）。
- `interpolated` 标记按 4×4×8 的 cell 角点采样：`(16/4+1)² × (384/8+1) = 5×5×49 = 1225` 个角点/标记/区块，cell 内三线性插值。

## 2. 密度场（overworld final_density）

JSON 图统计：**212 个节点、深度 34、19 个 noise 节点、5 个 interpolated 标记**，
若完全展开约 **52 个噪声 octave 求值/样本**。节点构成（前几类）：
constant 44、add 36、mul 26、noise 19、cache_once 8、range_choice 7、
y_clamped_gradient 6、flat_cache 6、min 5、interpolated 5、cache_2d 5、max 4、
abs 4、clamp 4、blend_alpha 4、weird_scaled_sampler 3、spline 3、cube 2、
squeeze 1、blend_density 1、old_blended_noise 1、quarter_negative 1、shifted_noise 1、
shift_b 1、shift_a 1、half_negative 1、blend_offset 1、square 1。

算子定义（`src/worldgen/density_function.cpp`）：

- `mapped`：abs / square / cube / half_negative(`x>0?x:x/2`) /
  quarter_negative(`x>0?x:x/4`) / invert(`1/x`) /
  squeeze(`clamp(x,-1,1)/2 - (clamp(x,-1,1))³/24`)。
- `clamp`、`add`、`mul`（短路 0）、`min/max`（用子树 min/max 边界短路）。
- `y_clamped_gradient`：`t=(y-fromY)/(toY-fromY)`，`clamp(t)` 后线性插值。
- `range_choice`：`v∈[min,max) ? in : out`（递归版只算选中分支）。
- `noise`：`H(x·xz, y·yScale, z·xz)`；`shifted_noise`：`H(x·xz+sx, y·yScale+sy, z·xz+sz)`。
- `shift_a/b/shift`：`4·H(x/4,0,z/4)` 等；`weird_scaled_sampler`：`rarity(v)·|H(x/r,y/r,z/r)|`。
- `spline`：三次 Hermite 分段（float 求值）；`old_blended_noise`：见下。
- `blend_alpha=1`、`blend_offset=0`、`beardifier=0`（当前实现）。
- `marker`：`interpolated` / `flat_cache` / `cache_2d` / `cache_once` /
  `cache_all_in_cell`；缓存只影响求值次数，不改变值。

## 3. 噪声数学

**NormalNoise**（`src/worldgen/noise.cpp`）：

`N(x,y,z) = (P₁(x,y,z) + P₂(k·x, k·y, k·z)) · v`，
`k = 1.0181268882175227`，`v = 0.16666666666666666 / expectedDeviation(octaves)`。

**PerlinNoise**：按 octave 求和

`P(x) = Σ_i a_i · ImprovedNoise(2^{o_i}·x) · f_i`

octave slot 保存 `inputFactor`、`amplitude`、`valueFactor`（低频/高频归一）。

**ImprovedNoise**：经典 Perlin + 置换表

`n(x,y,z) = lerp3(sx, sy, sz, d000, d100, d010, d110, d001, d101, d011, d111)`

其中 `d_c = g(hash(c)) · (f - c)`，`g` 取 16 个梯度之一，
`s(t) = t³(t(6t-15)+10)` 为 smoothstep，`hash` 来自 256 项置换表；
`yScale/yFudge` 实现跨 section 的 y 方向"抹平"：
`y' = y - floor(min(yFudge, frac(y))/yScale)·yScale`。

**BlendedNoise**（旧地形）：main 8 octaves，min/max 各 16 octaves，
`factor=(main/10+1)/2`，按 factor 在 min/max 之间混合，缩放 `684.412·xzScale/yScale`。

**每区块噪声预算**（由 JSON 图统计）：

| 插值标记 | octave/角点 | 角点/区块 | octave/区块 |
|---|---|---|---|
| final_density 的 4 个 noodle | 1 each | 1225 | 4,900 |
| final_density 的大合成（洞穴/地形） | 48 | 1225 | **58,800** |
| vein_toggle | 1 | 1225 | 1,225 |
| vein_ridged ×2 | 1 each | 1225 | 2,450 |
| 合计（不含含水层/地表） | | | **≈ 67,375** |

另有 `final_density` 外层非插值部分按方块求值（98,304/区块）与含水层逐方块噪声。

## 4. Cell 插值

cell 尺寸 4×4×8（`size_horizontal=1, size_vertical=2`）。
对 cell 内偏移 `(fx,fy,fz)∈[0,1)³`：

`D̂ = lerp3(fx, fy, fz, D000, D100, D010, D110, D001, D101, D011, D111)`

角点值来自该 marker 在 5×5×49 网格上的采样。

## 5. 含水层（NoiseBasedAquifer）

- 网格：x/z 每 16 格、y 每 12 格一个 cell；每 cell 的流体中心随机落在 10×9×10 的偏移范围内。
- 每方块取最近的 4 个 cell 中心，按平方距离 d 排序：
  `similarity(d₁,d₂) = 1 - (d₂ - d₁)/25`。
- 密度 `d > 0` → 固体；否则按最近 cell 的流体，并用 barrier noise 计算压力
  `barrier = s₁₂·pressure(c₁,c₂)`，若 `d + barrier > 0` 则仍为固体。
- 相似度阈值 `similarity(10²,12²)` 决定是否安排流体更新（邻接不同流体）。

## 6. 地表规则 / 生物群系 / 气候

- 地表：每列从 `WORLD_SURFACE_WG` 高度向下到 minY，遇到默认方块时按规则程序选择替换方块；
  条件含 Hole / AbovePreliminarySurface / Biome / VerticalGradient / YAbove / Water /
  Temperature / StoneDepth / NoiseThreshold / Steep。VerticalGradient 用
  `prob = map(y, trueBelow, falseAbove, 1, 0)` 与 `randomFactory.at(x,y,z).nextFloat()` 比较。
- 生物群系：6 维气候参数 `(temperature, humidity, continentalness, erosion, depth, weirdness)`
  量化到 1/10000 的整数，在 R-tree 上做最近邻：距离
  `Σ_i max(0, lo_i - t_i, t_i - hi_i)²`；叶节点给出 biome id。
- biome zoom：用混淆 seed 在 4/8 个相邻 quart 角点中选一个（`pickCornerIndex`）。
- 每区块气候采样 4×4×(384/16)×4 = **1536 次**。

## 7. 矿脉

`vein_toggle` / `vein_ridged` / `vein_gap` 均为 interpolated；
每方块在 `y∈[-60,50]` 且避开 `(-8,0)` 时推进插值器，按 veininess
（`|ridged| - gap` 与 toggle 的分段）选择 ore A/B。

## 8. 可去掉的哈希查找

| 位置 | 频率 | 现状 | 建议 |
|---|---|---|---|
| `NoiseChunk::wrap/wrapRec`（`wrapped_` + 每次调用新建 `localMemo`） | ~20 次 wrap/区块，约 300 节点 × 2 次查找 | 每区块数百次哈希 + 哈希表分配 | 在 router 构建时给每个 `DensityNode` 分配 slot；区块内用 `std::vector<DensityNodePtr>` 按 slot 索引，O(1) 无哈希 |
| `buildSurface` 的 `registries.findBiomeId(name)` | 256 列/区块 | 字符串哈希 + 可能的 memcmp | 让 biome source 直接返回 id（R-tree 叶已含 id），或按稳定 name 指针缓存 |
| 气候→生物群系 R-tree `searchFlatRec` | 1536 次/区块 | 已无哈希，但树遍历 + 重复计算 | 按 6 维量化参数做线程本地直接映射缓存（C2ME biome cache），相邻 quart 大量重复 |
| `WorldGenRegion::getChunkIfAllowed` | 每次 biome reader 调用（地表/群系） | chunk key 哈希 | 按 (qx,qz) 缓存最近 chunk 指针 |
| `findTemplatePool` / `resolvePoolAlias` | 每个 jigsaw 尝试 | 哈希 / 已改 view | 池 id 指针缓存（同一 registries 生命周期内稳定） |
| `carverBiomeCache_` | 每个 carver 区块 | mutex + 哈希 | 改分片无锁/直接按 (seed,x,z) 数组 |
| `BlockSection::set` 调色板线性查找 | 每方块 | 已有 last-index 记忆 | 可再加 8 项直接映射缓存 |

## 9. 高频堆分配

| 位置 | 规模 | 说明 |
|---|---|---|
| `NoiseChunk` 构造 | 每区块 | `interpolators_`、`cellCaches_`、`wrapped_` 映射、每个 marker 一个 `make_shared<State>` + 一个 `custom` 节点 |
| 每个 interpolator 的 `slice0/slice1` | 2×245 doubles ≈ 4 KB × ~8 个 ≈ 32 KB/区块 | 可跨区块复用（线程本地池） |
| 含水层 `aquiferCache_/aquiferLocationCache_` | 3×35×3 = 315 项/区块 | 两个 vector，可复用 |
| `postProcessing_` | 每 section 一个 vector，逐流体方块 `push_back` | 低海平面区块可达数千次 push/扩容 |
| 结构：`shuffledPoolTemplates` / `elementShuffledJigsaws` | 每次 jigsaw 尝试 | 权重展开的指针 vector；Feature 元素还会 `make_unique<JigsawBlockInfo>` |
| `buildSurface` 的 `std::function` biome reader | 每区块 1 个 | 可改成模板/轻量结构体 |
| `HeightmapBlockGetter`（`std::function`） | 非堆叠高度图路径 | overworld（cw=4）走堆叠路径，暂不触发 |

实测分配器占比：t24 下 `mallocx` + `dallocx` ≈ 3.2% CPU，`memmove` ≈ 1.4%。

## 10. 优先级建议

1. **NoiseChunk 每区块的 wrap 哈希与临时分配**：slot 化 + 线程本地 NoiseChunk/切片复用，预计 -1~2% CPU，并减少分配抖动。
2. **气候→生物群系缓存**：1536 次/区块的 R-tree 查询，直接映射缓存可去掉大部分重复，预计 -1~2%。
3. **postProcessing 预分配/扁平化**：低海拔区块可减少数百次扩容。
4. **密度求值**：不要再整树展平（见 perf 分析第 11 节）；改为按子树实测收益的局部特化。
5. **含水层/噪声**：保持现有数学实现，优先做内存与查找优化。
