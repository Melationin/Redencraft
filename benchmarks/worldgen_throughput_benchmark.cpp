// 离线世界生成吞吐基准：在内存中连续生成 size x size 个 FULL 区块（不写盘、不安装 World），
// 统计墙钟/CPU/每区块耗时与调度器统计，用于优化前后的同机对比。

#include "redstone/allocator.hpp"
#include "redstone/dimension.hpp"
#include "redstone/engine.hpp"
#include "redstone/worldgen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <sys/resource.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace redstone;
using namespace redstone::worldgen;

std::unique_ptr<NoiseBasedChunkGenerator>
makeGenerator(const WorldgenRegistries &registries, const std::string &dim) {
  if (dim == "nether") {
    const auto *settings = registries.findNoiseSettings("minecraft:nether");
    if (!settings)
      throw std::runtime_error("missing minecraft:nether noise settings");
    return std::make_unique<NoiseBasedChunkGenerator>(
        registries, *settings,
        std::make_shared<MultiNoiseBiomeSource>(
            MultiNoiseBiomeSource::createNether()),
        netherDimensionType().minY, netherDimensionType().height);
  }
  if (dim == "end") {
    const auto *settings = registries.findNoiseSettings("minecraft:end");
    if (!settings)
      throw std::runtime_error("missing minecraft:end noise settings");
    return std::make_unique<NoiseBasedChunkGenerator>(
        registries, *settings, std::make_shared<TheEndBiomeSource>(),
        endDimensionType().minY, endDimensionType().height);
  }
  const auto *settings = registries.findNoiseSettings("minecraft:overworld");
  if (!settings)
    throw std::runtime_error("missing minecraft:overworld noise settings");
  return std::make_unique<NoiseBasedChunkGenerator>(
      registries, *settings,
      std::make_shared<MultiNoiseBiomeSource>(
          MultiNoiseBiomeSource::createOverworld()),
      overworldDimensionType().minY, overworldDimensionType().height);
}

double processCpuSeconds() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return static_cast<double>(usage.ru_utime.tv_sec) +
         static_cast<double>(usage.ru_utime.tv_usec) / 1.0e6 +
         static_cast<double>(usage.ru_stime.tv_sec) +
         static_cast<double>(usage.ru_stime.tv_usec) / 1.0e6;
}

long statusKb(const char *field) {
  FILE *file = std::fopen("/proc/self/status", "r");
  if (!file)
    return -1;
  const std::size_t fieldLen = std::strlen(field);
  char line[256];
  long value = -1;
  while (std::fgets(line, sizeof(line), file)) {
    if (std::strncmp(line, field, fieldLen) == 0) {
      std::sscanf(line + fieldLen, "%ld", &value);
      break;
    }
  }
  std::fclose(file);
  return value;
}

long peakRssKb() { return statusKb("VmHWM:"); }
long currentRssKb() { return statusKb("VmRSS:"); }

#ifdef REDSTONE_USE_JEMALLOC
std::size_t jemallocStat(const char *name) {
  std::size_t value = 0;
  std::size_t len = sizeof(value);
  if (je_mallctl(name, &value, &len, nullptr, 0) != 0)
    return 0;
  return value;
}
#endif

#ifdef REDSTONE_USE_JEMALLOC
bool dumpJemallocHeapProfile(const char *path) {
  const char *name = path;
  return je_mallctl("prof.dump", nullptr, nullptr, &name, sizeof(name)) == 0;
}

void dumpJemallocStats(const char *path) {
  FILE *file = std::fopen(path, "w");
  if (!file)
    return;
  je_malloc_stats_print(
      [](void *opaque, const char *text) {
        std::fputs(text, static_cast<FILE *>(opaque));
      },
      file, "J");
  std::fclose(file);
}
#endif

DimensionTypeSpec dimensionFor(const std::string &dim) {
  if (dim == "nether")
    return netherDimensionType();
  if (dim == "end")
    return endDimensionType();
  return overworldDimensionType();
}

void printMemLine(const char *label, std::size_t bytes, double rssBytes) {
  std::printf("  %-26s %9.1f MB  %5.1f%%\n", label, bytes / 1048576.0,
              rssBytes > 0.0 ? 100.0 * static_cast<double>(bytes) / rssBytes
                             : 0.0);
}

struct FlyRunResult {
  double speedBlocksPerSec = 0.0;
  double speedChunksPerSec = 0.0;
  double durationSec = 0.0;
  double playerChunkReadyPct = 0.0;
  double avgViewReadyPct = 0.0;
  std::size_t backlogMid = 0;
  std::size_t backlogEnd = 0;
  std::size_t submitted = 0;
  std::size_t completed = 0;
  std::size_t cancelled = 0;
  std::size_t failed = 0;
  double arrivalCps = 0.0;
  double completedCps = 0.0;
  double latP50Ms = 0.0;
  double latP95Ms = 0.0;
  double latP99Ms = 0.0;
  double latMaxMs = 0.0;
  double rssMb = 0.0;
  bool cleanHorizonPass = false;
  bool throughputPass = false;
};

DimensionTypeSpec dimensionSpecForName(const std::string &dim) {
  if (dim == "nether")
    return netherDimensionType();
  if (dim == "end")
    return endDimensionType();
  return overworldDimensionType();
}

double percentileFromSorted(const std::vector<double> &sorted, double p) {
  if (sorted.empty())
    return 0.0;
  const double idx = p * static_cast<double>(sorted.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(idx));
  const auto hi = static_cast<std::size_t>(std::ceil(idx));
  if (lo == hi)
    return sorted[lo];
  const double frac = idx - static_cast<double>(lo);
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

FlyRunResult runFlySpeedTrial(const WorldgenRegistries &registries,
                              const std::string &dim, std::int64_t seed,
                              int threads, bool strongholds, int viewDistance,
                              double flyDurationSec, double speedBlocksPerSec,
                              bool withLight) {
  auto generator = makeGenerator(registries, dim);
  if (!strongholds)
    generator->setConcentricRingsEnabled(false);

  const DimensionTypeSpec dimSpec = dimensionSpecForName(dim);
  std::unique_ptr<World> world;
  std::shared_ptr<LightOpticalContext> lightContext;
  GeneratedLightQueue lightQueue;
  if (withLight) {
    world = std::make_unique<World>(dimSpec.minY, dimSpec.minY + dimSpec.height);
    LightWorkerOptions lightOptions;
    lightOptions.batchWindowMs = 0;
    lightOptions.hasSkyLight = dimSpec.hasSkyLight;
    world->enableLighting(lightOptions);
    lightContext = std::make_shared<LightOpticalContext>();
    lightContext->set(world->lightService()->opticalTable(),
                      world->lightService()->skyLightEnabled());
  }

  WorldGenSchedulerConfig config;
  if (threads > 0)
    config.threadCount = static_cast<std::size_t>(threads);

  WorldGenScheduler scheduler(*generator, seed, nullptr, nullptr, lightContext,
                              withLight ? &lightQueue : nullptr, config);

  const int side = 2 * viewDistance + 1;
  const std::size_t viewSquareCount =
      static_cast<std::size_t>(side) * static_cast<std::size_t>(side);

  std::unordered_set<std::uint64_t> readyChunks;
  readyChunks.reserve(viewSquareCount * 2);
  std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point>
      activeSubmitted;
  activeSubmitted.reserve(viewSquareCount * 2);
  std::unordered_map<std::uint64_t, ChunkTicketPos> pendingLightReady;
  pendingLightReady.reserve(viewSquareCount * 2);

  auto pumpLight = [&]() {
    if (!withLight || !world)
      return;
    for (GeneratedLightWork &work : lightQueue.drain()) {
      if (work.compute) {
        (void)world->requestGeneratedChunkLightCompute(work.position.x,
                                                       work.position.z);
      } else {
        (void)world->submitGeneratedChunkLight(std::move(work.input));
      }
    }
    world->drainLightPublications();
  };

  std::uint64_t nextEpoch = 1;
  {
    const std::array<ChunkTicketPos, 1> initCenter{{{0, 0}}};
    scheduler.updatePriorityCenters(initCenter, viewDistance + 13);
    // Pre-warm initial (2*VD+1)x(2*VD+1) square around (0, 0).
    for (int r = 0; r <= viewDistance; ++r) {
      for (int dx = -r; dx <= r; ++dx) {
        for (int dz = -r; dz <= r; ++dz) {
          if (std::max(std::abs(dx), std::abs(dz)) != r)
            continue;
          const ChunkTicketPos pos{dx, dz};
          const std::uint64_t key = packChunkKey(dx, dz);
          if (activeSubmitted.contains(key))
            continue;
          scheduler.submitDemand(DemandRequest{
              pos, ChunkStatusId::Full, true, true, nextEpoch++, 0});
          activeSubmitted.emplace(key, std::chrono::steady_clock::now());
        }
      }
    }
    const auto prewarmDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (readyChunks.size() < viewSquareCount) {
      pumpLight();
      for (CompletedResidency &item : scheduler.drainCompletedResidency()) {
        if (item.ok) {
          const std::uint64_t key = packChunkKey(item.pos.x, item.pos.z);
          if (!withLight ||
              world->lightService()->isChunkSettled(item.pos.x, item.pos.z)) {
            activeSubmitted.erase(key);
            pendingLightReady.erase(key);
            readyChunks.insert(key);
          } else {
            pendingLightReady.emplace(key, item.pos);
          }
        }
      }
      if (withLight) {
        for (auto it = pendingLightReady.begin();
             it != pendingLightReady.end();) {
          if (world->lightService()->isChunkSettled(it->second.x,
                                                    it->second.z)) {
            activeSubmitted.erase(it->first);
            readyChunks.insert(it->first);
            it = pendingLightReady.erase(it);
          } else {
            ++it;
          }
        }
      }
      (void)scheduler.drainEvictedChunks();
      if (readyChunks.size() >= viewSquareCount)
        break;
      if (std::chrono::steady_clock::now() >= prewarmDeadline)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  activeSubmitted.clear();
  pendingLightReady.clear();
  std::vector<double> flightLatenciesMs;
  flightLatenciesMs.reserve(4096);

  const StreamingDemandStats baselineStream = scheduler.streamingStats();
  int lastPlayerCx = 0;
  std::size_t totalTicks = 0;
  std::size_t playerChunkReadyTicks = 0;
  double viewReadyPctSum = 0.0;
  bool midSampled = false;
  std::size_t backlogMid = 0;
  std::size_t backlogEnd = 0;
  std::size_t flightSubmitted = 0;
  std::size_t flightCompleted = 0;

  constexpr auto kTickInterval = std::chrono::milliseconds(25);
  constexpr auto kDrainPoll = std::chrono::milliseconds(2);
  const auto flightStart = std::chrono::steady_clock::now();
  auto nextTickTime = flightStart + kTickInterval;

  auto drainFlightCompletions = [&](int minKeepCx) {
    pumpLight();
    const auto now = std::chrono::steady_clock::now();
    auto finishReady = [&](std::uint64_t key, int posX) {
      const auto subIt = activeSubmitted.find(key);
      if (subIt != activeSubmitted.end()) {
        const double latMs =
            std::chrono::duration<double, std::milli>(now - subIt->second)
                .count();
        flightLatenciesMs.push_back(latMs);
        ++flightCompleted;
        activeSubmitted.erase(subIt);
      } else {
        ++flightCompleted;
      }
      if (posX >= minKeepCx)
        readyChunks.insert(key);
    };
    for (CompletedResidency &item : scheduler.drainCompletedResidency()) {
      const std::uint64_t key = packChunkKey(item.pos.x, item.pos.z);
      if (!item.ok) {
        activeSubmitted.erase(key);
        pendingLightReady.erase(key);
        continue;
      }
      if (!withLight ||
          world->lightService()->isChunkSettled(item.pos.x, item.pos.z)) {
        pendingLightReady.erase(key);
        finishReady(key, item.pos.x);
      } else if (item.pos.x >= minKeepCx) {
        pendingLightReady.emplace(key, item.pos);
      } else {
        activeSubmitted.erase(key);
      }
    }
    if (withLight) {
      for (auto it = pendingLightReady.begin(); it != pendingLightReady.end();) {
        if (it->second.x < minKeepCx) {
          activeSubmitted.erase(it->first);
          it = pendingLightReady.erase(it);
        } else if (world->lightService()->isChunkSettled(it->second.x,
                                                         it->second.z)) {
          finishReady(it->first, it->second.x);
          it = pendingLightReady.erase(it);
        } else {
          ++it;
        }
      }
    }
    (void)scheduler.drainEvictedChunks();
  };

  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    const double elapsedSec =
        std::chrono::duration<double>(now - flightStart).count();
    if (elapsedSec >= flyDurationSec)
      break;

    const int minKeepCx = lastPlayerCx - viewDistance;
    drainFlightCompletions(minKeepCx);

    if (now < nextTickTime) {
      const auto sleepFor = std::min<std::chrono::steady_clock::duration>(
          kDrainPoll, nextTickTime - now);
      if (sleepFor > std::chrono::microseconds(100))
        std::this_thread::sleep_for(sleepFor);
      continue;
    }

    while (nextTickTime <= now)
      nextTickTime += kTickInterval;

    const double playerX = speedBlocksPerSec * elapsedSec;
    const int cx = static_cast<int>(std::floor(playerX / 16.0));
    const ChunkTicketPos playerChunk{cx, 0};

    if (cx != lastPlayerCx) {
      const std::array<ChunkTicketPos, 1> centers{playerChunk};
      scheduler.updatePriorityCenters(centers, viewDistance + 13);

      // Cancel pending demands that slid behind the view square.
      for (int ox = lastPlayerCx - viewDistance; ox < cx - viewDistance; ++ox) {
        for (int oz = -viewDistance; oz <= viewDistance; ++oz) {
          const std::uint64_t key = packChunkKey(ox, oz);
          if (activeSubmitted.erase(key) > 0) {
            scheduler.cancelDemand(ChunkTicketPos{ox, oz}, true);
          }
          pendingLightReady.erase(key);
          readyChunks.erase(key);
        }
      }

      // Submit newly entered columns in [cx - VD .. cx + VD, -VD .. VD].
      const auto submitNow = std::chrono::steady_clock::now();
      for (int nx = cx - viewDistance; nx <= cx + viewDistance; ++nx) {
        for (int nz = -viewDistance; nz <= viewDistance; ++nz) {
          const std::uint64_t key = packChunkKey(nx, nz);
          if (readyChunks.contains(key) || activeSubmitted.contains(key))
            continue;
          scheduler.submitDemand(DemandRequest{
              ChunkTicketPos{nx, nz}, ChunkStatusId::Full, true, true,
              nextEpoch++, 0});
          activeSubmitted.emplace(key, submitNow);
          ++flightSubmitted;
        }
      }

      scheduler.requestAsyncTrim();
      (void)scheduler.drainEvictedChunks();
      lastPlayerCx = cx;
    }

    drainFlightCompletions(cx - viewDistance);

    // Sample view readiness & pending backlog at this tick.
    ++totalTicks;
    if (readyChunks.contains(packChunkKey(cx, 0)))
      ++playerChunkReadyTicks;

    std::size_t inViewReady = 0;
    std::size_t inViewPending = 0;
    for (int vx = cx - viewDistance; vx <= cx + viewDistance; ++vx) {
      for (int vz = -viewDistance; vz <= viewDistance; ++vz) {
        if (readyChunks.contains(packChunkKey(vx, vz))) {
          ++inViewReady;
        } else {
          ++inViewPending;
        }
      }
    }

    viewReadyPctSum +=
        100.0 * static_cast<double>(inViewReady) / static_cast<double>(viewSquareCount);
    if (!midSampled && elapsedSec >= 0.5 * flyDurationSec) {
      backlogMid = inViewPending;
      midSampled = true;
    }
    backlogEnd = inViewPending;
  }

  const StreamingDemandStats endStream = scheduler.streamingStats();
  const long rssKbAtEnd = currentRssKb();
  scheduler.shutdownStreaming();
  if (withLight && world && world->lightService() != nullptr) {
    pumpLight();
    world->lightService()->flushAndWait();
    world->drainLightPublications();
  }

  std::sort(flightLatenciesMs.begin(), flightLatenciesMs.end());

  FlyRunResult result;
  result.speedBlocksPerSec = speedBlocksPerSec;
  result.speedChunksPerSec = speedBlocksPerSec / 16.0;
  result.durationSec = flyDurationSec;
  result.playerChunkReadyPct =
      totalTicks > 0
          ? 100.0 * static_cast<double>(playerChunkReadyTicks) /
                static_cast<double>(totalTicks)
          : 0.0;
  result.avgViewReadyPct =
      totalTicks > 0 ? viewReadyPctSum / static_cast<double>(totalTicks) : 0.0;
  result.backlogMid = midSampled ? backlogMid : backlogEnd;
  result.backlogEnd = backlogEnd;
  result.submitted = flightSubmitted;
  result.completed = flightCompleted;
  result.cancelled = endStream.cancelled - baselineStream.cancelled;
  result.failed = endStream.failed - baselineStream.failed;
  result.arrivalCps =
      flyDurationSec > 0.0
          ? static_cast<double>(flightSubmitted) / flyDurationSec
          : 0.0;
  result.completedCps =
      flyDurationSec > 0.0
          ? static_cast<double>(flightCompleted) / flyDurationSec
          : 0.0;
  result.latP50Ms = percentileFromSorted(flightLatenciesMs, 0.50);
  result.latP95Ms = percentileFromSorted(flightLatenciesMs, 0.95);
  result.latP99Ms = percentileFromSorted(flightLatenciesMs, 0.99);
  result.latMaxMs =
      !flightLatenciesMs.empty() ? flightLatenciesMs.back() : 0.0;
  result.rssMb =
      rssKbAtEnd > 0 ? static_cast<double>(rssKbAtEnd) / 1024.0 : 0.0;

  const std::size_t maxThroughputGrowth =
      static_cast<std::size_t>(2) * static_cast<std::size_t>(side);
  result.cleanHorizonPass = (result.playerChunkReadyPct >= 99.5) &&
                            (result.avgViewReadyPct >= 95.0) &&
                            (result.cancelled == 0) && (result.failed == 0);
  result.throughputPass =
      (result.playerChunkReadyPct >= 99.5) && (result.cancelled == 0) &&
      (result.failed == 0) &&
      (result.completedCps >= 0.92 * result.arrivalCps) &&
      (result.backlogEnd <= result.backlogMid + maxThroughputGrowth);
  return result;
}

std::vector<double> parseSpeedList(std::string_view spec) {
  std::vector<double> out;
  std::stringstream ss{std::string(spec)};
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (item.empty())
      continue;
    const double val = std::stod(item);
    if (val <= 0.0)
      throw std::invalid_argument("fly speed must be positive");
    out.push_back(val);
  }
  return out;
}

void printFlyResultRow(const FlyRunResult &r) {
  std::printf(
      "%6.1f | %5.2f | %7.2f%% | %7.2f%% | %6zu | %6zu | %7.1f | %7.1f | "
      "%5zu | %7.1f | %7.1f | %7.1f | %7.1f | %6.1f | %5s | %4s\n",
      r.speedBlocksPerSec, r.speedChunksPerSec, r.playerChunkReadyPct,
      r.avgViewReadyPct, r.backlogMid, r.backlogEnd, r.arrivalCps,
      r.completedCps, r.cancelled, r.latP50Ms, r.latP95Ms, r.latP99Ms,
      r.latMaxMs, r.rssMb, r.cleanHorizonPass ? "PASS" : "FAIL",
      r.throughputPass ? "PASS" : "FAIL");
  std::fflush(stdout);
}

int runFlyBench(const WorldgenRegistries &registries, const std::string &dim,
                std::int64_t seed, int threads, bool strongholds,
                int viewDistance, double flyDurationSec,
                const std::vector<double> &explicitSpeeds, bool refine,
                bool withLight) {
  const int side = 2 * viewDistance + 1;
  const std::size_t maxGrowth =
      static_cast<std::size_t>(2) * static_cast<std::size_t>(side);
  std::printf(
      "fly_bench dim=%s seed=%lld threads=%d strongholds=%d light=%d "
      "view_distance=%d square=%dx%d duration=%.2fs\n",
      dim.c_str(), static_cast<long long>(seed), threads,
      strongholds ? 1 : 0, withLight ? 1 : 0, viewDistance, side, side,
      flyDurationSec);
  std::printf(
      "clean_horizon:          plyr_rdy >= 99.5%% AND view_rdy >= 95.0%% AND "
      "canc == 0\n");
  std::printf(
      "throughput_sustainable: plyr_rdy >= 99.5%% AND canc == 0 AND cmp_cps >= "
      "0.92 * arr_cps AND bl_end <= bl_mid + %zu\n",
      maxGrowth);
  std::printf(
      " blk/s | chk/s | plyr_rdy | view_rdy | bl_mid | bl_end | arr_cps | "
      "cmp_cps |  canc |  p50_ms |  p95_ms |  p99_ms |  max_ms | rss_mb | "
      "clean | thru\n");
  std::printf(
      "-------+-------+----------+----------+--------+--------+---------+------"
      "---+-------+---------+---------+---------+---------+--------+-------+-----\n");

  double bestCleanPass = 0.0;
  double bestThruPass = 0.0;
  double firstThruFail = 0.0;
  int consecutiveThruFails = 0;

  std::vector<double> sweep = explicitSpeeds;
  const bool autoMode = sweep.empty();
  if (autoMode) {
    sweep = {40.0,  80.0,  120.0, 160.0, 200.0, 240.0,
             280.0, 320.0, 380.0, 440.0, 520.0, 600.0};
  }

  for (double speed : sweep) {
    FlyRunResult r = runFlySpeedTrial(registries, dim, seed, threads,
                                      strongholds, viewDistance, flyDurationSec,
                                      speed, withLight);
    printFlyResultRow(r);
    if (r.cleanHorizonPass)
      bestCleanPass = std::max(bestCleanPass, r.speedBlocksPerSec);
    if (r.throughputPass) {
      bestThruPass = std::max(bestThruPass, r.speedBlocksPerSec);
      consecutiveThruFails = 0;
    } else {
      if (firstThruFail == 0.0 && r.speedBlocksPerSec > bestThruPass)
        firstThruFail = r.speedBlocksPerSec;
      ++consecutiveThruFails;
      const bool clearlyOverloaded =
          r.playerChunkReadyPct < 99.5 || r.completedCps < 0.85 * r.arrivalCps ||
          consecutiveThruFails >= 2;
      if (autoMode && clearlyOverloaded)
        break;
    }
  }

  if (autoMode && firstThruFail == 0.0 && bestThruPass > 0.0) {
    for (double extra : {700.0, 820.0, 960.0}) {
      FlyRunResult r = runFlySpeedTrial(registries, dim, seed, threads,
                                        strongholds, viewDistance,
                                        flyDurationSec, extra, withLight);
      printFlyResultRow(r);
      if (r.cleanHorizonPass)
        bestCleanPass = std::max(bestCleanPass, r.speedBlocksPerSec);
      if (r.throughputPass) {
        bestThruPass = std::max(bestThruPass, r.speedBlocksPerSec);
      } else {
        firstThruFail = r.speedBlocksPerSec;
        break;
      }
    }
  }

  if (refine && bestThruPass > 0.0 && firstThruFail > bestThruPass) {
    double lo = bestThruPass;
    double hi = firstThruFail;
    for (int step = 0; step < 4 && (hi - lo) > 5.0; ++step) {
      const double mid = std::round((lo + hi) * 0.5);
      if (mid <= lo || mid >= hi)
        break;
      FlyRunResult r = runFlySpeedTrial(registries, dim, seed, threads,
                                        strongholds, viewDistance,
                                        flyDurationSec, mid, withLight);
      printFlyResultRow(r);
      if (r.cleanHorizonPass)
        bestCleanPass = std::max(bestCleanPass, mid);
      if (r.throughputPass) {
        lo = mid;
        bestThruPass = std::max(bestThruPass, mid);
      } else {
        hi = mid;
      }
    }
  }

  std::printf(
      "\nclean_horizon_max_speed:          %.1f blocks/s (%.2f player_chunks/s, "
      "%.1f resident_chunks/s)\n",
      bestCleanPass, bestCleanPass / 16.0,
      (bestCleanPass / 16.0) * static_cast<double>(side));
  std::printf(
      "throughput_sustainable_max_speed: %.1f blocks/s (%.2f player_chunks/s, "
      "%.1f resident_chunks/s)\n",
      bestThruPass, bestThruPass / 16.0,
      (bestThruPass / 16.0) * static_cast<double>(side));
  std::fflush(stdout);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: worldgen_throughput_benchmark <assets/worldgen26_1> "
                 "[--seed N] [--size N] [--threads N] [--repeat N] "
                 "[--no-strongholds] [--dim overworld|nether|end]\n"
                 "       [--light] [--serialize] [--install]\n"
                 "       [--fly-bench] [--view-distance N] [--fly-duration S] "
                 "[--fly-speeds \"40,80,...\"] [--no-refine]\n"
                 "       [--mem-report] [--jemalloc-dump FILE] "
                 "[--heap-profile FILE]\n");
    return 2;
  }
  const std::filesystem::path assetsDir(argv[1]);
  std::int64_t seed = 849201745213LL;
  int size = 20;
  int threads = 0;
  int repeat = 1;
  bool strongholds = true;
  bool memReport = false;
  bool withLight = false;
  bool withSerialize = false;
  bool withInstall = false;
  bool flyBench = false;
  int viewDistance = 10;
  double flyDurationSec = 4.0;
  bool refineFlySpeed = true;
  std::vector<double> flySpeeds;
  const char *jemallocDumpPath = nullptr;
  const char *heapProfilePath = nullptr;
  std::string dim = "overworld";
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--seed" && i + 1 < argc) {
      seed = std::stoll(argv[++i]);
    } else if (arg == "--size" && i + 1 < argc) {
      size = std::stoi(argv[++i]);
    } else if (arg == "--threads" && i + 1 < argc) {
      threads = std::stoi(argv[++i]);
    } else if (arg == "--repeat" && i + 1 < argc) {
      repeat = std::stoi(argv[++i]);
    } else if (arg == "--no-strongholds") {
      strongholds = false;
    } else if (arg == "--mem-report") {
      memReport = true;
    } else if (arg == "--light") {
      withLight = true;
    } else if (arg == "--serialize") {
      withSerialize = true;
    } else if (arg == "--install") {
      withInstall = true;
    } else if (arg == "--fly-bench") {
      flyBench = true;
    } else if (arg == "--view-distance" && i + 1 < argc) {
      viewDistance = std::stoi(argv[++i]);
    } else if (arg == "--fly-duration" && i + 1 < argc) {
      flyDurationSec = std::stod(argv[++i]);
    } else if (arg == "--fly-speeds" && i + 1 < argc) {
      flySpeeds = parseSpeedList(argv[++i]);
    } else if (arg == "--no-refine") {
      refineFlySpeed = false;
    } else if (arg == "--jemalloc-dump" && i + 1 < argc) {
      jemallocDumpPath = argv[++i];
    } else if (arg == "--heap-profile" && i + 1 < argc) {
      heapProfilePath = argv[++i];
    } else if (arg == "--dim" && i + 1 < argc) {
      dim = argv[++i];
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  if (size <= 0 || repeat <= 0 || viewDistance < 0 || flyDurationSec <= 0.0)
    return 2;

  const WorldgenRegistries registries =
      WorldgenRegistries::loadFromDirectory(assetsDir);

  if (flyBench) {
    return runFlyBench(registries, dim, seed, threads, strongholds,
                       viewDistance, flyDurationSec, flySpeeds,
                       refineFlySpeed, withLight);
  }

  for (int run = 0; run < repeat; ++run) {
    auto generator = makeGenerator(registries, dim);
    if (!strongholds)
      generator->setConcentricRingsEnabled(false);
    WorldGenSchedulerConfig config;
    if (threads > 0)
      config.threadCount = static_cast<std::size_t>(threads);

    // 运行态世界只在需要光照 / 序列化 / 晋升时创建：光照引擎由 World 持有，
    // 序列化与 ProtoChunk->World 晋升都围绕同一个世界进行（仍然不落盘）。
    std::unique_ptr<World> world;
    if (withLight || withSerialize || withInstall) {
      world = std::make_unique<World>(dimensionFor(dim));
      if (withLight) {
        LightWorkerOptions lightOptions;
        // 世界生成的 3x3 波前依赖邻居几何先就绪；打开 checkEdge 等价语义。
        lightOptions.deferUntilNeighborsReady = true;
        // 世界生成不看墙钟：批次只由依赖关系决定，避免时间相关的批次划分。
        lightOptions.batchWindowMs = 0;
        world->enableLighting(lightOptions);
      }
    }
    std::shared_ptr<LightOpticalContext> lightContext;
    if (withLight) {
      lightContext = std::make_shared<LightOpticalContext>();
      lightContext->set(world->lightService()->opticalTable(),
                        world->lightService()->skyLightEnabled());
    }
    GeneratedLightQueue lightQueue;
    WorldGenScheduler scheduler(*generator, seed, nullptr, nullptr, lightContext,
                                withLight ? &lightQueue : nullptr, config);

    std::vector<ChunkBatchRequest> requests;
    requests.reserve(static_cast<std::size_t>(size) * size);
    const int half = size / 2;
    for (int x = -half; x < size - half; ++x) {
      for (int z = -half; z < size - half; ++z)
        requests.push_back(
            ChunkBatchRequest{ChunkTicketPos{x, z}, ChunkStatusId::Full});
    }

    // 生成期光照由单线程 pump 提交：批处理调用会阻塞主线程，这里单独起投递线程，
    // 保持“生成 worker 只构造输入、LightService 只被一个线程提交”的契约。
    std::atomic<bool> lightPumpRunning{true};
    std::thread lightPump;
    auto drainLightQueue = [&]() {
      for (GeneratedLightWork &work : lightQueue.drain()) {
        if (work.compute)
          world->requestGeneratedChunkLightCompute(work.position.x,
                                                   work.position.z);
        else
          world->submitGeneratedChunkLight(std::move(work.input));
      }
    };
    if (withLight) {
      lightPump = std::thread([&]() {
        while (lightPumpRunning.load(std::memory_order_relaxed)) {
          drainLightQueue();
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      });
    }
    const auto wallStart = std::chrono::steady_clock::now();
    const double cpuStart = processCpuSeconds();
    scheduler.ensureStatusBatch(requests, ChunkTicketPos{0, 0});
    const double cpuEnd = processCpuSeconds();
    const auto wallEnd = std::chrono::steady_clock::now();
    lightPumpRunning.store(false, std::memory_order_relaxed);
    if (lightPump.joinable())
      lightPump.join();
    if (withLight)
      drainLightQueue();

    std::size_t resident = 0;
    for (const ChunkBatchRequest &request : requests) {
      if (scheduler.findChunk(request.pos) != nullptr)
        ++resident;
    }
    const WorldGenSchedulerStats stats = scheduler.stats();
    const double wallMs =
        std::chrono::duration<double, std::milli>(wallEnd - wallStart).count();
    const double cpuMs = (cpuEnd - cpuStart) * 1000.0;
    const std::size_t chunkCount = requests.size();

    const double cps =
        wallMs > 0.0 ? (static_cast<double>(chunkCount) * 1000.0 / wallMs) : 0.0;
    const double latAvgMs =
        stats.completedTargets > 0
            ? (static_cast<double>(stats.targetLatencySumUs) / 1000.0) /
                  static_cast<double>(stats.completedTargets)
            : 0.0;
    const double latP50Ms = stats.latencyPercentileMs(0.50);
    const double latP95Ms = stats.latencyPercentileMs(0.95);
    const double latP99Ms = stats.latencyPercentileMs(0.99);
    const double latMaxMs =
        static_cast<double>(stats.targetLatencyMaxUs) / 1000.0;

    std::printf(
        "run=%d chunks=%zu wall_ms=%.1f cpu_ms=%.1f ms_per_chunk=%.2f "
        "created=%zu steps=%zu inline=%zu parallel=%zu resident=%zu "
        "strongholds=%d rss_peak_kb=%ld "
        "lat_avg_ms=%.2f lat_p50_ms=%.2f lat_p95_ms=%.2f lat_p99_ms=%.2f "
        "lat_max_ms=%.2f cps=%.2f spatial_conflicts=%zu\n",
        run, chunkCount, wallMs, cpuMs, wallMs / static_cast<double>(chunkCount),
        stats.chunksCreated, stats.stepsExecuted, stats.inlineSteps,
        stats.parallelDispatches, resident, strongholds ? 1 : 0, peakRssKb(),
        latAvgMs, latP50Ms, latP95Ms, latP99Ms, latMaxMs, cps,
        stats.spatialConflicts);
    std::fflush(stdout);

    if (world) {
      // 批处理场景的收敛点：全部区块到 Full 后让光照队列排空一次，之后逐块
      // 刷新快照就是同一不动点的稳定值（流式场景由邻居到达时的增量更新保证）。
      double lightFlushMs = 0.0;
      if (world->lightService() != nullptr) {
        const auto flushStart = std::chrono::steady_clock::now();
        world->lightService()->flushAndWait();
        lightFlushMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - flushStart)
                .count();
      }
      const double totalLitWallMs = wallMs + lightFlushMs;
      const double litCps =
          totalLitWallMs > 0.0
              ? (static_cast<double>(chunkCount) * 1000.0 / totalLitWallMs)
              : 0.0;
      const auto serStart = std::chrono::steady_clock::now();
      double serializeMs = 0.0;
      double installMs = 0.0;
      std::size_t nbtBytes = 0;
      std::size_t nbtBlockLightChunks = 0;
      std::uint64_t lightChecksum = 0;
      std::size_t nbtSkyLightChunks = 0;
      std::size_t serialized = 0;
      std::size_t installed = 0;
      std::size_t lightCorrect = 0;
      std::size_t lightIncompleteInterior = 0;
      std::size_t lightIncompleteBorder = 0;
      std::size_t installMismatch = 0;
      std::size_t installSamples = 0;
      for (const ChunkBatchRequest &request : requests) {
        auto proto = scheduler.takeProtoChunk(request.pos);
        if (!proto)
          continue;
        // 光照数据只存在于光照引擎里，统计/序列化前先按"存档时"的方式取一次。
        if (world && world->lightService() != nullptr)
          refreshProtoChunkLight(*proto, *world->lightService());
        for (int s = 0; s < proto->sectionCount(); ++s) {
          const ProtoChunkSection &sec = proto->section(s);
          if (sec.blockLight.has_value())
            for (std::uint8_t value : *sec.blockLight)
              lightChecksum += value;
          if (sec.skyLight.has_value())
            for (std::uint8_t value : *sec.skyLight)
              lightChecksum += value;
        }
        // 诊断用：把每块的光照校验和逐行写出，便于两次运行逐块 diff。
        if (const char *dumpPath = std::getenv("REDSTONE_LIGHT_DUMP")) {
          for (int s = 0; s < proto->sectionCount(); ++s) {
            const ProtoChunkSection &sec = proto->section(s);
            std::uint64_t blockSum = 0;
            std::uint64_t skySum = 0;
            if (sec.blockLight.has_value())
              for (std::uint8_t value : *sec.blockLight)
                blockSum += value;
            if (sec.skyLight.has_value())
              for (std::uint8_t value : *sec.skyLight)
                skySum += value;
            if (FILE *dump = std::fopen(dumpPath, "a")) {
              std::fprintf(dump, "%d %d %d %llu %llu %d %d\n", request.pos.x,
                           request.pos.z, sec.sectionY(),
                           static_cast<unsigned long long>(blockSum),
                           static_cast<unsigned long long>(skySum),
                           sec.blockLight.has_value() ? 1 : 0,
                           sec.skyLight.has_value() ? 1 : 0);
              std::fclose(dump);
            }
          }
        }
        const bool settled =
            world && world->lightService() != nullptr &&
            world->lightService()->isChunkSettled(request.pos.x,
                                                  request.pos.z);
        if (settled || proto->isLightCorrect()) {
          ++lightCorrect;
        } else {
          const int halfSize = size / 2;
          const bool border =
              request.pos.x == -halfSize || request.pos.x == size - halfSize - 1 ||
              request.pos.z == -halfSize || request.pos.z == size - halfSize - 1;
          if (border)
            ++lightIncompleteBorder;
          else
            ++lightIncompleteInterior;
        }
        if (withSerialize) {
          const auto t0 = std::chrono::steady_clock::now();
          const nbt::RootTag root = encodeProtoChunkToNbt(*proto);
          const std::vector<std::uint8_t> encoded = nbt::writeRoot(root);
          nbtBytes += encoded.size();
          const auto hasTag = [&encoded](std::string_view name) {
            return std::search(encoded.begin(), encoded.end(), name.begin(),
                               name.end()) != encoded.end();
          };
          if (hasTag("BlockLight"))
            ++nbtBlockLightChunks;
          if (hasTag("SkyLight"))
            ++nbtSkyLightChunks;
          serializeMs += std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
          ++serialized;
        }
        if (withInstall) {
          // 晋升前采样少量格子，用于校验 ProtoChunk -> World 的方块内容一致。
          struct Sample {
            Pos pos;
            BlockState::Id id;
          };
          std::array<Sample, 6> samples{};
          const int ys[3] = {proto->minY() + 1, proto->minY() + 64,
                             proto->maxY() - 2};
          for (int i = 0; i < 6; ++i) {
            const int lx = (i % 2 == 0) ? 3 : 12;
            const int lz = (i / 2 == 0) ? 5 : 11;
            const int y = ys[i % 3];
            samples[static_cast<std::size_t>(i)] = {
                Pos{(proto->pos().x << 4) + lx, y, (proto->pos().z << 4) + lz},
                proto->getBlockState(lx, y, lz).id()};
          }
          const auto t0 = std::chrono::steady_clock::now();
          if (installProtoChunkIntoWorld(std::move(proto), *world)) {
            ++installed;
            for (const Sample &s : samples) {
              ++installSamples;
              if (world->getBlock(s.pos).id() != s.id)
                ++installMismatch;
            }
          }
          installMs += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
        }
      }
      const double pipelineMs = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - serStart)
                                    .count();
      // 安装完成后排空一次光照工作：adopt 命令会释放生成期句柄，这样 rss_kb
      // 反映的是稳态常驻，而不是尚未处理完的句柄。
      if (world && world->lightService() != nullptr) {
        world->lightService()->flushAndWait();
        world->drainLightPublications();
      }
      std::printf("pipeline light=%d serialize=%d install=%d "
                  "serialized=%zu installed=%zu nbt_bytes=%zu "
                  "nbt_blocklight_chunks=%zu nbt_skylight_chunks=%zu "
                  "serialize_ms=%.1f install_ms=%.1f pipeline_ms=%.1f "
                  "rss_kb=%ld\n",
                  withLight ? 1 : 0, withSerialize ? 1 : 0, withInstall ? 1 : 0,
                  serialized, installed, nbtBytes, nbtBlockLightChunks,
                  nbtSkyLightChunks, serializeMs, installMs, pipelineMs,
                  currentRssKb());
      std::printf("light_sum=%llu\n",
                  static_cast<unsigned long long>(lightChecksum));
      std::printf("verify light_correct=%zu/%zu "
                  "light_incomplete_interior=%zu light_incomplete_border=%zu "
                  "install_mismatch=%zu/%zu\n",
                  lightCorrect, installed, lightIncompleteInterior,
                  lightIncompleteBorder, installMismatch, installSamples);
      if (withLight) {
        const LightService::Stats ls = world->lightService()->stats();
        std::printf("light_stats gen_wall_ms=%.1f light_flush_ms=%.1f "
                    "total_lit_wall_ms=%.1f lit_cps=%.2f "
                    "batches=%llu published=%llu "
                    "worker_cpu_ms=%.1f block_cpu_ms=%.1f sky_cpu_ms=%.1f "
                    "light_queue=%llu unloaded_skips=%llu rebound=%llu\n",
                    wallMs, lightFlushMs, totalLitWallMs, litCps,
                    static_cast<unsigned long long>(ls.processedBatches),
                    static_cast<unsigned long long>(ls.published),
                    ls.workerCpuMs, ls.blockWorkerCpuMs, ls.skyWorkerCpuMs,
                    static_cast<unsigned long long>(ls.lightQueueEntries),
                    static_cast<unsigned long long>(ls.unloadedNeighborSkips),
                    static_cast<unsigned long long>(ls.rebindings));
        std::fflush(stdout);
      }
    }

    if (memReport) {
      ProtoChunkMemoryUsage usage;
      ProtoChunkMemoryUsage targetUsage;
      std::size_t protoChunks = 0;
      std::size_t otherChunks = 0;
      std::size_t targetChunks = 0;
      std::array<std::size_t, 16> statusHistogram{};
      scheduler.forEachHeldChunk([&](const BaseChunk &access) {
        const auto *proto = access.isProtoChunk()
                                ? static_cast<const ProtoChunk *>(&access)
                                : nullptr;
        if (proto == nullptr) {
          ++otherChunks;
          return;
        }
        const ProtoChunkMemoryUsage one = proto->memoryUsage();
        usage.add(one);
        ++protoChunks;
        const std::size_t statusIndex =
            static_cast<std::size_t>(chunkPersistedStatus(access));
        if (statusIndex < statusHistogram.size())
          ++statusHistogram[statusIndex];
      });
      for (const ChunkBatchRequest &request : requests) {
        const BaseChunk *found = scheduler.findChunk(request.pos);
        const auto *proto =
            (found != nullptr && found->isProtoChunk())
                ? static_cast<const ProtoChunk *>(found)
                : nullptr;
        if (proto != nullptr) {
          targetUsage.add(proto->memoryUsage());
          ++targetChunks;
        }
      }
      const long rssKb = currentRssKb();
      const long hwmKb = peakRssKb();
      const double rssBytes = static_cast<double>(rssKb) * 1024.0;
      std::printf("mem_report held_chunks=%zu target_chunks=%zu "
                  "other_chunks=%zu "
                  "sizeof_chunk=%zu sizeof_section=%zu rss_kb=%ld "
                  "rss_peak_kb=%ld\n",
                  protoChunks, targetChunks, otherChunks,
                  sizeof(ProtoChunk), sizeof(ProtoChunkSection), rssKb,
                  hwmKb);
      std::printf("  status histogram of held chunks:");
      for (std::size_t i = 0; i < statusHistogram.size(); ++i) {
        if (statusHistogram[i] == 0)
          continue;
        const std::string_view name =
            ChunkStatus::byId(static_cast<ChunkStatusId>(i)).name();
        std::printf(" %.*s=%zu", static_cast<int>(name.size()), name.data(),
                    statusHistogram[i]);
      }
      std::printf("\n");
      std::printf("  chunk-resident breakdown (share of current RSS):\n");
      printMemLine("chunk objects", usage.chunkObject, rssBytes);
      printMemLine("sections array", usage.sectionsArray, rssBytes);
      printMemLine("light layer buffers", usage.lightLayerBuffers, rssBytes);
      printMemLine("palette index words", usage.paletteIndexWords, rssBytes);
      printMemLine("palette entries", usage.paletteEntries, rssBytes);
      printMemLine("biome palette vectors", usage.biomePaletteVectors,
                   rssBytes);
      printMemLine("biome palette strings", usage.biomeStrings, rssBytes);
      printMemLine("heightmap data", usage.heightmapData, rssBytes);
      printMemLine("carving masks", usage.carvingMask, rssBytes);
      printMemLine("post processing", usage.postProcessing, rssBytes);
      printMemLine("scheduled ticks", usage.ticks, rssBytes);
      printMemLine("entity vector shells", usage.entities, rssBytes);
      printMemLine("block entity shells", usage.blockEntities, rssBytes);
      printMemLine("moving pistons", usage.movingPistons, rssBytes);
      printMemLine("structure references", usage.structureReferences,
                   rssBytes);
      printMemLine("CHUNK TOTAL (all held)", usage.total(), rssBytes);
      printMemLine("  of which target chunks", targetUsage.total(), rssBytes);
      const std::size_t chunkBytes = usage.total();
      const std::size_t rssTotal = static_cast<std::size_t>(rssKb) * 1024;
      printMemLine("non-chunk (caches/arena)",
                   rssTotal > chunkBytes ? rssTotal - chunkBytes : 0,
                   rssBytes);
#ifdef REDSTONE_USE_JEMALLOC
      std::printf("  jemalloc: allocated=%.1f MB active=%.1f MB "
                  "resident=%.1f MB mapped=%.1f MB metadata=%.1f MB\n",
                  jemallocStat("stats.allocated") / 1048576.0,
                  jemallocStat("stats.active") / 1048576.0,
                  jemallocStat("stats.resident") / 1048576.0,
                  jemallocStat("stats.mapped") / 1048576.0,
                  jemallocStat("stats.metadata") / 1048576.0);
#endif
      std::fflush(stdout);
    }
#ifdef REDSTONE_USE_JEMALLOC
    if (jemallocDumpPath != nullptr)
      dumpJemallocStats(jemallocDumpPath);
    if (heapProfilePath != nullptr &&
        !dumpJemallocHeapProfile(heapProfilePath))
      std::fprintf(stderr, "heap profile dump failed (MALLOC_CONF=prof:true?)\n");
#endif
  }
  return 0;
}
