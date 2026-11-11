#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "redstone/block.hpp"

namespace redstone {

// LightLayer matches net.minecraft.world.level.LightLayer. Block first mirrors
// LevelLightEngine.updateSectionStatus's block-then-sky calls.
enum class LightLayer : std::uint8_t { Block = 0, Sky = 1 };
enum class LightDirection : std::uint8_t { Down = 0, Up, North, South, West, East };
inline constexpr std::size_t kLightLayerCount = 2;
inline constexpr int kLightVolume = 16 * 16 * 16;
inline constexpr std::size_t kLightLayerBytes = 2048;

inline std::size_t lightIndex(int x, int y, int z) {
  // DataLayer.getIndex: y << 8 | z << 4 | x.
  return (static_cast<std::size_t>(y & 15) << 8) |
         (static_cast<std::size_t>(z & 15) << 4) |
         static_cast<std::size_t>(x & 15);
}

static_assert(sizeof(std::atomic<std::uint8_t>) == 1,
              "published light state and page bytes must stay one byte");
static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
              "published light nibbles require lock-free atomic bytes");
inline constexpr std::uint8_t kLightUniformFlag = 0x10;

// One independently owned light-layer descriptor. The state byte's high nibble
// is the uniform flag and its low nibble is the uniform value. Uniform and
// missing descriptors allocate no page. Mixed owns exactly one shared
// atomic<uint8_t>[2048] nibble page; its sole layer writer and all readers use
// relaxed atomic byte loads/stores. Topology changes replace the descriptor in
// a new section header, never its page pointer in place.
class LightLayerSnapshot {
public:
  enum class Storage : std::uint8_t { Missing, Uniform, Mixed };

  LightLayerSnapshot();
  LightLayerSnapshot(const LightLayerSnapshot &other);
  LightLayerSnapshot &operator=(const LightLayerSnapshot &other);
  LightLayerSnapshot(LightLayerSnapshot &&other) noexcept;
  LightLayerSnapshot &operator=(LightLayerSnapshot &&other) noexcept;
  ~LightLayerSnapshot();

  static LightLayerSnapshot missing();
  static LightLayerSnapshot uniform(std::uint8_t value);
  static LightLayerSnapshot mixed(
      const std::array<std::uint8_t, kLightVolume> &values);
  static LightLayerSnapshot mixedPacked(
      const std::array<std::uint8_t, kLightLayerBytes> &packed);
  // Allocates the single 2048-byte page for a worker-owned mixed layer.
  static LightLayerSnapshot mixedEmpty();

  Storage storage() const {
    const std::uint8_t state =
        state_.load(std::memory_order_acquire);
    if ((state & kLightUniformFlag) != 0)
      return Storage::Uniform;
    return packed_ ? Storage::Mixed : Storage::Missing;
  }
  bool isMissing() const { return storage() == Storage::Missing; }
  bool isUniform() const { return storage() == Storage::Uniform; }
  bool isMixed() const { return storage() == Storage::Mixed; }
  std::uint8_t stateByte() const {
    return state_.load(std::memory_order_relaxed);
  }
  std::uint8_t uniformValue() const {
    return static_cast<std::uint8_t>(
        stateByte() & static_cast<std::uint8_t>(~kLightUniformFlag & 15));
  }
  std::uint8_t get(int x, int y, int z) const;
  std::size_t packedByteCount() const {
    return isMixed() ? kLightLayerBytes : 0;
  }
  std::uint8_t packedByte(std::size_t index) const {
    if (index >= kLightLayerBytes || !packed_)
      return 0;
    return packed_->bytes[index].load(std::memory_order_relaxed);
  }
  // Only the layer's sole worker calls these writers. No CAS is required.
  void writeCell(std::size_t index, std::uint8_t value);
  void writePackedByte(std::size_t byte, std::uint8_t value);
  void readPacked(std::array<std::uint8_t, kLightLayerBytes> &out) const;
  bool operator==(const LightLayerSnapshot &other) const;

private:
  struct PackedPage {
    std::atomic<std::uint8_t> bytes[kLightLayerBytes];
  };
  static_assert(sizeof(PackedPage) == kLightLayerBytes);

  void makeMixed();

  std::atomic<std::uint8_t> state_{0};
  std::shared_ptr<PackedPage> packed_;
};

// Shared mutable layer payload. Section/header copies retain the same layer
// object, so worker persistence and main-thread latest share one payload.
// Replacing a layer in a later section header swaps this pointer; it never
// mutates a vector visible to a reader.
class LightLayerRef {
public:
  LightLayerRef() = default;
  LightLayerRef(const LightLayerSnapshot &value)
      : value_(copy(value)) {}
  LightLayerRef(LightLayerSnapshot &&value)
      : value_(move(std::move(value))) {}
  explicit LightLayerRef(std::shared_ptr<LightLayerSnapshot> value)
      : value_(std::move(value)) {}

  LightLayerRef &operator=(const LightLayerSnapshot &value) {
    value_ = copy(value);
    return *this;
  }
  LightLayerRef &operator=(LightLayerSnapshot &&value) {
    value_ = move(std::move(value));
    return *this;
  }
  LightLayerRef &operator=(std::shared_ptr<LightLayerSnapshot> value) {
    value_ = std::move(value);
    return *this;
  }

  LightLayerSnapshot::Storage storage() const {
    return value_ ? value_->storage() : LightLayerSnapshot::Storage::Missing;
  }
  bool isMissing() const { return !value_ || value_->isMissing(); }
  bool isUniform() const { return value_ && value_->isUniform(); }
  bool isMixed() const { return value_ && value_->isMixed(); }
  std::uint8_t uniformValue() const {
    return value_ ? value_->uniformValue() : 0;
  }
  std::uint8_t get(int x, int y, int z) const {
    return value_ ? value_->get(x, y, z) : 0;
  }
  std::uint8_t packedByte(std::size_t index) const {
    return value_ ? value_->packedByte(index) : 0;
  }
  void writeCell(std::size_t index, std::uint8_t value) {
    if (value_)
      value_->writeCell(index, value);
  }
  void readPacked(std::array<std::uint8_t, kLightLayerBytes> &out) const {
    if (value_)
      value_->readPacked(out);
    else
      out.fill(0);
  }
  std::shared_ptr<LightLayerSnapshot> shared() const { return value_; }
  std::size_t packedByteCount() const {
    return isMixed() ? kLightLayerBytes : 0;
  }
  bool operator==(const LightLayerRef &other) const {
    if (!value_ || !other.value_)
      return !value_ && !other.value_;
    return *value_ == *other.value_;
  }
  bool operator!=(const LightLayerRef &other) const {
    return !(*this == other);
  }

private:
  static std::shared_ptr<LightLayerSnapshot>
  copy(const LightLayerSnapshot &value) {
    return value.isMissing()
               ? std::shared_ptr<LightLayerSnapshot>{}
               : std::make_shared<LightLayerSnapshot>(value);
  }
  static std::shared_ptr<LightLayerSnapshot>
  move(LightLayerSnapshot &&value) {
    return value.isMissing()
               ? std::shared_ptr<LightLayerSnapshot>{}
               : std::make_shared<LightLayerSnapshot>(std::move(value));
  }

  std::shared_ptr<LightLayerSnapshot> value_;
};

// A section header has two independent layer slots. Production views populate
// only their own slot and leave the other Missing; optical direction masks are
// owned by the coordinator geometry rather than copied into light publication.
bool hasDifferentLightProperties(const BlockState &oldState,
                                 const BlockState &newState);
bool hasDifferentLightProperties(const BlockLightProperties &oldProperties,
                                 const BlockLightProperties &newProperties);
bool hasDifferentSectionLightProperties(bool wasEmpty, bool isEmpty);
bool lightFaceShapesOcclude(const BlockLightProperties &from,
                            const BlockLightProperties &to,
                            LightDirection direction);

struct LightSectionSnapshot {
  explicit LightSectionSnapshot(
      int sectionY = 0, bool empty = true,
      std::vector<BlockLightProperties> palette = {BlockLightProperties{}},
      std::vector<std::uint16_t> cells = std::vector<std::uint16_t>(kLightVolume,
                                                                   0),
      std::array<LightLayerRef, kLightLayerCount> layers = {});

  int sectionY = 0;
  bool empty = true;
  std::vector<BlockLightProperties> palette;
  std::vector<std::uint16_t> cells;
  std::array<LightLayerRef, kLightLayerCount> layers{};
  std::uint16_t unsupportedCells = 0;

  // An empty cell vector is the uniform representation: every cell uses
  // palette[0]. Non-uniform sections carry exactly kLightVolume indices.
  bool isUniform() const { return cells.empty(); }
  const BlockLightProperties &at(int x, int y, int z) const;
  std::uint8_t light(LightLayer layer, int x, int y, int z) const;
};

// Immutable optical-class table built from the registered BlockState range.
// It is published before any conversion or propagation worker starts.
struct LightOpticalClass {
  BlockLightProperties properties;
  bool air = false;
};

class LightOpticalTable {
public:
  LightOpticalTable();

  std::uint8_t classForState(BlockState::Id state) const noexcept;
  const LightOpticalClass &operator[](std::uint8_t index) const noexcept {
    return classes_[index];
  }
  std::size_t classCount() const noexcept { return classes_.size(); }

private:
  std::vector<LightOpticalClass> classes_;
  std::vector<std::uint8_t> stateClasses_;
};

std::shared_ptr<const LightOpticalTable> defaultLightOpticalTable();

// Compact optical geometry for one 16x16x16 block section. Uniform sections do
// not allocate a cell array. Mixed sections own exactly one 4096-byte array of
// light optical-class ids.
struct GeometrySection {
  using Cells = std::array<std::uint8_t, kLightVolume>;

  bool empty = true;
  bool uniform = true;
  std::uint8_t uniformClass = 0;
  std::unique_ptr<Cells> cells;
  std::uint16_t nonAir = 0;
  std::uint16_t unknown = 0;

  std::uint16_t at(std::size_t index) const {
    if (uniform)
      return uniformClass;
    return (*cells)[index];
  }
};

// Read-only view into a live World section. Conversion happens synchronously
// on the world thread; the view is never stored or read by a worker.
struct LightSectionView {
  bool empty = true;
  std::uint8_t bits = 0; // 0 = single, 8 = cells8, 16 = cells16
  BlockState::Id single = 0;
  std::span<const BlockState::Id> palette;
  std::span<const std::uint8_t> cells8;
  std::span<const std::uint16_t> cells16;
};

GeometrySection makeLightGeometrySection(const LightOpticalTable &optical,
                                         const LightSectionView &section);

struct LightChunkInput {
  int chunkX = 0;
  int chunkZ = 0;
  // Optional world-owned lifecycle token. Leave zero for a standalone service;
  // World supplies a token that changes on unload/reload so queued cell records
  // from the old lifetime cannot be applied to the new one.
  std::uint64_t lifecycleEpoch = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  // Production load input has exactly blockSectionCount entries converted
  // directly from World::Chunk::Section. The sections member is only the legacy
  // attribute-level fixture adapter.
  std::vector<GeometrySection> geometrySections;
  std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;

  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
  const LightSectionSnapshot *section(int sectionY) const;
};

// Fixed-size ordinary block-change payload. The worker owns coordinate
// validation and optical mapping; the main thread only publishes the final raw
// state ID.
struct alignas(16) LightCellUpdate {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t z = 0;
  BlockState::Id state = 0;
};
static_assert(sizeof(LightCellUpdate) == 16);

struct LightChunkSnapshot {
  struct LayerChange {
    int sectionY = 0;
    bool sky = false;
    bool block = false;
  };
  int chunkX = 0;
  int chunkZ = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  // Latest input request that touched this chunk.
  std::uint64_t generation = 0;
  // Lifecycle rejects unload/reload results; revision distinguishes completed
  // results within one loaded lifetime and is ordered by the single writer,
  // independently of the unordered batch's input generation.
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t revision = 0;
  bool skyLightEnabled = true;
  // Set on an independently published layer view. Legacy combined fixtures
  // leave this empty.
  std::optional<LightLayer> layer;
  // L-2a may complete the block layer while sky propagation and protocol
  // publication remain unfinished.
  bool blockLightComplete = false;
  bool skyLightComplete = false;
  bool propagationComplete = false;
  // Set only when every loaded chunk has complete optical metadata and both
  // enabled light layers converged. Protocol encoding never exposes an
  // unpublished or incomplete result.
  bool protocolReady = false;
  std::uint32_t unsupportedCells = 0;
  // Compatibility pointers for hand-built legacy fixtures. Production uses
  // separate latest layer chunks; they are null on production views.
  std::shared_ptr<const LightChunkSnapshot> blockLayerSnapshot;
  std::shared_ptr<const LightChunkSnapshot> skyLayerSnapshot;
  std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;
  // changedSections is a ready-to-ready delta. deltaBaseRevision identifies
  // the last protocol-ready snapshot it was computed from; incomplete drained
  // revisions do not advance that base.
  std::uint64_t deltaBaseRevision = 0;
  std::vector<LayerChange> changedSections;

  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
  const LightSectionSnapshot *section(int sectionY) const;
  // Sky queries follow SkyLightSectionStorage: a missing section takes the
  // bottom plane of the nearest stored section above it. Raw layer access stays
  // available through section().Missing sky data therefore differs from a
  // deliberately stored zero layer.
  int lightValue(LightLayer layer, int x, int y, int z) const;
};

struct LightBlockChange {
  int chunkX = 0;
  int chunkZ = 0;
  int sectionY = 0;
  std::uint16_t localIndex = 0;
  BlockLightProperties oldProperties;
  BlockLightProperties newProperties;
  bool wasEmpty = false;
  bool isEmpty = false;
  std::uint64_t generation = 0; // assigned by LightService
};

struct LightWorkerOptions {
  std::size_t batchSize = 1000;
  std::size_t maxQueuedCommands = 4096;

  bool startPausedForTesting = false;
  bool hasSkyLight = true;
  bool collectDiagnostics = false;
  // Affinity for the single light worker. Block and sky run sequentially on it.
  std::uintptr_t workerAffinityMask = 0;

  // Extra time the worker waits for more work before it starts a batch.
  // Production mixed pages update in place, while section headers and dirty
  // metadata are rebuilt only for real writes; zero takes work immediately.
  int batchWindowMs = 20;
  // Geometry input uses a 10 MiB SPSC queue by default. Tests may provide a
  // tiny explicit entry count. A full queue drops the newest record.
  std::size_t cellQueueCapacity = 0;
  std::size_t geometryQueueBytes = std::size_t{10} << 20;
  // One worker publishes complete block and sky layer results into this fixed
  // SPSC result queue. The main thread is the only consumer.
  std::size_t resultQueueBytes = std::size_t{4} << 20;
  // Deterministic test seam invoked once per enabled layer, before propagation.
  std::function<void(LightLayer)> beforeLayerForTesting;
};

// Main-thread submission and worker-thread owner. No method on the worker
// borrows the live World: every input is a value snapshot. Publications are
// drained on the main thread; the callback never runs on the light worker.
class LightService {
public:
  struct Stats {
    std::uint64_t submitted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t processedBatches = 0;
    std::uint64_t processedBlockChanges = 0;
    std::uint64_t droppedGeometryTasks = 0;
    std::uint64_t published = 0;
    std::uint64_t stalePublications = 0;
    std::uint64_t queueHighWater = 0;
    std::uint64_t pendingChunks = 0;
    std::uint64_t pendingPublications = 0;
    std::uint64_t lightQueueEntries = 0;
    std::uint64_t skySectionChanges = 0;
    std::uint64_t blockSectionChanges = 0;
    std::uint64_t receivedSkySections = 0;
    std::uint64_t receivedBlockSections = 0;
    double workerCpuMs = 0;
    std::uint64_t workerAffinityRequested = 0;
    std::uint64_t workerAffinityMask = 0;
    std::uint32_t workerAffinityError = 0;
    std::uint64_t workerCount = 0;
    double blockWorkerCpuMs = 0;
    double skyWorkerCpuMs = 0;
    std::uint64_t blockWorkerAffinityRequested = 0;
    std::uint64_t blockWorkerAffinityMask = 0;
    std::uint32_t blockWorkerAffinityError = 0;
    std::uint64_t skyWorkerAffinityRequested = 0;
    std::uint64_t skyWorkerAffinityMask = 0;
    std::uint32_t skyWorkerAffinityError = 0;
  };

  explicit LightService(int minBlockSectionY, int blockSectionCount,
                        LightWorkerOptions options = {});
  ~LightService();
  LightService(const LightService &) = delete;
  LightService &operator=(const LightService &) = delete;

  // The immutable state-id to optical-class table used for direct World-section
  // conversion. It is fully built before the service starts its workers.
  std::shared_ptr<const LightOpticalTable> opticalTable() const;
  // Valid updates are coalesced per chunk and are not rejected by queue
  // capacity. Returns 0 only when the service is closing or input is invalid.
  std::uint64_t submitChunk(LightChunkInput input);
  bool reportSectionStatus(int chunkX, int chunkZ, int sectionY, bool empty);
  bool reportBlockChange(LightBlockChange change);
  // Bulk form for callers that coalesce one tick's writes with their own bitmap.
  // The caller already decided which cells need a re-check, so the per-cell
  // property comparison is skipped and the service mutex is taken once for the
  // whole batch. The worker never reads `oldProperties`; it is stored only to
  // keep the merged struct consistent.
  void reportBlockChanges(const std::vector<LightBlockChange> &changes);
  // Owned hot-path hand-off. The record contains every optical value the worker
  // needs. A full ring merges the latest owned record into pending work instead
  // of dropping it.
  void reportBlockCell(LightCellUpdate update);
  bool unloadChunk(int chunkX, int chunkZ);

  // Main-thread publication handoff. Headers are copied at drain, while layer
  // descriptors and atomic 2048-byte pages are shared with the worker. Mixed
  // values may update in place after publication and readers may observe a
  // mixture of old and new nibbles.
  void drainPublished();
  std::shared_ptr<const LightChunkSnapshot> latestChunk(int chunkX,
                                                        int chunkZ) const;
  // Returns the independently saved layer envelope. Block always exists; sky
  // is null when that layer is absent.
  std::shared_ptr<const LightChunkSnapshot> latestLayerChunk(
      LightLayer layer, int chunkX, int chunkZ) const;
  // Test-only snapshot of the sky worker's 256 lowest direct-source heights.
  // Returns nullopt unless the service is currently idle.
  std::optional<std::array<int, 256>> skyColumnSourcesForTesting(
      int chunkX, int chunkZ) const;
  // Test-only read of the coordinator's frozen optical mirror. Production
  // light snapshots intentionally contain no secondary optical geometry.
  std::optional<BlockLightProperties> opticalPropertiesForTesting(
      int x, int y, int z) const;
  void setPublishCallback(
      std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback);

  Stats stats() const;
  // Resets monotonic counters after initial-lighting warmup. Pending work is
  // left visible; benchmark callers must invoke it only after an idle drain.
  void resetMeasurementStats();
  std::string workerError() const;
  bool waitIdleForTesting();
  // Waits until the worker has published its pre-sleep state. The following
  // report exercises the exact notify-before-wait window without a timing poll.
  bool waitWorkerSleepingForTesting();
  void pauseForTesting();
  void resumeForTesting();
  // Lets a paused service choose exactly one pending chunk for the worker's
  // next batch and pause again after it completes. Used to exercise publication
  // ordering without depending on unordered_map iteration.
  bool processChunkBatchForTesting(int chunkX, int chunkZ);
  void close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace redstone
