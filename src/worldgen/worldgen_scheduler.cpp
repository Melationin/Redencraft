#include "redstone/worldgen/worldgen_scheduler.hpp"

#include "redstone/engine.hpp"
#include "redstone/light/service.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <shared_mutex>
#include <stdexcept>

namespace redstone::worldgen {
namespace {

constexpr int kMaxDistBucket = 32;
constexpr int kStageCount = static_cast<int>(kChunkStatusCount); // 12
constexpr int kTotalBuckets = (kMaxDistBucket + 1) * kStageCount; // 396
constexpr std::size_t kBitsetWords = (kTotalBuckets + 63) / 64;   // 7

struct StageScheduleSpec {
  bool hasWait = false;
  int waitRadius = 0;
  ChunkStatusId waitStatus = ChunkStatusId::Empty;
  std::uint8_t writeRadius = 0;
};

StageScheduleSpec scheduleSpecForStage(ChunkStatusId stage, bool loadingPath) {
  if (loadingPath) {
    if (stage == ChunkStatusId::Light) {
      return StageScheduleSpec{true, 1, ChunkStatusId::InitializeLight, 0};
    }
    return StageScheduleSpec{false, 0, ChunkStatusId::Empty, 0};
  }
  switch (stage) {
  case ChunkStatusId::Empty:
  case ChunkStatusId::StructureStarts:
    return StageScheduleSpec{false, 0, ChunkStatusId::Empty, 0};
  case ChunkStatusId::StructureReferences:
    return StageScheduleSpec{true, 8, ChunkStatusId::StructureStarts, 0};
  case ChunkStatusId::Biomes:
    return StageScheduleSpec{false, 0, ChunkStatusId::Empty, 0};
  case ChunkStatusId::Noise:
    return StageScheduleSpec{true, 1, ChunkStatusId::Biomes, 0};
  case ChunkStatusId::Surface:
  case ChunkStatusId::Carvers:
    return StageScheduleSpec{false, 0, ChunkStatusId::Empty, 0};
  case ChunkStatusId::Features:
    return StageScheduleSpec{true, 1, ChunkStatusId::Carvers, 1};
  case ChunkStatusId::InitializeLight:
    return StageScheduleSpec{false, 0, ChunkStatusId::Empty, 0};
  case ChunkStatusId::Light:
    return StageScheduleSpec{true, 1, ChunkStatusId::InitializeLight, 0};
  case ChunkStatusId::Spawn:
  case ChunkStatusId::Full:
    return StageScheduleSpec{false, 0, ChunkStatusId::Empty, 0};
  }
  return StageScheduleSpec{};
}

std::uint64_t packCoordKey(ChunkTicketPos pos) noexcept {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.x))
          << 32) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.z));
}

std::size_t shardIndex(std::uint64_t key, std::size_t shardCount) noexcept {
  std::uint64_t z = key + 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z ^= (z >> 31);
  return static_cast<std::size_t>(z & (shardCount - 1));
}

std::uint8_t computeNineColor(ChunkTicketPos pos) noexcept {
  const int mx = ((pos.x % 3) + 3) % 3;
  const int mz = ((pos.z % 3) + 3) % 3;
  return static_cast<std::uint8_t>(mx * 3 + mz);
}

int chebyshevDist(ChunkTicketPos a, ChunkTicketPos b) noexcept {
  const int dx = a.x >= b.x ? (a.x - b.x) : (b.x - a.x);
  const int dz = a.z >= b.z ? (a.z - b.z) : (b.z - a.z);
  return std::max(dx, dz);
}

int clampDistBucket(int dist) noexcept {
  return std::clamp(dist, 0, kMaxDistBucket);
}

} // namespace

enum class ExecState : std::uint8_t {
  Idle = 0,
  WaitingDeps = 1,
  Queued = 2,
  Running = 3,
};

struct WorldGenScheduler::ChunkHolder {
  explicit ChunkHolder(ChunkTicketPos p, std::uint8_t col)
      : pos(p), color(col) {}

  std::uint8_t effectiveTarget() const noexcept {
    return std::max(explicitTarget, dependentTarget);
  }

  ChunkTicketPos pos;
  std::uint8_t color = 0;

  std::mutex mtx;
  std::unique_ptr<ChunkAccess> chunk;
  std::uint8_t currentStage = 0;
  std::uint8_t initialPersistedStage = 0;
  std::uint8_t explicitTarget = 0;
  std::uint8_t dependentTarget = 0;
  std::uint8_t pendingStage = 0;
  std::uint8_t pendingWriteRadius = 0;
  std::uint8_t distBucket = kMaxDistBucket;
  ExecState execState = ExecState::Idle;
  bool fromLoaded = false;
  bool r8StructuresEnsured = false;
  std::uint32_t generationId = 0;
  std::atomic<std::uint32_t> pendingDeps{0};

  std::array<std::vector<std::shared_ptr<ChunkHolder>>, kChunkStatusCount>
      waitersByStage;
  std::vector<ChunkAccess *> cachedR8View;
};

struct WorldGenScheduler::HolderShard {
  mutable std::shared_mutex mtx;
  std::unordered_map<std::uint64_t, std::shared_ptr<ChunkHolder>> map;
};

struct WorldGenScheduler::ReadyEntry {
  std::shared_ptr<ChunkHolder> holder;
  std::uint32_t genId = 0;
};

class WorldGenScheduler::PriorityBucketQueue {
public:
  bool empty() const noexcept {
    for (std::uint64_t w : nonEmpty_) {
      if (w != 0)
        return false;
    }
    return true;
  }

  void push(int distBucket, std::uint8_t nextStage,
            std::shared_ptr<ChunkHolder> holder, std::uint32_t genId) {
    const int d = std::clamp(distBucket, 0, kMaxDistBucket);
    const int s = std::clamp(static_cast<int>(nextStage), 0, kStageCount - 1);
    const int idx = d * kStageCount + (kStageCount - 1 - s);
    buckets_[static_cast<std::size_t>(idx)].push_back(
        ReadyEntry{std::move(holder), genId});
    nonEmpty_[static_cast<std::size_t>(idx >> 6)] |=
        (std::uint64_t{1} << (idx & 63));
  }

  bool pop(ReadyEntry &out) {
    for (std::size_t w = 0; w < kBitsetWords; ++w) {
      const std::uint64_t word = nonEmpty_[w];
      if (word == 0)
        continue;
      const int bit = std::countr_zero(word);
      const std::size_t idx = (w << 6) | static_cast<std::size_t>(bit);
      auto &q = buckets_[idx];
      out = std::move(q.front());
      q.pop_front();
      if (q.empty()) {
        nonEmpty_[w] &= ~(std::uint64_t{1} << bit);
      }
      return true;
    }
    return false;
  }

private:
  std::array<std::uint64_t, kBitsetWords> nonEmpty_{};
  std::array<std::deque<ReadyEntry>, kTotalBuckets> buckets_;
};

WorldGenScheduler::WorldGenScheduler(ChunkGenerator &generator,
                                     std::int64_t worldSeed,
                                     RawChunkStore *diskStore,
                                     World *residentWorld,
                                     LightService *lightService,
                                     WorldGenSchedulerConfig config)
    : generator_(generator), worldSeed_(worldSeed), diskStore_(diskStore),
      residentWorld_(residentWorld), lightService_(lightService),
      config_(config), shards_(std::make_unique<HolderShard[]>(kShardCount)),
      parallelQueue_(std::make_unique<PriorityBucketQueue>()),
      coloredQueues_(std::make_unique<PriorityBucketQueue[]>(9)) {
  if (config_.threadCount == 0) {
    const unsigned hc = std::thread::hardware_concurrency();
    config_.threadCount = hc > 0 ? static_cast<std::size_t>(hc) : 4;
  }
  workers_.reserve(config_.threadCount);
  for (std::size_t i = 0; i < config_.threadCount; ++i) {
    workers_.emplace_back([this]() { workerLoop(); });
  }
}

WorldGenScheduler::~WorldGenScheduler() {
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    stop_ = true;
  }
  queueCv_.notify_all();
  for (std::thread &t : workers_) {
    if (t.joinable())
      t.join();
  }
}

LightService *WorldGenScheduler::activeLightService() const noexcept {
  if (lightService_ != nullptr)
    return lightService_;
  return residentWorld_ ? residentWorld_->lightService() : nullptr;
}

std::shared_ptr<WorldGenScheduler::ChunkHolder>
WorldGenScheduler::findHolder(ChunkTicketPos pos) const {
  const std::uint64_t key = packCoordKey(pos);
  HolderShard &shard = shards_[shardIndex(key, kShardCount)];
  std::shared_lock<std::shared_mutex> lock(shard.mtx);
  auto it = shard.map.find(key);
  return it != shard.map.end() ? it->second : nullptr;
}

std::shared_ptr<WorldGenScheduler::ChunkHolder>
WorldGenScheduler::getOrCreateHolder(ChunkTicketPos pos,
                                       std::uint8_t distBucket) {
  const std::uint64_t key = packCoordKey(pos);
  HolderShard &shard = shards_[shardIndex(key, kShardCount)];
  {
    std::shared_lock<std::shared_mutex> rlock(shard.mtx);
    if (auto it = shard.map.find(key); it != shard.map.end()) {
      return it->second;
    }
  }

  // Create outside shard lock only if needed? Hold unique_lock on shard after
  // loading/creating chunk, keeping storeMutex_ strictly ordered outside shard.mtx.
  std::unique_ptr<ChunkAccess> loadedChunk;
  bool fromLoaded = false;
  if (residentWorld_ != nullptr || diskStore_ != nullptr) {
    std::lock_guard<std::mutex> slock(storeMutex_);
    if (residentWorld_ != nullptr &&
        residentWorld_->isChunkLoaded(pos.x, pos.z)) {
      loadedChunk =
          std::make_unique<ImposterProtoChunk>(*residentWorld_, pos, false);
      fromLoaded = true;
    } else if (diskStore_ != nullptr) {
      const StoreReadResult raw = diskStore_->readChunk(pos);
      if (raw.status == StoreReadStatus::Found) {
        std::string err;
        auto decoded = decodeProtoChunkNbt(raw.nbt, generator_.chunkMinY(),
                                           generator_.chunkHeight(), err);
        if (!decoded) {
          throw std::runtime_error("failed to decode persisted chunk (" +
                                   std::to_string(pos.x) + "," +
                                   std::to_string(pos.z) + "): " + err);
        }
        loadedChunk = std::make_unique<ProtoChunk>(std::move(*decoded));
        fromLoaded = true;
        chunksLoadedFromStore_.fetch_add(1, std::memory_order_relaxed);
      } else if (raw.status != StoreReadStatus::Missing) {
        throw std::runtime_error("disk chunk read error: " + raw.detail);
      }
    }
  }

  if (!loadedChunk) {
    loadedChunk = std::make_unique<ProtoChunk>(pos, generator_.chunkMinY(),
                                               generator_.chunkHeight());
  }

  auto candidate =
      std::make_shared<ChunkHolder>(pos, computeNineColor(pos));
  candidate->chunk = std::move(loadedChunk);
  const auto initSt =
      static_cast<std::uint8_t>(candidate->chunk->persistedStatus());
  candidate->currentStage = initSt;
  candidate->initialPersistedStage = initSt;
  candidate->fromLoaded = fromLoaded;
  candidate->distBucket = distBucket;
  if (fromLoaded && initSt >= static_cast<std::uint8_t>(ChunkStatusId::Spawn)) {
    candidate->r8StructuresEnsured = true;
  }

  std::unique_lock<std::shared_mutex> wlock(shard.mtx);
  auto [it, inserted] = shard.map.emplace(key, candidate);
  if (inserted) {
    if (!fromLoaded) {
      chunksCreated_.fetch_add(1, std::memory_order_relaxed);
    }
    return candidate;
  }
  return it->second;
}

ChunkAccess &WorldGenScheduler::ensureStatus(ChunkTicketPos pos,
                                             ChunkStatusId targetStatus) {
  const ChunkBatchRequest req{pos, targetStatus};
  ensureStatusBatch(std::span<const ChunkBatchRequest>(&req, 1), pos);
  auto h = findHolder(pos);
  if (!h || !h->chunk) {
    throw std::runtime_error("WorldGenScheduler missing completed chunk");
  }
  return *h->chunk;
}

void WorldGenScheduler::ensureStatusBatch(
    std::span<const ChunkBatchRequest> requests,
    ChunkTicketPos priorityAnchor) {
  if (requests.empty())
    return;

  std::vector<std::shared_ptr<ChunkHolder>> toArm;
  toArm.reserve(requests.size());

  for (const ChunkBatchRequest &req : requests) {
    const int d = req.pos == priorityAnchor
                      ? 0
                      : clampDistBucket(chebyshevDist(req.pos, priorityAnchor));
    auto h = getOrCreateHolder(req.pos, static_cast<std::uint8_t>(d));
    const std::uint8_t tgt = static_cast<std::uint8_t>(req.targetStatus);
    std::lock_guard<std::mutex> hlock(h->mtx);
    if (d < h->distBucket)
      h->distBucket = static_cast<std::uint8_t>(d);
    if (h->explicitTarget < tgt)
      h->explicitTarget = tgt;
    if (h->currentStage < h->effectiveTarget() &&
        h->execState == ExecState::Idle) {
      h->execState = ExecState::WaitingDeps;
      activeHolders_.fetch_add(1, std::memory_order_acq_rel);
      toArm.push_back(h);
    }
  }

  if (!toArm.empty()) {
    armHoldersIterative(std::move(toArm));
  }
  waitUntilIdle();
}

void WorldGenScheduler::armHoldersIterative(
    std::vector<std::shared_ptr<ChunkHolder>> workStack) {
  while (!workStack.empty()) {
    auto holder = std::move(workStack.back());
    workStack.pop_back();

    std::uint8_t nextStage = 0;
    std::uint8_t writeRadius = 0;
    std::uint8_t distBucket = kMaxDistBucket;
    int waitRadius = 0;
    ChunkStatusId waitStatus = ChunkStatusId::Empty;
    bool hasPrimaryWait = false;
    bool needR8Ensure = false;

    {
      std::lock_guard<std::mutex> lock(holder->mtx);
      if (holder->execState != ExecState::WaitingDeps)
        continue;
      if (holder->currentStage >= holder->effectiveTarget()) {
        holder->execState = ExecState::Idle;
        releaseActiveHolder();
        continue;
      }

      nextStage = static_cast<std::uint8_t>(holder->currentStage + 1);
      const auto nextEnum = static_cast<ChunkStatusId>(nextStage);
      const bool loadingPath =
          holder->fromLoaded &&
          holder->initialPersistedStage >=
              static_cast<std::uint8_t>(ChunkStatusId::Spawn);
      const StageScheduleSpec spec =
          scheduleSpecForStage(nextEnum, loadingPath);

      hasPrimaryWait = spec.hasWait;
      waitRadius = spec.waitRadius;
      waitStatus = spec.waitStatus;
      writeRadius = spec.writeRadius;
      distBucket = holder->distBucket;

      if (!loadingPath && nextStage >= 2 && nextStage <= 7 &&
          !holder->r8StructuresEnsured) {
        needR8Ensure = true;
        holder->r8StructuresEnsured = true;
      }

      holder->pendingStage = nextStage;
      holder->pendingWriteRadius = writeRadius;
      // +1 self-sentinel keeps pendingDeps >= 1 throughout neighbor scanning.
      holder->pendingDeps.store(1, std::memory_order_release);
    }

    const int scanRadius = needR8Ensure ? std::max(waitRadius, 8) : waitRadius;
    if ((hasPrimaryWait || needR8Ensure) && scanRadius > 0) {
      for (int dz = -scanRadius; dz <= scanRadius; ++dz) {
        for (int dx = -scanRadius; dx <= scanRadius; ++dx) {
          if (dx == 0 && dz == 0)
            continue;
          const int dist = std::max(std::abs(dx), std::abs(dz));
          std::uint8_t reqStage = 0;
          if (hasPrimaryWait && dist <= waitRadius) {
            reqStage = std::max(reqStage, static_cast<std::uint8_t>(waitStatus));
          }
          if (needR8Ensure && dist <= 8) {
            reqStage = std::max(
                reqStage,
                static_cast<std::uint8_t>(ChunkStatusId::StructureStarts));
          }
          if (reqStage == 0)
            continue;

          const ChunkTicketPos nPos{holder->pos.x + dx, holder->pos.z + dz};
          const int nBucket =
              clampDistBucket(static_cast<int>(distBucket) + dist);
          auto neighbor =
              getOrCreateHolder(nPos, static_cast<std::uint8_t>(nBucket));

          // CRITICAL: increment holder->pendingDeps BEFORE registering on neighbor,
          // so any concurrent neighbor completion sees our +1 and never underflows.
          holder->pendingDeps.fetch_add(1, std::memory_order_acq_rel);

          bool registered = false;
          bool armNeighbor = false;
          {
            std::lock_guard<std::mutex> nlock(neighbor->mtx);
            if (nBucket < neighbor->distBucket)
              neighbor->distBucket = static_cast<std::uint8_t>(nBucket);
            if (neighbor->currentStage < reqStage) {
              neighbor->waitersByStage[reqStage].push_back(holder);
              registered = true;
              if (neighbor->dependentTarget < reqStage)
                neighbor->dependentTarget = reqStage;
              if (neighbor->execState == ExecState::Idle &&
                  neighbor->currentStage < neighbor->effectiveTarget()) {
                neighbor->execState = ExecState::WaitingDeps;
                activeHolders_.fetch_add(1, std::memory_order_acq_rel);
                armNeighbor = true;
              }
            }
          }

          if (!registered) {
            // Already satisfied; safely drop the speculative +1 (self-sentinel >= 1 remains).
            holder->pendingDeps.fetch_sub(1, std::memory_order_acq_rel);
          } else if (armNeighbor) {
            workStack.push_back(std::move(neighbor));
          }
        }
      }
    }

    // Release +1 self-sentinel; if it reaches 0, every dependency is already done.
    if (holder->pendingDeps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::uint32_t genId = 0;
      std::uint8_t curDist = kMaxDistBucket;
      bool shouldEnqueue = false;
      {
        std::lock_guard<std::mutex> lock(holder->mtx);
        if (holder->currentStage >= holder->effectiveTarget()) {
          holder->execState = ExecState::Idle;
        } else {
          holder->execState = ExecState::Queued;
          genId = ++holder->generationId;
          curDist = holder->distBucket;
          shouldEnqueue = true;
        }
      }
      if (shouldEnqueue) {
        enqueueReady(holder, nextStage, writeRadius, curDist, genId);
      } else {
        releaseActiveHolder();
      }
    }
  }
}

void WorldGenScheduler::enqueueReady(const std::shared_ptr<ChunkHolder> &holder,
                                     std::uint8_t nextStage,
                                     std::uint8_t writeRadius,
                                     std::uint8_t distBucket,
                                     std::uint32_t genId) {
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (writeRadius == 0) {
      parallelQueue_->push(distBucket, nextStage, holder, genId);
      parallelDispatches_.fetch_add(1, std::memory_order_relaxed);
    } else {
      coloredQueues_[holder->color].push(distBucket, nextStage, holder, genId);
      coloredDispatches_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  queueCv_.notify_one();
}

void WorldGenScheduler::workerLoop() {
  for (;;) {
    ReadyEntry entry;
    bool wasColored = false;

    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait(lock, [this]() {
        if (stop_)
          return true;
        if (!parallelQueue_->empty())
          return true;
        if (!coloredQueues_[activeColor_].empty() &&
            activeColorDispatched_ < config_.colorTurnDispatchLimit) {
          return true;
        }
        if (activeColorInFlight_ == 0) {
          for (int c = 0; c < 9; ++c) {
            if (!coloredQueues_[c].empty())
              return true;
          }
        }
        return false;
      });

      if (stop_)
        return;

      // Prefer active color wavefront when ready, with fairness rotation cap.
      bool popped = false;
      bool otherColorHasWork = false;
      for (int off = 1; off < 9; ++off) {
        if (!coloredQueues_[(activeColor_ + off) % 9].empty()) {
          otherColorHasWork = true;
          break;
        }
      }

      if (!coloredQueues_[activeColor_].empty() &&
          (!otherColorHasWork ||
           activeColorDispatched_ < config_.colorTurnDispatchLimit)) {
        popped = coloredQueues_[activeColor_].pop(entry);
        if (popped) {
          wasColored = true;
          ++activeColorInFlight_;
          ++activeColorDispatched_;
        }
      } else if (activeColorInFlight_ == 0) {
        for (int off = 1; off <= 9; ++off) {
          const std::uint8_t cand =
              static_cast<std::uint8_t>((activeColor_ + off) % 9);
          if (!coloredQueues_[cand].empty()) {
            activeColor_ = cand;
            activeColorDispatched_ = 0;
            popped = coloredQueues_[cand].pop(entry);
            if (popped) {
              wasColored = true;
              ++activeColorInFlight_;
              ++activeColorDispatched_;
            }
            break;
          }
        }
      }

      // Fill any color-switch gap using lock-free parallel tasks (Stages 0..6, 9..11).
      if (!popped && !parallelQueue_->empty()) {
        popped = parallelQueue_->pop(entry);
      }

      if (!popped)
        continue;
    }

    executeHolder(entry.holder, entry.genId, wasColored);
  }
}

void WorldGenScheduler::executeHolder(
    const std::shared_ptr<ChunkHolder> &holder, std::uint32_t expectedGenId,
    bool wasColored) {
  auto releaseColorFlight = [this, &wasColored]() {
    if (!wasColored)
      return;
    wasColored = false;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (activeColorInFlight_ > 0)
        --activeColorInFlight_;
    }
    queueCv_.notify_all();
  };

  std::uint8_t cur = 0;
  bool loadingPath = false;
  {
    std::lock_guard<std::mutex> lock(holder->mtx);
    if (holder->execState != ExecState::Queued ||
        holder->generationId != expectedGenId) {
      releaseColorFlight();
      return;
    }
    if (holder->currentStage >= holder->effectiveTarget()) {
      holder->execState = ExecState::Idle;
      releaseColorFlight();
      releaseActiveHolder();
      return;
    }
    holder->execState = ExecState::Running;
    cur = holder->currentStage;
    loadingPath =
        holder->fromLoaded &&
        holder->initialPersistedStage >=
            static_cast<std::uint8_t>(ChunkStatusId::Spawn);
  }

  const ChunkPyramid &pyramid = loadingPath ? ChunkPyramid::loadingPyramid()
                                            : ChunkPyramid::generationPyramid();

  for (;;) {
    const std::uint8_t nextStage = static_cast<std::uint8_t>(cur + 1);
    const auto nextEnum = static_cast<ChunkStatusId>(nextStage);

    runSingleStage(*holder, nextEnum, pyramid);
    stepsExecuted_.fetch_add(1, std::memory_order_relaxed);

    std::vector<std::shared_ptr<ChunkHolder>> waiters;
    bool inlineNext = false;
    bool needScheduleNext = false;
    bool finished = false;

    {
      std::lock_guard<std::mutex> lock(holder->mtx);
      holder->currentStage = nextStage;
      waiters = std::move(holder->waitersByStage[nextStage]);
      holder->waitersByStage[nextStage].clear();

      if (nextStage < holder->effectiveTarget()) {
        const auto upcomingEnum =
            static_cast<ChunkStatusId>(nextStage + 1);
        const StageScheduleSpec nextSpec =
            scheduleSpecForStage(upcomingEnum, loadingPath);
        const bool needsR8 =
            !loadingPath && (nextStage + 1) >= 2 && (nextStage + 1) <= 7 &&
            !holder->r8StructuresEnsured;
        if (!nextSpec.hasWait && nextSpec.writeRadius == 0 && !needsR8) {
          inlineNext = true;
          cur = nextStage;
        } else {
          holder->execState = ExecState::WaitingDeps;
          needScheduleNext = true;
        }
      } else {
        holder->execState = ExecState::Idle;
        finished = true;
      }
    }

    if (inlineNext) {
      if (!waiters.empty()) {
        notifyWaiters(waiters);
      }
      inlineSteps_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    // Release 9-color flight slot before waking waiters or lowering activeHolders_
    // so next color turn & waitUntilIdle() observe clean completion ordering.
    releaseColorFlight();
    if (!waiters.empty()) {
      notifyWaiters(waiters);
    }
    if (needScheduleNext) {
      std::vector<std::shared_ptr<ChunkHolder>> nextWork;
      nextWork.push_back(holder);
      armHoldersIterative(std::move(nextWork));
      break;
    }
    if (finished) {
      releaseActiveHolder();
      break;
    }
  }
}

void WorldGenScheduler::runSingleStage(ChunkHolder &holder,
                                       ChunkStatusId nextStage,
                                       const ChunkPyramid &pyramid) {
  auto *proto = dynamic_cast<ProtoChunk *>(holder.chunk.get());
  if (!proto)
    return;

  const ChunkStep &step = pyramid.getStepTo(nextStage);
  const int radius = step.directDependencies().getRadius();
  const int stride = radius * 2 + 1;
  const std::size_t expectedSize = static_cast<std::size_t>(stride) * stride;

  std::vector<ChunkAccess *> squareCache;
  const bool useR8Cache =
      (radius == 8 && !holder.cachedR8View.empty() &&
       holder.cachedR8View.size() == expectedSize);

  if (useR8Cache) {
    squareCache = holder.cachedR8View;
  } else {
    squareCache.assign(expectedSize, nullptr);
    for (int dz = -radius; dz <= radius; ++dz) {
      for (int dx = -radius; dx <= radius; ++dx) {
        const std::size_t idx =
            static_cast<std::size_t>(dz + radius) * stride +
            static_cast<std::size_t>(dx + radius);
        if (dx == 0 && dz == 0) {
          squareCache[idx] = holder.chunk.get();
          continue;
        }
        const int dist = std::max(std::abs(dx), std::abs(dz));
        if (static_cast<std::size_t>(dist) < step.directDependencies().size()) {
          if (auto nh =
                  findHolder({holder.pos.x + dx, holder.pos.z + dz})) {
            squareCache[idx] = nh->chunk.get();
          }
        }
      }
    }
    if (radius == 8 &&
        static_cast<int>(nextStage) >=
            static_cast<int>(ChunkStatusId::StructureReferences) &&
        static_cast<int>(nextStage) <=
            static_cast<int>(ChunkStatusId::Features)) {
      holder.cachedR8View = squareCache;
    }
  }

  WorldGenRegion region(step, *holder.chunk, std::move(squareCache), radius,
                        worldSeed_);

  LightService *ls = activeLightService();
  if (ls != nullptr && (nextStage == ChunkStatusId::InitializeLight ||
                        nextStage == ChunkStatusId::Light)) {
    std::lock_guard<std::mutex> llock(lightMutex_);
    executeChunkGenerationStep(generator_, region, *proto, nextStage,
                               worldSeed_, ls);
  } else {
    executeChunkGenerationStep(generator_, region, *proto, nextStage,
                               worldSeed_, nullptr);
  }

  if (nextStage == ChunkStatusId::Features && !holder.cachedR8View.empty()) {
    std::vector<ChunkAccess *>{}.swap(holder.cachedR8View);
  }
}

void WorldGenScheduler::notifyWaiters(
    std::vector<std::shared_ptr<ChunkHolder>> &waiters) {
  for (auto &w : waiters) {
    if (w->pendingDeps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::uint8_t nextStage = 0;
      std::uint8_t writeRadius = 0;
      std::uint8_t distBucket = kMaxDistBucket;
      std::uint32_t genId = 0;
      bool shouldEnqueue = false;
      {
        std::lock_guard<std::mutex> wlock(w->mtx);
        if (w->currentStage >= w->effectiveTarget()) {
          w->execState = ExecState::Idle;
        } else {
          w->execState = ExecState::Queued;
          nextStage = w->pendingStage;
          writeRadius = w->pendingWriteRadius;
          distBucket = w->distBucket;
          genId = ++w->generationId;
          shouldEnqueue = true;
        }
      }
      if (shouldEnqueue) {
        enqueueReady(w, nextStage, writeRadius, distBucket, genId);
      } else {
        releaseActiveHolder();
      }
    }
  }
}

void WorldGenScheduler::releaseActiveHolder() {
  if (activeHolders_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    {
      std::lock_guard<std::mutex> lock(idleMutex_);
    }
    idleCv_.notify_all();
  }
}

void WorldGenScheduler::waitUntilIdle() {
  std::unique_lock<std::mutex> lock(idleMutex_);
  idleCv_.wait(lock, [this]() {
    return activeHolders_.load(std::memory_order_acquire) == 0;
  });
}

ChunkAccess *WorldGenScheduler::findChunk(ChunkTicketPos pos) const {
  auto h = findHolder(pos);
  return h ? h->chunk.get() : nullptr;
}

std::optional<ProtoChunk> WorldGenScheduler::takeProtoChunk(ChunkTicketPos pos) {
  const std::uint64_t key = packCoordKey(pos);
  HolderShard &shard = shards_[shardIndex(key, kShardCount)];
  std::unique_lock<std::shared_mutex> lock(shard.mtx);
  auto it = shard.map.find(key);
  if (it == shard.map.end())
    return std::nullopt;
  auto h = it->second;
  auto *proto = dynamic_cast<ProtoChunk *>(h->chunk.get());
  if (!proto)
    return std::nullopt;
  ProtoChunk moved(std::move(*proto));
  shard.map.erase(it);
  return moved;
}

bool WorldGenScheduler::generateAndInstallIntoWorld(ChunkTicketPos pos,
                                                    World &world) {
  ensureStatus(pos, ChunkStatusId::Full);
  auto protoOpt = takeProtoChunk(pos);
  if (!protoOpt)
    return world.isChunkLoaded(pos.x, pos.z);
  return installProtoChunkIntoWorld(std::move(*protoOpt), world);
}

std::size_t WorldGenScheduler::evictIdleOutsideRadius(
    std::span<const ChunkTicketPos> keepCenters, int keepRadius) {
  std::size_t evicted = 0;
  for (std::size_t s = 0; s < kShardCount; ++s) {
    HolderShard &shard = shards_[s];
    std::unique_lock<std::shared_mutex> lock(shard.mtx);
    for (auto it = shard.map.begin(); it != shard.map.end();) {
      ChunkHolder &h = *it->second;
      bool canEvict = false;
      {
        std::lock_guard<std::mutex> hlock(h.mtx);
        if (h.execState == ExecState::Idle) {
          bool nearKeep = false;
          for (const ChunkTicketPos &c : keepCenters) {
            if (chebyshevDist(h.pos, c) <= keepRadius) {
              nearKeep = true;
              break;
            }
          }
          if (!nearKeep)
            canEvict = true;
        }
      }
      if (canEvict) {
        it = shard.map.erase(it);
        ++evicted;
      } else {
        ++it;
      }
    }
  }
  holdersEvicted_.fetch_add(evicted, std::memory_order_relaxed);
  return evicted;
}

std::size_t WorldGenScheduler::heldChunkCount() const {
  std::size_t count = 0;
  for (std::size_t s = 0; s < kShardCount; ++s) {
    std::shared_lock<std::shared_mutex> lock(shards_[s].mtx);
    for (const auto &entry : shards_[s].map) {
      if (entry.second->chunk)
        ++count;
    }
  }
  return count;
}

void WorldGenScheduler::forEachHeldChunk(
    const std::function<void(const ChunkAccess &)> &visitor) const {
  for (std::size_t s = 0; s < kShardCount; ++s) {
    std::shared_lock<std::shared_mutex> lock(shards_[s].mtx);
    for (const auto &entry : shards_[s].map) {
      if (entry.second->chunk)
        visitor(*entry.second->chunk);
    }
  }
}

WorldGenSchedulerStats WorldGenScheduler::stats() const {
  return WorldGenSchedulerStats{
      chunksCreated_.load(std::memory_order_relaxed),
      chunksLoadedFromStore_.load(std::memory_order_relaxed),
      stepsExecuted_.load(std::memory_order_relaxed),
      inlineSteps_.load(std::memory_order_relaxed),
      parallelDispatches_.load(std::memory_order_relaxed),
      coloredDispatches_.load(std::memory_order_relaxed),
      holdersEvicted_.load(std::memory_order_relaxed),
  };
}

} // namespace redstone::worldgen
