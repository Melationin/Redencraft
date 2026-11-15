#include "redstone/allocator.hpp"
#include "redstone/engine.hpp"
#include "redstone/storage.hpp"
#include "redstone/terrain_codec.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __linux__
#include <cerrno>
#include <sched.h>
#include <system_error>
#include <time.h>
#endif

using namespace redstone;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
  std::filesystem::path worldDir = "validation/random_tick_bench/runtime/world_master";
  int chunkCount = 9000;
  int warmupTicks = 100;
  int measureTicks = 1000;
  int randomTickSpeed = 3;
  bool diag = false;
};

int parseIntArg(std::string_view value, const char *flag) {
  int parsed = 0;
  const auto *begin = value.data();
  const auto *end = begin + value.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc{} || ptr != end || parsed < 0) {
    throw std::runtime_error(std::string("Invalid integer for ") + flag + ": " +
                             std::string(value));
  }
  return parsed;
}

Options parseArgs(int argc, char **argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto needValue = [&](const char *flag) -> std::string_view {
      if (i + 1 >= argc)
        throw std::runtime_error(std::string("Missing value for ") + flag);
      return argv[++i];
    };
    if (arg == "--world") {
      opt.worldDir = needValue("--world");
    } else if (arg == "--chunks") {
      opt.chunkCount = parseIntArg(needValue("--chunks"), "--chunks");
    } else if (arg == "--warmup") {
      opt.warmupTicks = parseIntArg(needValue("--warmup"), "--warmup");
    } else if (arg == "--ticks") {
      opt.measureTicks = parseIntArg(needValue("--ticks"), "--ticks");
    } else if (arg == "--speed") {
      opt.randomTickSpeed = parseIntArg(needValue("--speed"), "--speed");
    } else if (arg == "--diag") {
      opt.diag = true;
    } else {
      throw std::runtime_error("Unknown option: " + std::string(arg));
    }
  }
  if (opt.chunkCount <= 0 || opt.measureTicks <= 0) {
    throw std::runtime_error("--chunks and --ticks must be positive");
  }
  return opt;
}

std::vector<std::pair<int, int>> buildChunkTargets(int count) {
  constexpr int kRadius = 47; // (2 * 47 + 1)^2 = 9025 >= 9000
  std::vector<std::pair<int, int>> out;
  out.reserve(static_cast<std::size_t>(count));
  for (int cz = -kRadius; cz <= kRadius && static_cast<int>(out.size()) < count;
       ++cz) {
    for (int cx = -kRadius; cx <= kRadius && static_cast<int>(out.size()) < count;
         ++cx) {
      out.emplace_back(cx, cz);
    }
  }
  return out;
}

struct Stats {
  double totalMs = 0.0;
  double meanMs = 0.0;
  double medianMs = 0.0;
  double p95Ms = 0.0;
  double p99Ms = 0.0;
  double maxMs = 0.0;
};

Stats summarize(std::vector<std::int64_t> samples) {
  std::sort(samples.begin(), samples.end());
  const std::size_t n = samples.size();
  double sum = 0.0;
  for (std::int64_t ns : samples) {
    sum += static_cast<double>(ns);
  }
  const double totalMs = sum / 1.0e6;
  const double meanMs = totalMs / static_cast<double>(n);
  const double medianMs =
      (n % 2 == 1
           ? static_cast<double>(samples[n / 2])
           : (static_cast<double>(samples[n / 2 - 1]) +
              static_cast<double>(samples[n / 2])) /
                 2.0) /
      1.0e6;
  const std::size_t p95Idx = std::min(
      n - 1, static_cast<std::size_t>(std::floor(0.95 * static_cast<double>(n - 1))));
  const std::size_t p99Idx = std::min(
      n - 1, static_cast<std::size_t>(std::floor(0.99 * static_cast<double>(n - 1))));
  return Stats{
      .totalMs = totalMs,
      .meanMs = meanMs,
      .medianMs = medianMs,
      .p95Ms = static_cast<double>(samples[p95Idx]) / 1.0e6,
      .p99Ms = static_cast<double>(samples[p99Idx]) / 1.0e6,
      .maxMs = static_cast<double>(samples.back()) / 1.0e6,
  };
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options opt = parseArgs(argc, argv);
    const std::filesystem::path regionDir = defaultRegionDirectory(opt.worldDir);
    const auto targets = buildChunkTargets(opt.chunkCount);
    if (static_cast<int>(targets.size()) != opt.chunkCount) {
      throw std::runtime_error("Target grid smaller than requested chunkCount");
    }

    const auto loadStart = Clock::now();
    World world(-64, 320);
    terrain::DecodeOptions decodeOpt;
    decodeOpt.allowUnimplementedBlocks = true;

    std::size_t nonAirBlocks = 0;
    std::size_t unsupportedPaletteCells = 0;
    for (const auto [cx, cz] : targets) {
      terrain::DecodedChunk decoded =
          terrain::decodeChunkAt(regionDir, cx, cz, decodeOpt);
      if (decoded.status != terrain::DecodeStatus::Ok) {
        throw std::runtime_error("Failed to decode chunk (" + std::to_string(cx) +
                                 ", " + std::to_string(cz) + "): " +
                                 decoded.detail);
      }
      world.loadChunk(cx, cz);

      PersistedChunkLight persisted;
      persisted.isLightOn = decoded.isLightOn;
      if (decoded.isLightOn) {
        persisted.sections.reserve(decoded.sections.size());
      }

      for (const terrain::Section &sec : decoded.sections) {
        if (decoded.isLightOn && (sec.blockLight || sec.skyLight)) {
          persisted.sections.push_back(PersistedLightSection{
              .sectionY = sec.y,
              .blockLight = sec.blockLight,
              .skyLight = sec.skyLight,
          });
        }
        if (!sec.blockStates) {
          continue;
        }
        const auto &packed = *sec.blockStates;
        const int baseY = static_cast<int>(sec.y) * 16;
        if (packed.bitsPerEntry == 0) {
          const auto &entry = packed.palette[0];
          if (entry.state.isAir()) {
            continue;
          }
          for (int i = 0; i < 4096; ++i) {
            const int y = baseY + (i >> 8);
            if (y < world.minY() || y >= world.maxY())
              continue;
            const int x = cx * 16 + (i & 15);
            const int z = cz * 16 + ((i >> 4) & 15);
            world.rawSetBlock(Pos{x, y, z}, entry.state);
            ++nonAirBlocks;
            if (!entry.supported)
              ++unsupportedPaletteCells;
          }
          continue;
        }
        for (int i = 0; i < 4096; ++i) {
          const std::size_t pIdx = packed.cell(static_cast<std::size_t>(i));
          if (pIdx >= packed.palette.size()) {
            throw std::runtime_error("Invalid palette index in chunk (" +
                                     std::to_string(cx) + ", " +
                                     std::to_string(cz) + ")");
          }
          const auto &entry = packed.palette[pIdx];
          if (entry.state.isAir())
            continue;
          const int y = baseY + (i >> 8);
          if (y < world.minY() || y >= world.maxY())
            continue;
          const int x = cx * 16 + (i & 15);
          const int z = cz * 16 + ((i >> 4) & 15);
          world.rawSetBlock(Pos{x, y, z}, entry.state);
          ++nonAirBlocks;
          if (!entry.supported)
            ++unsupportedPaletteCells;
        }
      }

      if (decoded.isLightOn) {
        world.setChunkPersistedLight(cx, cz, std::move(persisted));
      }
    }

    world.primeHeightmaps();
    world.randomTickSpeed = opt.randomTickSpeed;
    const auto loadEnd = Clock::now();
    const double loadMs =
        std::chrono::duration<double, std::milli>(loadEnd - loadStart).count();

    for (int i = 0; i < opt.warmupTicks; ++i) {
      world.tick();
    }

    world.randomTickStats.reset();
    std::unordered_map<std::string, std::uint64_t> unsupportedCounts;
    if (opt.diag) {
      world.unsupportedRandomTickObserver = [&](Pos pos, BlockState state,
                                                bool isFluid) {
        std::string key = (isFluid ? "fluid:" : "block:") +
                          std::string(state.block().name());
        if (!isFluid && state.is(Blocks::GRASS_BLOCK)) {
          key += " (above=" +
                 std::string(world.getBlock(pos.relative(Direction::Up))
                                 .block()
                                 .name()) +
                 ")";
        }
        ++unsupportedCounts[std::move(key)];
      };
    }
    std::vector<std::int64_t> fullTickNs;
    std::vector<std::int64_t> randomTickNs;
    fullTickNs.reserve(static_cast<std::size_t>(opt.measureTicks));
    randomTickNs.reserve(static_cast<std::size_t>(opt.measureTicks));
    for (int i = 0; i < opt.measureTicks; ++i) {
      const auto t0 = Clock::now();
      world.tick();
      const auto t1 = Clock::now();
      fullTickNs.push_back(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      randomTickNs.push_back(
          static_cast<std::int64_t>(world.randomTickStats.lastTickNs));
    }

    const Stats fullStats = summarize(std::move(fullTickNs));
    const Stats randomStats = summarize(std::move(randomTickNs));
    const auto &rt = world.randomTickStats;
    std::printf(
        "CPP_RTBENCH_RESULT allocator=%s ticks=%d chunks=%d speed=%d load_ms=%.3f "
        "non_air=%zu unsupported_cells=%zu "
        "randomtick_total_ms=%.3f randomtick_mean_ms=%.4f randomtick_median_ms=%.4f randomtick_p95_ms=%.4f randomtick_p99_ms=%.4f randomtick_max_ms=%.4f "
        "full_total_ms=%.3f full_mean_ms=%.4f full_median_ms=%.4f full_p95_ms=%.4f full_p99_ms=%.4f full_max_ms=%.4f "
        "chunks_visited=%llu sections_visited=%llu sections_sparse=%llu sections_dense=%llu "
        "positions_probed=%llu blocks_ticked=%llu fluids_ticked=%llu ticks_applied=%llu ticks_unsupported=%llu\n",
        memory::backend(), opt.measureTicks, opt.chunkCount, opt.randomTickSpeed,
        loadMs, nonAirBlocks, unsupportedPaletteCells,
        randomStats.totalMs, randomStats.meanMs, randomStats.medianMs,
        randomStats.p95Ms, randomStats.p99Ms, randomStats.maxMs,
        fullStats.totalMs, fullStats.meanMs, fullStats.medianMs,
        fullStats.p95Ms, fullStats.p99Ms, fullStats.maxMs,
        static_cast<unsigned long long>(rt.chunksVisited),
        static_cast<unsigned long long>(rt.sectionsVisited),
        static_cast<unsigned long long>(rt.sectionsSparsePath),
        static_cast<unsigned long long>(rt.sectionsDensePath),
        static_cast<unsigned long long>(rt.positionsProbed),
        static_cast<unsigned long long>(rt.blocksTicked),
        static_cast<unsigned long long>(rt.fluidsTicked),
        static_cast<unsigned long long>(rt.ticksApplied),
        static_cast<unsigned long long>(rt.ticksUnsupported));
    if (opt.diag && !unsupportedCounts.empty()) {
      std::vector<std::pair<std::uint64_t, std::string>> sorted;
      sorted.reserve(unsupportedCounts.size());
      for (const auto &[k, v] : unsupportedCounts) {
        sorted.emplace_back(v, k);
      }
      std::sort(sorted.begin(), sorted.end(),
                [](const auto &a, const auto &b) { return a.first > b.first; });
      std::printf("--- Unsupported RandomTick Breakdown (%zu kinds) ---\n",
                  sorted.size());
      for (const auto &[cnt, name] : sorted) {
        std::printf("  %10llu  %s\n", static_cast<unsigned long long>(cnt),
                    name.c_str());
      }
    }
    return 0;
  } catch (const std::exception &ex) {
    std::fprintf(stderr, "random_tick_benchmark error: %s\n", ex.what());
    return 1;
  }
}
