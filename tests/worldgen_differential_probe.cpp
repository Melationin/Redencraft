#include "redstone/engine.hpp"
#include "redstone/worldgen.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace redstone;
using namespace redstone::worldgen;

struct ProbeRequest {
  std::int64_t seed = 0;
  std::string dimension;
  int cx = 0;
  int cz = 0;
  std::vector<std::string> stages;
};

ChunkStatusId parseStage(std::string_view name) {
  if (name == "BIOMES")
    return ChunkStatusId::Biomes;
  if (name == "NOISE")
    return ChunkStatusId::Noise;
  if (name == "SURFACE")
    return ChunkStatusId::Surface;
  if (name == "CARVERS")
    return ChunkStatusId::Carvers;
  if (name == "FEATURES")
    return ChunkStatusId::Features;
  if (name == "FULL")
    return ChunkStatusId::Full;
  throw std::invalid_argument("unknown stage: " + std::string(name));
}

std::string jsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (unsigned char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::string formatCanonicalState(const BlockState &s) {
  const auto props = s.properties();
  if (props.empty())
    return s.name();
  std::string out = s.name();
  out.push_back('[');
  bool first = true;
  for (const auto &[k, v] : props) {
    if (!first)
      out.push_back(',');
    first = false;
    out += k;
    out.push_back('=');
    out += v;
  }
  out.push_back(']');
  return out;
}

std::unique_ptr<NoiseBasedChunkGenerator>
makeGenerator(const WorldgenRegistries &reg, const std::string &dim) {
  if (dim == "minecraft:overworld") {
    const auto *settings = reg.findNoiseSettings("minecraft:overworld");
    if (!settings)
      throw std::runtime_error("missing minecraft:overworld noise settings");
    return std::make_unique<NoiseBasedChunkGenerator>(
        reg, *settings,
        std::make_shared<MultiNoiseBiomeSource>(
            MultiNoiseBiomeSource::createOverworld()));
  }
  if (dim == "minecraft:the_nether") {
    const auto *settings = reg.findNoiseSettings("minecraft:nether");
    if (!settings)
      throw std::runtime_error("missing minecraft:nether noise settings");
    return std::make_unique<NoiseBasedChunkGenerator>(
        reg, *settings,
        std::make_shared<MultiNoiseBiomeSource>(
            MultiNoiseBiomeSource::createNether()),
        redstone::netherDimensionType().minY,
        redstone::netherDimensionType().height);
  }
  if (dim == "minecraft:the_end") {
    const auto *settings = reg.findNoiseSettings("minecraft:end");
    if (!settings)
      throw std::runtime_error("missing minecraft:end noise settings");
    return std::make_unique<NoiseBasedChunkGenerator>(
        reg, *settings, std::make_shared<TheEndBiomeSource>(),
        redstone::endDimensionType().minY,
        redstone::endDimensionType().height);
  }
  throw std::invalid_argument("unsupported dimension: " + dim);
}

void writeChunkSnapshot(std::ostream &os, std::int64_t seed,
                        const std::string &dim, ChunkTicketPos pos,
                        const std::string &stageName, ChunkAccess &chunk) {
  const int minY = chunk.minY();
  const int height = chunk.height();
  const int baseX = pos.x << 4;
  const int baseZ = pos.z << 4;

  // 1. Quart biomes (qy, qz, qx)
  std::vector<std::string> biomePalette;
  std::unordered_map<std::string, int> biomeIdx;
  std::vector<std::pair<int, int>> biomeRle;
  const int quartMinY = minY >> 2;
  const int quartHeight = height >> 2;
  const int quartBaseX = pos.x << 2;
  const int quartBaseZ = pos.z << 2;
  for (int qy = 0; qy < quartHeight; ++qy) {
    for (int qz = 0; qz < 4; ++qz) {
      for (int qx = 0; qx < 4; ++qx) {
        const std::string biome(
            chunk.getNoiseBiome(quartBaseX + qx, quartMinY + qy, quartBaseZ + qz));
        auto [it, inserted] =
            biomeIdx.emplace(biome, static_cast<int>(biomePalette.size()));
        if (inserted)
          biomePalette.push_back(biome);
        const int idx = it->second;
        if (!biomeRle.empty() && biomeRle.back().first == idx) {
          ++biomeRle.back().second;
        } else {
          biomeRle.emplace_back(idx, 1);
        }
      }
    }
  }

  // 2. Blocks (y, z, x)
  std::vector<std::string> blockPalette;
  std::unordered_map<BlockState::Id, int> blockIdx;
  std::vector<std::pair<int, int>> blockRle;
  for (int y = minY; y < minY + height; ++y) {
    for (int lz = 0; lz < 16; ++lz) {
      for (int lx = 0; lx < 16; ++lx) {
        const BlockState st = chunk.getBlockState(baseX + lx, y, baseZ + lz);
        auto it = blockIdx.find(st.id());
        int idx = 0;
        if (it == blockIdx.end()) {
          idx = static_cast<int>(blockPalette.size());
          blockPalette.push_back(formatCanonicalState(st));
          blockIdx.emplace(st.id(), idx);
        } else {
          idx = it->second;
        }
        if (!blockRle.empty() && blockRle.back().first == idx) {
          ++blockRle.back().second;
        } else {
          blockRle.emplace_back(idx, 1);
        }
      }
    }
  }

  // 3. Heightmaps
  const struct {
    const char *key;
    HeightmapType type;
  } hmTypes[] = {
      {"WORLD_SURFACE_WG", HeightmapType::WorldSurfaceWG},
      {"OCEAN_FLOOR_WG", HeightmapType::OceanFloorWG},
      {"MOTION_BLOCKING", HeightmapType::MotionBlocking},
      {"WORLD_SURFACE", HeightmapType::WorldSurface},
  };

  os << "{\"seed\":" << seed << ",\"dim\":\"" << jsonEscape(dim)
     << "\",\"cx\":" << pos.x << ",\"cz\":" << pos.z << ",\"stage\":\""
     << jsonEscape(stageName) << "\",\"min_y\":" << minY
     << ",\"height\":" << height << ",\"biome_palette\":[";
  for (std::size_t i = 0; i < biomePalette.size(); ++i) {
    if (i > 0)
      os << ',';
    os << '"' << jsonEscape(biomePalette[i]) << '"';
  }
  os << "],\"biome_rle\":[";
  for (std::size_t i = 0; i < biomeRle.size(); ++i) {
    if (i > 0)
      os << ',';
    os << '[' << biomeRle[i].first << ',' << biomeRle[i].second << ']';
  }
  os << "],\"heightmaps\":{";
  for (std::size_t t = 0; t < 4; ++t) {
    if (t > 0)
      os << ',';
    os << '"' << hmTypes[t].key << "\":[";
    bool first = true;
    for (int lz = 0; lz < 16; ++lz) {
      for (int lx = 0; lx < 16; ++lx) {
        if (!first)
          os << ',';
        first = false;
        os << chunk.getHeight(hmTypes[t].type, lx, lz);
      }
    }
    os << ']';
  }
  os << "},\"block_palette\":[";
  for (std::size_t i = 0; i < blockPalette.size(); ++i) {
    if (i > 0)
      os << ',';
    os << '"' << jsonEscape(blockPalette[i]) << '"';
  }
  os << "],\"block_rle\":[";
  for (std::size_t i = 0; i < blockRle.size(); ++i) {
    if (i > 0)
      os << ',';
    os << '[' << blockRle[i].first << ',' << blockRle[i].second << ']';
  }
  os << "]}\n";
}

std::vector<ProbeRequest> parseSpec(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot open spec file: " + path.string());
  std::vector<ProbeRequest> out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream iss(line);
    ProbeRequest req;
    std::string stagesCsv;
    if (!(iss >> req.seed >> req.dimension >> req.cx >> req.cz >> stagesCsv))
      throw std::runtime_error("invalid spec line: " + line);
    std::stringstream ss(stagesCsv);
    std::string item;
    while (std::getline(ss, item, ',')) {
      if (!item.empty())
        req.stages.push_back(item);
    }
    out.push_back(std::move(req));
  }
  return out;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    std::cerr << "usage: worldgen_differential_probe <assets/worldgen26_1> "
                 "<spec.txt> <out.jsonl> [--threads N] [--window W]\n";
    return 2;
  }
  try {
    const std::filesystem::path assetsDir(argv[1]);
    const std::filesystem::path specPath(argv[2]);
    const std::filesystem::path outPath(argv[3]);
    std::size_t threadCount = 0;
    std::size_t windowSize = 16;
    for (int i = 4; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      if (arg == "--threads" && i + 1 < argc) {
        threadCount = static_cast<std::size_t>(std::stoul(argv[++i]));
      } else if (arg == "--window" && i + 1 < argc) {
        windowSize = static_cast<std::size_t>(std::stoul(argv[++i]));
        if (windowSize == 0)
          windowSize = 1;
      }
    }

    const WorldgenRegistries reg =
        WorldgenRegistries::loadFromDirectory(assetsDir);
    const std::vector<ProbeRequest> requests = parseSpec(specPath);

    std::ofstream out(outPath, std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot open output: " + outPath.string());

    struct DimState {
      std::unique_ptr<NoiseBasedChunkGenerator> gen;
      std::unique_ptr<WorldGenScheduler> sched;
    };
    std::map<std::pair<std::int64_t, std::string>, DimState> schedulers;

    auto getDimState = [&](std::int64_t seed,
                           const std::string &dim) -> DimState & {
      const auto key = std::make_pair(seed, dim);
      auto it = schedulers.find(key);
      if (it == schedulers.end()) {
        DimState st;
        st.gen = makeGenerator(reg, dim);
        WorldGenSchedulerConfig cfg;
        cfg.threadCount = threadCount;
        st.sched = std::make_unique<WorldGenScheduler>(
            *st.gen, seed, nullptr, nullptr, nullptr, nullptr, cfg);
        it = schedulers.emplace(key, std::move(st)).first;
      }
      return it->second;
    };

    std::size_t idx = 0;
    while (idx < requests.size()) {
      const std::int64_t winSeed = requests[idx].seed;
      const std::string &winDim = requests[idx].dimension;
      std::size_t endIdx = idx;
      while (endIdx < requests.size() && endIdx - idx < windowSize &&
             requests[endIdx].seed == winSeed &&
             requests[endIdx].dimension == winDim) {
        ++endIdx;
      }

      DimState &st = getDimState(winSeed, winDim);
      // Collect ordered distinct stages across [idx, endIdx).
      std::vector<std::pair<ChunkStatusId, std::string>> stageOrder;
      for (std::size_t k = idx; k < endIdx; ++k) {
        for (const std::string &sName : requests[k].stages) {
          const ChunkStatusId sid = parseStage(sName);
          bool exists = false;
          for (const auto &p : stageOrder) {
            if (p.first == sid) {
              exists = true;
              break;
            }
          }
          if (!exists)
            stageOrder.emplace_back(sid, sName);
        }
      }
      std::sort(stageOrder.begin(), stageOrder.end(),
                [](const auto &a, const auto &b) {
                  return static_cast<int>(a.first) < static_cast<int>(b.first);
                });

      for (const auto &[sid, sName] : stageOrder) {
        std::vector<ChunkBatchRequest> batch;
        std::vector<std::size_t> reqIndices;
        for (std::size_t k = idx; k < endIdx; ++k) {
          bool wants = false;
          for (const std::string &reqStage : requests[k].stages) {
            if (parseStage(reqStage) == sid) {
              wants = true;
              break;
            }
          }
          if (wants) {
            batch.push_back(ChunkBatchRequest{
                ChunkTicketPos{requests[k].cx, requests[k].cz}, sid});
            reqIndices.push_back(k);
          }
        }
        if (batch.empty())
          continue;
        st.sched->ensureStatusBatch(batch, batch.front().pos);
        for (std::size_t k : reqIndices) {
          const ChunkTicketPos pos{requests[k].cx, requests[k].cz};
          ChunkAccess *chunk = st.sched->findChunk(pos);
          if (!chunk)
            throw std::runtime_error("missing scheduled chunk snapshot");
          writeChunkSnapshot(out, winSeed, winDim, pos, sName, *chunk);
        }
      }

      // Retain only peripheral holders near remaining future requests in this (seed, dim).
      std::vector<ChunkTicketPos> keepCenters;
      for (std::size_t fut = endIdx; fut < requests.size(); ++fut) {
        if (requests[fut].seed == winSeed && requests[fut].dimension == winDim) {
          keepCenters.push_back(
              ChunkTicketPos{requests[fut].cx, requests[fut].cz});
        }
      }
      st.sched->evictIdleOutsideRadius(keepCenters, 12);
      idx = endIdx;
    }
    out.flush();
    for (const auto &[key, st] : schedulers) {
      const auto s = st.sched->stats();
      std::cerr << "[scheduler] " << key.second
                << " created=" << s.chunksCreated
                << " steps=" << s.stepsExecuted
                << " inline=" << s.inlineSteps
                << " parallel=" << s.parallelDispatches
                << " colored=" << s.coloredDispatches
                << " evicted=" << s.holdersEvicted << "\n";
    }
    std::cout << "CPP_WORLDGEN_DIFF_OK " << requests.size() << "\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "worldgen_differential_probe failed: " << ex.what() << "\n";
    return 1;
  }
}
