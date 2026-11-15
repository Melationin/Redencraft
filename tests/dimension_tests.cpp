#include "redstone/dimension.hpp"
#include "redstone/entity.hpp"
#include "redstone/light/service.hpp"
#include "redstone/nbt.hpp"
#include "redstone/server_worlds.hpp"
#include "redstone/storage.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <zlib.h>

using namespace redstone;

namespace {

using Bytes = std::vector<std::uint8_t>;

void writeGzipNbt(const std::filesystem::path &path, const nbt::RootTag &root) {
  std::filesystem::create_directories(path.parent_path());
  const Bytes raw = nbt::writeRoot(root);
  const Bytes gz = nbt::compress(raw, 31);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  assert(out.good());
  out.write(reinterpret_cast<const char *>(gz.data()),
            static_cast<std::streamsize>(gz.size()));
  assert(out.good());
}

nbt::Tag byteTag(std::int8_t v) {
  nbt::Tag t;
  t.type = 1;
  t.number = v;
  return t;
}

nbt::Tag intTag(std::int32_t v) {
  nbt::Tag t;
  t.type = 3;
  t.number = v;
  return t;
}

nbt::Tag longTag(std::int64_t v) {
  nbt::Tag t;
  t.type = 4;
  t.number = v;
  return t;
}

nbt::Tag floatTag(float v) {
  nbt::Tag t;
  t.type = 5;
  t.real = v;
  return t;
}

nbt::Tag doubleTag(double v) {
  nbt::Tag t;
  t.type = 6;
  t.real = v;
  return t;
}

nbt::Tag strTag(std::string s) {
  nbt::Tag t;
  t.type = 8;
  t.text = std::move(s);
  return t;
}

nbt::Tag compoundTag(std::map<std::string, nbt::Tag> entries) {
  nbt::Tag t;
  t.type = 10;
  t.compound = std::move(entries);
  return t;
}

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const char *tag) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           (std::string("redstone-dim-") + tag + "-" + std::to_string(now));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void writeMinimalLevelDat(const std::filesystem::path &worldDir,
                          std::int64_t gameTime = 100) {
  nbt::Tag data = compoundTag({
      {"DataVersion", intTag(4786)},
      {"Time", longTag(gameTime)},
  });
  writeGzipNbt(worldDir / "level.dat", nbt::RootTag{"", compoundTag({{"Data", data}})});
}

template <class Fn> bool throwsInvalidArg(Fn &&fn) {
  try {
    fn();
    return false;
  } catch (const std::invalid_argument &) {
    return true;
  }
}

void testBuiltinDimensionSpecsAndValidation() {
  const auto &ow = overworldDimensionType();
  const auto &caves = overworldCavesDimensionType();
  const auto &nether = netherDimensionType();
  const auto &end = endDimensionType();

  assert(!ow.hasFixedTime && ow.hasSkyLight && !ow.hasCeiling);
  assert(ow.coordinateScale == 1.0 && ow.minY == -64 && ow.height == 384 &&
         ow.maxY() == 320 && ow.logicalHeight == 384);
  assert(!ow.waterEvaporates && !ow.fastLava && ow.defaultClock == "minecraft:overworld");

  assert(caves.hasCeiling && caves.hasSkyLight && caves.minY == -64 &&
         caves.height == 384);

  assert(nether.hasFixedTime && !nether.hasSkyLight && nether.hasCeiling);
  assert(nether.coordinateScale == 8.0 && nether.minY == 0 && nether.height == 256 &&
         nether.maxY() == 256 && nether.logicalHeight == 128);
  assert(std::abs(nether.ambientLight - 0.1f) < 1e-6f);
  assert(nether.waterEvaporates && nether.fastLava && nether.defaultClock.empty());

  assert(end.hasFixedTime && end.hasSkyLight && !end.hasCeiling &&
         end.hasEnderDragonFight);
  assert(end.coordinateScale == 1.0 && end.minY == 0 && end.height == 256 &&
         end.maxY() == 256 && end.defaultClock == "minecraft:the_end");

  assert(std::abs(teleportationScale(ow, nether) - 0.125) < 1e-9);
  assert(std::abs(teleportationScale(nether, ow) - 8.0) < 1e-9);

  assert(normalizeDimensionId("the_nether") == "minecraft:the_nether");
  assert(normalizeDimensionId("custom:void/realm") == "custom:void/realm");
  assert(throwsInvalidArg([] { (void)normalizeDimensionId(""); }));
  assert(throwsInvalidArg([] { (void)normalizeDimensionId("bad/../path"); }));
  assert(throwsInvalidArg([] { (void)normalizeDimensionId("Upper:case"); }));

  DimensionTypeSpec bad = nether;
  bad.minY = -8;
  assert(throwsInvalidArg([&] { validateDimensionType(bad); }));
  bad = nether;
  bad.height = 8;
  assert(throwsInvalidArg([&] { validateDimensionType(bad); }));
  bad = nether;
  bad.logicalHeight = 300;
  assert(throwsInvalidArg([&] { validateDimensionType(bad); }));
  bad = ow;
  bad.height = 512;
  validateDimensionType(bad);
  assert(throwsInvalidArg([&] { validateEngineDimensionType(bad, 24); }));
}

void testStoragePathResolution() {
  TempDir tmp("paths");
  const auto &dir = tmp.path;

  // Canonical paths returned by default when no legacy region dir exists.
  assert(dimensionRegionDirectory(dir, "minecraft:overworld") ==
         dir / "dimensions" / "minecraft" / "overworld" / "region");
  assert(dimensionRegionDirectory(dir, "the_nether") ==
         dir / "dimensions" / "minecraft" / "the_nether" / "region");
  assert(dimensionRegionDirectory(dir, "the_end") ==
         dir / "dimensions" / "minecraft" / "the_end" / "region");
  assert(dimensionRegionDirectory(dir, "custom:sky/island") ==
         dir / "dimensions" / "custom" / "sky" / "island" / "region");

  // Legacy fallback when DIM-1/region exists without canonical region/.
  std::filesystem::create_directories(dir / "DIM-1" / "region");
  std::filesystem::create_directories(dir / "region");
  assert(dimensionRegionDirectory(dir, "minecraft:overworld") == dir / "region");
  assert(dimensionRegionDirectory(dir, "minecraft:the_nether") ==
         dir / "DIM-1" / "region");
  assert(defaultRegionDirectory(dir) == dir / "region");

  // Modern 26.1 canonical path takes precedence once created.
  std::filesystem::create_directories(
      dir / "dimensions" / "minecraft" / "overworld" / "region");
  std::filesystem::create_directories(
      dir / "dimensions" / "minecraft" / "the_nether" / "region");
  assert(defaultRegionDirectory(dir) ==
         dir / "dimensions" / "minecraft" / "overworld" / "region");
  assert(dimensionRegionDirectory(dir, "minecraft:the_nether") ==
         dir / "dimensions" / "minecraft" / "the_nether" / "region");

  assert(resolveDimensionDataDirectory(
             dir / "dimensions" / "minecraft" / "the_end" / "region") ==
         dir / "dimensions" / "minecraft" / "the_end");
}

void testWorldGenSettingsDimensionsAndCustomType() {
  TempDir tmp("wgs");
  writeMinimalLevelDat(tmp.path, 42);

  nbt::Tag customType = compoundTag({
      {"has_fixed_time", byteTag(1)},
      {"has_skylight", byteTag(1)},
      {"has_ceiling", byteTag(0)},
      {"has_ender_dragon_fight", byteTag(0)},
      {"coordinate_scale", doubleTag(2.0)},
      {"min_y", intTag(0)},
      {"height", intTag(128)},
      {"logical_height", intTag(128)},
      {"ambient_light", floatTag(0.5f)},
      {"default_clock", strTag("minecraft:overworld")},
      {"attributes",
       compoundTag({
           {"minecraft:gameplay/water_evaporates", byteTag(1)},
           {"minecraft:gameplay/fast_lava", byteTag(1)},
       })},
  });
  nbt::Tag wgs = compoundTag({
      {"DataVersion", intTag(4786)},
      {"data",
       compoundTag({
           {"seed", longTag(123456789LL)},
           {"dimensions",
            compoundTag({
                {"custom:pocket", compoundTag({{"type", customType}})},
                {"minecraft:the_nether",
                 compoundTag({{"type", strTag("minecraft:the_nether")}})},
                {"minecraft:overworld",
                 compoundTag({{"type", strTag("minecraft:overworld")}})},
                {"minecraft:the_end",
                 compoundTag({{"type", strTag("minecraft:the_end")}})},
            })},
       })},
  });
  writeGzipNbt(tmp.path / "data" / "minecraft" / "world_gen_settings.dat",
               nbt::RootTag{"", wgs});

  const WorldMetadata meta = loadWorldMetadata(tmp.path);
  assert(meta.gameTime == 42);
  assert(meta.seed == 123456789LL);
  assert(meta.dimensions.size() == 4);
  assert(meta.dimensions[0].id == "minecraft:overworld");
  assert(meta.dimensions[1].id == "minecraft:the_nether");
  assert(meta.dimensions[2].id == "minecraft:the_end");
  assert(meta.dimensions[3].id == "custom:pocket");

  const DimensionStem *pocket = meta.findDimension("custom:pocket");
  assert(pocket != nullptr);
  assert(pocket->type.minY == 0 && pocket->type.height == 128 &&
         pocket->type.maxY() == 128);
  assert(pocket->type.coordinateScale == 2.0);
  assert(pocket->type.waterEvaporates && pocket->type.fastLava);
  assert(pocket->type.defaultClock == "minecraft:overworld");
}

void testFastLavaAndWaterEvaporationRules() {
  // Verify vanilla LavaFluid FAST_LAVA: tickDelay 10 (vs 30) and dropOff 1 (vs 2).
  World ow(overworldDimensionType());
  World nether(netherDimensionType());
  assert(nether.minY() == 0 && nether.maxY() == 256);
  assert(nether.waterEvaporates && nether.fastLava);
  assert(!ow.waterEvaporates && !ow.fastLava);

  const auto setupChannel = [](World &w) {
    for (int cx = -1; cx <= 1; ++cx)
      for (int cz = -1; cz <= 1; ++cz)
        w.loadChunk(cx, cz);
    for (int x = -2; x <= 10; ++x)
      for (int z = -2; z <= 2; ++z)
        w.rawSetBlock({x, 9, z}, defaultState("smooth_stone"));
    for (int z = -1; z <= 1; ++z) {
      w.rawSetBlock({-1, 10, z}, defaultState("smooth_stone"));
      if (z != 0)
        for (int x = 0; x <= 9; ++x)
          w.rawSetBlock({x, 10, z}, defaultState("smooth_stone"));
    }
    w.setBlock({0, 10, 0}, defaultState("lava").with("level", "0"));
  };
  setupChannel(ow);
  setupChannel(nether);

  for (int t = 0; t < 12; ++t) {
    ow.tick();
    nether.tick();
  }
  // At 12 ticks: Nether lava (delay=10) has reached x=1; Overworld lava (delay=30) has not.
  assert(nether.getBlock({1, 10, 0}).name() == "minecraft:lava");
  assert(ow.getBlock({1, 10, 0}).isAir());

  for (int t = 12; t < 260; ++t) {
    ow.tick();
    nether.tick();
  }
  // Steady horizontal spread distance: Nether dropOff=1 reaches x=7, Overworld dropOff=2 stops at x=3.
  assert(nether.getBlock({7, 10, 0}).name() == "minecraft:lava");
  assert(nether.getBlock({8, 10, 0}).isAir());
  assert(ow.getBlock({3, 10, 0}).name() == "minecraft:lava");
  assert(ow.getBlock({4, 10, 0}).isAir());
}

void testNetherHasNoSkyLightAndMultiWorldIndependence() {
  TempDir tmp("multi");
  writeMinimalLevelDat(tmp.path, 500);
  nbt::Tag wgs = compoundTag({
      {"DataVersion", intTag(4786)},
      {"data",
       compoundTag({
           {"seed", longTag(20261LL)},
           {"dimensions",
            compoundTag({
                {"minecraft:overworld",
                 compoundTag({{"type", strTag("minecraft:overworld")}})},
                {"minecraft:the_nether",
                 compoundTag({{"type", strTag("minecraft:the_nether")}})},
                {"minecraft:the_end",
                 compoundTag({{"type", strTag("minecraft:the_end")}})},
            })},
       })},
  });
  writeGzipNbt(tmp.path / "data" / "minecraft" / "world_gen_settings.dat",
               nbt::RootTag{"", wgs});

  ServerWorldOptions openOpts;
  openOpts.withChunkManagers = false;
  ServerWorldSet worlds = ServerWorldSet::open(tmp.path, openOpts);
  assert(worlds.levels().size() == 3);
  DimensionLevel *ow = worlds.find("minecraft:overworld");
  DimensionLevel *nether = worlds.find("minecraft:the_nether");
  DimensionLevel *end = worlds.find("minecraft:the_end");
  assert(ow && nether && end);
  assert(ow->seed != nether->seed && nether->seed != end->seed);

  // Per-dimension RNG divergence & independent entities.
  assert(ow->world->random.nextLong() != nether->world->random.nextLong());
  ow->world->loadChunk(0, 0);
  nether->world->loadChunk(0, 0);
  ow->world->rawSetBlock({4, 64, 4}, defaultState("smooth_stone"));
  ow->world->spawnItemAt({4.5, 66.0, 4.5}, "minecraft:redstone", 1);
  assert(ow->world->entities.size() == 1);
  assert(nether->world->entities.empty());

  // Nether lighting explicitly disables sky light even with default options.
  nether->world->enableLighting();
  assert(nether->world->lightService() != nullptr);
  assert(nether->world->lightService()->flushAndWait(
      std::chrono::milliseconds{2000}));
  nether->world->drainLightPublications();
  assert(nether->world->lightService()->latestLayerChunk(LightLayer::Sky, 0, 0) ==
         nullptr);
  const auto blockSnap =
      nether->world->lightService()->latestLayerChunk(LightLayer::Block, 0, 0);
  assert(blockSnap && !blockSnap->skyLightEnabled);
  assert(nether->world->getBrightness({8, 10, 8}) == 0);
  nether->world->disableLighting();

  // Ticking ServerWorldSet advances shared clocks once per tick and syncs gameTime.
  const std::int64_t initialClock =
      worlds.metadata().worldClocks.state(BuiltinWorldClock::Overworld).totalTicks;
  worlds.tick(true, true);
  worlds.tick(true, true);
  assert(worlds.metadata().gameTime == 502);
  assert(ow->world->gameTime() == 502);
  assert(nether->world->gameTime() == 502);
  assert(end->world->gameTime() == 502);
  assert(worlds.metadata().worldClocks.state(BuiltinWorldClock::Overworld).totalTicks ==
         initialClock + 2);
}

void testRealWorldMasterFixtureIfPresent() {
  const std::filesystem::path master =
      "validation/random_tick_bench/runtime/world_master";
  if (!std::filesystem::exists(master / "level.dat") ||
      !std::filesystem::exists(
          master / "data" / "minecraft" / "world_gen_settings.dat")) {
    return;
  }
  const WorldMetadata meta = loadWorldMetadata(master);
  assert(meta.dimensions.size() == 3);
  assert(meta.findDimension("minecraft:overworld") != nullptr);
  assert(meta.findDimension("minecraft:the_nether") != nullptr);
  assert(meta.findDimension("minecraft:the_end") != nullptr);
}

} // namespace

int main() {
  testBuiltinDimensionSpecsAndValidation();
  testStoragePathResolution();
  testWorldGenSettingsDimensionsAndCustomType();
  testFastLavaAndWaterEvaporationRules();
  testNetherHasNoSkyLightAndMultiWorldIndependence();
  testRealWorldMasterFixtureIfPresent();
  return 0;
}
