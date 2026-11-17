#include "redstone/engine.hpp"
#include "redstone/light/service.hpp"
#include "redstone/worldgen.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace redstone;
using namespace redstone::worldgen;

std::map<std::string, int> countBlocks(const ChunkAccess &chunk) {
  std::map<std::string, int> counts;
  const int x0 = chunk.pos().x << 4;
  const int z0 = chunk.pos().z << 4;
  for (int y = chunk.minY(); y < chunk.maxY(); ++y) {
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz) {
        ++counts[chunk.getBlockState(x0 + lx, y, z0 + lz).name()];
      }
    }
  }
  return counts;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: worldgen_g3_features_tests <assets/worldgen26_1>\n";
    return 2;
  }
  const std::filesystem::path worldgenDir(argv[1]);
  const WorldgenRegistries reg = WorldgenRegistries::loadFromDirectory(worldgenDir);
  assert(reg.placedFeatures().size() == 258);
  assert(reg.configuredFeatures().size() == 221);

  constexpr std::int64_t kSeed = 123456789LL;

  // 1. Overworld FeatureSorter step counts parity with server-26.1.jar
  const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
  assert(owSettings != nullptr);
  auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
      MultiNoiseBiomeSource::createOverworld());
  const auto owSteps =
      buildFeaturesPerStep(reg, owBiomes->possibleBiomes());
  assert(owSteps.size() == 11);
  const std::vector<std::size_t> expectedOwCounts = {
      0, 2, 5, 4, 4, 0, 34, 5, 3, 106, 1};
  std::cerr << "C++ OW_STEP_COUNTS=";
  for (std::size_t i = 0; i < owSteps.size(); ++i) {
    std::cerr << owSteps[i].featureIds.size() << (i + 1 < owSteps.size() ? "," : "\n");
  }
  for (std::size_t i = 0; i < owSteps.size(); ++i) {
    assert(owSteps[i].featureIds.size() == expectedOwCounts[i]);
  }

  // 2. End FeatureSorter step counts + EndSpike chunk (2, 0)
  const auto *endSettings = reg.findNoiseSettings("minecraft:end");
  assert(endSettings != nullptr);
  auto endBiomes = std::make_shared<TheEndBiomeSource>();
  const auto endSteps =
      buildFeaturesPerStep(reg, endBiomes->possibleBiomes());
  assert(endSteps.size() == 11);
  const std::vector<std::size_t> expectedEndCounts = {
      1, 0, 0, 0, 2, 0, 0, 0, 0, 1, 1};
  for (std::size_t i = 0; i < endSteps.size(); ++i) {
    assert(endSteps[i].featureIds.size() == expectedEndCounts[i]);
  }

  NoiseBasedChunkGenerator endGen(reg, *endSettings, endBiomes);
  ChunkGenerationPipeline endPipe(endGen, kSeed);
  ChunkAccess &endChunk =
      endPipe.ensureStatus(ChunkTicketPos{2, 0}, ChunkStatusId::Features);
  const auto endCounts = countBlocks(endChunk);
  std::cerr << "C++ END(2,0) obsidian=" << endCounts.at("minecraft:obsidian")
            << " bedrock="
            << (endCounts.count("minecraft:bedrock")
                    ? endCounts.at("minecraft:bedrock")
                    : 0)
            << "\n";
  assert(endCounts.count("minecraft:obsidian") &&
         endCounts.at("minecraft:obsidian") > 200);
  assert(endCounts.count("minecraft:bedrock") &&
         endCounts.at("minecraft:bedrock") == 1);

  // 3. Overworld Forest (3, -2) Features (trees, vegetation, ores)
  NoiseBasedChunkGenerator owGen(reg, *owSettings, owBiomes);
  ChunkGenerationPipeline owPipe(owGen, kSeed);
  ChunkAccess &owForest =
      owPipe.ensureStatus(ChunkTicketPos{3, -2}, ChunkStatusId::Features);
  const auto owForestCounts = countBlocks(owForest);
  auto cnt = [](const std::map<std::string, int> &m, const char *k) {
    auto it = m.find(k);
    return it == m.end() ? 0 : it->second;
  };
  std::cerr << "C++ OW(3,-2) oak_log=" << cnt(owForestCounts, "minecraft:oak_log")
            << " oak_leaves=" << cnt(owForestCounts, "minecraft:oak_leaves")
            << " birch_log=" << cnt(owForestCounts, "minecraft:birch_log")
            << " birch_leaves=" << cnt(owForestCounts, "minecraft:birch_leaves")
            << " leaf_litter=" << cnt(owForestCounts, "minecraft:leaf_litter")
            << "\n";
  assert(cnt(owForestCounts, "minecraft:oak_log") > 0);
  assert(cnt(owForestCounts, "minecraft:oak_leaves") > 0);
  assert(cnt(owForestCounts, "minecraft:leaf_litter") > 0);
  assert(owForestCounts.count("minecraft:coal_ore") &&
         owForestCounts.count("minecraft:iron_ore") &&
         owForestCounts.count("minecraft:copper_ore"));

  // 4. Overworld Village (-51, -62) full Features (Jigsaw structure blocks placed!)
  ChunkAccess &owVillage =
      owPipe.ensureStatus(ChunkTicketPos{-51, -62}, ChunkStatusId::Features);
  const auto owVillageCounts = countBlocks(owVillage);
  std::cerr << "C++ VILLAGE(-51,-62) cobblestone="
            << (owVillageCounts.count("minecraft:cobblestone")
                    ? owVillageCounts.at("minecraft:cobblestone")
                    : 0)
            << " dirt_path="
            << (owVillageCounts.count("minecraft:dirt_path")
                    ? owVillageCounts.at("minecraft:dirt_path")
                    : 0)
            << " oak_planks="
            << (owVillageCounts.count("minecraft:oak_planks")
                    ? owVillageCounts.at("minecraft:oak_planks")
                    : 0)
            << "\n";
  assert(owVillageCounts.count("minecraft:cobblestone") &&
         owVillageCounts.at("minecraft:cobblestone") > 0);
  assert(owVillageCounts.count("minecraft:dirt_path") &&
         owVillageCounts.at("minecraft:dirt_path") > 0);

  // 5. Nether (-1, 0) Features (ancient_debris / quartz / gold / gravel)
  const auto *neSettings = reg.findNoiseSettings("minecraft:nether");
  assert(neSettings != nullptr);
  auto neBiomes = std::make_shared<MultiNoiseBiomeSource>(
      MultiNoiseBiomeSource::createNether());
  NoiseBasedChunkGenerator neGen(reg, *neSettings, neBiomes);
  ChunkGenerationPipeline nePipe(neGen, kSeed);
  ChunkAccess &neChunk =
      nePipe.ensureStatus(ChunkTicketPos{-1, 0}, ChunkStatusId::Features);
  const auto neCounts = countBlocks(neChunk);
  std::cerr << "C++ NETHER(-1,0) ancient_debris="
            << (neCounts.count("minecraft:ancient_debris")
                    ? neCounts.at("minecraft:ancient_debris")
                    : 0)
            << " quartz_ore="
            << (neCounts.count("minecraft:nether_quartz_ore")
                    ? neCounts.at("minecraft:nether_quartz_ore")
                    : 0)
            << " gold_ore="
            << (neCounts.count("minecraft:nether_gold_ore")
                    ? neCounts.at("minecraft:nether_gold_ore")
                    : 0)
            << " blackstone="
            << (neCounts.count("minecraft:blackstone")
                    ? neCounts.at("minecraft:blackstone")
                    : 0)
            << " magma="
            << (neCounts.count("minecraft:magma_block")
                    ? neCounts.at("minecraft:magma_block")
                    : 0)
            << " gravel="
            << (neCounts.count("minecraft:gravel")
                    ? neCounts.at("minecraft:gravel")
                    : 0)
            << "\n";
  assert(neCounts.count("minecraft:nether_quartz_ore") &&
         neCounts.at("minecraft:nether_quartz_ore") > 0);
  assert(neCounts.count("minecraft:nether_gold_ore") &&
         neCounts.at("minecraft:nether_gold_ore") > 0);
  assert(neCounts.count("minecraft:gravel") &&
         neCounts.at("minecraft:gravel") > 0);

  // 6. All 26.1 Tree placers / decorators / root placer / fallen tree / fungus / mushrooms
  {
    auto runFeatureOnce = [&](const char *cfId, const BlockState &floorBlock,
                              Pos origin, std::int64_t rngSeed,
                              auto &&verifyFn) {
      std::vector<std::unique_ptr<ProtoChunk>> chunks;
      for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
          auto pc = std::make_unique<ProtoChunk>(ChunkTicketPos{dx, dz}, -64, 384);
          pc->primeHeightmaps(ChunkStatus::features().heightmapsAfter());
          for (int lx = 0; lx < 16; ++lx) {
            for (int lz = 0; lz < 16; ++lz) {
              for (int y = 48; y < 63; ++y)
                pc->setBlockState(lx, y, lz, Blocks::STONE.defaultState());
              pc->setBlockState(lx, 63, lz, floorBlock);
            }
          }
          pc->setPersistedStatus(ChunkStatusId::Carvers);
          chunks.push_back(std::move(pc));
        }
      }
      std::vector<ChunkAccess *> grid(9, nullptr);
      for (std::size_t i = 0; i < 9; ++i)
        grid[i] = chunks[i].get();
      const auto &step =
          ChunkPyramid::generationPyramid().getStepTo(ChunkStatusId::Features);
      WorldGenRegion region(step, *chunks[4], grid, 1, kSeed);
      const ConfiguredFeature *cf = reg.findConfiguredFeature(cfId);
      assert(cf != nullptr);
      XoroshiroRandomSource rng(rngSeed);
      const bool ok = cf->place(region, owGen, kSeed, rng, origin);
      assert(ok);
      verifyFn(countBlocks(*chunks[4]), *chunks[4]);
    };
    auto testFeature = [&](const char *cfId, const BlockState &floorBlock,
                           Pos origin, auto &&verifyFn) {
      runFeatureOnce(cfId, floorBlock, origin, kSeed + 42, verifyFn);
    };

    testFeature("minecraft:cherry_bees_005", Blocks::GRASS_BLOCK.defaultState(),
                Pos{8, 64, 8}, [](const auto &m, const ChunkAccess &) {
                  assert(m.count("minecraft:cherry_log") && m.at("minecraft:cherry_log") > 0);
                  assert(m.count("minecraft:cherry_leaves") && m.at("minecraft:cherry_leaves") > 0);
                });
    testFeature("minecraft:mangrove", Blocks::MUD.defaultState(),
                Pos{8, 64, 8}, [](const auto &m, const ChunkAccess &) {
                  assert(m.count("minecraft:mangrove_log") && m.at("minecraft:mangrove_log") > 0);
                  assert(m.count("minecraft:mangrove_leaves") && m.at("minecraft:mangrove_leaves") > 0);
                  assert(m.count("minecraft:mangrove_roots") +
                             (m.count("minecraft:muddy_mangrove_roots") ? m.at("minecraft:muddy_mangrove_roots") : 0) > 0);
                });
    testFeature("minecraft:dark_oak", Blocks::GRASS_BLOCK.defaultState(),
                Pos{8, 64, 8}, [](const auto &m, const ChunkAccess &) {
                  assert(m.count("minecraft:dark_oak_log") && m.at("minecraft:dark_oak_log") >= 4);
                  assert(m.count("minecraft:dark_oak_leaves") && m.at("minecraft:dark_oak_leaves") > 10);
                });
    testFeature("minecraft:mega_pine", Blocks::GRASS_BLOCK.defaultState(),
                Pos{8, 64, 8}, [](const auto &m, const ChunkAccess &) {
                  assert(m.count("minecraft:spruce_log") && m.at("minecraft:spruce_log") >= 10);
                  assert(m.count("minecraft:spruce_leaves") && m.at("minecraft:spruce_leaves") > 10);
                  assert(m.count("minecraft:podzol") && m.at("minecraft:podzol") > 0);
                });
    {
      bool sawCreakingHeart = false;
      bool sawHangingMoss = false;
      for (int attempt = 0; attempt < 16 && (!sawCreakingHeart || !sawHangingMoss);
           ++attempt) {
        runFeatureOnce(
            "minecraft:pale_oak_creaking", Blocks::GRASS_BLOCK.defaultState(),
            Pos{8, 64, 8}, kSeed + 100 + attempt,
            [&](const auto &m, const ChunkAccess &) {
              assert(m.count("minecraft:pale_oak_log") &&
                     m.at("minecraft:pale_oak_log") >= 4);
              assert(m.count("minecraft:pale_oak_leaves") &&
                     m.at("minecraft:pale_oak_leaves") > 10);
              if (m.count("minecraft:creaking_heart") &&
                  m.at("minecraft:creaking_heart") >= 1)
                sawCreakingHeart = true;
              if (m.count("minecraft:pale_hanging_moss") &&
                  m.at("minecraft:pale_hanging_moss") >= 1)
                sawHangingMoss = true;
            });
      }
      assert(sawCreakingHeart);
      assert(sawHangingMoss);
    }
    testFeature("minecraft:fallen_oak_tree", Blocks::GRASS_BLOCK.defaultState(),
                Pos{8, 64, 8}, [](const auto &m, const ChunkAccess &) {
                  assert(m.count("minecraft:oak_log") && m.at("minecraft:oak_log") >= 2);
                });
    testFeature("minecraft:huge_red_mushroom", Blocks::MYCELIUM.defaultState(),
                Pos{8, 64, 8}, [](const auto &m, const ChunkAccess &) {
                  assert(m.count("minecraft:red_mushroom_block") && m.at("minecraft:red_mushroom_block") > 5);
                  assert(m.count("minecraft:mushroom_stem") && m.at("minecraft:mushroom_stem") >= 3);
                });
    testFeature("minecraft:crimson_fungus", Blocks::CRIMSON_NYLIUM.defaultState(),
                Pos{8, 64, 8}, [](const auto &m, const ChunkAccess &) {
                  assert(m.count("minecraft:crimson_stem") && m.at("minecraft:crimson_stem") >= 4);
                  assert(m.count("minecraft:nether_wart_block") && m.at("minecraft:nether_wart_block") > 5);
                });
  }

  // 7. End-to-end 26.1 Overworld Full chunk + LightService installation into World
  {
    World litWorld(-64, 320);
    LightWorkerOptions lightOpts;
    lightOpts.batchWindowMs = 0;
    litWorld.enableLighting(lightOpts);

    ChunkGenerationPipeline litPipe(owGen, kSeed, nullptr, &litWorld);
    assert(litPipe.generateAndInstallIntoWorld(ChunkTicketPos{3, -2}, litWorld));
    litWorld.lightService()->flushAndWait();
    litWorld.drainLightPublications();

    assert(litWorld.isChunkLoaded(3, -2));
    // Verify surface has daylight (15) above top surface and deep underground cave/bedrock is dark (0).
    const int surfY = litWorld.height(HeightmapType::WorldSurface, 56, -24);
    assert(surfY > 50);
    assert(litWorld.getBrightness({56, surfY + 1, -24}, 0) == 15);
    assert(litWorld.getBrightness({56, -60, -24}, 0) == 0);
  }

  // 8. Verify all 221 configured_features parse cleanly (zero Kind::Unsupported)
  //    and test deterministic block placement + properties for all 29 new feature types.
  {
    int parsedCount = 0;
    for (const auto &[cfId, cfPtr] : reg.configuredFeatures()) {
      assert(cfPtr != nullptr);
      if (cfPtr->kind == ConfiguredFeature::Kind::Unsupported) {
        std::cerr << "Unsupported configured_feature: " << cfId
                  << " type=" << cfPtr->rawType << "\n";
      }
      assert(cfPtr->kind != ConfiguredFeature::Kind::Unsupported);
      ++parsedCount;
    }
    assert(parsedCount == 221);

    auto runExtraOnce = [&](const char *cfId, const NoiseBasedChunkGenerator &gen,
                            Pos origin, std::int64_t rngSeed, auto &&setupFn,
                            auto &&verifyFn) {
      auto runPass = [&]() {
        std::vector<std::unique_ptr<ProtoChunk>> chunks;
        for (int dz = -1; dz <= 1; ++dz) {
          for (int dx = -1; dx <= 1; ++dx) {
            auto pc = std::make_unique<ProtoChunk>(
                ChunkTicketPos{dx, dz}, gen.minY(), gen.height());
            pc->primeHeightmaps(ChunkStatus::features().heightmapsAfter());
            pc->setPersistedStatus(ChunkStatusId::Carvers);
            chunks.push_back(std::move(pc));
          }
        }
        setupFn(chunks);
        std::vector<ChunkAccess *> grid(9, nullptr);
        for (std::size_t i = 0; i < 9; ++i)
          grid[i] = chunks[i].get();
        const auto &step =
            ChunkPyramid::generationPyramid().getStepTo(ChunkStatusId::Features);
        WorldGenRegion region(step, *chunks[4], grid, 1, kSeed);
        const ConfiguredFeature *cf = reg.findConfiguredFeature(cfId);
        assert(cf != nullptr);
        XoroshiroRandomSource rng(rngSeed);
        const bool ok = cf->place(region, gen, kSeed, rng, origin);
        if (!ok) {
          std::cerr << "Feature failed: " << cfId << " at (" << origin.x
                    << "," << origin.y << "," << origin.z << ")\n";
        }
        assert(ok);
        return chunks;
      };
      auto chunks1 = runPass();
      auto chunks2 = runPass();
      const auto counts1 = countBlocks(*chunks1[4]);
      const auto counts2 = countBlocks(*chunks2[4]);
      assert(counts1 == counts2);
      // Also verify every block in center chunk matches deterministically
      for (int y = chunks1[4]->minY(); y < chunks1[4]->maxY(); ++y) {
        for (int lx = 0; lx < 16; ++lx) {
          for (int lz = 0; lz < 16; ++lz) {
            assert(chunks1[4]->getBlockState(lx, y, lz) ==
                   chunks2[4]->getBlockState(lx, y, lz));
          }
        }
      }
      verifyFn(counts1, *chunks1[4]);
    };

    auto fillGridFloor = [](auto &chunks, int minY, int maxY,
                            const BlockState &st) {
      for (auto &pc : chunks) {
        for (int lx = 0; lx < 16; ++lx) {
          for (int lz = 0; lz < 16; ++lz) {
            for (int y = minY; y <= maxY; ++y)
              pc->setBlockState(lx, y, lz, st);
          }
        }
      }
    };
    auto setWorld = [](auto &chunks, int x, int y, int z, const BlockState &st) {
      const int cx = (x >> 4) + 1;
      const int cz = (z >> 4) + 1;
      if (cx >= 0 && cx < 3 && cz >= 0 && cz < 3) {
        chunks[static_cast<std::size_t>(cz * 3 + cx)]->setBlockState(
            x & 15, y, z & 15, st);
      }
    };

    // 1. minecraft:seagrass (all 4 variants)
    runExtraOnce(
        "minecraft:seagrass_short", owGen, Pos{8, 50, 8}, kSeed + 1,
        [&](auto &chunks) {
          fillGridFloor(chunks, 45, 50, Blocks::SAND.defaultState());
          fillGridFloor(chunks, 51, 62, Blocks::WATER.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:seagrass") || m.count("minecraft:tall_seagrass"));
        });
    runExtraOnce(
        "minecraft:seagrass_mid", owGen, Pos{8, 50, 8}, kSeed + 41,
        [&](auto &chunks) {
          fillGridFloor(chunks, 45, 50, Blocks::SAND.defaultState());
          fillGridFloor(chunks, 51, 62, Blocks::WATER.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:seagrass") || m.count("minecraft:tall_seagrass"));
        });
    runExtraOnce(
        "minecraft:seagrass_slightly_less_short", owGen, Pos{8, 50, 8}, kSeed + 42,
        [&](auto &chunks) {
          fillGridFloor(chunks, 45, 50, Blocks::SAND.defaultState());
          fillGridFloor(chunks, 51, 62, Blocks::WATER.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:seagrass") || m.count("minecraft:tall_seagrass"));
        });
    runExtraOnce(
        "minecraft:seagrass_tall", owGen, Pos{8, 50, 8}, kSeed + 2,
        [&](auto &chunks) {
          fillGridFloor(chunks, 45, 50, Blocks::SAND.defaultState());
          fillGridFloor(chunks, 51, 62, Blocks::WATER.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:tall_seagrass") &&
                 m.at("minecraft:tall_seagrass") >= 2);
          bool sawLower = false, sawUpper = false;
          for (int y = 50; y <= 62; ++y) {
            for (int x = 0; x < 16; ++x) {
              for (int z = 0; z < 16; ++z) {
                const BlockState s = c.getBlockState(x, y, z);
                if (s.is(Blocks::TALL_SEAGRASS)) {
                  if (s.getView("half") == "lower") {
                    sawLower = true;
                    assert(c.getBlockState(x, y + 1, z).is(Blocks::TALL_SEAGRASS) &&
                           c.getBlockState(x, y + 1, z).getView("half") == "upper");
                  } else if (s.getView("half") == "upper") {
                    sawUpper = true;
                  }
                }
              }
            }
          }
          assert(sawLower && sawUpper);
        });

    // 2. minecraft:bamboo (no_podzol + some_podzol)
    runExtraOnce(
        "minecraft:bamboo_no_podzol", owGen, Pos{8, 64, 8}, kSeed + 3,
        [&](auto &chunks) {
          fillGridFloor(chunks, 60, 63, Blocks::GRASS_BLOCK.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:bamboo") && m.at("minecraft:bamboo") >= 5);
          assert(c.getBlockState(8, 64, 8).is(Blocks::BAMBOO));
          assert(!m.count("minecraft:podzol"));
        });
    {
      bool sawPodzol = false;
      for (int attempt = 0; attempt < 32 && !sawPodzol; ++attempt) {
        runExtraOnce(
            "minecraft:bamboo_some_podzol", owGen, Pos{8, 64, 8},
            kSeed + 100 + attempt,
            [&](auto &chunks) {
              fillGridFloor(chunks, 60, 63, Blocks::GRASS_BLOCK.defaultState());
            },
            [&](const auto &m, const ChunkAccess &c) {
              assert(m.count("minecraft:bamboo") && m.at("minecraft:bamboo") >= 5);
              assert(c.getBlockState(8, 64, 8).is(Blocks::BAMBOO));
              if (m.count("minecraft:podzol") && m.at("minecraft:podzol") > 0)
                sawPodzol = true;
            });
      }
      assert(sawPodzol);
    }

    // 3. minecraft:basalt_columns (large + small)
    runExtraOnce(
        "minecraft:large_basalt_columns", neGen, Pos{8, 40, 8}, kSeed + 4,
        [&](auto &chunks) {
          fillGridFloor(chunks, 35, 39, Blocks::BASALT.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:basalt") &&
                 m.at("minecraft:basalt") > 5 * 256);
          assert(c.getBlockState(8, 40, 8).is(Blocks::BASALT));
        });
    runExtraOnce(
        "minecraft:small_basalt_columns", neGen, Pos{8, 40, 8}, kSeed + 43,
        [&](auto &chunks) {
          fillGridFloor(chunks, 35, 39, Blocks::BASALT.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:basalt") &&
                 m.at("minecraft:basalt") > 5 * 256);
          assert(c.getBlockState(8, 40, 8).is(Blocks::BASALT));
        });

    // 4. minecraft:basalt_pillar
    runExtraOnce(
        "minecraft:basalt_pillar", neGen, Pos{8, 80, 8}, kSeed + 5,
        [&](auto &chunks) {
          fillGridFloor(chunks, 60, 65, Blocks::NETHERRACK.defaultState());
          fillGridFloor(chunks, 81, 85, Blocks::NETHERRACK.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:basalt") && m.at("minecraft:basalt") >= 15);
          assert(c.getBlockState(8, 80, 8).is(Blocks::BASALT));
          assert(c.getBlockState(8, 66, 8).is(Blocks::BASALT));
        });

    // 5. minecraft:block_blob (forest_rock)
    runExtraOnce(
        "minecraft:forest_rock", owGen, Pos{8, 68, 8}, kSeed + 6,
        [&](auto &chunks) {
          fillGridFloor(chunks, 60, 63, Blocks::DIRT.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:mossy_cobblestone") &&
                 m.at("minecraft:mossy_cobblestone") >= 1);
          assert(c.getBlockState(8, 64, 8).is(Blocks::MOSSY_COBBLESTONE));
        });

    // 6. minecraft:blue_ice
    runExtraOnce(
        "minecraft:blue_ice", owGen, Pos{8, 55, 8}, kSeed + 7,
        [&](auto &chunks) {
          fillGridFloor(chunks, 50, 62, Blocks::WATER.defaultState());
          setWorld(chunks, 8, 56, 8, Blocks::PACKED_ICE.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(c.getBlockState(8, 55, 8).is(Blocks::BLUE_ICE));
          assert(m.count("minecraft:blue_ice") && m.at("minecraft:blue_ice") >= 1);
        });

    // 7. minecraft:bonus_chest
    runExtraOnce(
        "minecraft:bonus_chest", owGen, Pos{8, 64, 8}, kSeed + 8,
        [&](auto &chunks) {
          fillGridFloor(chunks, 60, 63, Blocks::GRASS_BLOCK.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:chest") && m.at("minecraft:chest") == 1);
          assert(m.count("minecraft:torch") && m.at("minecraft:torch") == 4);
        });

    // 8. minecraft:chorus_plant
    runExtraOnce(
        "minecraft:chorus_plant", endGen, Pos{8, 64, 8}, kSeed + 9,
        [&](auto &chunks) {
          fillGridFloor(chunks, 55, 63, Blocks::END_STONE.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:chorus_plant") &&
                 m.at("minecraft:chorus_plant") >= 2);
          assert(m.count("minecraft:chorus_flower") &&
                 m.at("minecraft:chorus_flower") >= 1);
          assert(c.getBlockState(8, 64, 8).is(Blocks::CHORUS_PLANT));
          assert(c.getBlockState(8, 64, 8).getView("down") == "true");
        });

    // 9. minecraft:delta_feature
    runExtraOnce(
        "minecraft:delta", neGen, Pos{8, 40, 8}, kSeed + 10,
        [&](auto &chunks) {
          fillGridFloor(chunks, 35, 40, Blocks::BASALT.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:lava") && m.at("minecraft:lava") > 0);
        });

    // 10. minecraft:desert_well
    runExtraOnce(
        "minecraft:desert_well", owGen, Pos{8, 68, 8}, kSeed + 11,
        [&](auto &chunks) {
          fillGridFloor(chunks, 60, 63, Blocks::SAND.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:sandstone") && m.at("minecraft:sandstone") > 20);
          assert(m.count("minecraft:sandstone_slab") &&
                 m.at("minecraft:sandstone_slab") == 12);
          assert(m.count("minecraft:suspicious_sand") &&
                 m.at("minecraft:suspicious_sand") >= 1);
          assert(c.getBlockState(8, 63, 8).is(Blocks::WATER));
        });

    // 11. minecraft:dripstone_cluster
    runExtraOnce(
        "minecraft:dripstone_cluster", owGen, Pos{8, 50, 8}, kSeed + 12,
        [&](auto &chunks) {
          fillGridFloor(chunks, 40, 44, Blocks::STONE.defaultState());
          fillGridFloor(chunks, 55, 60, Blocks::STONE.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:dripstone_block") &&
                 m.at("minecraft:dripstone_block") > 0);
          assert(m.count("minecraft:pointed_dripstone") &&
                 m.at("minecraft:pointed_dripstone") > 0);
        });

    // 12. minecraft:end_gateway (delayed + return)
    runExtraOnce(
        "minecraft:end_gateway_delayed", endGen, Pos{8, 70, 8}, kSeed + 44,
        [&](auto &) {},
        [](const auto &m, const ChunkAccess &c) {
          assert(c.getBlockState(8, 70, 8).is(Blocks::END_GATEWAY));
          assert(m.count("minecraft:bedrock") && m.at("minecraft:bedrock") == 12);
        });
    runExtraOnce(
        "minecraft:end_gateway_return", endGen, Pos{8, 70, 8}, kSeed + 13,
        [&](auto &) {},
        [](const auto &m, const ChunkAccess &c) {
          assert(c.getBlockState(8, 70, 8).is(Blocks::END_GATEWAY));
          assert(m.count("minecraft:bedrock") && m.at("minecraft:bedrock") == 12);
        });

    // 13. minecraft:end_platform
    runExtraOnce(
        "minecraft:end_platform", endGen, Pos{8, 50, 8}, kSeed + 14,
        [&](auto &) {},
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:obsidian") && m.at("minecraft:obsidian") == 25);
          assert(c.getBlockState(8, 49, 8).is(Blocks::OBSIDIAN));
          assert(c.getBlockState(8, 50, 8).isAir());
        });

    // 14. minecraft:fossil (coal + diamonds)
    runExtraOnce(
        "minecraft:fossil_coal", owGen, Pos{8, 40, 8}, kSeed + 15,
        [&](auto &chunks) {
          fillGridFloor(chunks, 0, 45, Blocks::STONE.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert((m.count("minecraft:bone_block") ? m.at("minecraft:bone_block") : 0) +
                     (m.count("minecraft:coal_ore") ? m.at("minecraft:coal_ore") : 0) >
                 0);
        });
    runExtraOnce(
        "minecraft:fossil_diamonds", owGen, Pos{8, 40, 8}, kSeed + 16,
        [&](auto &chunks) {
          fillGridFloor(chunks, -20, 45, Blocks::DEEPSLATE.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert((m.count("minecraft:bone_block") ? m.at("minecraft:bone_block") : 0) +
                     (m.count("minecraft:deepslate_diamond_ore")
                          ? m.at("minecraft:deepslate_diamond_ore")
                          : 0) >
                 0);
        });

    // 15. minecraft:geode
    runExtraOnce(
        "minecraft:amethyst_geode", owGen, Pos{8, 20, 8}, kSeed + 17,
        [&](auto &chunks) {
          fillGridFloor(chunks, 0, 50, Blocks::STONE.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:smooth_basalt") &&
                 m.at("minecraft:smooth_basalt") > 10);
          assert(m.count("minecraft:calcite") && m.at("minecraft:calcite") > 10);
          assert(m.count("minecraft:amethyst_block") &&
                 m.at("minecraft:amethyst_block") > 5);
        });

    // 16. minecraft:iceberg (blue + packed)
    runExtraOnce(
        "minecraft:iceberg_blue", owGen, Pos{8, 63, 8}, kSeed + 45,
        [&](auto &chunks) {
          fillGridFloor(chunks, 40, 62, Blocks::WATER.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:blue_ice") &&
                 m.at("minecraft:blue_ice") > 20);
        });
    runExtraOnce(
        "minecraft:iceberg_packed", owGen, Pos{8, 63, 8}, kSeed + 18,
        [&](auto &chunks) {
          fillGridFloor(chunks, 40, 62, Blocks::WATER.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:packed_ice") &&
                 m.at("minecraft:packed_ice") > 20);
        });

    // 17. minecraft:kelp
    runExtraOnce(
        "minecraft:kelp", owGen, Pos{8, 45, 8}, kSeed + 19,
        [&](auto &chunks) {
          fillGridFloor(chunks, 40, 44, Blocks::SAND.defaultState());
          fillGridFloor(chunks, 45, 62, Blocks::WATER.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:kelp") && m.at("minecraft:kelp") >= 1);
        });

    // 18. minecraft:large_dripstone
    runExtraOnce(
        "minecraft:large_dripstone", owGen, Pos{8, 30, 8}, kSeed + 20,
        [&](auto &chunks) {
          fillGridFloor(chunks, -10, 15, Blocks::STONE.defaultState());
          fillGridFloor(chunks, 45, 70, Blocks::STONE.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:dripstone_block") &&
                 m.at("minecraft:dripstone_block") > 5);
        });

    // 19. minecraft:monster_room
    runExtraOnce(
        "minecraft:monster_room", owGen, Pos{8, 30, 8}, kSeed + 21,
        [&](auto &chunks) {
          fillGridFloor(chunks, 25, 36, Blocks::STONE.defaultState());
          // Carve one valid doorway on the west wall (y=30 and y=31)
          for (int dx = -4; dx <= -2; ++dx) {
            setWorld(chunks, 8 + dx, 30, 8, Blocks::AIR.defaultState());
            setWorld(chunks, 8 + dx, 31, 8, Blocks::AIR.defaultState());
          }
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(c.getBlockState(8, 30, 8).is(Blocks::SPAWNER));
          assert(m.count("minecraft:cobblestone") &&
                 m.at("minecraft:cobblestone") > 20);
          assert(m.count("minecraft:mossy_cobblestone") &&
                 m.at("minecraft:mossy_cobblestone") > 0);
        });

    // 20. minecraft:multiface_growth (glow_lichen + sculk_vein)
    runExtraOnce(
        "minecraft:glow_lichen", owGen, Pos{8, 30, 8}, kSeed + 22,
        [&](auto &chunks) {
          fillGridFloor(chunks, 31, 35, Blocks::STONE.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:glow_lichen") &&
                 m.at("minecraft:glow_lichen") >= 1);
          assert(c.getBlockState(8, 30, 8).is(Blocks::GLOW_LICHEN));
          assert(c.getBlockState(8, 30, 8).getView("up") == "true");
        });
    runExtraOnce(
        "minecraft:sculk_vein", owGen, Pos{8, 30, 8}, kSeed + 23,
        [&](auto &chunks) {
          fillGridFloor(chunks, 25, 29, Blocks::DEEPSLATE.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:sculk_vein") &&
                 m.at("minecraft:sculk_vein") >= 1);
          assert(c.getBlockState(8, 30, 8).is(Blocks::SCULK_VEIN));
          assert(c.getBlockState(8, 30, 8).getView("down") == "true");
        });

    // 21. minecraft:root_system
    runExtraOnce(
        "minecraft:rooted_azalea_tree", owGen, Pos{8, 50, 8}, kSeed + 24,
        [&](auto &chunks) {
          fillGridFloor(chunks, 45, 49, Blocks::STONE.defaultState());
          fillGridFloor(chunks, 51, 59, Blocks::STONE.defaultState());
          fillGridFloor(chunks, 60, 62, Blocks::DIRT.defaultState());
          fillGridFloor(chunks, 63, 63, Blocks::GRASS_BLOCK.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:oak_log") && m.at("minecraft:oak_log") > 0);
          assert(m.count("minecraft:rooted_dirt") &&
                 m.at("minecraft:rooted_dirt") > 0);
          assert(m.count("minecraft:azalea_leaves") ||
                 m.count("minecraft:flowering_azalea_leaves"));
        });

    // 22. minecraft:sculk_patch (ancient_city + deep_dark)
    runExtraOnce(
        "minecraft:sculk_patch_ancient_city", owGen, Pos{8, 20, 8}, kSeed + 25,
        [&](auto &chunks) {
          fillGridFloor(chunks, 15, 19, Blocks::DEEPSLATE.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:sculk") && m.at("minecraft:sculk") > 0);
        });
    runExtraOnce(
        "minecraft:sculk_patch_deep_dark", owGen, Pos{8, 20, 8}, kSeed + 46,
        [&](auto &chunks) {
          fillGridFloor(chunks, 15, 19, Blocks::DEEPSLATE.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:sculk") && m.at("minecraft:sculk") > 0);
        });

    // 23. minecraft:sea_pickle
    runExtraOnce(
        "minecraft:sea_pickle", owGen, Pos{8, 45, 8}, kSeed + 26,
        [&](auto &chunks) {
          fillGridFloor(chunks, 40, 44, Blocks::SAND.defaultState());
          fillGridFloor(chunks, 45, 62, Blocks::WATER.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:sea_pickle") &&
                 m.at("minecraft:sea_pickle") >= 1);
        });

    // 24. minecraft:spike (ice_spike)
    runExtraOnce(
        "minecraft:ice_spike", owGen, Pos{8, 66, 8}, kSeed + 27,
        [&](auto &chunks) {
          fillGridFloor(chunks, 60, 63, Blocks::SNOW_BLOCK.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:packed_ice") &&
                 m.at("minecraft:packed_ice") >= 7);
        });

    // 25. minecraft:twisting_vines (normal + bonemeal)
    runExtraOnce(
        "minecraft:twisting_vines", neGen, Pos{8, 40, 8}, kSeed + 28,
        [&](auto &chunks) {
          fillGridFloor(chunks, 35, 39, Blocks::WARPED_NYLIUM.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert((m.count("minecraft:twisting_vines")
                      ? m.at("minecraft:twisting_vines")
                      : 0) > 0);
        });
    runExtraOnce(
        "minecraft:twisting_vines_bonemeal", neGen, Pos{8, 40, 8}, kSeed + 47,
        [&](auto &chunks) {
          fillGridFloor(chunks, 35, 39, Blocks::WARPED_NYLIUM.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert((m.count("minecraft:twisting_vines")
                      ? m.at("minecraft:twisting_vines")
                      : 0) > 0);
        });

    // 26. minecraft:underwater_magma
    runExtraOnce(
        "minecraft:underwater_magma", owGen, Pos{8, 48, 8}, kSeed + 29,
        [&](auto &chunks) {
          fillGridFloor(chunks, 40, 45, Blocks::STONE.defaultState());
          fillGridFloor(chunks, 46, 62, Blocks::WATER.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:magma_block") &&
                 m.at("minecraft:magma_block") >= 1);
        });

    // 27. minecraft:vines
    runExtraOnce(
        "minecraft:vines", owGen, Pos{8, 70, 8}, kSeed + 30,
        [&](auto &chunks) {
          setWorld(chunks, 8, 70, 7, Blocks::OAK_LOG.defaultState());
        },
        [](const auto &m, const ChunkAccess &c) {
          assert(m.count("minecraft:vine") && m.at("minecraft:vine") == 1);
          assert(c.getBlockState(8, 70, 8).getView("north") == "true");
        });

    // 28. minecraft:void_start_platform
    runExtraOnce(
        "minecraft:void_start_platform", owGen, Pos{8, 60, 8}, kSeed + 31,
        [&](auto &) {},
        [](const auto &m, const ChunkAccess &c) {
          assert(c.getBlockState(8, 63, 8).is(Blocks::COBBLESTONE));
          assert(m.count("minecraft:stone") && m.at("minecraft:stone") == 255);
          assert(m.count("minecraft:cobblestone") &&
                 m.at("minecraft:cobblestone") == 1);
        });

    // 29. minecraft:weeping_vines
    runExtraOnce(
        "minecraft:weeping_vines", neGen, Pos{8, 80, 8}, kSeed + 32,
        [&](auto &chunks) {
          fillGridFloor(chunks, 81, 85, Blocks::NETHERRACK.defaultState());
        },
        [](const auto &m, const ChunkAccess &) {
          assert(m.count("minecraft:nether_wart_block") &&
                 m.at("minecraft:nether_wart_block") >= 1);
          assert(m.count("minecraft:weeping_vines") &&
                 m.at("minecraft:weeping_vines") >= 1);
        });
  }

  std::cout << "worldgen_g3_features_tests passed!\n";
  return 0;
}
