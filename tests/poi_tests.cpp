#include "redstone/poi.hpp"

#include "redstone/block.hpp"
#include "redstone/engine.hpp"
#include "redstone/nbt.hpp"
#include "redstone/region.hpp"
#include "redstone/storage.hpp"
#include "redstone/world_chunk_manager.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using namespace redstone;

BlockState stateWithProp(const Block &block, std::string_view key,
                         std::string_view value) {
  for (std::uint16_t i = 0; i < block.stateCount(); ++i) {
    const BlockState st = BlockState::fromId(block.firstState() + i);
    if (st.getView(key) == value)
      return st;
  }
  assert(false && "missing property value");
  return block.defaultState();
}

void testAllVanillaPoiTypesAndMasks() {
  ensurePoiStateTable();

  // Workstations & special POIs
  assert(poiTypeForState(Blocks::BLAST_FURNACE.defaultState()) ==
         PoiTypeId::Armorer);
  assert(poiTypeForState(Blocks::SMOKER.defaultState()) == PoiTypeId::Butcher);
  assert(poiTypeForState(Blocks::CARTOGRAPHY_TABLE.defaultState()) ==
         PoiTypeId::Cartographer);
  assert(poiTypeForState(Blocks::BREWING_STAND.defaultState()) ==
         PoiTypeId::Cleric);
  assert(poiTypeForState(Blocks::COMPOSTER.defaultState()) ==
         PoiTypeId::Farmer);
  assert(poiTypeForState(Blocks::BARREL.defaultState()) ==
         PoiTypeId::Fisherman);
  assert(poiTypeForState(Blocks::FLETCHING_TABLE.defaultState()) ==
         PoiTypeId::Fletcher);

  // All 4 cauldron blocks -> Leatherworker
  assert(poiTypeForState(Blocks::CAULDRON.defaultState()) ==
         PoiTypeId::Leatherworker);
  assert(poiTypeForState(Blocks::WATER_CAULDRON.defaultState()) ==
         PoiTypeId::Leatherworker);
  assert(poiTypeForState(Blocks::LAVA_CAULDRON.defaultState()) ==
         PoiTypeId::Leatherworker);
  assert(poiTypeForState(Blocks::POWDER_SNOW_CAULDRON.defaultState()) ==
         PoiTypeId::Leatherworker);

  assert(poiTypeForState(Blocks::LECTERN.defaultState()) ==
         PoiTypeId::Librarian);
  assert(poiTypeForState(Blocks::STONECUTTER.defaultState()) ==
         PoiTypeId::Mason);
  assert(poiTypeForState(Blocks::LOOM.defaultState()) == PoiTypeId::Shepherd);
  assert(poiTypeForState(Blocks::SMITHING_TABLE.defaultState()) ==
         PoiTypeId::Toolsmith);
  assert(poiTypeForState(Blocks::GRINDSTONE.defaultState()) ==
         PoiTypeId::Weaponsmith);

  // Bell: maxTickets = 32
  assert(poiTypeForState(Blocks::BELL.defaultState()) == PoiTypeId::Meeting);
  assert(poiTypeSpec(PoiTypeId::Meeting).maxTickets == 32);

  // Bee homes
  assert(poiTypeForState(Blocks::BEEHIVE.defaultState()) ==
         PoiTypeId::Beehive);
  assert(poiTypeForState(Blocks::BEE_NEST.defaultState()) ==
         PoiTypeId::BeeNest);

  // Nether portal, lodestone, test_instance_block
  assert(poiTypeForState(Blocks::NETHER_PORTAL.defaultState()) ==
         PoiTypeId::NetherPortal);
  assert(poiTypeForState(Blocks::LODESTONE.defaultState()) ==
         PoiTypeId::Lodestone);
  assert(poiTypeForState(Blocks::TEST_INSTANCE_BLOCK.defaultState()) ==
         PoiTypeId::TestInstance);

  // All 8 lightning rod variants -> LightningRod
  const Block *rods[] = {
      &Blocks::LIGHTNING_ROD,
      &Blocks::EXPOSED_LIGHTNING_ROD,
      &Blocks::WEATHERED_LIGHTNING_ROD,
      &Blocks::OXIDIZED_LIGHTNING_ROD,
      &Blocks::WAXED_LIGHTNING_ROD,
      &Blocks::WAXED_EXPOSED_LIGHTNING_ROD,
      &Blocks::WAXED_WEATHERED_LIGHTNING_ROD,
      &Blocks::WAXED_OXIDIZED_LIGHTNING_ROD,
  };
  for (const Block *rod : rods) {
    for (std::uint16_t i = 0; i < rod->stateCount(); ++i) {
      const BlockState st = BlockState::fromId(rod->firstState() + i);
      assert(poiTypeForState(st) == PoiTypeId::LightningRod);
    }
  }

  // All 16 bed colors: only part=head counts as Home; part=foot must be None.
  const Block *beds[] = {
      &Blocks::WHITE_BED,      &Blocks::ORANGE_BED, &Blocks::MAGENTA_BED,
      &Blocks::LIGHT_BLUE_BED, &Blocks::YELLOW_BED, &Blocks::LIME_BED,
      &Blocks::PINK_BED,       &Blocks::GRAY_BED,   &Blocks::LIGHT_GRAY_BED,
      &Blocks::CYAN_BED,       &Blocks::PURPLE_BED, &Blocks::BLUE_BED,
      &Blocks::BROWN_BED,      &Blocks::GREEN_BED,  &Blocks::RED_BED,
      &Blocks::BLACK_BED,
  };
  for (const Block *bed : beds) {
    assert(poiTypeForState(stateWithProp(*bed, "part", "head")) ==
           PoiTypeId::Home);
    assert(poiTypeForState(stateWithProp(*bed, "part", "foot")) ==
           PoiTypeId::None);
  }

  // Non-POI states
  assert(poiTypeForState(Blocks::AIR.defaultState()) == PoiTypeId::None);
  assert(poiTypeForState(Blocks::STONE.defaultState()) == PoiTypeId::None);

  // Tag bitmask helpers & names
  assert((kAcquirableJobSitePoiMask & poiTypeMask(PoiTypeId::Armorer)) != 0);
  assert((kAcquirableJobSitePoiMask & poiTypeMask(PoiTypeId::Home)) == 0);
  assert((kVillagePoiMask & poiTypeMask(PoiTypeId::Meeting)) != 0);
  assert((kBeeHomePoiMask & poiTypeMask(PoiTypeId::Beehive)) != 0);
  assert(poiTypeFromName("minecraft:nether_portal") == PoiTypeId::NetherPortal);
  assert(poiTypeFromName("minecraft:meeting") == PoiTypeId::Meeting);
}

void testDenseSectionUpgradeAndTypeMasks() {
  PoiStorage pois(-64, 320);

  // Add 64 POIs in chunk (0, 0) section y=0..15 -> crosses kLinearScanLimit=32.
  for (int i = 0; i < 64; ++i) {
    const int x = i & 15;
    const int z = (i >> 4) & 15;
    const PoiTypeId type =
        (i % 2 == 0) ? PoiTypeId::Librarian : PoiTypeId::Cartographer;
    assert(pois.add(PoiPos{x, 0, z}, type));
  }

  const PoiChunkColumn *col = pois.findColumn(0, 0);
  assert(col != nullptr);
  const int si = (0 - (-64)) >> 4;
  assert(col->sections[si] != nullptr);
  assert(col->sections[si]->entries.size() == 64);
  assert(!col->sections[si]->indexByLocal.empty());
  assert((col->typeMask & poiTypeMask(PoiTypeId::Librarian)) != 0);
  assert((col->typeMask & poiTypeMask(PoiTypeId::Cartographer)) != 0);

  // Remove every Cartographer via swap-and-pop.
  for (int i = 1; i < 64; i += 2) {
    const int x = i & 15;
    const int z = (i >> 4) & 15;
    assert(pois.remove(PoiPos{x, 0, z}));
  }

  assert(col->sections[si]->entries.size() == 32);
  assert((col->typeMask & poiTypeMask(PoiTypeId::Cartographer)) == 0);
  assert((col->typeMask & poiTypeMask(PoiTypeId::Librarian)) != 0);
  assert(col->candidateSections(poiTypeMask(PoiTypeId::Cartographer)) == 0);
  assert(col->candidateSections(poiTypeMask(PoiTypeId::Librarian)) != 0);

  // Verify remaining Librarian entries still resolve cleanly.
  for (int i = 0; i < 64; ++i) {
    const int x = i & 15;
    const int z = (i >> 4) & 15;
    if (i % 2 == 0) {
      assert(pois.getType(PoiPos{x, 0, z}) == PoiTypeId::Librarian);
    } else {
      assert(pois.getType(PoiPos{x, 0, z}) == PoiTypeId::None);
    }
  }
}

void testTicketsAndSpatialQueries() {
  PoiStorage pois(-64, 320);

  const PoiPos bellPos{10, 64, 10};
  const PoiPos composterNear{12, 74, 10}; // distSq from (10,64,10) = 4 + 100 = 104
  const PoiPos composterFar{18, 64, 10};  // distSq from (10,64,10) = 64
  const PoiPos farOutside{60, 64, 60};

  assert(pois.add(bellPos, PoiTypeId::Meeting));
  assert(pois.add(composterNear, PoiTypeId::Farmer));
  assert(pois.add(composterFar, PoiTypeId::Farmer));
  assert(pois.add(farOutside, PoiTypeId::Farmer));

  // Bell supports 32 concurrent tickets.
  assert(pois.freeTickets(bellPos) == 32);
  for (int i = 0; i < 32; ++i)
    assert(pois.acquireTicket(bellPos));
  assert(pois.freeTickets(bellPos) == 0);
  assert(!pois.acquireTicket(bellPos));
  assert(pois.exists(bellPos, poiTypeMask(PoiTypeId::Meeting)));
  assert(pois.countInRange(bellPos, 1, poiTypeMask(PoiTypeId::Meeting),
                           PoiOccupancy::HasSpace) == 0);
  assert(pois.releaseTicket(bellPos));
  assert(pois.freeTickets(bellPos) == 1);

  // Composter has maxTickets = 1.
  // Between composterNear (dx=2, dy=10 -> d2=104) and composterFar (dx=8, dy=0 -> d2=64),
  // even though composterFar is in chunk (1,0) and composterNear is in chunk (0,0),
  // findClosest MUST return composterFar because 64 < 104!
  const auto closestFarmer = pois.findClosest(
      bellPos, 16, poiTypeMask(PoiTypeId::Farmer), PoiOccupancy::HasSpace);
  assert(closestFarmer.has_value());
  assert(*closestFarmer == composterFar);

  // take follows getInRange chunk order (chunk (0,0) before chunk (1,0)).
  const auto taken = pois.take(bellPos, 16, poiTypeMask(PoiTypeId::Farmer));
  assert(taken.has_value() && *taken == composterNear);
  assert(pois.freeTickets(composterNear) == 0);

  // Next HasSpace query skips occupied composterNear and finds composterFar.
  const auto nextClosest = pois.findClosest(
      bellPos, 16, poiTypeMask(PoiTypeId::Farmer), PoiOccupancy::HasSpace);
  assert(nextClosest.has_value() && *nextClosest == composterFar);

  // IsOccupied only sees composterFar within radius 16.
  assert(pois.countInRange(bellPos, 16, poiTypeMask(PoiTypeId::Farmer),
                           PoiOccupancy::IsOccupied) == 1);
  assert(pois.countInRange(bellPos, 16, poiTypeMask(PoiTypeId::Farmer),
                           PoiOccupancy::Any) == 2);

  // Out of vertical bounds is rejected cleanly.
  assert(!pois.add(PoiPos{0, -65, 0}, PoiTypeId::Farmer));
  assert(!pois.add(PoiPos{0, 320, 0}, PoiTypeId::Farmer));
}

void testFindClosestPortalSemantics() {
  PoiStorage pois(-64, 320);

  // Portal 1: dx=20, dz=0, y=70 -> 3D distSq=400, rejected by filter
  // Portal 2: dx=0, dz=25, y=80 (dy=+10) -> 3D distSq=100+625=725
  // Portal 3: dx=0, dz=-25, y=60 (dy=-10) -> 3D distSq=100+625=725 (must beat Portal 2 via Y ASC)
  const PoiPos center{0, 70, 0};
  const PoiPos blockedClosest{20, 70, 0};
  const PoiPos tiedHigh{0, 80, 25};
  const PoiPos tiedLow{0, 60, -25};
  // Outside square radius 32: dx=33, dz=0 (ignored even if closer than something)
  const PoiPos outOfSquare{33, 70, 0};

  assert(pois.add(blockedClosest, PoiTypeId::NetherPortal));
  assert(pois.add(tiedHigh, PoiTypeId::NetherPortal));
  assert(pois.add(tiedLow, PoiTypeId::NetherPortal));
  assert(pois.add(outOfSquare, PoiTypeId::NetherPortal));

  const auto found = pois.findClosestPortal(
      center, 32, [&](const PoiMatch &m) { return m.pos != blockedClosest; });
  assert(found.has_value());
  assert(*found == tiedLow);

  // Also test near +/- 30,000,000 world border so int64 distSq never overflows.
  PoiStorage borderPois(0, 128);
  const PoiPos borderCenter{29'999'900, 64, -29'999'900};
  const PoiPos borderNear{29'999'910, 100, -29'999'910}; // distSq = 200
  const PoiPos borderFar{29'999'940, 10, -29'999'940};   // distSq = 3200
  assert(borderPois.add(borderNear, PoiTypeId::NetherPortal));
  assert(borderPois.add(borderFar, PoiTypeId::NetherPortal));
  const auto borderFound = borderPois.findClosestPortal(borderCenter, 128);
  assert(borderFound.has_value());
  assert(*borderFound == borderNear);
}

void testRegionCodecAndValidZeroReconciliation(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  PoiStorage writer(-64, 320);
  const PoiPos bellPos{-12, 65, -20};
  const PoiPos portalPos{-11, -40, -19};
  assert(writer.add(bellPos, PoiTypeId::Meeting));
  for (int i = 0; i < 5; ++i)
    assert(writer.acquireTicket(bellPos)); // freeTickets = 27
  assert(writer.add(portalPos, PoiTypeId::NetherPortal));

  const PoiChunkColumn *col = writer.findColumn(-1, -2);
  assert(col != nullptr);
  savePoiChunkToRegion(dir, -1, -2, -4, 24, *col);

  const DecodedPoiChunk loaded = loadPoiChunkFromRegion(dir, -1, -2, -4, 19);
  assert(loaded.status == DecodedPoiChunk::Status::Ok);
  assert(loaded.dataVersion == 4786);

  // Loading decoded Valid=0 sections into fresh PoiStorage, then reconciling
  // against actual terrain scan keeps valid freeTickets and drops stale POIs.
  PoiStorage reader(-64, 320);
  DecodedPoiChunk unverified = loaded;
  for (DecodedPoiSection &sec : unverified.sections)
    sec.valid = false;
  reader.applyDecodedChunk(-1, -2, unverified);

  // Bell still exists in terrain scan, portal was removed offline.
  const int bellSection = (bellPos.y - (-64)) >> 4;
  const std::uint16_t bellLocal = static_cast<std::uint16_t>(
      ((bellPos.y - (-64 + bellSection * 16)) << 8) | ((bellPos.z & 15) << 4) |
      (bellPos.x & 15));
  const std::pair<std::uint16_t, PoiTypeId> bellScan[] = {
      {bellLocal, PoiTypeId::Meeting}};
  reader.reconcileSectionFromScan(-1, -2, bellSection, bellScan, true);
  reader.markSectionValidEmpty(-1, -2, (portalPos.y - (-64)) >> 4);

  assert(reader.freeTickets(bellPos) == 27);
  assert(!reader.exists(portalPos));
  assert(reader.isSectionValid(-1, -2, bellSection));

  // Verify wrong DataVersion is rejected cleanly.
  const std::vector<std::uint8_t> encoded =
      encodePoiChunk(-1, -2, -4, 24, *col);
  nbt::RootTag root = nbt::parseRoot(encoded);
  root.value.compound["DataVersion"].number = 9999;
  const std::vector<std::uint8_t> badVersion = nbt::writeRoot(root);
  const DecodedPoiChunk rejected = decodePoiChunk(badVersion, -4, 19);
  assert(rejected.status == DecodedPoiChunk::Status::Unsupported);

  std::filesystem::remove_all(dir, ec);
}

nbt::Tag makeIntTag(std::int32_t v) {
  nbt::Tag t;
  t.type = 3;
  t.number = v;
  return t;
}

nbt::Tag makeByteTag(std::int8_t v) {
  nbt::Tag t;
  t.type = 1;
  t.number = v;
  return t;
}

nbt::Tag makeStringTag(std::string s) {
  nbt::Tag t;
  t.type = 8;
  t.text = std::move(s);
  return t;
}

std::vector<std::uint8_t> makePortalChunkNbt(int chunkX, int chunkZ) {
  nbt::RootTag root;
  root.value.type = 10;
  root.value.compound["DataVersion"] = makeIntTag(4786);
  root.value.compound["xPos"] = makeIntTag(chunkX);
  root.value.compound["zPos"] = makeIntTag(chunkZ);
  root.value.compound["Status"] = makeStringTag("minecraft:full");
  root.value.compound["isLightOn"] = makeByteTag(1);

  nbt::Tag airEntry;
  airEntry.type = 10;
  airEntry.compound["Name"] = makeStringTag("minecraft:air");
  nbt::Tag composterEntry;
  composterEntry.type = 10;
  composterEntry.compound["Name"] = makeStringTag("minecraft:composter");
  nbt::Tag props;
  props.type = 10;
  props.compound["level"] = makeStringTag("0");
  composterEntry.compound["Properties"] = std::move(props);

  nbt::Tag palette;
  palette.type = 9;
  palette.listElementType = 10;
  palette.list.push_back(std::move(airEntry));
  palette.list.push_back(std::move(composterEntry));

  // Index (y=64 -> ly=0, z=3, x=4) => (0 << 8) | (3 << 4) | 4 = 52; 4 bits per entry
  // 16 entries per long => 256 longs; entry 52 is word 3, slot 4 (bits 16..19).
  nbt::Tag dataTag;
  dataTag.type = 12; // LongArray
  dataTag.longs.assign(256, 0);
  dataTag.longs[52 / 16] = std::int64_t{1} << ((52 % 16) * 4);

  nbt::Tag blockStates;
  blockStates.type = 10;
  blockStates.compound["palette"] = std::move(palette);
  blockStates.compound["data"] = std::move(dataTag);

  nbt::Tag section;
  section.type = 10;
  section.compound["Y"] = makeByteTag(4); // section y=64..79
  section.compound["block_states"] = std::move(blockStates);

  nbt::Tag sections;
  sections.type = 9;
  sections.listElementType = 10;
  sections.list.push_back(std::move(section));
  root.value.compound["sections"] = std::move(sections);
  return nbt::writeRoot(root);
}

void testWorldIntegrationAndDimensionIsolation(
    const std::filesystem::path &managedDir) {
  // Overworld bounds [-64, 320), Nether bounds [0, 128).
  World overworld(-64, 320);
  World nether(netherDimensionType(), 10000, "minecraft:the_nether");

  overworld.loadChunk(0, 0);
  overworld.loadChunk(1, 0);

  // Importing an all-stone / all-air section MUST NOT allocate in PoiStorage.
  std::vector<BlockState::Id> stonePalette{Blocks::STONE.defaultState().id()};
  std::array<std::uint16_t, kLightVolume> zeroIndices{};
  overworld.importSectionBlocks(1, 0, 0, stonePalette, zeroIndices);
  assert(overworld.pois().chunkCount() == 0);

  // Importing a mixed palette containing 1 bell indexes it in chunk (0, 0).
  std::vector<BlockState::Id> mixedPalette{Blocks::AIR.defaultState().id(),
                                           Blocks::BELL.defaultState().id()};
  std::array<std::uint16_t, kLightVolume> bellIndices{};
  bellIndices[(4 << 8) | (5 << 4) | 6] = 1; // y = 64+4=68, z=5, x=6
  overworld.importSectionBlocks(0, 0, 4, mixedPalette, bellIndices);
  assert(overworld.pois().chunkCount() == 1);
  assert(overworld.pois().getType(PoiPos{6, 68, 5}) == PoiTypeId::Meeting);

  // setBlock updates PoiStorage when placing / removing a bed head vs foot.
  const BlockState head = stateWithProp(Blocks::RED_BED, "part", "head");
  const BlockState foot = stateWithProp(Blocks::RED_BED, "part", "foot");
  assert(overworld.setBlock(Pos{10, 64, 10}, head, 2));
  assert(overworld.setBlock(Pos{10, 64, 11}, foot, 2));
  assert(overworld.pois().getType(PoiPos{10, 64, 10}) == PoiTypeId::Home);
  assert(overworld.pois().getType(PoiPos{10, 64, 11}) == PoiTypeId::None);
  assert(overworld.setBlock(Pos{10, 64, 10}, Blocks::AIR.defaultState(), 2));
  assert(overworld.pois().getType(PoiPos{10, 64, 10}) == PoiTypeId::None);

  // Managed chunk install + eviction through WorldChunkManager reconciles and removes POIs.
  std::error_code ec;
  std::filesystem::remove_all(managedDir, ec);
  std::filesystem::create_directories(managedDir / "region", ec);
  const std::vector<std::uint8_t> portalNbt = makePortalChunkNbt(2, 0);
  const region::WriteResult wr =
      region::writeChunkNbt(managedDir / "region", 2, 0, portalNbt);
  assert(wr.wrote);
  {
    WorldChunkManager manager(overworld, managedDir / "region", 2);
    (void)manager.setForced(ChunkTicketPos{2, 0}, true);
    for (int i = 0; i < 500; ++i) {
      manager.pump();
      if (overworld.pois().getType(PoiPos{36, 64, 3}) == PoiTypeId::Farmer)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(overworld.pois().getType(PoiPos{36, 64, 3}) == PoiTypeId::Farmer);

    (void)manager.setForced(ChunkTicketPos{2, 0}, false);
    for (int i = 0; i < 500; ++i) {
      manager.pump();
      if (overworld.pois().getType(PoiPos{36, 64, 3}) == PoiTypeId::None)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(overworld.pois().getType(PoiPos{36, 64, 3}) == PoiTypeId::None);
  }
  std::filesystem::remove_all(managedDir, ec);

  // Separate World dimension owns independent PoiStorage.
  nether.loadChunk(0, 0);
  nether.rawSetBlock(Pos{6, 68, 5}, Blocks::LODESTONE.defaultState());
  assert(nether.pois().getType(PoiPos{6, 68, 5}) == PoiTypeId::Lodestone);
  assert(overworld.pois().getType(PoiPos{6, 68, 5}) == PoiTypeId::Meeting);
}

} // namespace

int main(int argc, char **argv) {
  const std::filesystem::path scratch =
      (argc > 1)
          ? std::filesystem::path(argv[1])
          : (std::filesystem::temp_directory_path() / "redstone-poi-test");

  testAllVanillaPoiTypesAndMasks();
  testDenseSectionUpgradeAndTypeMasks();
  testTicketsAndSpatialQueries();
  testFindClosestPortalSemantics();
  testRegionCodecAndValidZeroReconciliation(scratch);
  testWorldIntegrationAndDimensionIsolation(scratch / "managed");

  std::cout << "poi tests passed\n";
  return 0;
}
