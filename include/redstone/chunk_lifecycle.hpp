#pragma once
// 异步区块驻留生命周期控制器（`ChunkLifecycleController`）及底层区域存储抽象。
// 承接票据系统（`tickets.hpp`）的加载等级变更，在后台单 I/O 线程完成 Anvil 地形与实体区读取、
// 增量补丁落盘与模板保留，并通过无阻塞 `drain()` 在主线程完成安装与驱逐，避免磁盘 I/O 卡顿主循环。

#include "redstone/nbt.hpp"
#include "redstone/chunk_section.hpp"
#include "redstone/engine_types.hpp"
#include "redstone/entity_codec.hpp"
#include "redstone/terrain_codec.hpp"
#include "redstone/tickets.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace redstone {

// 区块在内存中的驻留生命周期状态：
// 驱动异步读取 -> 主线程安装 -> 脏写回磁盘 -> 无需求且干净后安全驱逐的状态转移。
enum class ChunkResidencyState {
  Absent,       // 无需求且未驻留
  Reading,      // 已派发后台读取，等待完成并安装
  Resident,     // 已安装到主线程且当前干净或待保存
  Saving,       // 后台正在写回，完成后再决定继续驻留或进入驱逐
  EvictPending, // 需求已降为非加载态，等待脏数据落盘及驱逐回调确认后释放
  Failed,       // 读取或安装失败
  Missing,      // 磁盘中不存在该区块（未挂载世界生成时停在此状态）
};

enum class ChunkLoadFailure {
  None,
  Missing,
  Unsupported,
  Corrupt,
  Io,
  InstallRejected,
};

enum class StoreReadStatus {
  Found,
  Missing,
  Unsupported,
  Corrupt,
  Io,
};

struct EntityChunkDataResult {
  enum class Status { Empty, NonEmpty, Unsupported, Error, Unavailable };
  Status status = Status::Unavailable;
  std::string detail;
};
using EntityChunkDataProbe =
    std::function<EntityChunkDataResult(const ChunkTicketPos &)>;

struct StoreReadResult {
  StoreReadStatus status = StoreReadStatus::Io;
  std::vector<std::uint8_t> nbt;
  std::string detail;
  // 随成功读取一并移入对应的安装事件；失败或过期读取会随结果自动释放，
  // 从而无需额外维护带外（out-of-band）的逐读取令牌表。
  std::optional<EntityChunkDataResult> entityData;
};

struct StoreWriteResult {
  bool wrote = false;
  bool cleanupPending = false;
  std::string detail;
};

// 供后台 worker 使用的原始地形存储边界。一个控制器只拥有一个 I/O worker，
// 因此即使同一 region 内有多个区块同时读写，所有存储方法也严格串行调用；
// 实现不得在外部并发打开静态 region 状态。
class RawChunkStore {
public:
  virtual ~RawChunkStore() = default;
  virtual StoreReadResult readChunk(const ChunkTicketPos &position) = 0;
  virtual StoreWriteResult writeChunk(const ChunkTicketPos &position,
                                      std::span<const std::uint8_t> nbt,
                                      int timestampSeconds) = 0;
};

// 控制器在同一条单 I/O worker 线程上串行执行地形（`region/`）与实体（`entities/`）读写。
class RawEntityStore {
public:
  virtual ~RawEntityStore() = default;
  virtual StoreReadResult readEntities(const ChunkTicketPos &position) = 0;
  virtual StoreWriteResult writeEntities(
      const ChunkTicketPos &position,
      std::span<const std::uint8_t> nbt, int timestampSeconds) = 0;
};

class RegionEntityStore final : public RawEntityStore {
public:
  explicit RegionEntityStore(std::filesystem::path entityDirectory);
  StoreReadResult readEntities(const ChunkTicketPos &position) override;
  StoreWriteResult writeEntities(
      const ChunkTicketPos &position,
      std::span<const std::uint8_t> nbt, int timestampSeconds) override;

private:
  std::filesystem::path entityDirectory_;
};

class RegionFileStore final : public RawChunkStore {
public:
  explicit RegionFileStore(std::filesystem::path regionDirectory);
  StoreReadResult readChunk(const ChunkTicketPos &position) override;
  StoreWriteResult writeChunk(const ChunkTicketPos &position,
                              std::span<const std::uint8_t> nbt,
                              int timestampSeconds) override;

private:
  std::filesystem::path regionDirectory_;
};

// 跨线程串行化同一 region 目录的读写：ChunkLifecycleController 的单 I/O worker
// 与专用世界生成线程共享同一个实例，保证任何时刻只有一个线程打开/改写 region 文件。
// 存在原因：26.1 的 RegionFile 头部扇区分配不是并发的；生成线程只读也必须与写盘互斥。
class SerializedChunkStore final : public RawChunkStore {
public:
  explicit SerializedChunkStore(std::shared_ptr<RawChunkStore> inner);
  StoreReadResult readChunk(const ChunkTicketPos &position) override;
  StoreWriteResult writeChunk(const ChunkTicketPos &position,
                              std::span<const std::uint8_t> nbt,
                              int timestampSeconds) override;

private:
  std::shared_ptr<RawChunkStore> inner_;
  std::mutex mutex_;
};

// 将解码后的轻量视图移动给主线程接收方；原始解码出的 NBT 根节点仍由 I/O worker 持有作为保存模板，
// 以便后续落盘时直接在原 NBT 树上打补丁，保留未解析的第三方或高版本标签而不必主线程长期占用内存。
struct LifecycleLoadedChunk {
  ChunkTicketPos position;
  std::uint64_t epoch = 0;
  std::optional<EntityChunkDataResult> entityData;
  std::int32_t chunkX = 0;
  std::int32_t chunkZ = 0;
  std::string statusName;
  bool isLightOn = false;
  std::vector<terrain::Section> sections;
  std::vector<terrain::SavedTick> blockTicks;
  std::vector<terrain::SavedTick> fluidTicks;
  std::vector<terrain::MovingPistonEntity> movingPistons;
  bool hasUnsupportedBlockEntities = false;
  std::string blockEntityError;
  std::vector<entity_codec::EntityRecord> entityRecords;
  bool entitySlotPresent = false;
  // 由世界生成产出的区块（promoteProtoChunkToLoaded）标记为 true：管理器据此在安装后
  // 把区块纳入待落盘状态，使专用 I/O 线程把这次生成真正写回 Anvil。
  bool generated = false;
  // 生成期光照交接：主线程把 worker 构造的几何/首次计算请求提交给 LightService 后
  // 记录的请求序号。安装时据此跳过整份几何重传，并对齐光照生命周期代次。
  std::uint64_t lightSubmissionGeneration = 0;
  std::uint64_t lightComputeGeneration = 0;
  // 生成路径零拷贝晋升：区段（位宽/调色板/计数器/随机刻分桶）与生物群系存储整体
  // move 过来，安装时直接替换 World 区块区段，不再 pack -> unpack 两次重编码。
  // 为空表示仍走通用的 terrain::Section 解码路径（磁盘载入 / getter 需要光照层）。
  std::optional<std::array<BlockSection, kSectionCapacity>> prebuiltSections;
  std::optional<std::array<std::shared_ptr<SectionBiomes>, kSectionCapacity>>
      prebuiltBiomes;
  int prebuiltSectionCount = 0;
  int prebuiltMinSectionY = 0;
  // ProtoChunk::postProcessing 展开后的世界坐标。安装后管理器按原版
  // postLoadProtoChunk 语义为这些位置重新调度流体刻（例如特征生成的水源/岩浆源）。
  std::vector<Pos> postProcessingPositions;
  // 仅用于测试夹具的原始实体标签列表；受管在线加载使用 `entityRecords`。
  std::vector<nbt::Tag> entities;

  LifecycleLoadedChunk() = default;
  LifecycleLoadedChunk(LifecycleLoadedChunk &&) = default;
  LifecycleLoadedChunk &operator=(LifecycleLoadedChunk &&) = default;
  LifecycleLoadedChunk(const LifecycleLoadedChunk &) = delete;
  LifecycleLoadedChunk &operator=(const LifecycleLoadedChunk &) = delete;
};

struct ChunkResidentCallbacks {
  // 在主线程/`drain()` 线程中以移动语义传入解码后的区段与计划刻视图。
  // 返回 true 确认安装并将状态由 `Reading` 推进为 `Resident`。
  std::function<bool(LifecycleLoadedChunk &&)> install;
  // 仅当区块已干净（无未保存修改）且仍无加载需求时，在主线程/`drain()` 线程调用。
  // 返回 true 确认移除；返回 false 则保持 `EvictPending` 状态等待后续重试。
  std::function<bool(const ChunkTicketPos &, std::uint64_t epoch)> evict;
  // 磁盘读取返回 Missing 时在主线程调用。返回 true 表示已把该坐标交给世界生成，
  // 条目保持 `Reading` 等待 `completeGeneratedChunk`；返回 false 则按原来的
  // Missing 失败路径处理（没有生成器时行为完全不变）。
  std::function<bool(const ChunkTicketPos &, std::uint64_t epoch,
                     int loadingLevel)>
      requestGeneration;
};

struct ChunkLifecycleStatus {
  ChunkResidencyState state = ChunkResidencyState::Absent;
  ChunkLoadFailure failure = ChunkLoadFailure::None;
  // 驻留代数令牌：同一坐标卸载再重载会递增 `epoch`，用于丢弃在途但已过期的异步读盘结果。
  std::uint64_t epoch = 0;
  int demandLevel = ticket_levels::NoTicketSource;
  bool demand = false;
  // 单调递增的脏版本号与已落盘版本号，确保慢速异步写入回执不会错误清掉较新的内存修改。
  std::uint64_t dirtyVersion = 0;
  std::uint64_t savedDirtyVersion = 0;
  std::string detail;
};

// 异步区块生命周期状态机控制器（仅负责加载/保存/驱逐调度；世界生成与刻门控由上层管理器串联）。
// `updateDemand` / `drain` / `status` 均为仅限主线程调用的接口。
class ChunkLifecycleController {
public:
  ChunkLifecycleController(std::filesystem::path regionDirectory,
                           ChunkResidentCallbacks callbacks);
  ChunkLifecycleController(
      std::shared_ptr<RawChunkStore> store,
      ChunkResidentCallbacks callbacks,
      std::shared_ptr<RawEntityStore> entityStore = {});
  ~ChunkLifecycleController();

  ChunkLifecycleController(const ChunkLifecycleController &) = delete;
  ChunkLifecycleController &operator=(const ChunkLifecycleController &) = delete;

  // 接收一条加载等级跟踪结果。等级小于或等于 `FullChunkLevel`（33）时产生数据驻留需求；
  // 高于该等级则释放需求（若已干净则触发驱逐）。
  bool updateDemand(const ChunkTicketPos &position, int loadingLevel);
  bool applyLoadingChanges(std::span<const ChunkLevelChange> changes);

  // 仅推进脏版本号而不立即入队写盘。旧版本号的落盘确认永远无法清除此后新产生的脏标记。
  bool markDirty(const ChunkTicketPos &position, std::uint64_t version);

  struct ChunkTickSaveData {
    std::int64_t snapshotTime = 0;
    terrain::TickListPatch ticks;
  };

  // worker 在下一次写回前将该不可变增量补丁应用到保留的 NBT 模板上。
  // 使用 `shared_ptr` 共享不可变补丁，使失败的重试请求或与新修改合并时无需在重试队列与命令队列间重复拷贝向量。
  using ChunkEditPatch =
      std::shared_ptr<const std::vector<terrain::CompactBlockEdit>>;

  struct ChunkSavePayload {
    ChunkEditPatch edits;
    // `nullopt` 表示保持磁盘上原有的计划刻列表不变；有值时则携带完整的当前方块/流体计划刻列表，
    // 其内部有值但为空的 `optional` 可显式清空对应列表。
    std::optional<ChunkTickSaveData> ticks;
    // 有值表示该区块当前拥有经校验的“仅含移动活塞”方块实体视图；
    // 传入空向量会显式清空旧的已持久化移动活塞列表。
    std::optional<std::vector<terrain::MovingPistonEntity>> movingPistons;
    // `nullopt` 保持实体存储槽位不动；有值（含空列表）则替换实体区域中的 `Entities` 列表。
    std::optional<std::vector<entity_codec::EntityRecord>> entityRecords;
    // `nullopt` 保留现有的 `isLightOn` 与区段光照半字节数组；
    // 有值时原子替换 `isLightOn` 与 `BlockLight`/`SkyLight` 数组。
    std::optional<terrain::ChunkLightPatch> light;
  };

  // `snapshotVersion` 精确标识本次请求所对应的快照版本（未必等于当前最新脏版本号）。
  // 若该快照旧于当前驻留代数中最后一次已受理或已成功写入的快照，则请求被拒绝；
  // 写入失败的快照允许以同一版本号重试。
  bool requestSave(const ChunkTicketPos &position,
                   std::uint64_t snapshotVersion,
                   std::vector<terrain::BlockEdit> edits = {});
  bool requestSavePayload(const ChunkTicketPos &position,
                          std::uint64_t snapshotVersion,
                          ChunkSavePayload payload);
  // 移交一次成功解码所得完整 NBT 根节点的所有权。
  // 校验完全完成后 worker 才会替换其保留模板，且仅在请求通过快照版本检查后才会移动该根节点。
  bool requestSaveOwnedRoot(const ChunkTicketPos &position,
                            std::uint64_t snapshotVersion,
                            nbt::RootTag &&root);

  // 世界生成线程（经管理器）在主线程交付一个新生成区块：完整安装载荷与可用于后续
  // 增量存盘的 NBT 模板。控制器校验 epoch/需求后交给 I/O worker 校验并持有模板，
  // 再由 `drain()` 走普通 install 回调。过期结果返回 false 并由调用方丢弃。
  bool completeGeneratedChunk(const ChunkTicketPos &position,
                              std::uint64_t epoch,
                              LifecycleLoadedChunk &&loaded, nbt::RootTag &&root);
  // 生成失败：仅在 epoch 仍有效时把条目推进为 Failed 并保留 detail，不自动重试。
  void failGeneratedChunk(const ChunkTicketPos &position, std::uint64_t epoch,
                          std::string detail);

  // 在此前 `evict` 回调拒绝移除后，对单个已处于 `EvictPending` 状态的区块重试驱逐。
  // 采用按坐标直接查找而非全局扫描，且在 `close()` 之后仍可用，使主线程能在 worker 退出后完成收尾清理。
  bool retryEviction(const ChunkTicketPos &position);

  // 非阻塞排空：将已完成的加载/保存事件交给 `install` / `evict` 回调，
  // 并在移交前校验事件的 `epoch` 是否匹配当前主线程代数。
  std::size_t drain();

  std::optional<ChunkLifecycleStatus>
  status(const ChunkTicketPos &position) const;
  bool accepting() const;

  // 仅用于测试的容器计数校验：确保成功卸载或失效读取不会为历史坐标永久残留映射节点。
  std::size_t activeEntryCountForTest() const;
  std::size_t workerLedgerCountForTest();
  std::size_t templateCountForTest();

  // 停止接收新请求，排空已受理的后台命令后等待 I/O worker 线程退出。
  // 正常主循环运行期间绝不阻塞等待磁盘。
  void close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace redstone
