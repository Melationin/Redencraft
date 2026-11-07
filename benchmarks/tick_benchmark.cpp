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
}

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 4)
      throw std::runtime_error(
          "usage: redstone_tick_benchmark <litematic> [ticks] [--fingerprint]");
    int ticks = 3000;
    bool fingerprints = false, haveTicks = false;
    for (int i = 2; i < argc; ++i) {
      std::string_view arg(argv[i]);
      if (arg == "--fingerprint" && !fingerprints) {
        fingerprints = true;
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
#ifdef _WIN32
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
      if (fingerprints && (tick % 250 == 0 || tick == ticks))
        fingerprint(world, tick, minX, maxX, minZ, maxZ, events);
    }
#ifdef _WIN32
    // Whole loop CPU, including diagnostics when --fingerprint is selected.
    const auto cpuMs = threadCpuMs() - cpuStart;
#endif
    std::sort(perTick.begin(), perTick.end());
    std::printf("TOTAL ticks=%d total_ms=%.3f mean_ms=%.3f median_ms=%.3f "
                "p95_ms=%.3f\n",
                ticks, total, total / ticks, perTick[perTick.size() / 2],
                perTick[perTick.size() * 95 / 100]);
    std::printf(
        "FINAL entities=%zu scheduled=%zu moving_pistons=%zu events=%zu\n",
        world.entities.size(), world.scheduledTickCount(),
        world.pistonEntities.size(), world.blockEventCount());
#ifdef _WIN32
    std::printf("CPU loop_thread_ms=%.3f\n", cpuMs);
#endif
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
