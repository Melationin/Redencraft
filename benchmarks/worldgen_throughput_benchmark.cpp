// 离线世界生成吞吐基准：在内存中连续生成 size x size 个 FULL 区块（不写盘、不安装 World），
// 统计墙钟/CPU/每区块耗时与调度器统计，用于优化前后的同机对比。

#include "redstone/allocator.hpp"
#include "redstone/dimension.hpp"
#include "redstone/engine.hpp"
#include "redstone/worldgen.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <sys/resource.h>
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

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: worldgen_throughput_benchmark <assets/worldgen26_1> "
                 "[--seed N] [--size N] [--threads N] [--repeat N] "
                 "[--no-strongholds] [--dim overworld|nether|end]\n"
                 "       [--light] [--low-memory|--no-low-memory] "
                 "[--serialize] [--install]\n"
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
  bool lowMemory = true;
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
    } else if (arg == "--low-memory") {
      lowMemory = true;
    } else if (arg == "--no-low-memory") {
      lowMemory = false;
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
  if (size <= 0 || repeat <= 0)
    return 2;

  const WorldgenRegistries registries =
      WorldgenRegistries::loadFromDirectory(assetsDir);

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
        // 低内存模式让光照工作线程按需直读 World 区块几何，不再为每个区块
        // 常驻一份几何镜像（必须在 enableLighting 之前设置）。
        world->setLowMemoryMode(lowMemory);
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
      lightContext->set(world->lightService()->opticalTable());
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

    std::printf(
        "run=%d chunks=%zu wall_ms=%.1f cpu_ms=%.1f ms_per_chunk=%.2f "
        "created=%zu steps=%zu inline=%zu parallel=%zu resident=%zu "
        "strongholds=%d rss_peak_kb=%ld\n",
        run, chunkCount, wallMs, cpuMs, wallMs / static_cast<double>(chunkCount),
        stats.chunksCreated, stats.stepsExecuted, stats.inlineSteps,
        stats.parallelDispatches, resident, strongholds ? 1 : 0, peakRssKb());
    std::fflush(stdout);

    if (world) {
      // 批处理场景的收敛点：全部区块到 Full 后让光照队列排空一次，之后逐块
      // 刷新快照就是同一不动点的稳定值（流式场景由邻居到达时的增量更新保证）。
      if (world->lightService() != nullptr)
        world->lightService()->flushAndWait();
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
        std::printf("light_stats batches=%llu published=%llu "
                    "worker_cpu_ms=%.1f block_cpu_ms=%.1f sky_cpu_ms=%.1f "
                    "light_queue=%llu unloaded_skips=%llu\n",
                    static_cast<unsigned long long>(ls.processedBatches),
                    static_cast<unsigned long long>(ls.published),
                    ls.workerCpuMs, ls.blockWorkerCpuMs, ls.skyWorkerCpuMs,
                    static_cast<unsigned long long>(ls.lightQueueEntries),
                    static_cast<unsigned long long>(ls.unloadedNeighborSkips));
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
      scheduler.forEachHeldChunk([&](const ChunkAccess &access) {
        const auto *proto = dynamic_cast<const ProtoChunk *>(&access);
        if (proto == nullptr) {
          ++otherChunks;
          return;
        }
        const ProtoChunkMemoryUsage one = proto->memoryUsage();
        usage.add(one);
        ++protoChunks;
        const std::size_t statusIndex =
            static_cast<std::size_t>(access.persistedStatus());
        if (statusIndex < statusHistogram.size())
          ++statusHistogram[statusIndex];
      });
      for (const ChunkBatchRequest &request : requests) {
        const auto *proto =
            dynamic_cast<const ProtoChunk *>(scheduler.findChunk(request.pos));
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
