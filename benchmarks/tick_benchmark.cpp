// Offline litematic workload; fingerprints are diagnostic, not a proof of
// parity.
#include "redstone/allocator.hpp"
#include "redstone/entity.hpp"
#include "redstone/litematic.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <sched.h>
#include <system_error>
#include <time.h>
#endif

using namespace redstone;
using Clock = std::chrono::steady_clock;

#ifdef _WIN32
static double threadCpuMs() {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
    throw std::runtime_error("GetThreadTimes failed");
  const auto ticks = [](FILETIME t) {
    return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  return double(ticks(kernel) + ticks(user)) / 10000.0;
}

static std::uint64_t currentThreadAffinityMask() {
  GROUP_AFFINITY group{};
  return GetThreadGroupAffinity(GetCurrentThread(), &group)
             ? static_cast<std::uint64_t>(group.Mask)
             : 0;
}

static std::uint64_t bindCurrentThread(std::uint64_t mask) {
  if (!mask)
    return currentThreadAffinityMask();
  if (!SetThreadAffinityMask(GetCurrentThread(),
                             static_cast<DWORD_PTR>(mask)))
    throw std::runtime_error("SetThreadAffinityMask failed");
  return currentThreadAffinityMask();
}

static std::uint64_t bindProcess(std::uint64_t mask) {
  if (!mask)
    return 0;
  if (!SetProcessAffinityMask(GetCurrentProcess(),
                              static_cast<DWORD_PTR>(mask)))
    throw std::runtime_error("SetProcessAffinityMask failed");
  DWORD_PTR processMask = 0, systemMask = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
    throw std::runtime_error("GetProcessAffinityMask failed");
  return static_cast<std::uint64_t>(processMask);
}
#elif defined(__linux__)
static double threadCpuMs() {
  timespec time{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0)
    throw std::system_error(errno, std::generic_category(), "clock_gettime");
  return static_cast<double>(time.tv_sec) * 1000.0 + time.tv_nsec / 1.0e6;
}

static std::uint64_t bindCurrentThread(std::uint64_t mask) {
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  if (mask) {
    for (unsigned cpu = 0; cpu < 64; ++cpu)
      if ((mask >> cpu) & 1)
        CPU_SET(cpu, &cpus);
    if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0)
      throw std::system_error(errno, std::generic_category(), "sched_setaffinity");
  }
  if (sched_getaffinity(0, sizeof(cpus), &cpus) != 0)
    throw std::system_error(errno, std::generic_category(), "sched_getaffinity");
  std::uint64_t actual = 0;
  for (unsigned cpu = 0; cpu < 64; ++cpu)
    if (CPU_ISSET(cpu, &cpus))
      actual |= std::uint64_t{1} << cpu;
  if (mask && mask != actual)
    throw std::runtime_error("requested CPU mask is not available");
  return actual;
}

static std::uint64_t bindProcess(std::uint64_t mask) {
  // The benchmark starts single-threaded. The worker inherits this mask until
  // it receives its own affinity; the main thread then binds independently.
  return bindCurrentThread(mask);
}
#endif

struct Hash {
  std::uint64_t value = 14695981039346656037ULL;
  void add(std::uint64_t n) {
    for (int i = 0; i < 8; ++i) {
      value = (value ^ (n & 255)) * 1099511628211ULL;
      n >>= 8;
    }
  }
  void text(std::string_view s) {
    add(s.size());
    for (auto c : s)
      add(static_cast<unsigned char>(c));
  }
  void pos(Pos p) {
    add(p.x);
    add(p.y);
    add(p.z);
  }
  void real(double n) { add(std::bit_cast<std::uint64_t>(n)); }
};

static void fingerprint(World &w, int tick, int minX, int maxX, int minZ,
                        int maxZ, const Hash &events) {
  Hash blocks, scheduled, entities, pistons, pendingEvents;
  for (int x = minX; x <= maxX; ++x)
    for (int z = minZ; z <= maxZ; ++z) {
      auto chunk = w.snapshotChunk(x, z);
      for (std::size_t y = 0; y < chunk.sections.size(); ++y) {
        const auto &s = chunk.sections[y];
        // A section containing only air-like states can still contain a
        // nonzero state ID. Ignore it only when all its cells are state 0.
        if (!s.nonEmpty) {
          bool any = s.bits == 0 ? s.single != 0 : false;
          if (s.bits != 0)
            for (std::size_t i = 0; i < 4096 && !any; ++i)
              any =
                  s.palette[s.bits <= 8 ? s.indices8[i] : s.indices16[i]] != 0;
          if (!any)
            continue;
        }
        blocks.add(x);
        blocks.add(z);
        blocks.add(y);
        for (std::size_t i = 0; i < 4096; ++i) {
          const auto id =
              s.bits == 0
                  ? s.single
                  : s.palette[s.bits <= 8 ? s.indices8[i] : s.indices16[i]];
          blocks.add(id);
        }
      }
    }
  for (const auto &t : w.scheduledTicks()) {
    scheduled.pos(t.pos);
    scheduled.text(t.block.view());
    scheduled.add(t.trigger);
    scheduled.add(t.priority);
    scheduled.add(t.order);
  }
  for (const auto &e : w.entities) {
    if (!e) {
      entities.add(0);
      continue;
    }
    entities.add(e->id);
    entities.text(e->typeName());
    entities.real(e->position.x);
    entities.real(e->position.y);
    entities.real(e->position.z);
    entities.real(e->deltaMovement.x);
    entities.real(e->deltaMovement.y);
    entities.real(e->deltaMovement.z);
    entities.add(e->tickCount);
    entities.add(e->isRemoved());
    if (auto *item = dynamic_cast<ItemEntity *>(e.get())) {
      entities.text(item->item.id);
      entities.add(item->item.count);
      entities.add(item->age);
    } else if (auto *tnt = dynamic_cast<PrimedTnt *>(e.get()))
      entities.add(tnt->fuse);
    else if (auto *falling = dynamic_cast<FallingBlockEntity *>(e.get())) {
      entities.add(falling->blockState.id());
      entities.add(falling->time);
    }
  }
  for (const auto &[pos, p] : w.pistonEntities) {
    pistons.pos(pos);
    if (!p) {
      pistons.add(0);
      continue;
    }
    pistons.add(p->movedState.id());
    pistons.add(static_cast<int>(p->direction));
    pistons.add(p->extending);
    pistons.add(p->source);
    pistons.real(p->progress);
    pistons.real(p->progressO);
    pistons.add(p->lastTicked);
  }
  for (const auto &e : w.pendingBlockEvents()) {
    pendingEvents.pos(e.pos);
    pendingEvents.text(e.block.view());
    pendingEvents.add(e.type);
    pendingEvents.add(e.data);
  }
  std::printf(
      "FINGERPRINT tick=%d blocks=%016llx scheduled=%016llx entities=%016llx "
      "pistons=%016llx accepted_events=%016llx pending_events=%016llx "
      "pending_event_count=%zu\n",
      tick, (unsigned long long)blocks.value,
      (unsigned long long)scheduled.value, (unsigned long long)entities.value,
      (unsigned long long)pistons.value, (unsigned long long)events.value,
      (unsigned long long)pendingEvents.value, w.blockEventCount());
  if (const auto *service = w.lightService()) {
    Hash sky, block;
    for (int x = minX; x <= maxX; ++x)
      for (int z = minZ; z <= maxZ; ++z) {
        for (LightLayer type : {LightLayer::Block, LightLayer::Sky}) {
          const auto snapshot = service->latestLayerChunk(type, x, z);
          if (!snapshot)
            continue;
          auto &target =
              type == LightLayer::Sky ? sky : block;
          target.add(snapshot->revision);
          const std::size_t index = static_cast<std::size_t>(type);
          for (const auto &section : snapshot->sections) {
            const auto &layer = section->layers[index];
            target.add(static_cast<std::uint64_t>(layer.storage()));
            target.add(layer.uniformValue());
            if (layer.isMixed())
              for (std::size_t byte = 0; byte < kLightLayerBytes; ++byte)
                target.add(layer.packedByte(byte));
          }
        }
      }
    std::printf("LIGHT_FINGERPRINT tick=%d sky=%016llx block=%016llx\n", tick,
                (unsigned long long)sky.value,
                (unsigned long long)block.value);
  }
}

int main(int argc, char **argv) {
  try {
    if (argc < 2)
      throw std::runtime_error(
          "usage: redstone_tick_benchmark <litematic> [ticks] "
          "[--fingerprint] [--lighting|--no-lighting] [--diagnose-light] "
          "[--process-affinity mask] [--main-affinity mask] "
          "[--light-affinity mask] [--light-workers 1] "
          "[--block-affinity mask]");
    int ticks = 3000;
    bool fingerprints = false, haveTicks = false, lighting = false,
         diagnostics = false;
    std::uint64_t processAffinity = 0, mainAffinity = 0, lightAffinity = 0;
    const auto parseMask = [](std::string_view text) {
      int base = 10;
      if (text.size() > 2 && text[0] == '0' &&
          (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
      }
      std::uint64_t value = 0;
      auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), value, base);
      if (error != std::errc{} || end != text.data() + text.size() || !value)
        throw std::runtime_error("affinity mask must be a positive integer");
      return value;
    };
    for (int i = 2; i < argc; ++i) {
      std::string_view arg(argv[i]);
      if (arg == "--fingerprint" && !fingerprints) {
        fingerprints = true;
        diagnostics = true;
        continue;
      }
      if (arg == "--lighting") {
        lighting = true;
        continue;
      }
      if (arg == "--no-lighting") {
        lighting = false;
        continue;
      }
      if (arg == "--diagnose-light") {
        diagnostics = true;
        continue;
      }
      if (arg == "--light-workers") {
        if (++i >= argc)
          throw std::runtime_error("--light-workers only supports 1");
        unsigned value = 0;
        const std::string_view text(argv[i]);
        auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() ||
            value != 1)
          throw std::runtime_error("--light-workers only supports 1");
        continue;
      }
      if (arg == "--process-affinity" || arg == "--main-affinity" ||
          arg == "--light-affinity" || arg == "--block-affinity") {
        if (++i >= argc)
          throw std::runtime_error("affinity option needs a mask");
        const auto mask = parseMask(argv[i]);
        if (arg == "--process-affinity")
          processAffinity = mask;
        else if (arg == "--main-affinity")
          mainAffinity = mask;
        else
          lightAffinity = mask;
        continue;
      }
      if (haveTicks)
        throw std::runtime_error("invalid benchmark arguments");
      auto [end, error] =
          std::from_chars(arg.data(), arg.data() + arg.size(), ticks);
      if (error != std::errc{} || end != arg.data() + arg.size() || ticks <= 0)
        throw std::runtime_error("ticks must be a positive integer");
      haveTicks = true;
    }
    std::uint64_t actualProcessMask = 0;
#if defined(_WIN32) || defined(__linux__)
    actualProcessMask = bindProcess(processAffinity);
#else
    if (processAffinity || mainAffinity || lightAffinity)
      throw std::runtime_error("affinity options require Windows or Linux");
#endif
    std::printf("AFFINITY process=0x%llx main_requested=0x%llx "
                "light_requested=0x%llx\n",
                static_cast<unsigned long long>(actualProcessMask),
                static_cast<unsigned long long>(mainAffinity),
                static_cast<unsigned long long>(lightAffinity));
    LitematicOptions options;
    auto imported = loadLitematic(std::filesystem::path(argv[1]), options);
    World world;
    importWorld(imported.world, world);
    int minX = int(std::floor(imported.min.x / 16.0)) - 2;
    int minZ = int(std::floor(imported.min.z / 16.0)) - 2;
    int maxX = int(std::floor((imported.maxExclusive.x - 1) / 16.0)) + 2;
    int maxZ = int(std::floor((imported.maxExclusive.z - 1) / 16.0)) + 2;
    int chunks = 0;
    for (int x = minX; x <= maxX; ++x)
      for (int z = minZ; z <= maxZ; ++z) {
        world.loadChunk(x, z);
        ++chunks;
      }
    std::printf("IMPORT blocks=%zu entities=%zu forced_chunks=%d ticks=%d "
                "allocator=%s\n",
                imported.world.blocks.size(), world.entities.size(), chunks,
                ticks, memory::backend());
    const Pos clicked{2, 101, 3};
    const auto old = world.getBlock(clicked);
    if (!old.is(Blocks::NOTE_BLOCK))
      throw std::runtime_error(
          "activation position (2,101,3) is not a note block");
    const auto tuned = old.with(
        "note", std::to_string((std::stoi(old.get("note", "0")) + 1) % 25));
    world.setBlock(clicked, tuned, 3);
    const bool event =
        isAirState(world.getBlock(clicked.relative(Direction::Up)));
    if (event)
      world.blockEvent(clicked, old.name(), 0, 0);
    std::printf("ACTIVATE note=%s->%s event=%d\n", old.get("note").c_str(),
                tuned.get("note").c_str(), event);
    LightService *light = nullptr;
    LightService::Stats initialBeforeDrain{};
    LightService::Stats initialAfterDrain{};
    double initialWallMs = 0, initialMainCpuMs = 0;
    bool mainBound = false;
    const auto bindMain = [&] {
      if (mainBound)
        return;
      std::uint64_t actualMainMask = 0;
#if defined(_WIN32) || defined(__linux__)
      actualMainMask = bindCurrentThread(mainAffinity);
#endif
      mainBound = true;
      std::printf("AFFINITY main_actual=0x%llx\n",
                  static_cast<unsigned long long>(actualMainMask));
    };
    if (lighting) {
      LightWorkerOptions lightOptions;
      lightOptions.workerAffinityMask =
          static_cast<std::uintptr_t>(lightAffinity);
      lightOptions.collectDiagnostics = diagnostics;
      const auto initialWallStart = Clock::now();
#if defined(_WIN32) || defined(__linux__)
      const double initialCpuStart = threadCpuMs();
#endif
      world.enableLighting(lightOptions);
      // Create the worker while the process mask contains both cores, then
      // restrict the simulation thread. This avoids inherited creator affinity
      // preventing the dedicated light thread from moving to CPU 4.
      bindMain();
      light = world.lightService();
      if (!light || !light->waitIdleForTesting())
        throw std::runtime_error("initial lighting failed");
      initialBeforeDrain = light->stats();
      light->drainPublished();
      initialAfterDrain = light->stats();
      if (initialAfterDrain.workerCount != 1)
        throw std::runtime_error("light worker count is not one");
      if (lightAffinity &&
          (initialAfterDrain.blockWorkerAffinityError ||
           initialAfterDrain.blockWorkerAffinityMask != lightAffinity))
        throw std::runtime_error(
            "could not bind the block light worker to the requested CPU mask");
      initialWallMs =
          std::chrono::duration<double, std::milli>(Clock::now() - initialWallStart)
              .count();
#if defined(_WIN32) || defined(__linux__)
      initialMainCpuMs = threadCpuMs() - initialCpuStart;
#endif
      std::printf(
          "LIGHT_INITIAL wall_ms=%.3f main_cpu_ms=%.3f worker_cpu_ms=%.3f "
          "worker_affinity_requested=0x%llx worker_affinity=0x%llx "
          "worker_affinity_error=%u workers=%llu block_cpu_ms=%.3f "
          "sky_cpu_ms=%.3f block_affinity_requested=0x%llx "
          "block_affinity=0x%llx block_affinity_error=%u "
          "sky_affinity_requested=0x%llx sky_affinity=0x%llx "
          "sky_affinity_error=%u submitted=%llu batches=%llu changes=%llu "
          "published=%llu pending_chunks=%llu pending_publications=%llu\n",
          initialWallMs, initialMainCpuMs, initialAfterDrain.workerCpuMs,
          (unsigned long long)initialAfterDrain.workerAffinityRequested,
          (unsigned long long)initialAfterDrain.workerAffinityMask,
          initialAfterDrain.workerAffinityError,
          (unsigned long long)initialAfterDrain.workerCount,
          initialAfterDrain.blockWorkerCpuMs,
          initialAfterDrain.skyWorkerCpuMs,
          (unsigned long long)initialAfterDrain.blockWorkerAffinityRequested,
          (unsigned long long)initialAfterDrain.blockWorkerAffinityMask,
          initialAfterDrain.blockWorkerAffinityError,
          (unsigned long long)initialAfterDrain.skyWorkerAffinityRequested,
          (unsigned long long)initialAfterDrain.skyWorkerAffinityMask,
          initialAfterDrain.skyWorkerAffinityError,
          (unsigned long long)initialAfterDrain.submitted,
          (unsigned long long)initialAfterDrain.processedBatches,
          (unsigned long long)initialAfterDrain.processedBlockChanges,
          (unsigned long long)initialAfterDrain.published,
          (unsigned long long)initialBeforeDrain.pendingChunks,
          (unsigned long long)initialBeforeDrain.pendingPublications);
      light->resetMeasurementStats();
    }
    bindMain();
    Hash events;
    if (fingerprints)
      world.blockEventObserver = [&](const BlockEvent &e) {
        events.add(world.gameTime());
        events.pos(e.pos);
        events.text(e.block.view());
        events.add(e.type);
        events.add(e.data);
      };
    std::vector<double> perTick;
    perTick.reserve(static_cast<std::size_t>(ticks));
    double total = 0;
#ifdef REDSTONE_COUNT_ALLOCATIONS
    std::size_t allocs = 0, bytes = 0;
#endif
#if defined(_WIN32) || defined(__linux__)
    const auto cpuStart = threadCpuMs();
#endif
    for (int tick = 1; tick <= ticks; ++tick) {
#ifdef REDSTONE_COUNT_ALLOCATIONS
      const auto before = memory::allocationCounts();
#endif
      auto start = Clock::now();
      world.tick();
      world.log.clear();
      const double ms =
          std::chrono::duration<double, std::milli>(Clock::now() - start)
              .count();
#ifdef REDSTONE_COUNT_ALLOCATIONS
      const auto after = memory::allocationCounts();
      allocs += after.allocations - before.allocations;
      bytes += after.requestedBytes - before.requestedBytes;
#endif
      total += ms;
      perTick.push_back(ms);
      if (fingerprints && (tick % 250 == 0 || tick == ticks)) {
        if (light) {
          if (!light->waitIdleForTesting())
            throw std::runtime_error("light worker failed during fingerprint");
          light->drainPublished();
        }
        fingerprint(world, tick, minX, maxX, minZ, maxZ, events);
      }
    }
#if defined(_WIN32) || defined(__linux__)
    // Main-loop CPU only. Formal runs do not enable fingerprints.
    const auto cpuMs = threadCpuMs() - cpuStart;
#endif
    // Freeze the worker gauges immediately after the last measured tick and
    // main-thread CPU sample. Sorting, printing and the final drain stay outside
    // the formal boundary.
    LightService::Stats atTickEnd;
    if (light)
      atTickEnd = light->stats();
    std::sort(perTick.begin(), perTick.end());
    const auto slowTicks = static_cast<std::size_t>(std::count_if(
        perTick.begin(), perTick.end(), [](double ms) { return ms > 50.0; }));
    std::printf("TOTAL ticks=%d total_ms=%.3f mean_ms=%.3f median_ms=%.3f "
                "p95_ms=%.3f p99_ms=%.3f max_ms=%.3f over50=%zu "
                "lighting=%s diagnostics=%s\n",
                ticks, total, total / ticks, perTick[perTick.size() / 2],
                perTick[perTick.size() * 95 / 100],
                perTick[perTick.size() * 99 / 100], perTick.back(), slowTicks,
                lighting ? "on" : "off", diagnostics ? "on" : "off");
    std::printf(
        "FINAL entities=%zu scheduled=%zu moving_pistons=%zu events=%zu\n",
        world.entities.size(), world.scheduledTickCount(),
        world.pistonEntities.size(), world.blockEventCount());
#if defined(_WIN32) || defined(__linux__)
    std::printf("CPU loop_thread_ms=%.3f\n", cpuMs);
#endif
    if (light) {
      std::printf(
          "LIGHT_TICK submitted=%llu rejected=%llu dropped_geometry=%llu "
          "batches=%llu changes=%llu "
          "published=%llu stale=%llu sky_sections=%llu block_sections=%llu "
          "queue_high=%llu light_queue=%llu pending_chunks=%llu "
          "pending_publications=%llu "
          "received_sky_sections=%llu received_block_sections=%llu "
          "worker_cpu_ms_completed_lower_bound=%.3f "
          "block_worker_cpu_ms=%.3f sky_worker_cpu_ms=%.3f\n",
          (unsigned long long)atTickEnd.submitted,
          (unsigned long long)atTickEnd.rejected,
          (unsigned long long)atTickEnd.droppedGeometryTasks,
          (unsigned long long)atTickEnd.processedBatches,
          (unsigned long long)atTickEnd.processedBlockChanges,
          (unsigned long long)atTickEnd.published,
          (unsigned long long)atTickEnd.stalePublications,
          (unsigned long long)atTickEnd.skySectionChanges,
          (unsigned long long)atTickEnd.blockSectionChanges,
          (unsigned long long)atTickEnd.queueHighWater,
          (unsigned long long)atTickEnd.lightQueueEntries,
          (unsigned long long)atTickEnd.pendingChunks,
          (unsigned long long)atTickEnd.pendingPublications,
          (unsigned long long)atTickEnd.receivedSkySections,
          (unsigned long long)atTickEnd.receivedBlockSections,
          atTickEnd.workerCpuMs - initialAfterDrain.workerCpuMs,
          atTickEnd.blockWorkerCpuMs - initialAfterDrain.blockWorkerCpuMs,
          atTickEnd.skyWorkerCpuMs - initialAfterDrain.skyWorkerCpuMs);
#if defined(_WIN32) || defined(__linux__)
      const double drainCpuStart = threadCpuMs();
#endif
      const auto waitStart = Clock::now();
      if (!light->waitIdleForTesting())
        throw std::runtime_error("light worker failed before final drain");
      const double waitMs =
          std::chrono::duration<double, std::milli>(Clock::now() - waitStart)
              .count();
      const auto drainStart = Clock::now();
      light->drainPublished();
      const double drainMs =
          std::chrono::duration<double, std::milli>(Clock::now() - drainStart)
              .count();
#if defined(_WIN32) || defined(__linux__)
      const double drainMainCpuMs = threadCpuMs() - drainCpuStart;
#else
      const double drainMainCpuMs = 0;
#endif
      const auto afterDrain = light->stats();
      std::printf(
          "LIGHT_DRAIN wait_ms=%.3f drain_ms=%.3f main_cpu_ms=%.3f "
          "worker_cpu_delta_ms=%.3f pending_chunks_after=%llu "
          "pending_publications_after=%llu worker_affinity=0x%llx "
          "workers=%llu block_worker_cpu_delta_ms=%.3f "
          "sky_worker_cpu_delta_ms=%.3f sky_affinity=0x%llx\n",
          waitMs, drainMs, drainMainCpuMs,
          afterDrain.workerCpuMs - initialAfterDrain.workerCpuMs,
          (unsigned long long)afterDrain.pendingChunks,
          (unsigned long long)afterDrain.pendingPublications,
          (unsigned long long)afterDrain.workerAffinityMask,
          (unsigned long long)afterDrain.workerCount,
          afterDrain.blockWorkerCpuMs - initialAfterDrain.blockWorkerCpuMs,
          afterDrain.skyWorkerCpuMs - initialAfterDrain.skyWorkerCpuMs,
          (unsigned long long)afterDrain.skyWorkerAffinityMask);
    }
#ifdef REDSTONE_COUNT_ALLOCATIONS
    std::printf("ALLOCATIONS tick_new_calls=%zu tick_requested_bytes=%zu "
                "(global C++ new only)\n",
                allocs, bytes);
#endif
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 2;
  }
}
