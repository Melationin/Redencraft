#include "redstone/worldgen/worldgen_scheduler.hpp"

#include "redstone/engine.hpp"
#include "redstone/light/service.hpp"
#include "redstone/terrain_codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace redstone::worldgen {
namespace {

constexpr int kMaxDistBucket = 31;
constexpr int kStageCount = static_cast<int>(kChunkStatusCount); // 12
constexpr int kTotalBuckets = 64 * kStageCount;                  // 768
constexpr std::size_t kBitsetWords = (kTotalBuckets + 63) / 64;  // 12

struct StageScheduleSpec {
  bool hasWait = false;
  int waitRadius = 0;
  ChunkStatusId waitStatus = ChunkStatusId::Empty;
  std::uint8_t writeRadius = 0;
};

StageScheduleSpec scheduleSpecForStage(ChunkStatusId stage, bool loadingPath) noexcept {
  if (loadingPath)
    return stage == ChunkStatusId::Light ? StageScheduleSpec{true, 1, ChunkStatusId::InitializeLight, 0} : StageScheduleSpec{};
  switch (stage) {
  case ChunkStatusId::Noise:
  case ChunkStatusId::Surface:
  case ChunkStatusId::Spawn:    return {true, 1, ChunkStatusId::Biomes, 0};
  case ChunkStatusId::Features: return {true, 1, ChunkStatusId::Carvers, 1};
  case ChunkStatusId::Light:    return {true, 1, ChunkStatusId::InitializeLight, 0};
  default:                      return {};
  }
}

bool canChainInlineWithCurrentPins(const StageScheduleSpec &cur, const StageScheduleSpec &next) noexcept {
  return next.writeRadius == 0 &&
         (!next.hasWait || (cur.hasWait && cur.waitRadius >= next.waitRadius &&
                            static_cast<int>(cur.waitStatus) >= static_cast<int>(next.waitStatus)));
}

int chebyshevDist(ChunkTicketPos a, ChunkTicketPos b) noexcept { return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z)); }

std::int64_t squaredEuclideanDist(ChunkTicketPos a, ChunkTicketPos b) noexcept {
  const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x, dz = static_cast<std::int64_t>(a.z) - b.z;
  return dx * dx + dz * dz;
}

std::pair<std::int64_t, std::uint8_t> bestCenterDist(ChunkTicketPos pos, std::span<const ChunkTicketPos> centers) noexcept {
  if (centers.empty()) return {0, 0};
  std::int64_t bestSq = std::numeric_limits<std::int64_t>::max();
  int bestCheb = std::numeric_limits<int>::max();
  for (const ChunkTicketPos &c : centers) {
    bestSq = std::min(bestSq, squaredEuclideanDist(pos, c));
    bestCheb = std::min(bestCheb, chebyshevDist(pos, c));
  }
  return {bestSq, static_cast<std::uint8_t>(std::clamp(bestCheb, 0, kMaxDistBucket))};
}

std::uint64_t nowNs() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t elapsedUsSince(std::uint64_t startNs) noexcept {
  const std::uint64_t endNs = startNs > 0 ? nowNs() : 0;
  return endNs > startNs ? ((endNs - startNs) / 1000ULL) : 0ULL;
}

std::shared_ptr<BaseChunk> resolveDepChunk(const ChunkHolder::GenTaskState::PinnedDep &dep) {
  return (dep.chunk && !dep.chunk->isInvalidated()) ? dep.chunk : (dep.holder ? dep.holder->chunkShared() : nullptr);
}

void setReadyGenStateLocked(ChunkHolder::GenTaskState &gen, std::uint8_t stage, bool fromLoaded, std::uint8_t distBucket) noexcept {
  gen.currentStage = gen.initialPersistedStage = stage;
  gen.fromLoaded = fromLoaded;
  gen.dependencyFailed = false;
  gen.initState = ChunkHolder::GenTaskState::InitState::Ready;
  gen.distBucket = std::min(gen.distBucket, distBucket);
}

void notifyIdleIfZero(std::atomic<std::size_t> &counter, std::mutex &mtx, std::condition_variable &cv) noexcept {
  if (counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    { std::lock_guard<std::mutex> lock(mtx); }
    cv.notify_all();
  }
}

} // namespace

double WorldGenSchedulerStats::latencyPercentileMs(double p) const noexcept {
  std::uint64_t total = 0, cum = 0;
  for (std::uint64_t c : targetLatencyHistUs) total += c;
  if (total == 0) return 0.0;
  const double target = std::clamp(p, 0.0, 1.0) * static_cast<double>(total);
  for (std::size_t b = 0; b < targetLatencyHistUs.size(); ++b) {
    const std::uint64_t count = targetLatencyHistUs[b];
    if (count == 0) continue;
    cum += count;
    if (static_cast<double>(cum) >= target || b + 1 == targetLatencyHistUs.size()) {
      const std::uint64_t lo = (b == 0) ? 0ULL : (std::uint64_t{1} << b);
      const std::uint64_t hi = (b == 0) ? 1ULL : (b == 31 ? std::numeric_limits<std::uint32_t>::max() : ((std::uint64_t{1} << (b + 1)) - 1ULL));
      const double frac = std::clamp((target - static_cast<double>(cum - count)) / static_cast<double>(count), 0.0, 1.0);
      const double us = static_cast<double>(lo) + frac * static_cast<double>(hi - lo);
      return (targetLatencyMaxUs > 0 ? std::min(us, static_cast<double>(targetLatencyMaxUs)) : us) / 1000.0;
    }
  }
  return static_cast<double>(targetLatencyMaxUs) / 1000.0;
}

struct WorldGenScheduler::ReadyEntry { std::shared_ptr<ChunkHolder> holder; std::uint32_t genId = 0; std::uint8_t nextStage = 0; bool isFinalize = false; };

class WorldGenScheduler::PriorityBucketQueue {
public:
  bool empty() const noexcept { return std::none_of(nonEmpty_.begin(), nonEmpty_.end(), [](std::uint64_t w) { return w != 0; }); }
  void push(std::uint8_t distBucket, bool residencyPath, std::uint8_t nextStage,
            std::shared_ptr<ChunkHolder> holder, std::uint32_t genId, bool isFinalize = false) {
    const int idx = ((residencyPath ? 0 : 32) + std::clamp<int>(distBucket, 0, kMaxDistBucket)) * kStageCount +
                    (kStageCount - 1 - std::clamp<int>(nextStage, 0, kStageCount - 1));
    buckets_[static_cast<std::size_t>(idx)].push_back({std::move(holder), genId, nextStage, isFinalize});
    nonEmpty_[static_cast<std::size_t>(idx >> 6)] |= (std::uint64_t{1} << (idx & 63));
  }
  bool pop(ReadyEntry &out) {
    for (std::size_t w = 0; w < kBitsetWords; ++w) {
      if (nonEmpty_[w] == 0) continue;
      const int bit = std::countr_zero(nonEmpty_[w]);
      auto &q = buckets_[(w << 6) | static_cast<std::size_t>(bit)];
      out = std::move(q.front()); q.pop_front();
      if (q.empty()) nonEmpty_[w] &= ~(std::uint64_t{1} << bit);
      return true;
    }
    return false;
  }
private:
  std::array<std::uint64_t, kBitsetWords> nonEmpty_{};
  std::array<std::deque<ReadyEntry>, kTotalBuckets> buckets_;
};

enum class CtrlCmdKind : std::uint8_t { Submit, Cancel, UpdateCenters, Trim, Shutdown };
struct WorldGenScheduler::ControlCommand {
  CtrlCmdKind kind = CtrlCmdKind::Submit; DemandRequest demand{}; ChunkTicketPos pos{};
  bool cancelResidency = false; std::uint64_t onlyIfEpoch = 0;
  std::vector<ChunkTicketPos> centers{}; int keepRadius = 15;
};
struct WorldGenScheduler::DemandEntry {
  ChunkTicketPos pos{}; std::uint8_t residencyTarget = 0, bufferTarget = 0;
  std::uint64_t residencyEpoch = 0, armedAtNs = 0; std::int64_t gameTime = 0;
  bool knownMissingOnDisk = false, residencyDispatched = false, residencyFinalizing = false, bufferDispatched = false, bufferCompleted = false;
};
struct WorldGenScheduler::CancelAction { ChunkTicketPos pos{}; std::uint8_t remainingExplicitTarget = 0; bool clearResidencyPath = true; };
struct WorldGenScheduler::AdmitAction {
  ChunkTicketPos pos{}; std::uint8_t targetStatus = 0, distBucket = 0;
  bool residency = false, knownMissingOnDisk = false; std::uint64_t epoch = 0; std::int64_t gameTime = 0;
};
struct WorldGenScheduler::WaitOutcome {
  enum class Kind : std::uint8_t { None, DepFailed, BecameIdle, Enqueue };
  Kind kind = Kind::None; std::uint8_t nextStage = 0, reachedStage = 0, distBucket = kMaxDistBucket;
  bool residencyPath = false; std::uint32_t genId = 0; std::uint64_t armedNs = 0;
};

class WorldGenScheduler::SpatialRegionGuard {
public:
  explicit SpatialRegionGuard(WorldGenScheduler &sched) noexcept : sched_(sched) {}
  ~SpatialRegionGuard() { release(); }
  SpatialRegionGuard(const SpatialRegionGuard &) = delete;
  SpatialRegionGuard &operator=(const SpatialRegionGuard &) = delete;
  void arm(std::array<std::shared_ptr<ChunkHolder>, 9> &h, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) holders_[i] = std::move(h[i]);
    count_ = n;
  }
  void armSingle(std::shared_ptr<ChunkHolder> h) noexcept { holders_[0] = std::move(h); count_ = 1; }
  void release() {
    if (count_ == 0) return;
    std::vector<ChunkHolder::SpatialWaiter> woken;
    std::array<std::shared_ptr<ChunkHolder>, 9> released;
    const std::size_t n = count_;
    {
      std::lock_guard<std::mutex> lock(sched_.spatialMutex_);
      for (std::size_t i = 0; i < n; ++i) {
        released[i] = std::move(holders_[i]);
        if (ChunkHolder *h = released[i].get()) {
          h->writeRegionLocked = false;
          h->writeRegionLockedAtomic.store(false, std::memory_order_release);
          woken.insert(woken.end(), std::make_move_iterator(h->writeWaiters.begin()), std::make_move_iterator(h->writeWaiters.end()));
          h->writeWaiters.clear();
        }
      }
      count_ = 0;
    }
    sched_.wakeSpatialWaiters(woken);
  }
private:
  WorldGenScheduler &sched_;
  std::array<std::shared_ptr<ChunkHolder>, 9> holders_{};
  std::size_t count_ = 0;
};

WorldGenScheduler::WorldGenScheduler(
    ChunkMap &chunkMap, ChunkGenerator &generator, std::int64_t worldSeed, RawChunkStore *diskStore,
    std::shared_ptr<LightOpticalContext> lightContext, GeneratedLightQueue *lightQueue, WorldGenSchedulerConfig config)
    : chunkMap_(&chunkMap), generator_(generator), worldSeed_(worldSeed), diskStore_(diskStore),
      lightContext_(std::move(lightContext)), lightQueue_(lightQueue), config_(config),
      readyQueue_(std::make_unique<PriorityBucketQueue>()) { startWorkers(); }

WorldGenScheduler::WorldGenScheduler(
    ChunkGenerator &generator, std::int64_t worldSeed, RawChunkStore *diskStore, World *residentWorld,
    std::shared_ptr<LightOpticalContext> lightContext, GeneratedLightQueue *lightQueue, WorldGenSchedulerConfig config)
    : ownedChunkMap_(residentWorld ? nullptr : std::make_unique<ChunkMap>(nullptr)),
      chunkMap_(residentWorld ? &residentWorld->chunkMap() : ownedChunkMap_.get()),
      generator_(generator), worldSeed_(worldSeed), diskStore_(diskStore),
      lightContext_(std::move(lightContext)), lightQueue_(lightQueue), config_(config),
      readyQueue_(std::make_unique<PriorityBucketQueue>()) { startWorkers(); }

void WorldGenScheduler::startWorkers() {
  if (config_.threadCount == 0) config_.threadCount = std::max<std::size_t>(1, std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 4);
  if (config_.maxInFlightResidency == 0) config_.maxInFlightResidency = std::clamp<std::size_t>(config_.threadCount * 3, 16, 64);
  if (config_.maxInFlightBuffer == 0) config_.maxInFlightBuffer = std::clamp<std::size_t>(config_.threadCount * 2, 8, 32);
  if (config_.batchWavefrontLimit == 0) config_.batchWavefrontLimit = std::clamp<std::size_t>(config_.threadCount * 4, 32, 128);
  workers_.reserve(config_.threadCount);
  for (std::size_t i = 0; i < config_.threadCount; ++i) workers_.emplace_back([this]() { workerLoop(); });
}

WorldGenScheduler::~WorldGenScheduler() {
  { std::lock_guard<std::mutex> lock(queueMutex_); stop_ = true; }
  queueCv_.notify_all();
  for (std::thread &t : workers_) if (t.joinable()) t.join();
  { std::lock_guard<std::mutex> lock(completedMutex_); completedResidency_.clear(); }
  if (!chunkMap_) return;
  chunkMap_->holders().for_each([](const ChunkMap::HolderMap::value_type &entry) {
    if (!entry.second) return;
    entry.second->writeRegionLocked = false;
    entry.second->writeRegionLockedAtomic.store(false, std::memory_order_release);
    entry.second->writeWaiters.clear();
    if (auto *gen = entry.second->genState()) {
      std::lock_guard<std::mutex> glock(gen->mtx);
      gen->clearPinnedDeps();
      for (auto &list : gen->waitersByStage) list.clear();
      gen->depTargetCounts.fill(0); gen->dependentTarget = 0;
      gen->arming = false; gen->pendingDeps.store(0, std::memory_order_release); gen->satisfiedDeps = 0;
      gen->execState = ChunkHolder::ExecState::Idle; gen->isActive = false;
    }
  });
}

void WorldGenScheduler::markActiveLocked(ChunkHolder::GenTaskState &gen) noexcept {
  if (!gen.isActive) { gen.isActive = true; activeHolders_.fetch_add(1, std::memory_order_acq_rel); }
}
void WorldGenScheduler::markInactiveLocked(ChunkHolder::GenTaskState &gen) noexcept {
  if (gen.isActive) { gen.isActive = false; releaseActiveHolder(); }
}
std::shared_ptr<ChunkHolder> WorldGenScheduler::findHolder(ChunkTicketPos pos) const { return chunkMap_->findHolder(pos); }

void WorldGenScheduler::ensureHolderInitialized(
    const std::shared_ptr<ChunkHolder> &holder, std::uint8_t distBucket, bool knownMissingOnDisk) {
  auto &gen = holder->ensureGenState();
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(gen.mtx);
      if (gen.initState == ChunkHolder::GenTaskState::InitState::Initializing) {
        ++gen.initWaiters;
        initCv_.wait(lock, [&]() { return gen.initState != ChunkHolder::GenTaskState::InitState::Initializing; });
        --gen.initWaiters; continue;
      }
      const HeldChunkKind kind = holder->heldKind();
      if (kind == HeldChunkKind::World) { setReadyGenStateLocked(gen, static_cast<std::uint8_t>(ChunkStatusId::Full), true, distBucket); return; }
      if (kind == HeldChunkKind::Proto) {
        if (gen.initState == ChunkHolder::GenTaskState::InitState::Ready) gen.distBucket = std::min(gen.distBucket, distBucket);
        else { auto p = holder->protoShared(); setReadyGenStateLocked(gen, p ? static_cast<std::uint8_t>(p->persistedStatus()) : 0, false, distBucket); }
        return;
      }
      gen.initState = ChunkHolder::GenTaskState::InitState::Initializing;
    }
    struct InitResetGuard {
      ChunkHolder::GenTaskState &g; std::condition_variable &cv; bool committed = false;
      ~InitResetGuard() {
        if (committed) return;
        bool wake = false;
        { std::lock_guard<std::mutex> lock(g.mtx); g.initState = ChunkHolder::GenTaskState::InitState::Uninitialized; wake = g.initWaiters > 0; }
        if (wake) cv.notify_all();
      }
    } resetGuard{gen, initCv_};

    std::shared_ptr<ProtoChunk> loadedProto;
    bool fromLoaded = false;
    if (diskStore_ != nullptr && !knownMissingOnDisk) {
      const StoreReadResult raw = diskStore_->readChunk(holder->pos());
      if (raw.status == StoreReadStatus::Found) {
        std::string err;
        auto decoded = decodeProtoChunkNbt(raw.nbt, generator_.chunkMinY(), generator_.chunkHeight(), err);
        if (!decoded) throw std::runtime_error("failed to decode persisted chunk (" + std::to_string(holder->pos().x) + "," + std::to_string(holder->pos().z) + "): " + err);
        loadedProto = std::make_shared<ProtoChunk>(std::move(*decoded));
        fromLoaded = true; chunksLoadedFromStore_.fetch_add(1, std::memory_order_relaxed);
      } else if (raw.status != StoreReadStatus::Missing) {
        throw std::runtime_error("disk chunk read error: " + raw.detail);
      }
    }
    if (!loadedProto) { loadedProto = std::make_shared<ProtoChunk>(holder->pos(), generator_.chunkMinY(), generator_.chunkHeight()); chunksCreated_.fetch_add(1, std::memory_order_relaxed); }

    bool wake = false;
    {
      std::lock_guard<std::mutex> lock(gen.mtx);
      resetGuard.committed = true; wake = gen.initWaiters > 0;
      const HeldChunkKind kindNow = holder->heldKind();
      if (holder->evicted()) gen.initState = ChunkHolder::GenTaskState::InitState::Uninitialized;
      else if (kindNow == HeldChunkKind::World) setReadyGenStateLocked(gen, static_cast<std::uint8_t>(ChunkStatusId::Full), true, distBucket);
      else if (kindNow == HeldChunkKind::Empty) {
        const auto st = static_cast<std::uint8_t>(loadedProto->persistedStatus());
        holder->setProto(std::move(loadedProto)); setReadyGenStateLocked(gen, st, fromLoaded, distBucket);
      } else { gen.initState = ChunkHolder::GenTaskState::InitState::Ready; gen.distBucket = std::min(gen.distBucket, distBucket); }
    }
    if (wake) initCv_.notify_all();
    return;
  }
}

std::shared_ptr<ChunkHolder> WorldGenScheduler::getOrCreateHolder(
    ChunkTicketPos pos, std::uint8_t distBucket, bool knownMissingOnDisk) {
  for (;;) {
    auto holder = chunkMap_->getOrCreateHolder(pos);
    if (!holder->evicted()) {
      ensureHolderInitialized(holder, distBucket, knownMissingOnDisk);
      if (!holder->evicted()) return holder;
    }
  }
}

bool WorldGenScheduler::armExplicitTarget(
    const std::shared_ptr<ChunkHolder> &holder, std::uint8_t distBucket, std::uint8_t targetStatus,
    bool residency, bool registerBatchTarget, std::vector<std::shared_ptr<ChunkHolder>> &workStack) {
  auto &gen = holder->ensureGenState();
  std::lock_guard<std::mutex> lock(gen.mtx);
  gen.dependencyFailed = false;
  gen.distBucket = std::min(gen.distBucket, distBucket);
  if (residency) gen.residencyPath = true;
  gen.explicitTarget = std::max(gen.explicitTarget, targetStatus);
  if (gen.currentStage >= targetStatus) return false;
  if (gen.firstArmedNs == 0) gen.firstArmedNs = nowNs();
  if (registerBatchTarget) {
    std::lock_guard<std::mutex> block(batchMutex_);
    activeBatchTargets_[holder->key()] = targetStatus;
    batchInFlight_ = activeBatchTargets_.size();
  }
  if (gen.execState == ChunkHolder::ExecState::Idle) {
    gen.execState = ChunkHolder::ExecState::WaitingDeps;
    markActiveLocked(gen);
    workStack.push_back(holder);
  }
  return true;
}

BaseChunk &WorldGenScheduler::ensureStatus(ChunkTicketPos pos, ChunkStatusId targetStatus) {
  const ChunkBatchRequest req{pos, targetStatus};
  ensureStatusBatch(std::span<const ChunkBatchRequest>(&req, 1), pos);
  auto h = findHolder(pos);
  auto chunk = h ? h->chunkShared() : nullptr;
  if (!chunk) throw std::runtime_error("WorldGenScheduler missing completed chunk");
  return *chunk;
}

void WorldGenScheduler::ensureStatusBatch(std::span<const ChunkBatchRequest> requests, ChunkTicketPos priorityAnchor) {
  if (requests.empty()) return;
  { std::lock_guard<std::mutex> lock(errorMutex_); lastError_.clear(); }

  phmap::flat_hash_map<std::uint64_t, ChunkStatusId, ChunkKeyHash> dedup;
  dedup.reserve(requests.size());
  for (const ChunkBatchRequest &req : requests) {
    auto [it, ins] = dedup.emplace(packChunkKey(req.pos.x, req.pos.z), req.targetStatus);
    if (!ins && static_cast<int>(req.targetStatus) > static_cast<int>(it->second)) it->second = req.targetStatus;
  }
  std::vector<ChunkBatchRequest> sortedReqs; sortedReqs.reserve(dedup.size());
  for (const auto &[key, status] : dedup) sortedReqs.push_back({unpackChunkKey(key), status});
  std::sort(sortedReqs.begin(), sortedReqs.end(), [&](const ChunkBatchRequest &a, const ChunkBatchRequest &b) {
    const int da = chebyshevDist(a.pos, priorityAnchor), db = chebyshevDist(b.pos, priorityAnchor);
    return da != db ? da < db : squaredEuclideanDist(a.pos, priorityAnchor) < squaredEuclideanDist(b.pos, priorityAnchor);
  });

  { std::lock_guard<std::mutex> block(batchMutex_); activeBatchTargets_.clear(); batchInFlight_ = 0; batchFailed_ = false; }
  const std::size_t limit = config_.batchWavefrontLimit;
  for (std::size_t nextIdx = 0; nextIdx < sortedReqs.size();) {
    std::size_t slots = 0;
    {
      std::unique_lock<std::mutex> block(batchMutex_);
      batchCv_.wait(block, [&]() { return batchFailed_ || batchInFlight_ < limit; });
      if (batchFailed_) break;
      slots = limit - batchInFlight_;
    }
    std::vector<std::shared_ptr<ChunkHolder>> toArm; toArm.reserve(std::min(slots, sortedReqs.size() - nextIdx));
    while (slots > 0 && nextIdx < sortedReqs.size()) {
      const ChunkBatchRequest &req = sortedReqs[nextIdx++];
      const std::uint8_t d = static_cast<std::uint8_t>(std::clamp(chebyshevDist(req.pos, priorityAnchor), 0, kMaxDistBucket));
      if (armExplicitTarget(getOrCreateHolder(req.pos, d), d, static_cast<std::uint8_t>(req.targetStatus), false, true, toArm)) --slots;
      else recordTargetLatencyUs(0);
    }
    if (!toArm.empty()) armHoldersIterative(std::move(toArm));
  }
  {
    std::unique_lock<std::mutex> block(batchMutex_);
    batchCv_.wait(block, [&]() { return batchFailed_ || activeBatchTargets_.empty(); });
    activeBatchTargets_.clear(); batchInFlight_ = 0;
  }
  bool hasDemands = false;
  { std::lock_guard<std::mutex> dlock(demandMutex_); hasDemands = !demands_.empty(); }
  if (!hasDemands) waitUntilIdle();
  std::string failure = lastError();
  if (!failure.empty()) throw std::runtime_error(failure);
}

std::string WorldGenScheduler::lastError() const { std::lock_guard<std::mutex> lock(errorMutex_); return lastError_; }

void WorldGenScheduler::recordTargetLatencyUs(std::uint64_t latencyUs) noexcept {
  completedTargets_.fetch_add(1, std::memory_order_relaxed);
  targetLatencySumUs_.fetch_add(latencyUs, std::memory_order_relaxed);
  std::uint64_t curMax = targetLatencyMaxUs_.load(std::memory_order_relaxed);
  while (latencyUs > curMax && !targetLatencyMaxUs_.compare_exchange_weak(curMax, latencyUs, std::memory_order_relaxed)) {}
  targetLatencyHistUs_[latencyUs == 0 ? 0 : std::min<std::size_t>(31, std::bit_width(latencyUs) - 1)].fetch_add(1, std::memory_order_relaxed);
}

std::vector<ChunkHolder::GenTaskState::WaiterRef>
WorldGenScheduler::recordStageFailure(ChunkHolder &holder, std::string detail) {
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    if (lastError_.empty())
      lastError_ = "worldgen stage failed for chunk " + std::to_string(holder.pos().x) + "," + std::to_string(holder.pos().z) + ": " + detail;
  }
  std::vector<ChunkHolder::GenTaskState::WaiterRef> waiters;
  auto &gen = holder.ensureGenState();
  {
    std::lock_guard<std::mutex> lock(gen.mtx);
    for (auto &list : gen.waitersByStage) { waiters.insert(waiters.end(), list.begin(), list.end()); list.clear(); }
    gen.depTargetCounts.fill(0); gen.dependentTarget = 0; gen.clearPinnedDeps();
    ++gen.armSeq; ++gen.generationId;
    gen.arming = false;
    gen.pendingDeps.store(0, std::memory_order_release);
    gen.satisfiedDeps = 0;
    gen.execState = ChunkHolder::ExecState::Idle; markInactiveLocked(gen);
  }
  for (const auto &w : waiters) {
    if (!w.holder) continue;
    auto &wGen = w.holder->ensureGenState();
    std::lock_guard<std::mutex> wlock(wGen.mtx);
    if (wGen.execState == ChunkHolder::ExecState::WaitingDeps && wGen.armSeq == w.armSeq) wGen.dependencyFailed = true;
  }
  return waiters;
}

void WorldGenScheduler::unregisterPinnedDeps(
    const ChunkHolder *waiter, std::uint32_t armSeq,
    std::vector<ChunkHolder::GenTaskState::PinnedDep> &pinned,
    std::vector<std::shared_ptr<ChunkHolder>> &cascadeWork) {
  for (const auto &dep : pinned) {
    if (dep.chunk) dep.chunk->releaseGenerationUser();
    if (dep.holder) dep.holder->generationRefCount.fetch_sub(1, std::memory_order_acq_rel);
    auto *nGen = (dep.holder && dep.waitedStage > 0 && dep.waitedStage < kChunkStatusCount) ? dep.holder->genState() : nullptr;
    if (!nGen) continue;
    bool cascade = false;
    {
      std::lock_guard<std::mutex> nlock(nGen->mtx);
      auto &list = nGen->waitersByStage[dep.waitedStage];
      std::size_t erased = 0;
      for (std::size_t i = 0; i < list.size();) {
        if (list[i].holder.get() == waiter && list[i].armSeq == armSeq) { list[i] = std::move(list.back()); list.pop_back(); ++erased; }
        else ++i;
      }
      if (erased > 0) {
        auto &cnt = nGen->depTargetCounts[dep.waitedStage];
        cnt = cnt > erased ? static_cast<std::uint16_t>(cnt - erased) : 0;
        nGen->recomputeDependentTargetLocked();
        cascade = nGen->currentStage >= nGen->effectiveTarget() && nGen->execState != ChunkHolder::ExecState::Idle;
      }
    }
    if (cascade) cascadeWork.push_back(dep.holder);
  }
  pinned.clear();
}

void WorldGenScheduler::cancelHolderWorklist(
    std::vector<std::shared_ptr<ChunkHolder>> worklist, bool applyExplicitToFirst,
    std::uint8_t newExplicitTarget, bool clearResidencyPath) {
  while (!worklist.empty()) {
    auto holder = std::move(worklist.back()); worklist.pop_back();
    auto *gen = holder ? holder->genState() : nullptr;
    if (!gen) { applyExplicitToFirst = false; continue; }
    std::uint32_t oldArmSeq = 0;
    std::vector<ChunkHolder::GenTaskState::PinnedDep> pinned;
    {
      std::lock_guard<std::mutex> lock(gen->mtx);
      if (applyExplicitToFirst) {
        gen->explicitTarget = newExplicitTarget;
        if (clearResidencyPath && gen->dependentTarget == 0) gen->residencyPath = false;
        applyExplicitToFirst = false;
      }
      if (gen->currentStage >= gen->effectiveTarget() &&
          (gen->execState == ChunkHolder::ExecState::Queued ||
           gen->execState == ChunkHolder::ExecState::WaitingDeps)) {
        if (gen->execState == ChunkHolder::ExecState::Queued) { ++gen->generationId; gen->clearPinnedDeps(); }
        else {
          oldArmSeq = gen->armSeq; ++gen->armSeq;
          gen->arming = false;
          gen->pendingDeps.store(0, std::memory_order_release);
          gen->satisfiedDeps = 0;
          pinned = std::move(gen->stagePinnedDeps); gen->stagePinnedDeps.clear();
        }
        gen->firstArmedNs = 0;
        gen->execState = ChunkHolder::ExecState::Idle; markInactiveLocked(*gen);
      }
    }
    if (!pinned.empty()) unregisterPinnedDeps(holder.get(), oldArmSeq, pinned, worklist);
  }
}

WorldGenScheduler::WaitOutcome
WorldGenScheduler::resolveWaitingLocked(ChunkHolder::GenTaskState &gen, std::uint8_t nextStage) noexcept {
  if (gen.dependencyFailed) return {WaitOutcome::Kind::DepFailed};
  if (gen.currentStage >= gen.effectiveTarget()) {
    const WaitOutcome out{WaitOutcome::Kind::BecameIdle, 0, gen.currentStage, gen.distBucket, gen.residencyPath, 0, gen.firstArmedNs};
    gen.firstArmedNs = 0; gen.clearPinnedDeps();
    gen.execState = ChunkHolder::ExecState::Idle; markInactiveLocked(gen);
    return out;
  }
  gen.execState = ChunkHolder::ExecState::Queued;
  return {WaitOutcome::Kind::Enqueue, nextStage, 0, gen.distBucket, gen.residencyPath, ++gen.generationId, 0};
}

void WorldGenScheduler::dispatchWaitingOutcome(
    const std::shared_ptr<ChunkHolder> &holder, const WaitOutcome &outcome,
    std::vector<ChunkHolder::GenTaskState::WaiterRef> *cascadeFailedOut) {
  if (outcome.kind == WaitOutcome::Kind::DepFailed) {
    auto subWaiters = recordStageFailure(*holder, "neighbor dependency stage failed");
    onHolderFailed(holder->pos(), "neighbor dependency stage failed");
    if (cascadeFailedOut != nullptr) cascadeFailedOut->insert(cascadeFailedOut->end(), subWaiters.begin(), subWaiters.end());
    else if (!subWaiters.empty()) notifyWaiters(subWaiters);
  } else if (outcome.kind == WaitOutcome::Kind::BecameIdle) {
    onHolderProgressedOrIdle(holder, outcome.reachedStage, outcome.armedNs);
  } else if (outcome.kind == WaitOutcome::Kind::Enqueue) {
    enqueueReady(holder, outcome.nextStage, outcome.distBucket, outcome.residencyPath, outcome.genId);
  }
}

void WorldGenScheduler::armHoldersIterative(std::vector<std::shared_ptr<ChunkHolder>> workStack) {
  while (!workStack.empty()) {
    auto holder = std::move(workStack.back()); workStack.pop_back();
    auto &gen = holder->ensureGenState();
    std::uint8_t nextStage = 0, distBucket = kMaxDistBucket;
    bool residencyPath = false; std::uint32_t myArmSeq = 0; StageScheduleSpec spec{};
    {
      std::unique_lock<std::mutex> lock(gen.mtx);
      if (gen.execState != ChunkHolder::ExecState::WaitingDeps || gen.arming ||
          gen.pendingDeps.load(std::memory_order_relaxed) > 0)
        continue;
      if (gen.dependencyFailed || gen.currentStage >= gen.effectiveTarget()) {
        const WaitOutcome early = resolveWaitingLocked(gen, 0); lock.unlock();
        dispatchWaitingOutcome(holder, early); continue;
      }
      nextStage = static_cast<std::uint8_t>(gen.currentStage + 1);
      const bool lp = gen.fromLoaded && gen.initialPersistedStage >= static_cast<std::uint8_t>(ChunkStatusId::Spawn);
      spec = scheduleSpecForStage(static_cast<ChunkStatusId>(nextStage), lp);
      distBucket = gen.distBucket; residencyPath = gen.residencyPath; myArmSeq = ++gen.armSeq;
      gen.pendingStage = nextStage; gen.pendingWriteRadius = spec.writeRadius;
      gen.arming = true;
      gen.pendingDeps.store(0, std::memory_order_relaxed);
      gen.satisfiedDeps = 0;
    }

    std::uint32_t registeredCount = 0;
    std::vector<ChunkHolder::GenTaskState::PinnedDep> pinnedForStage;
    if (spec.hasWait && spec.waitRadius > 0) {
      const auto reqStage = static_cast<std::uint8_t>(spec.waitStatus);
      pinnedForStage.reserve(static_cast<std::size_t>((spec.waitRadius * 2 + 1) * (spec.waitRadius * 2 + 1) - 1));
      for (int dz = -spec.waitRadius; dz <= spec.waitRadius; ++dz) {
        for (int dx = -spec.waitRadius; dx <= spec.waitRadius; ++dx) {
          if (dx == 0 && dz == 0) continue;
          const ChunkTicketPos nPos{holder->pos().x + dx, holder->pos().z + dz};
          for (;;) {
            auto neighbor = getOrCreateHolder(nPos, distBucket);
            neighbor->generationRefCount.fetch_add(1, std::memory_order_acq_rel);
            auto nChunk = neighbor->chunkShared(); if (nChunk) nChunk->addGenerationUser();
            auto &nGen = neighbor->ensureGenState();
            bool evictedRace = false, registered = false, armNeighbor = false;
            {
              std::lock_guard<std::mutex> nlock(nGen.mtx);
              if (neighbor->evicted()) evictedRace = true;
              else {
                nGen.distBucket = std::min(nGen.distBucket, distBucket);
                if (residencyPath) nGen.residencyPath = true;
                if (nGen.currentStage < reqStage) {
                  nGen.waitersByStage[reqStage].push_back({holder, myArmSeq});
                  ++nGen.depTargetCounts[reqStage]; nGen.recomputeDependentTargetLocked();
                  registered = true;
                  if (nGen.execState == ChunkHolder::ExecState::Idle && nGen.currentStage < nGen.effectiveTarget()) {
                    nGen.dependencyFailed = false; nGen.execState = ChunkHolder::ExecState::WaitingDeps;
                    markActiveLocked(nGen); armNeighbor = true;
                  }
                }
              }
            }
            if (evictedRace) {
              if (nChunk) nChunk->releaseGenerationUser();
              neighbor->generationRefCount.fetch_sub(1, std::memory_order_acq_rel); continue;
            }
            pinnedForStage.push_back({neighbor, std::move(nChunk), registered ? reqStage : std::uint8_t{0}, static_cast<std::int8_t>(dx), static_cast<std::int8_t>(dz)});
            if (registered) {
              ++registeredCount;
              if (armNeighbor) workStack.push_back(std::move(neighbor));
            }
            break;
          }
        }
      }
    }

    bool aborted = false; WaitOutcome outcome;
    {
      std::lock_guard<std::mutex> lock(gen.mtx);
      if (gen.execState != ChunkHolder::ExecState::WaitingDeps || gen.armSeq != myArmSeq) aborted = true;
      else {
        gen.arming = false;
        gen.clearPinnedDeps(); gen.stagePinnedDeps = std::move(pinnedForStage);
        const std::uint32_t remaining = registeredCount > gen.satisfiedDeps ? (registeredCount - gen.satisfiedDeps) : 0;
        gen.pendingDeps.store(remaining, std::memory_order_release);
        if (remaining == 0) outcome = resolveWaitingLocked(gen, nextStage);
      }
    }
    if (aborted) {
      std::vector<std::shared_ptr<ChunkHolder>> cascadeWork;
      unregisterPinnedDeps(holder.get(), myArmSeq, pinnedForStage, cascadeWork);
      if (!cascadeWork.empty()) cancelHolderWorklist(std::move(cascadeWork), false, 0, false);
      continue;
    }
    dispatchWaitingOutcome(holder, outcome);
  }
}

void WorldGenScheduler::enqueueReady(const std::shared_ptr<ChunkHolder> &holder,
                                     std::uint8_t nextStage, std::uint8_t distBucket,
                                     bool residencyPath, std::uint32_t genId, bool isFinalize) {
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    readyQueue_->push(distBucket, residencyPath, nextStage, holder, genId, isFinalize);
    if (!isFinalize && nextStage > 0 && scheduleSpecForStage(static_cast<ChunkStatusId>(nextStage), false).writeRadius == 0)
      parallelDispatches_.fetch_add(1, std::memory_order_relaxed);
  }
  queueCv_.notify_one();
}

void WorldGenScheduler::wakeSpatialWaiters(std::vector<ChunkHolder::SpatialWaiter> &woken) {
  if (woken.empty()) return;
  if (woken.size() > 1) {
    std::vector<ChunkHolder::SpatialWaiter> dedup;
    dedup.reserve(woken.size());
    for (auto &w : woken) {
      if (!w.holder) continue;
      if (w.isFinalize) { dedup.push_back(std::move(w)); continue; } // Keep 1:1 with activeFinalizeTasks_.
      auto it = std::find_if(dedup.begin(), dedup.end(), [&](const auto &d) { return !d.isFinalize && d.holder.get() == w.holder.get(); });
      if (it == dedup.end()) dedup.push_back(std::move(w));
      else if (w.genId > it->genId) *it = w;
    }
    woken.swap(dedup);
  }
  for (const auto &w : woken) {
    if (w.holder)
      enqueueReady(w.holder, w.isFinalize ? static_cast<std::uint8_t>(ChunkStatusId::Full) : w.nextStage,
                   w.distBucket, w.isFinalize || w.residencyPath, w.isFinalize ? 0U : w.genId, w.isFinalize);
  }
}

void WorldGenScheduler::workerLoop() {
  for (;;) {
    if (controlPending_.load(std::memory_order_seq_cst)) tryDrainControl();
    ReadyEntry entry;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait(lock, [this]() {
        return stop_ || !readyQueue_->empty() ||
               (controlPending_.load(std::memory_order_seq_cst) && !controlDrainActive_.load(std::memory_order_seq_cst));
      });
      if (stop_) return;
      if (!readyQueue_->pop(entry)) continue;
    }
    if (entry.isFinalize) finalizeResidencyHolder(entry.holder);
    else executeHolder(entry.holder, entry.genId);
  }
}

bool WorldGenScheduler::tryAcquireSpatial3x3OrDefer(
    const std::shared_ptr<ChunkHolder> &holder, ChunkHolder::GenTaskState &gen,
    std::array<std::shared_ptr<ChunkHolder>, 9> &regionHolders,
    std::size_t regionCount, std::uint8_t nextStage, SpatialRegionGuard &spatialGuard) {
  {
    std::lock_guard<std::mutex> slock(spatialMutex_);
    if (std::none_of(regionHolders.begin(), regionHolders.begin() + regionCount, [](const auto &h) { return h->writeRegionLocked; })) {
      for (std::size_t i = 0; i < regionCount; ++i) {
        regionHolders[i]->writeRegionLocked = true;
        regionHolders[i]->writeRegionLockedAtomic.store(true, std::memory_order_release);
      }
      spatialGuard.arm(regionHolders, regionCount);
      coloredDispatches_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  spatialConflicts_.fetch_add(1, std::memory_order_relaxed);
  WaitOutcome outcome;
  { std::lock_guard<std::mutex> lock(gen.mtx); outcome = resolveWaitingLocked(gen, nextStage); }
  if (outcome.kind != WaitOutcome::Kind::Enqueue) { dispatchWaitingOutcome(holder, outcome); return false; }
  bool parked = false;
  {
    std::lock_guard<std::mutex> slock(spatialMutex_);
    for (std::size_t i = 0; i < regionCount; ++i) {
      if (regionHolders[i]->writeRegionLocked) {
        regionHolders[i]->writeWaiters.push_back({holder, outcome.genId, nextStage, outcome.distBucket, outcome.residencyPath, false});
        parked = true; break;
      }
    }
  }
  if (!parked) enqueueReady(holder, nextStage, outcome.distBucket, outcome.residencyPath, outcome.genId, false);
  return false;
}

void WorldGenScheduler::executeHolder(const std::shared_ptr<ChunkHolder> &holder, std::uint32_t expectedGenId) {
  auto &gen = holder->ensureGenState();
  std::uint8_t cur = 0; bool loadingPath = false;
  {
    std::unique_lock<std::mutex> lock(gen.mtx);
    if (gen.execState != ChunkHolder::ExecState::Queued || gen.generationId != expectedGenId) return;
    if (gen.dependencyFailed || gen.currentStage >= gen.effectiveTarget()) {
      const WaitOutcome done = resolveWaitingLocked(gen, 0); lock.unlock();
      dispatchWaitingOutcome(holder, done); return;
    }
    gen.execState = ChunkHolder::ExecState::Running; cur = gen.currentStage;
    loadingPath = gen.fromLoaded && gen.initialPersistedStage >= static_cast<std::uint8_t>(ChunkStatusId::Spawn);
  }

  const ChunkPyramid &pyramid = loadingPath ? ChunkPyramid::loadingPyramid() : ChunkPyramid::generationPyramid();
  auto failHolder = [&](const std::string &msg) {
    auto w = recordStageFailure(*holder, msg); onHolderFailed(holder->pos(), msg);
    if (!w.empty()) notifyWaiters(w);
  };

  for (;;) {
    const std::uint8_t nextStage = static_cast<std::uint8_t>(cur + 1);
    const auto nextEnum = static_cast<ChunkStatusId>(nextStage);
    const StageScheduleSpec spec = scheduleSpecForStage(nextEnum, loadingPath);
    bool depFailed = false, depsMissing = false;
    std::array<std::shared_ptr<ChunkHolder>, 9> regionHolders{}; std::size_t regionCount = 0;
    {
      std::lock_guard<std::mutex> lock(gen.mtx);
      depFailed = gen.dependencyFailed;
      if (!depFailed && spec.hasWait && spec.waitRadius > 0) {
        const std::size_t expected = static_cast<std::size_t>((spec.waitRadius * 2 + 1) * (spec.waitRadius * 2 + 1) - 1);
        std::size_t validCount = 0;
        for (const auto &dep : gen.stagePinnedDeps) {
          if (std::abs(dep.dx) > spec.waitRadius || std::abs(dep.dz) > spec.waitRadius || (dep.dx == 0 && dep.dz == 0)) continue;
          auto nc = resolveDepChunk(dep);
          if (!nc || static_cast<int>(chunkPersistedStatus(*nc)) < static_cast<int>(spec.waitStatus)) { depsMissing = true; break; }
          ++validCount;
        }
        depsMissing = depsMissing || validCount < expected;
      }
      if (!depFailed && !depsMissing && spec.writeRadius > 0) {
        regionHolders[regionCount++] = holder;
        for (const auto &dep : gen.stagePinnedDeps)
          if (dep.holder && std::abs(dep.dx) <= 1 && std::abs(dep.dz) <= 1 && !(dep.dx == 0 && dep.dz == 0) && regionCount < 9)
            regionHolders[regionCount++] = dep.holder;
      }
    }
    if (depFailed) { failHolder("neighbor dependency stage failed"); return; }
    if (depsMissing) {
      { std::lock_guard<std::mutex> lock(gen.mtx); gen.clearPinnedDeps(); gen.execState = ChunkHolder::ExecState::WaitingDeps; }
      armHoldersIterative({holder}); return;
    }

    SpatialRegionGuard spatialGuard(*this);
    if (spec.writeRadius > 0 && !tryAcquireSpatial3x3OrDefer(holder, gen, regionHolders, regionCount, nextStage, spatialGuard)) return;
    try { runSingleStage(*holder, nextEnum, pyramid, loadingPath, spec.waitRadius, spec.writeRadius); }
    catch (const std::exception &error) { spatialGuard.release(); failHolder(error.what()); return; }
    spatialGuard.release(); // Release 3x3 lock immediately after Features before InitializeLight.
    stepsExecuted_.fetch_add(1, std::memory_order_relaxed);

    std::vector<ChunkHolder::GenTaskState::WaiterRef> waiters;
    bool inlineNext = false, needScheduleNext = false; std::uint64_t armedNs = 0;
    {
      std::lock_guard<std::mutex> lock(gen.mtx);
      gen.currentStage = nextStage;
      waiters = std::move(gen.waitersByStage[nextStage]); gen.waitersByStage[nextStage].clear();
      gen.depTargetCounts[nextStage] = 0; gen.recomputeDependentTargetLocked();
      if (nextStage < gen.effectiveTarget()) {
        const StageScheduleSpec nextSpec = scheduleSpecForStage(static_cast<ChunkStatusId>(nextStage + 1), loadingPath);
        if (canChainInlineWithCurrentPins(spec, nextSpec)) { inlineNext = true; cur = nextStage; if (!nextSpec.hasWait) gen.clearPinnedDeps(); }
        else { armedNs = gen.firstArmedNs; gen.clearPinnedDeps(); gen.execState = ChunkHolder::ExecState::WaitingDeps; needScheduleNext = true; }
      } else {
        armedNs = gen.firstArmedNs; gen.firstArmedNs = 0; gen.clearPinnedDeps();
        gen.execState = ChunkHolder::ExecState::Idle; markInactiveLocked(gen);
      }
    }
    if (!waiters.empty()) notifyWaiters(waiters);
    if (inlineNext) { inlineSteps_.fetch_add(1, std::memory_order_relaxed); continue; }
    onHolderProgressedOrIdle(holder, nextStage, armedNs);
    if (needScheduleNext) armHoldersIterative({holder});
    return;
  }
}

void WorldGenScheduler::runSingleStage(ChunkHolder &holder, ChunkStatusId nextStage,
                                       const ChunkPyramid &pyramid, bool loadingPath,
                                       int waitRadius, std::uint8_t writeRadius) {
  (void)loadingPath;
  auto centerShared = holder.chunkShared();
  if (!centerShared || !centerShared->isProtoChunk()) return;
  centerShared->addGenerationUser();
  struct Guard { BaseChunk *c; ~Guard() { c->releaseGenerationUser(); } } centerGuard{centerShared.get()};
  auto *proto = static_cast<ProtoChunk *>(centerShared.get());
  const ChunkStep &step = pyramid.getStepTo(nextStage);
  const int radius = step.directDependencies().getRadius(), stride = radius * 2 + 1;
  const int populateRadius = std::min(radius, std::max(waitRadius, static_cast<int>(writeRadius)));
  std::vector<BaseChunk *> squareCache(static_cast<std::size_t>(stride) * stride, nullptr);
  std::vector<std::shared_ptr<BaseChunk>> pinned{centerShared};
  squareCache[static_cast<std::size_t>(radius) * stride + static_cast<std::size_t>(radius)] = proto;
  if (populateRadius > 0) {
    for (const auto &dep : holder.ensureGenState().stagePinnedDeps) {
      if (std::abs(dep.dx) > populateRadius || std::abs(dep.dz) > populateRadius || (dep.dx == 0 && dep.dz == 0)) continue;
      if (auto nc = resolveDepChunk(dep)) {
        squareCache[static_cast<std::size_t>(dep.dz + radius) * stride + static_cast<std::size_t>(dep.dx + radius)] = nc.get();
        pinned.push_back(std::move(nc));
      }
    }
  }
  WorldGenRegion region(step, *proto, std::move(squareCache), radius, worldSeed_, std::move(pinned));
  executeChunkGenerationStep(generator_, region, *proto, nextStage, worldSeed_, lightContext_.get(),
                             lightQueue_, chunkMap_->world() == nullptr ? centerShared : nullptr,
                             config_.asyncWorldGenLight);
}

void WorldGenScheduler::notifyWaiters(std::vector<ChunkHolder::GenTaskState::WaiterRef> &waiters) {
  std::vector<ChunkHolder::GenTaskState::WaiterRef> cascadeFailed;
  for (auto &ref : waiters) {
    if (!ref.holder) continue;
    auto &wGen = ref.holder->ensureGenState();
    WaitOutcome outcome;
    {
      std::lock_guard<std::mutex> wlock(wGen.mtx);
      if (wGen.execState != ChunkHolder::ExecState::WaitingDeps || wGen.armSeq != ref.armSeq) continue;
      if (wGen.arming) {
        ++wGen.satisfiedDeps;
        continue;
      }
      const std::uint32_t prev = wGen.pendingDeps.load(std::memory_order_relaxed);
      const std::uint32_t rem = prev > 0 ? prev - 1 : 0;
      wGen.pendingDeps.store(rem, std::memory_order_release);
      if (rem != 0) continue;
      outcome = resolveWaitingLocked(wGen, wGen.pendingStage);
    }
    dispatchWaitingOutcome(ref.holder, outcome, &cascadeFailed);
  }
  if (!cascadeFailed.empty()) notifyWaiters(cascadeFailed);
}

void WorldGenScheduler::releaseActiveHolder() { notifyIdleIfZero(activeHolders_, idleMutex_, idleCv_); }
void WorldGenScheduler::decrementFinalizeTasks() noexcept { notifyIdleIfZero(activeFinalizeTasks_, idleMutex_, idleCv_); }
void WorldGenScheduler::waitUntilIdle() {
  std::unique_lock<std::mutex> lock(idleMutex_);
  idleCv_.wait(lock, [this]() { return activeHolders_.load(std::memory_order_acquire) == 0; });
}
void WorldGenScheduler::waitUntilStreamingIdle() {
  std::unique_lock<std::mutex> lock(idleMutex_);
  idleCv_.wait(lock, [this]() { return activeHolders_.load(std::memory_order_acquire) == 0 && activeFinalizeTasks_.load(std::memory_order_acquire) == 0; });
}

// ---------------------------------------------------------------------------
// Streaming Control Plane & Parallel Finalization
// ---------------------------------------------------------------------------

void WorldGenScheduler::pushControlCommand(ControlCommand cmd) {
  if (streamingShuttingDownAtomic_.load(std::memory_order_acquire)) return;
  { std::lock_guard<std::mutex> lock(ctrlMutex_); ctrlQueue_.push_back(std::move(cmd)); controlPending_.store(true, std::memory_order_seq_cst); }
  { std::lock_guard<std::mutex> lock(queueMutex_); }
  queueCv_.notify_one();
}
void WorldGenScheduler::submitDemand(const DemandRequest &request) {
  ControlCommand cmd; cmd.kind = CtrlCmdKind::Submit; cmd.demand = request; pushControlCommand(std::move(cmd));
}
void WorldGenScheduler::cancelDemand(ChunkTicketPos pos, bool cancelResidency, std::uint64_t onlyIfEpoch) {
  ControlCommand cmd; cmd.kind = CtrlCmdKind::Cancel; cmd.pos = pos; cmd.cancelResidency = cancelResidency; cmd.onlyIfEpoch = onlyIfEpoch; pushControlCommand(std::move(cmd));
}
void WorldGenScheduler::updatePriorityCenters(std::span<const ChunkTicketPos> centers, int keepRadius) {
  ControlCommand cmd; cmd.kind = CtrlCmdKind::UpdateCenters; cmd.centers.assign(centers.begin(), centers.end()); cmd.keepRadius = keepRadius; pushControlCommand(std::move(cmd));
}
void WorldGenScheduler::requestAsyncTrim() { ControlCommand cmd; cmd.kind = CtrlCmdKind::Trim; pushControlCommand(std::move(cmd)); }
std::vector<CompletedResidency> WorldGenScheduler::drainCompletedResidency() { std::lock_guard<std::mutex> lock(completedMutex_); return std::exchange(completedResidency_, {}); }
std::vector<ChunkTicketPos> WorldGenScheduler::drainEvictedChunks() { std::lock_guard<std::mutex> lock(evictedMutex_); return std::exchange(evictedChunks_, {}); }
StreamingDemandStats WorldGenScheduler::streamingStats() const {
  std::lock_guard<std::mutex> lock(demandMutex_);
  StreamingDemandStats out = streamingStats_; out.evicted = holdersEvicted_.load(std::memory_order_relaxed);
  return out;
}

void WorldGenScheduler::shutdownStreaming() {
  streamingShuttingDownAtomic_.store(true, std::memory_order_seq_cst);
  ControlCommand cmd; cmd.kind = CtrlCmdKind::Shutdown;
  { std::lock_guard<std::mutex> lock(ctrlMutex_); ctrlQueue_.push_back(std::move(cmd)); controlPending_.store(true, std::memory_order_seq_cst); }
  { std::lock_guard<std::mutex> lock(queueMutex_); }
  queueCv_.notify_all();
  while (controlDrainActive_.load(std::memory_order_seq_cst) || controlPending_.load(std::memory_order_seq_cst)) {
    tryDrainControl();
    if (controlDrainActive_.load(std::memory_order_seq_cst)) std::this_thread::yield();
  }
  waitUntilStreamingIdle();
}

void WorldGenScheduler::tryDrainControl() {
  for (;;) {
    if (controlDrainActive_.exchange(true, std::memory_order_seq_cst)) return;
    std::vector<ControlCommand> cmds;
    {
      std::lock_guard<std::mutex> lock(ctrlMutex_);
      cmds.swap(ctrlQueue_);
      controlPending_.store(false, std::memory_order_seq_cst);
    }

    std::vector<CancelAction> toCancel;
    std::vector<AdmitAction> toAdmit;
    bool doTrim = false;
    std::vector<ChunkTicketPos> trimCenters;
    int trimKeepRadius = 15;
    {
      std::lock_guard<std::mutex> dlock(demandMutex_);
      for (ControlCommand &cmd : cmds) applyControlCommandLocked(std::move(cmd), toCancel);
      selectAdmissionsLocked(toAdmit);
      if (trimRequested_ && !streamingShuttingDown_) {
        trimRequested_ = false; doTrim = true; trimCenters = priorityCenters_; trimKeepRadius = keepRadius_;
      }
    }

    for (const CancelAction &act : toCancel)
      if (auto root = findHolder(act.pos))
        cancelHolderWorklist({std::move(root)}, true, act.remainingExplicitTarget, act.clearResidencyPath);
    if (!toAdmit.empty()) executeAdmissions(toAdmit);
    if (doTrim) {
      std::vector<ChunkTicketPos> evicted;
      evictIdleOutsideRadius(trimCenters, trimKeepRadius, &evicted);
      if (!evicted.empty()) { std::lock_guard<std::mutex> elock(evictedMutex_); evictedChunks_.insert(evictedChunks_.end(), evicted.begin(), evicted.end()); }
    }

    controlDrainActive_.store(false, std::memory_order_seq_cst);
    if (!controlPending_.load(std::memory_order_seq_cst)) break;
  }
}

void WorldGenScheduler::retireResidencyLocked(DemandEntry &entry, bool isComplete, bool isFail, bool isCancel) {
  if (entry.residencyTarget == 0) return;
  if (entry.residencyDispatched && residencyInFlight_ > 0) --residencyInFlight_;
  entry.residencyTarget = 0; entry.residencyEpoch = 0; entry.residencyDispatched = entry.residencyFinalizing = false;
  if (isComplete) ++streamingStats_.completed; else if (isFail) ++streamingStats_.failed; else if (isCancel) ++streamingStats_.cancelled;
}

void WorldGenScheduler::retireBufferLocked(DemandEntry &entry, bool isComplete, bool isFail) {
  if (entry.bufferTarget == 0) return;
  if (entry.bufferDispatched && bufferInFlight_ > 0) --bufferInFlight_;
  entry.bufferDispatched = false;
  if (isComplete) { entry.bufferCompleted = true; ++streamingStats_.bufferCompleted; }
  else { entry.bufferTarget = 0; entry.bufferCompleted = false; if (isFail) ++streamingStats_.failed; }
}

void WorldGenScheduler::applyControlCommandLocked(ControlCommand cmd, std::vector<CancelAction> &toCancel) {
  switch (cmd.kind) {
  case CtrlCmdKind::Submit: {
    if (streamingShuttingDown_) return;
    const DemandRequest &req = cmd.demand; const std::uint8_t tgt = static_cast<std::uint8_t>(req.targetStatus);
    if (tgt == 0 && !req.residency) return;
    auto &entry = demands_[packChunkKey(req.pos.x, req.pos.z)]; entry.pos = req.pos;
    if (req.residency) {
      if (entry.residencyTarget > 0 && entry.residencyEpoch == req.epoch) return;
      if (entry.residencyTarget > 0) retireResidencyLocked(entry, false, false, true);
      entry.residencyTarget = tgt; entry.residencyEpoch = req.epoch; entry.gameTime = req.gameTime;
      entry.knownMissingOnDisk = req.knownMissingOnDisk; ++streamingStats_.requested;
    } else {
      if (entry.bufferTarget == tgt && !entry.bufferCompleted) return;
      if (entry.bufferDispatched && tgt > entry.bufferTarget) { if (bufferInFlight_ > 0) --bufferInFlight_; entry.bufferDispatched = false; }
      if (entry.bufferTarget == 0) ++streamingStats_.bufferRequests;
      entry.bufferTarget = tgt; entry.bufferCompleted = false;
      if (entry.residencyTarget == 0) entry.knownMissingOnDisk = req.knownMissingOnDisk;
    }
    break;
  }
  case CtrlCmdKind::Cancel: {
    auto it = demands_.find(packChunkKey(cmd.pos.x, cmd.pos.z));
    if (it == demands_.end()) return;
    DemandEntry &entry = it->second; bool needCancel = false;
    if (cmd.cancelResidency && entry.residencyTarget > 0 && (cmd.onlyIfEpoch == 0 || entry.residencyEpoch == cmd.onlyIfEpoch)) {
      retireResidencyLocked(entry, false, false, true); needCancel = true;
    }
    if (cmd.onlyIfEpoch == 0 && entry.bufferTarget > 0) { retireBufferLocked(entry, false, false); needCancel = true; }
    const std::uint8_t rem = std::max(entry.residencyTarget, entry.bufferTarget); const bool clearRes = (entry.residencyTarget == 0);
    if (rem == 0) demands_.erase(it);
    if (needCancel) toCancel.push_back({cmd.pos, rem, clearRes});
    break;
  }
  case CtrlCmdKind::UpdateCenters: priorityCenters_ = std::move(cmd.centers); keepRadius_ = cmd.keepRadius; break;
  case CtrlCmdKind::Trim:          trimRequested_ = true; break;
  case CtrlCmdKind::Shutdown:
    streamingShuttingDown_ = true;
    for (auto &[key, entry] : demands_) { (void)key; if (entry.residencyTarget > 0) ++streamingStats_.cancelled; toCancel.push_back({entry.pos, 0, true}); }
    demands_.clear(); residencyInFlight_ = bufferInFlight_ = 0; trimRequested_ = false;
    break;
  }
}

void WorldGenScheduler::selectAdmissionsLocked(std::vector<AdmitAction> &toAdmit) {
  if (streamingShuttingDown_ || demands_.empty()) return;
  struct Cand { DemandEntry *entry; std::int64_t sqDist; std::uint8_t distBucket, target; };
  auto admitKind = [&](bool residency, std::size_t slots) {
    if (slots == 0) return;
    std::vector<Cand> cands;
    for (auto &[key, entry] : demands_) {
      (void)key;
      const bool want = residency ? (entry.residencyTarget > 0 && !entry.residencyDispatched)
                                  : (entry.residencyTarget == 0 && entry.bufferTarget > 0 && !entry.bufferDispatched && !entry.bufferCompleted);
      if (want) { const auto [sq, db] = bestCenterDist(entry.pos, priorityCenters_); cands.push_back({&entry, sq, db, residency ? entry.residencyTarget : entry.bufferTarget}); }
    }
    if (cands.empty()) return;
    auto cmp = [residency](const Cand &a, const Cand &b) { return a.sqDist != b.sqDist ? a.sqDist < b.sqDist : (!residency && a.target > b.target); };
    if (cands.size() > slots) { std::partial_sort(cands.begin(), cands.begin() + slots, cands.end(), cmp); cands.resize(slots); }
    else if (cands.size() > 1) std::sort(cands.begin(), cands.end(), cmp);
    const std::uint64_t ts = residency ? nowNs() : 0;
    for (const Cand &c : cands) {
      DemandEntry &e = *c.entry;
      if (residency) { e.residencyDispatched = true; e.armedAtNs = ts; ++residencyInFlight_; }
      else { e.bufferDispatched = true; ++bufferInFlight_; }
      ++streamingStats_.dispatched;
      toAdmit.push_back({e.pos, c.target, c.distBucket, residency, e.knownMissingOnDisk, residency ? e.residencyEpoch : 0, residency ? e.gameTime : 0});
    }
  };
  if (residencyInFlight_ < config_.maxInFlightResidency) admitKind(true, config_.maxInFlightResidency - residencyInFlight_);
  if (bufferInFlight_ < config_.maxInFlightBuffer) admitKind(false, config_.maxInFlightBuffer - bufferInFlight_);
}

void WorldGenScheduler::executeAdmissions(const std::vector<AdmitAction> &toAdmit) {
  std::vector<std::shared_ptr<ChunkHolder>> workStack; workStack.reserve(toAdmit.size());
  for (const AdmitAction &act : toAdmit) {
    try {
      auto holder = getOrCreateHolder(act.pos, act.distBucket, act.knownMissingOnDisk);
      if (armExplicitTarget(holder, act.distBucket, act.targetStatus, act.residency, false, workStack)) continue;
      if (!act.residency) { onHolderProgressedOrIdle(holder, act.targetStatus, 0); continue; }
      bool enq = false;
      {
        std::lock_guard<std::mutex> dlock(demandMutex_);
        auto it = demands_.find(holder->key());
        if (it != demands_.end() && it->second.residencyTarget > 0 && it->second.residencyEpoch == act.epoch &&
            it->second.residencyDispatched && !it->second.residencyFinalizing) {
          it->second.residencyFinalizing = true; activeFinalizeTasks_.fetch_add(1, std::memory_order_acq_rel); enq = true;
        }
      }
      if (enq) enqueueReady(holder, static_cast<std::uint8_t>(ChunkStatusId::Full), act.distBucket, true, 0, true);
    } catch (const std::exception &ex) { onHolderFailed(act.pos, ex.what(), act.residency ? act.epoch : 0); }
  }
  if (!workStack.empty()) armHoldersIterative(std::move(workStack));
}

void WorldGenScheduler::onHolderProgressedOrIdle(
    const std::shared_ptr<ChunkHolder> &holder, std::uint8_t reachedStage, std::uint64_t armedNs) {
  bool batchMatched = false;
  {
    std::lock_guard<std::mutex> block(batchMutex_);
    auto it = activeBatchTargets_.find(holder->key());
    if (it != activeBatchTargets_.end() && reachedStage >= it->second) { activeBatchTargets_.erase(it); batchInFlight_ = activeBatchTargets_.size(); batchMatched = true; }
  }
  if (batchMatched) { recordTargetLatencyUs(elapsedUsSince(armedNs)); batchCv_.notify_all(); }

  bool triggerFinalize = false, admitNew = false;
  {
    std::lock_guard<std::mutex> dlock(demandMutex_);
    auto it = demands_.find(holder->key());
    if (it != demands_.end()) {
      DemandEntry &e = it->second;
      if (e.bufferTarget > 0 && e.bufferDispatched && !e.bufferCompleted && reachedStage >= e.bufferTarget) { retireBufferLocked(e, true, false); admitNew = true; }
      if (e.residencyTarget > 0 && e.residencyDispatched && !e.residencyFinalizing && reachedStage >= e.residencyTarget) {
        e.residencyFinalizing = true; activeFinalizeTasks_.fetch_add(1, std::memory_order_acq_rel); triggerFinalize = true;
      }
      if (e.residencyTarget == 0 && (e.bufferTarget == 0 || e.bufferCompleted)) demands_.erase(it);
    }
  }
  if (admitNew) { controlPending_.store(true, std::memory_order_seq_cst); tryDrainControl(); }
  if (triggerFinalize) finalizeResidencyHolder(holder);
}

void WorldGenScheduler::onHolderFailed(ChunkTicketPos pos, const std::string &detail, std::uint64_t onlyIfEpoch) {
  if (onlyIfEpoch == 0) {
    std::lock_guard<std::mutex> block(batchMutex_);
    if (!activeBatchTargets_.empty()) { activeBatchTargets_.clear(); batchInFlight_ = 0; batchFailed_ = true; batchCv_.notify_all(); }
  }
  bool pushFailure = false, admitNew = false; std::uint64_t failEpoch = 0, armedAtNs = 0;
  {
    std::lock_guard<std::mutex> dlock(demandMutex_);
    auto it = demands_.find(packChunkKey(pos.x, pos.z));
    if (it != demands_.end()) {
      DemandEntry &e = it->second;
      if (e.residencyTarget > 0 && e.residencyDispatched && (onlyIfEpoch == 0 || e.residencyEpoch == onlyIfEpoch)) {
        armedAtNs = e.armedAtNs; failEpoch = e.residencyEpoch; retireResidencyLocked(e, false, true, false); pushFailure = admitNew = true;
      }
      if (onlyIfEpoch == 0 && e.bufferTarget > 0 && e.bufferDispatched) { retireBufferLocked(e, false, true); admitNew = true; }
      if (e.residencyTarget == 0 && e.bufferTarget == 0) demands_.erase(it);
    }
  }
  if (pushFailure) { std::lock_guard<std::mutex> clock(completedMutex_); completedResidency_.push_back({pos, failEpoch, false, detail, {}, {}, {}, elapsedUsSince(armedAtNs)}); }
  if (admitNew) { controlPending_.store(true, std::memory_order_seq_cst); tryDrainControl(); }
}

void WorldGenScheduler::finalizeResidencyHolder(const std::shared_ptr<ChunkHolder> &holder) {
  if (!holder) { decrementFinalizeTasks(); return; }
  struct FinalizeGuard { WorldGenScheduler &s; bool active = true; ~FinalizeGuard() { if (active) s.decrementFinalizeTasks(); } } finalizeGuard{*this, true};
  SpatialRegionGuard spatialGuard(*this);
  {
    std::lock_guard<std::mutex> slock(spatialMutex_);
    if (holder->writeRegionLocked) {
      holder->writeWaiters.push_back({holder, 0, static_cast<std::uint8_t>(ChunkStatusId::Full), 0, true, true});
      finalizeGuard.active = false; return;
    }
    holder->writeRegionLocked = true; holder->writeRegionLockedAtomic.store(true, std::memory_order_release);
    spatialGuard.armSingle(holder);
  }

  std::uint64_t epoch = 0, armedAtNs = 0; std::int64_t gameTime = 0;
  {
    std::lock_guard<std::mutex> dlock(demandMutex_);
    auto it = demands_.find(holder->key());
    if (it == demands_.end() || it->second.residencyTarget == 0 || !it->second.residencyFinalizing) return;
    epoch = it->second.residencyEpoch; gameTime = it->second.gameTime; armedAtNs = it->second.armedAtNs;
  }

  auto proto = holder->protoShared();
  if (!proto) { spatialGuard.release(); onHolderFailed(holder->pos(), "generation scheduler did not retain the generated proto chunk", epoch); return; }
  holder->generationRefCount.fetch_add(1, std::memory_order_acq_rel);
  std::shared_ptr<void> holderPin(holder.get(), [h = holder](void *) { h->generationRefCount.fetch_sub(1, std::memory_order_acq_rel); });
  nbt::RootTag root;
  try { proto->loadEpoch = proto->lightEpoch = epoch; root = encodeProtoChunkToNbt(*proto, gameTime); }
  catch (const std::exception &ex) { holderPin.reset(); spatialGuard.release(); onHolderFailed(holder->pos(), ex.what(), epoch); return; }
  spatialGuard.release();

  const std::uint64_t latencyUs = elapsedUsSince(armedAtNs); bool accepted = false;
  {
    std::lock_guard<std::mutex> dlock(demandMutex_);
    auto it = demands_.find(holder->key());
    if (it != demands_.end() && it->second.residencyTarget > 0 && it->second.residencyEpoch == epoch) {
      retireResidencyLocked(it->second, true, false, false);
      if (it->second.bufferTarget == 0) demands_.erase(it);
      if (++completedSinceTrim_ >= 32) { completedSinceTrim_ = 0; trimRequested_ = true; }
      controlPending_.store(true, std::memory_order_seq_cst); accepted = true;
    }
  }
  if (accepted) {
    recordTargetLatencyUs(latencyUs);
    { std::lock_guard<std::mutex> clock(completedMutex_); completedResidency_.push_back({holder->pos(), epoch, true, {}, std::move(proto), std::move(holderPin), std::move(root), latencyUs}); }
    tryDrainControl();
  }
}

// ---------------------------------------------------------------------------
// Queries, Standalone Install & Eviction
// ---------------------------------------------------------------------------

std::shared_ptr<BaseChunk> WorldGenScheduler::findChunkShared(ChunkTicketPos pos) const { auto h = findHolder(pos); return h ? h->chunkShared() : nullptr; }
BaseChunk *WorldGenScheduler::findChunk(ChunkTicketPos pos) const { return findChunkShared(pos).get(); }

std::shared_ptr<ProtoChunk> WorldGenScheduler::takeProtoChunk(ChunkTicketPos pos) {
  auto holder = findHolder(pos);
  auto proto = holder ? holder->protoShared() : nullptr;
  if (!proto) return nullptr;
  holder->markEvicted(); holder->resetChunk();
  chunkMap_->eraseHolderIf(holder->key(), [&](ChunkHolder &h) { return &h == holder.get(); });
  return proto;
}

bool WorldGenScheduler::generateAndInstallIntoWorld(ChunkTicketPos pos, World &world) {
  ensureStatus(pos, ChunkStatusId::Full);
  if (&world.chunkMap() == chunkMap_) {
    auto holder = findHolder(pos);
    return !holder ? world.isChunkLoaded(pos.x, pos.z)
                   : (holder->heldKind() == HeldChunkKind::World || world.installGeneratedProtoChunk(holder->protoShared()));
  }
  std::shared_ptr<ProtoChunk> proto = takeProtoChunk(pos);
  return proto ? installProtoChunkIntoWorld(std::move(proto), world) : world.isChunkLoaded(pos.x, pos.z);
}

std::size_t WorldGenScheduler::evictIdleOutsideRadius(
    std::span<const ChunkTicketPos> keepCenters, int keepRadius, std::vector<ChunkTicketPos> *evictedOut) {
  std::vector<std::shared_ptr<ChunkHolder>> candidates;
  chunkMap_->holders().for_each([&](const ChunkMap::HolderMap::value_type &e) { if (e.second) candidates.push_back(e.second); });

  std::size_t evicted = 0;
  for (const auto &holder : candidates) {
    if (std::any_of(keepCenters.begin(), keepCenters.end(), [&](const ChunkTicketPos &c) { return chebyshevDist(holder->pos(), c) <= keepRadius; }) ||
        holder->writeRegionLockedAtomic.load(std::memory_order_acquire) || holder->generationRefCount.load(std::memory_order_acquire) > 0)
      continue;

    auto *gen = holder->genState();
    auto isIdleForEvict = [&](std::uint32_t expectedRefs) {
      return !holder->evicted() && holder->heldKind() == HeldChunkKind::Proto &&
             holder->generationRefCount.load(std::memory_order_acquire) == expectedRefs &&
             (!gen || (gen->execState == ChunkHolder::ExecState::Idle && !gen->isActive && !gen->arming &&
                       gen->initState != ChunkHolder::GenTaskState::InitState::Initializing &&
                       gen->pendingDeps.load(std::memory_order_acquire) == 0 && gen->currentStage >= gen->effectiveTarget()));
    };

    std::shared_ptr<ProtoChunk> proto;
    struct EvictPinGuard {
      SpatialRegionGuard sg; ChunkHolder *h = nullptr;
      ~EvictPinGuard() { release(); }
      void release() { if (h) { ChunkHolder *p = std::exchange(h, nullptr); sg.release(); p->generationRefCount.fetch_sub(1, std::memory_order_acq_rel); } }
    } pinGuard{SpatialRegionGuard(*this), nullptr};

    {
      std::unique_lock<std::mutex> hlock; if (gen) hlock = std::unique_lock<std::mutex>(gen->mtx);
      if (isIdleForEvict(0) && (!(proto = holder->protoShared()) || proto->generationUserCount() == 0)) {
        std::lock_guard<std::mutex> slock(spatialMutex_);
        if (!holder->writeRegionLocked && holder->generationRefCount.load(std::memory_order_acquire) == 0) {
          holder->writeRegionLocked = true; holder->writeRegionLockedAtomic.store(true, std::memory_order_release);
          pinGuard.sg.armSingle(holder); holder->generationRefCount.fetch_add(1, std::memory_order_acq_rel); pinGuard.h = holder.get();
        }
      }
    }
    if (!pinGuard.h) continue;
    { std::lock_guard<std::mutex> dlock(demandMutex_); if (demands_.contains(holder->key())) continue; }
    if (diskStore_ != nullptr && proto != nullptr && proto->persistedStatus() != ChunkStatusId::Empty)
      diskStore_->writeChunkAsync(holder->pos(), terrain::encodeChunkNbt(encodeProtoChunkToNbt(*proto)), static_cast<int>(std::time(nullptr)));

    bool canEvict = false;
    {
      std::unique_lock<std::mutex> hlock; if (gen) hlock = std::unique_lock<std::mutex>(gen->mtx);
      if (isIdleForEvict(1) && (!proto || proto->generationUserCount() == 0)) canEvict = holder->tryEvictProto();
    }
    pinGuard.release();
    if (canEvict) {
      const ChunkTicketPos pos = holder->pos();
      chunkMap_->eraseHolderIf(holder->key(), [&](ChunkHolder &h) { return &h == holder.get() && h.evicted() && h.heldKind() == HeldChunkKind::Empty; });
      if (evictedOut != nullptr) evictedOut->push_back(pos);
      ++evicted;
    }
  }
  holdersEvicted_.fetch_add(evicted, std::memory_order_relaxed);
  return evicted;
}

std::size_t WorldGenScheduler::heldChunkCount() const {
  std::size_t count = 0;
  chunkMap_->holders().for_each([&](const ChunkMap::HolderMap::value_type &e) { if (e.second && !e.second->empty()) ++count; });
  return count;
}

void WorldGenScheduler::forEachHeldChunk(const std::function<void(const BaseChunk &)> &visitor) const {
  chunkMap_->holders().for_each([&](const ChunkMap::HolderMap::value_type &e) { if (e.second) if (auto chunk = e.second->chunkShared()) visitor(*chunk); });
}

WorldGenSchedulerStats WorldGenScheduler::stats() const {
  WorldGenSchedulerStats out{
      chunksCreated_.load(std::memory_order_relaxed),      chunksLoadedFromStore_.load(std::memory_order_relaxed),
      stepsExecuted_.load(std::memory_order_relaxed),      inlineSteps_.load(std::memory_order_relaxed),
      parallelDispatches_.load(std::memory_order_relaxed), coloredDispatches_.load(std::memory_order_relaxed),
      holdersEvicted_.load(std::memory_order_relaxed),     spatialConflicts_.load(std::memory_order_relaxed),
      completedTargets_.load(std::memory_order_relaxed),   targetLatencySumUs_.load(std::memory_order_relaxed),
      targetLatencyMaxUs_.load(std::memory_order_relaxed), {}};
  for (std::size_t i = 0; i < out.targetLatencyHistUs.size(); ++i)
    out.targetLatencyHistUs[i] = targetLatencyHistUs_[i].load(std::memory_order_relaxed);
  return out;
}

} // namespace redstone::worldgen
