#include "redstone/storage.hpp"
#include "redstone/tick_rate.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using redstone::BuiltinWorldClock;
using redstone::WorldClockState;
using redstone::WorldClocks;
using redstone::nbt::RootTag;
using redstone::nbt::Tag;

int failures = 0;
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL " << message << '\n';
    ++failures;
  }
}
Tag text(std::string value) {
  Tag tag;
  tag.type = 8;
  tag.text = std::move(value);
  return tag;
}
Tag byte(std::int64_t value) {
  Tag tag;
  tag.type = 1;
  tag.number = value;
  return tag;
}
Tag integer(std::int64_t value) {
  Tag tag;
  tag.type = 3;
  tag.number = value;
  return tag;
}
Tag longValue(std::int64_t value) {
  Tag tag;
  tag.type = 4;
  tag.number = value;
  return tag;
}
Tag real(float value) {
  Tag tag;
  tag.type = 5;
  tag.real = static_cast<double>(value);
  return tag;
}
Tag compound() {
  Tag tag;
  tag.type = 10;
  return tag;
}
void put(Tag &parent, std::string name, Tag child) {
  parent.compoundOrder.push_back(name);
  parent.compound.emplace(std::move(name), std::move(child));
}
RootTag officialShape() {
  RootTag root;
  root.value.type = 10;
  const auto addRoot = [&](std::string name, Tag child) {
    root.value.compoundOrder.push_back(name);
    root.value.compound.emplace(std::move(name), std::move(child));
  };
  addRoot("DataVersion", integer(4786));
  addRoot("future_root", longValue(0x123456789LL));
  Tag data = compound();
  Tag overworld = compound();
  put(overworld, "total_ticks", longValue(48));
  put(overworld, "future_field", text("keep"));
  Tag end = compound();
  put(end, "total_ticks", longValue(48));
  put(end, "rate", real(2.0f));
  Tag custom = compound();
  put(custom, "total_ticks", longValue(99));
  put(custom, "future_flag", byte(1));
  put(data, "minecraft:overworld", std::move(overworld));
  put(data, "minecraft:the_end", std::move(end));
  put(data, "example:custom", std::move(custom));
  addRoot("data", std::move(data));
  return root;
}
void writeBytes(const std::filesystem::path &path,
                const std::vector<std::uint8_t> &bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  if (!out)
    throw std::runtime_error("test write failed");
}
std::vector<std::uint8_t> readBytes(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void writeDocument(const std::filesystem::path &path, const RootTag &root) {
  writeBytes(path, redstone::nbt::compress(redstone::nbt::writeRoot(root), 31));
}
bool same(const WorldClockState &a, const WorldClockState &b) {
  return a.totalTicks == b.totalTicks && a.partialTick == b.partialTick &&
         a.rate == b.rate && a.paused == b.paused;
}
} // namespace

int main(int argc, char **argv) {
  std::filesystem::path scratch =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() /
                     "redstone-world-clock-tests";
  std::error_code ignored;
  std::filesystem::remove_all(scratch, ignored);
  std::filesystem::create_directories(scratch);
  const auto clocksPath = scratch / "data/minecraft/world_clocks.dat";

  const RootTag originalRoot = officialShape();
  writeDocument(clocksPath, originalRoot);
  const auto originalBytes = readBytes(clocksPath);
  WorldClocks clocks = WorldClocks::load(clocksPath);
  check(clocks.state(BuiltinWorldClock::Overworld).totalTicks == 48,
        "overworld total_ticks");
  check(clocks.state(BuiltinWorldClock::Overworld).partialTick == 0.0f,
        "partial_tick default");
  check(clocks.state(BuiltinWorldClock::Overworld).rate == 1.0f,
        "rate default");
  check(!clocks.state(BuiltinWorldClock::Overworld).paused,
        "paused default");
  check(clocks.state(BuiltinWorldClock::TheEnd).rate == 2.0f,
        "explicit end rate");
  check(!clocks.dirty(), "load is clean");

  check(!clocks.saveIfDirty(clocksPath), "clean save is a no-op");
  check(readBytes(clocksPath) == originalBytes,
        "clean save preserves exact existing bytes");

  const auto defaults = WorldClocks::load(scratch / "missing/world_clocks.dat");
  check(defaults.state(BuiltinWorldClock::Overworld).totalTicks == 0 &&
            defaults.state(BuiltinWorldClock::TheEnd).totalTicks == 0 &&
            defaults.state(BuiltinWorldClock::Overworld).rate == 1.0f,
        "missing file defaults");

  auto overworld = clocks.state(BuiltinWorldClock::Overworld);
  overworld.rate = 0.5f;
  check(clocks.setClock(BuiltinWorldClock::Overworld, overworld),
        "set overworld rate");
  auto end = clocks.state(BuiltinWorldClock::TheEnd);
  end.rate = 2.5f;
  check(clocks.setClock(BuiltinWorldClock::TheEnd, end), "set end rate");

  const auto beforeAdvanceFalse = clocks.state(BuiltinWorldClock::Overworld);
  redstone::World independentGameTime;
  independentGameTime.setGameTime(100);
  independentGameTime.tick();
  clocks.tick(true, false);
  check(independentGameTime.gameTime() == 101,
        "gameTime advances independently");
  check(same(beforeAdvanceFalse,
             clocks.state(BuiltinWorldClock::Overworld)),
        "advance_time=false preserves clocks");

  redstone::TickRateManager tickRate;
  tickRate.setFrozen(true);
  tickRate.tick();
  const auto beforeFrozen = clocks.state(BuiltinWorldClock::Overworld);
  const auto beforeFrozenEnd = clocks.state(BuiltinWorldClock::TheEnd);
  clocks.tick(tickRate.runsNormally(), true);
  check(same(beforeFrozen, clocks.state(BuiltinWorldClock::Overworld)) &&
            same(beforeFrozenEnd, clocks.state(BuiltinWorldClock::TheEnd)),
        "freeze preserves clocks");

  check(tickRate.stepGameIfPaused(2), "start two-step");
  for (int i = 0; i < 2; ++i) {
    tickRate.tick();
    clocks.tick(tickRate.runsNormally(), true);
  }
  check(clocks.state(BuiltinWorldClock::Overworld).totalTicks == 49 &&
            clocks.state(BuiltinWorldClock::Overworld).partialTick == 0.0f,
        "overworld step carry");
  check(clocks.state(BuiltinWorldClock::TheEnd).totalTicks == 53 &&
            clocks.state(BuiltinWorldClock::TheEnd).partialTick == 0.0f,
        "end independent step carry");
  tickRate.tick();
  clocks.tick(tickRate.runsNormally(), true);
  check(clocks.state(BuiltinWorldClock::Overworld).totalTicks == 49 &&
            clocks.state(BuiltinWorldClock::TheEnd).totalTicks == 53,
        "completed step returns to frozen");

  end = clocks.state(BuiltinWorldClock::TheEnd);
  overworld = clocks.state(BuiltinWorldClock::Overworld);
  overworld.paused = true;
  check(clocks.setClock(BuiltinWorldClock::Overworld, overworld),
        "pause overworld");
  clocks.tick(true, true);
  check(clocks.state(BuiltinWorldClock::Overworld).totalTicks == 49 &&
            clocks.state(BuiltinWorldClock::Overworld).partialTick == 0.0f,
        "paused clock does not advance");
  check(clocks.state(BuiltinWorldClock::TheEnd).totalTicks == 55 &&
            clocks.state(BuiltinWorldClock::TheEnd).partialTick == 0.5f,
        "unpaused clock advances independently");

  check(clocks.saveIfDirty(clocksPath), "dirty save writes");
  check(!clocks.dirty(), "successful save clears dirty");
  const auto savedBytes = readBytes(clocksPath);
  check(savedBytes != originalBytes, "dirty save changes bytes");
  auto saved = redstone::nbt::parseRoot(
      redstone::nbt::decompress(savedBytes, 31));
  check(saved.value.get("future_root") != nullptr,
        "unknown root field preserved");
  Tag *savedData = saved.value.get("data");
  check(savedData && savedData->get("example:custom") != nullptr,
        "unknown clock preserved");
  Tag *savedOverworld = savedData ? savedData->get("minecraft:overworld") : nullptr;
  check(savedOverworld && savedOverworld->get("future_field") != nullptr,
        "unknown clock field preserved");
  check(savedOverworld &&
            savedOverworld->get("total_ticks")->number == 49 &&
            savedOverworld->get("paused")->number == 1,
        "saved overworld state");
  Tag *savedEnd = savedData ? savedData->get("minecraft:the_end") : nullptr;
  check(savedEnd && savedEnd->get("total_ticks")->number == 55 &&
            static_cast<float>(savedEnd->get("partial_tick")->real) == 0.5f &&
            static_cast<float>(savedEnd->get("rate")->real) == 2.5f,
        "saved end state");

  WorldClocks roundTrip = WorldClocks::load(clocksPath);
  check(same(roundTrip.state(BuiltinWorldClock::Overworld),
             clocks.state(BuiltinWorldClock::Overworld)) &&
            same(roundTrip.state(BuiltinWorldClock::TheEnd),
                 clocks.state(BuiltinWorldClock::TheEnd)),
        "load/save round trip");
  check(!roundTrip.saveIfDirty(clocksPath), "round-trip clean");
  check(readBytes(clocksPath) == savedBytes,
        "round-trip clean save preserves bytes");

  Tag rulesRoot;
  rulesRoot.type = 10;
  put(rulesRoot, "DataVersion", integer(4786));
  Tag rulesData = compound();
  put(rulesData, "minecraft:advance_time", byte(0));
  put(rulesData, "minecraft:random_tick_speed", integer(7));
  put(rulesData, "unknown:rule", text("ignored"));
  put(rulesRoot, "data", std::move(rulesData));
  const auto rulesPath = scratch / "data/minecraft/game_rules.dat";
  writeDocument(rulesPath, RootTag{"", std::move(rulesRoot)});
  const auto loadedRules = redstone::loadWorldGameRules(rulesPath);
  check(!loadedRules.advanceTime, "advance_time=false loaded");
  check(loadedRules.randomTickSpeed == 7,
        "random_tick_speed=7 loaded");
  check(redstone::WorldGameRules{}.advanceTime,
        "advance_time defaults true");
  check(redstone::WorldGameRules{}.randomTickSpeed == 3,
        "random_tick_speed defaults 3");

  const auto corruptPath = scratch / "corrupt.dat";
  writeBytes(corruptPath, {'n', 'o', 't', ' ', 'n', 'b', 't'});
  const auto corruptBytes = readBytes(corruptPath);
  bool rejected = false;
  try {
    (void)WorldClocks::load(corruptPath);
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, "corrupt file rejected");
  check(readBytes(corruptPath) == corruptBytes,
        "corrupt file is not overwritten");

  RootTag badVersion = officialShape();
  badVersion.value.get("DataVersion")->number = 4785;
  writeDocument(corruptPath, badVersion);
  rejected = false;
  try {
    (void)WorldClocks::load(corruptPath);
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, "unsupported DataVersion rejected");

  RootTag badRate = officialShape();
  badRate.value.get("data")->get("minecraft:the_end")->compound["rate"] =
      real(0.0f);
  writeDocument(corruptPath, badRate);
  rejected = false;
  try {
    (void)WorldClocks::load(corruptPath);
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, "non-positive rate rejected");

  WorldClocks failedWrite = WorldClocks::load(scratch / "another-missing.dat");
  overworld = failedWrite.state(BuiltinWorldClock::Overworld);
  overworld.totalTicks = 1;
  failedWrite.setClock(BuiltinWorldClock::Overworld, overworld);
  const auto badParent = scratch / "not-a-directory";
  writeBytes(badParent, {'x'});
  rejected = false;
  try {
    (void)failedWrite.saveIfDirty(badParent / "world_clocks.dat");
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, "save error propagates");
  check(failedWrite.dirty(), "failed save retains dirty state");

  std::filesystem::remove_all(scratch, ignored);
  if (failures) {
    std::cerr << failures << " world clock test failures\n";
    return 1;
  }
  std::cout << "world clock tests passed\n";
  return 0;
}
