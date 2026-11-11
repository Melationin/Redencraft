# 紧凑光照 worker 最终设计（2026-11-12）

本文件已按用户最新要求收敛；旧的“同批等齐发布”“不可变光值/COW”等设计作废。不得将用户允许的弱一致光值读升级成快照一致性要求。

## 目标链路

World::setBlock 仅投递 Pos + 新 BlockState::Id → 后台唯一光学 world 更新 section → block/sky worker 只读该批冻结几何、各自原地更新自己的光层 → 各层完成独立发布 → 主线程分别保存两层 shared_ptr → getBrightness 时才合成。

block和sky固定由同一个持久worker顺序执行；block完成立即独立发布，sky完成再发布自己的结果，不等待共同批次。两层都结束后才更新下一批几何。

## 0. 已完成的原版统计

同版本原版26.1 jar注册表：1168 blocks、29873 states，包含isAir及原版实际规范化六面、发光/衰减等字段的光学签名共136类，另1未知哨兵，8bit足够。全量中5408 states的有效面包含多个AABB，不能近似成单矩形。当前C++表覆盖68 blocks、2366 states，对应31个含isAir的原版类；本轮维持当前支持范围，不将未知当透明。

复现：/home/zhddsj/.local/state/redstone-cpp/wsl-light-signatures-20261111/run_probe.sh
out/analysis.json SHA-256：6819ed1e1cd242b40129e5edb8b03d42ff1633f59b7a38b6884b537c5acccd90

## 1. 输入和后台几何

- 几何输入与结果交接使用分段SPSC queue。几何首段默认10MiB、结果首段默认4MiB；段满时producer分配并追加同一初始大小的新段，已排队记录不搬移、不复制。每段一个producer和一个consumer，跨段仍保持FIFO；不增加逐记录互斥锁，扩容分配只发生在段切换慢路径。
- 普通几何记录业务载荷为三个int32坐标+uint32 stateId共16B，附加单调sequence。主线程不比较/解析光学属性，不为光照额外检查section空态。只有实际内存分配失败才计入失败；不再因固定容量丢弃newest。
- SPSC记录可靠有序，加载/刷新/卸载控制与普通记录保持同一顺序。不得把刷新前旧记录或旧代记录重放到新区块；内部额外序号数组等开销必须单列，不能只报告16B业务载荷。
- 加载和权威刷新交owned原始ID section快照，保持rawSetBlock导入后刷新契约。World目前没有写COW，不能把主世界storage指针直接共享给后台。
- 后台维护唯一chunk→sectionY数组→uint8光学类几何，Uniform无数组、Mixed4096B。维护nonAir/unknown计数和stateId映射；等价变化不传播，但保留形状、未知状态和空态的必要保守重查。
- 两层同批只读冻结镜像；普通改块不克隆整块几何，不把光学几何复制进生产发布对象。属性级fixture接口隔离。

## 2. 严格独立的共享可变nibble光层

- block、sky各自持有稳定的 shared_ptr<atomic<uint8_t>[2048]> 页，偶格低nibble、奇格高nibble，每格严格4bit。两层都Mixed时有效载荷共4096B；Uniform/Missing无页。传播工作区直接共享该页，禁止4096B解包。
- 每层只有一个writer。Mixed值使用 atomic<uint8_t> relaxed load/store，不需要CAS。协议、指纹、查询逐原子字节读取，不可对并发可写页裸memcpy。
- worker与main各自拥有chunk/section小header，只共享层描述符和2048B页。Mixed跨批原地更新，发布不复制2KiB页；允许main看到旧值或混合新旧值。
- 表示类型/页指针/section拓扑变化需安全发布稳定描述符，持shared_ptr保证旧页活着；不得并发改读者遍历的vector/map。
- 两层持久数据、section视图、结果、revision、delta基准、完成状态完全独立。禁止合成双层section对象或共同发布信封。main latestBlock/latestSky分别更新；getBrightness按原版max(block,sky-skyDampen)，不合成存储数组。
- dirty基于实际写入跟踪。不能通过旧/新页内容比较或指针相等判断delta，因为引用的共享页会原地更新。积压发布合并需保留未发送dirty、各层独立delta基准。
- 初始区块等必需两层都可用才发；增量允许仅一层更新。生命周期逐层校验与清理，旧epoch页不能复用到新代。
- 网络消费允许同一包内各字节来自不同时间，但层拓扑和协议mask必须自洽。

## 3. section优先传播（阶段1/2验收后）

先复用已解析section/工作层/旧值，保持亮度快筛先于读光学属性。检查→撤光→增光全局屏障不变，撤光初期保持FIFO。方块光尝试同section优先增光，跨section入外部待处理队列，并加连续处理预算。

天空光跨缺层原版有直接覆盖目标值语义，不可假设任意重排等价。第一版可仅优先处理足够深入section内部、不可能触及边界和底面继承的增光后续任务（亮度L，离任一section面至少L格）；其余保持FIFO。扩大范围须证明和差分。

## 大拷贝要求

尽量避免大拷贝，发现应立即报告位置、每次大小、频率和去除方案。
加载和权威刷新在World线程上直接读取当前Chunk::Section，通过预建BlockState ID到uint8光学类表转换为GeometrySection；不再创建LightRawSection或复制第二份4/8KiB索引输入。Mixed section只分配唯一的4096B光学类数组，转换期间的临时palette映射很小并随即释放。禁止发布光值深拷贝和4096B解包；Uniform展开2048B初始化与编码到最终网络包属于必要写入。

## 验收与分工

编码和相关直接测试由coder完成；reviewer审查diff/日志，不重复跑CTest（禁止CTest）。
阶段1：16B输入、唯一几何、分层结果与独立发布/主线程缓存及协议查询。
阶段2：共享可变atomic packed工作层，消除解包/发布复制；为避免反复重构可以与阶段1合并实现。
阶段3：section局部访问和传播调度；默认切换前比较FIFO值/raw flags和吞吐。

验证：tiny segment跨段增长+控制顺序、刷新/卸载重载、空态/未知、奇偶nibble、Uniform/Missing转换、旧页生命、单/双worker、阻塞sky时block可领取、同shared_ptr跨批dirty仍送达；原版sky/block golden不改。通过后同WSL世吞、main/block/sky CPU 2/4/6，交错开关光和改前/改后对照；主线程mean/CPU相对关光目标<10%，报告P95、实际提交量、后台CPU/排空、内存有效载荷和模拟/排空光照指纹。不得拿既有已开光版本的<10%回退当目标达成。

## 最新 Uniform 容器布局（用户明确，2026-11-12）

每一层由一个稳定描述符表示：一个 std::atomic<uint8_t> 状态字节，加上仅在Mixed时非空的 shared_ptr<atomic<uint8_t>[2048]>；section的两层槽各持有一个这样的描述符。状态高半字节 0x10 表示Uniform，低半字节保存0..15；Mixed为空页标志且使用页。Uniform不分配页，Mixed页每层2048B。已实现static_assert atomic<uint8_t>为1字节且lock-free、page大小为2048。

两槽容器不意味着合成发布：block/sky每层独立单writer、页、状态与发布/查询，不能加双层完成门槛。描述符一旦发布，其状态和页指针不再原地修改；Uniform/Mixed拓扑转换创建新描述符并换section header，旧页由旧读者shared_ptr保生命。Mixed页光值继续原地atomic relaxed读写。Uniform展开按旧亮度重复填充两个nibble，完整初始化后发布；Missing继续保持独立presence/null语义，不得等同uniform0。网络只保存每层epoch、revision和未发送dirty，不依赖旧页内容算delta。
