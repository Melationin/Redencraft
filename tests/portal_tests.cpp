#include "redstone/portal.hpp"

#include "redstone/nbt.hpp"
#include "redstone/poi.hpp"
#include "redstone/region.hpp"
#include "redstone/server_worlds.hpp"
#include "redstone/shapes.hpp"
#include "redstone/tickets.hpp"
#include "redstone/world_chunk_manager.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace redstone;

namespace {

void buildObsidianFrame(World &w, Pos bottomLeftInterior, PortalAxis axis,
                        int interiorWidth, int interiorHeight,
                        bool includeCorners = true) {
  const Direction dir =
      axis == PortalAxis::X ? Direction::West : Direction::South;
  const Direction back = opposite(dir);
  auto stepDir = [](Pos p, Direction d, int k) -> Pos {
    switch (d) {
    case Direction::East:
      return {p.x + k, p.y, p.z};
    case Direction::West:
      return {p.x - k, p.y, p.z};
    case Direction::North:
      return {p.x, p.y, p.z - k};
    case Direction::South:
      return {p.x, p.y, p.z + k};
    case Direction::Up:
      return {p.x, p.y + k, p.z};
    case Direction::Down:
      return {p.x, p.y - k, p.z};
    }
    return p;
  };
  (void)back;

  const BlockState obs = Blocks::OBSIDIAN.defaultState();
  const int wStart = includeCorners ? -1 : 0;
  const int wEnd = includeCorners ? interiorWidth : (interiorWidth - 1);
  for (int i = wStart; i <= wEnd; ++i) {
    w.setBlock(stepDir({bottomLeftInterior.x, bottomLeftInterior.y - 1,
                        bottomLeftInterior.z},
                       dir, i),
               obs, 3);
    w.setBlock(stepDir({bottomLeftInterior.x,
                        bottomLeftInterior.y + interiorHeight,
                        bottomLeftInterior.z},
                       dir, i),
               obs, 3);
  }
  for (int h = 0; h < interiorHeight; ++h) {
    w.setBlock(stepDir({bottomLeftInterior.x, bottomLeftInterior.y + h,
                        bottomLeftInterior.z},
                       dir, -1),
               obs, 3);
    w.setBlock(stepDir({bottomLeftInterior.x, bottomLeftInterior.y + h,
                        bottomLeftInterior.z},
                       dir, interiorWidth),
               obs, 3);
  }
}

void buildFloor(World &w, int minX, int maxX, int y, int minZ, int maxZ,
                const BlockState &state) {
  for (int x = minX; x <= maxX; ++x)
    for (int z = minZ; z <= maxZ; ++z)
      w.setBlock({x, y, z}, state, 2);
}

void testBlockPropertiesAndShapes() {
  assert(Blocks::NETHER_PORTAL.supported());
  assert(Blocks::FIRE.supported());
  assert(Blocks::SOUL_FIRE.supported());

  const BlockState portalX =
      Blocks::NETHER_PORTAL.defaultState().with("axis", "x");
  const BlockState portalZ =
      Blocks::NETHER_PORTAL.defaultState().with("axis", "z");
  assert(collisionShape(portalX).empty());
  assert(collisionShape(portalZ).empty());
  assert(!outlineShape(portalX).empty());
  assert(!outlineShape(portalZ).empty());
  assert(collisionShape(Blocks::FIRE.defaultState()).empty());
  assert(!outlineShape(Blocks::FIRE.defaultState()).empty());
  assert(collisionShape(Blocks::SOUL_FIRE.defaultState()).empty());
  assert(!outlineShape(Blocks::SOUL_FIRE.defaultState()).empty());
}

void testPortalShapeBoundsAndIgnition() {
  // 1. Minimal 2x3 portal without corners on X axis and Z axis.
  {
    World w;
    w.loadChunk(0, 0);
    w.loadChunk(-1, 0);
    buildObsidianFrame(w, {5, 65, 5}, PortalAxis::X, 2, 3, false);

    // Fire inside frame creates NETHER_PORTAL across all 2x3 cells.
    assert(w.setBlock({5, 65, 5}, Blocks::FIRE.defaultState(), 3));
    for (int dx = 0; dx < 2; ++dx) {
      for (int dy = 0; dy < 3; ++dy) {
        const Pos p{5 - dx, 65 + dy, 5};
        assert(w.getBlock(p).is(Blocks::NETHER_PORTAL));
        assert(w.getBlock(p).get("axis") == "x");
        assert(w.pois().getType({p.x, p.y, p.z}) == PoiTypeId::NetherPortal);
      }
    }

    // Placing/breaking a block on the wrong (perpendicular Z) side must NOT break portal.
    assert(w.setBlock({5, 66, 6}, Blocks::STONE.defaultState(), 3));
    assert(w.setBlock({5, 66, 6}, Blocks::AIR.defaultState(), 3));
    assert(w.getBlock({5, 65, 5}).is(Blocks::NETHER_PORTAL));

    // Breaking a single obsidian top frame block collapses all 6 portal blocks.
    assert(w.setBlock({4, 68, 5}, Blocks::AIR.defaultState(), 3));
    for (int dx = 0; dx < 2; ++dx) {
      for (int dy = 0; dy < 3; ++dy) {
        const Pos p{5 - dx, 65 + dy, 5};
        assert(w.getBlock(p).isAir());
        assert(w.pois().getType({p.x, p.y, p.z}) == PoiTypeId::None);
      }
    }
  }

  // 2. Z-axis portal ignited by Soul Fire + interior break cascade.
  {
    World w(0, 256);
    w.setDimension(netherDimensionType(), "minecraft:the_nether");
    w.loadChunk(0, 0);
    buildObsidianFrame(w, {4, 70, 4}, PortalAxis::Z, 3, 4, true);

    assert(w.setBlock({4, 70, 5}, Blocks::SOUL_FIRE.defaultState(), 3));
    for (int dz = 0; dz < 3; ++dz) {
      for (int dy = 0; dy < 4; ++dy) {
        const Pos p{4, 70 + dy, 4 + dz};
        assert(w.getBlock(p).is(Blocks::NETHER_PORTAL));
        assert(w.getBlock(p).get("axis") == "z");
      }
    }

    // Breaking one interior portal block collapses the whole sheet.
    assert(w.setBlock({4, 71, 5}, Blocks::AIR.defaultState(), 3));
    for (int dz = 0; dz < 3; ++dz) {
      for (int dy = 0; dy < 4; ++dy) {
        assert(w.getBlock({4, 70 + dy, 4 + dz}).isAir());
      }
    }
  }

  // 3. Undersize (1x3, 2x2) and oversize (22x3) frames must be rejected.
  {
    World w;
    for (int cx = -2; cx <= 2; ++cx)
      w.loadChunk(cx, 0);

    // 1x3 too narrow
    buildObsidianFrame(w, {2, 65, 2}, PortalAxis::X, 1, 3, true);
    assert(!PortalShape::findEmptyPortalShape(w, {2, 65, 2}, PortalAxis::X)
                .has_value());

    // 2x2 too short
    buildObsidianFrame(w, {8, 65, 2}, PortalAxis::X, 2, 2, true);
    assert(!PortalShape::findEmptyPortalShape(w, {8, 65, 2}, PortalAxis::X)
                .has_value());

    // 22x3 too wide (> 21)
    buildObsidianFrame(w, {25, 65, 10}, PortalAxis::X, 22, 3, true);
    assert(!PortalShape::findEmptyPortalShape(w, {25, 65, 10}, PortalAxis::X)
                .has_value());

    // 21x21 max valid portal
    buildObsidianFrame(w, {25, 65, 14}, PortalAxis::X, 21, 21, true);
    auto maxShape =
        PortalShape::findEmptyPortalShape(w, {25, 65, 14}, PortalAxis::X);
    assert(maxShape.has_value());
    assert(maxShape->width() == 21 && maxShape->height() == 21);
  }

  // 4. Fire inside obsidian frame in minecraft:the_end must NOT light a portal.
  {
    World endWorld(0, 256);
    endWorld.setDimension(endDimensionType(), "minecraft:the_end");
    endWorld.loadChunk(0, 0);
    buildObsidianFrame(endWorld, {5, 65, 5}, PortalAxis::X, 2, 3, true);
    assert(endWorld.setBlock({5, 65, 5}, Blocks::FIRE.defaultState(), 3));
    assert(endWorld.getBlock({5, 65, 5}).is(Blocks::FIRE));
    assert(!endWorld.getBlock({4, 65, 5}).is(Blocks::NETHER_PORTAL));
  }
}

void testPortalForcerSearchAndCreation() {
  // 1. Closest portal selection: horizontal distSqr ASC, then lowest Y ASC,
  // plus Nether (16) vs Overworld (128) search radius & border filtering.
  {
    World w(0, 256);
    w.setDimension(netherDimensionType(), "minecraft:the_nether");
    for (int cx = 0; cx <= 2; ++cx)
      w.loadChunk(cx, 0);

    // Build two portals at same X/Z column with equal distance squared (9) from y=75:
    // upper portal blocks y=78..80 (dy=+3) vs lower portal blocks y=70..72 (dy=-3).
    // Vanilla ties break by lowest Y -> {10, 72, 5}.
    buildObsidianFrame(w, {10, 78, 5}, PortalAxis::X, 2, 3, true);
    w.setBlock({10, 78, 5}, Blocks::FIRE.defaultState(), 3);
    buildObsidianFrame(w, {10, 70, 5}, PortalAxis::X, 2, 3, true);
    w.setBlock({10, 70, 5}, Blocks::FIRE.defaultState(), 3);

    // Also build a farther portal at (5, 75, 5) for radius tests.
    buildObsidianFrame(w, {5, 75, 5}, PortalAxis::X, 2, 3, true);
    w.setBlock({5, 75, 5}, Blocks::FIRE.defaultState(), 3);

    PortalForcer forcer(w);
    auto closest = forcer.findClosestPortalPosition({10, 75, 5}, true);
    assert(closest.has_value());
    assert(*closest == (Pos{10, 72, 5}));

    // Radius 16 misses portal > 16 blocks away, while Overworld radius 128 finds it.
    auto outOfNetherRadius = forcer.findClosestPortalPosition({30, 75, 5}, true);
    assert(!outOfNetherRadius.has_value());
    auto inOverworldRadius = forcer.findClosestPortalPosition({30, 75, 5}, false);
    assert(inOverworldRadius.has_value());
  }

  // 2. PortalForcer::createPortal prefers full -1/+1 clearance over partial,
  // and falls back to Y=70 obsidian platform when terrain is empty loaded chunks.
  {
    World w;
    for (int cx = -1; cx <= 1; ++cx)
      for (int cz = -1; cz <= 1; ++cz)
        w.loadChunk(cx, cz);

    // Loaded air chunks with no ground -> fallback obsidian platform at Y=70.
    PortalForcer forcer(w);
    auto created = forcer.createPortal({2, 40, 2}, PortalAxis::X);
    assert(created.has_value());
    assert(created->axis1Size == 2 && created->axis2Size == 3);
    assert(created->minCorner.y == 70);
    for (int dx = 0; dx < 2; ++dx)
      for (int dy = 0; dy < 3; ++dy)
        assert(w.getBlock({created->minCorner.x + dx, 70 + dy, created->minCorner.z})
                   .is(Blocks::NETHER_PORTAL));
  }
}

void testUngeneratedDestinationAbortsWithoutPortalOrTeleport() {
  // Case A: Target dimension is completely missing.
  {
    World ow;
    ow.setDimension(overworldDimensionType(), "minecraft:overworld");
    ow.loadChunk(0, 0);
    buildObsidianFrame(ow, {5, 65, 5}, PortalAxis::X, 2, 3, true);
    assert(ow.setBlock({5, 65, 5}, Blocks::FIRE.defaultState(), 3));

    auto item = std::make_shared<ItemEntity>(
        ow, Vec3{4.5, 65.1, 5.5}, itemStack("minecraft:diamond", 1), Vec3{});
    ow.addEntity(item);

    auto noNether = [](std::string_view) -> World * { return nullptr; };
    assert(!teleportEntityThroughPortal(ow, item, {4, 65, 5}, noNether));
    assert(ow.entities.size() == 1);
    assert(!item->isRemoved());
  }

  // Case B: Destination Nether world exists, but target chunks around (0, 0)
  // are UNLOADED / UNGENERATED. Must NOT create a portal and must NOT teleport!
  {
    ServerWorldSet worlds;
    auto owLevel = std::make_unique<DimensionLevel>();
    owLevel->stem = {"minecraft:overworld", "minecraft:overworld",
                     overworldDimensionType()};
    owLevel->world = std::make_unique<World>();
    owLevel->world->setDimension(owLevel->stem.type, owLevel->stem.id);
    owLevel->world->loadChunk(0, 0);

    auto netherLevel = std::make_unique<DimensionLevel>();
    netherLevel->stem = {"minecraft:the_nether", "minecraft:the_nether",
                         netherDimensionType()};
    netherLevel->world = std::make_unique<World>(0, 256);
    netherLevel->world->setDimension(netherLevel->stem.type, netherLevel->stem.id);
    // Intentionally do NOT load chunk (0, 0) in netherLevel!
    // Load only a distant unrelated chunk (50, 50) to verify partial worlds still
    // refuse to teleport into an ungenerated destination region.
    netherLevel->world->loadChunk(50, 50);

    World &ow = *owLevel->world;
    World &nether = *netherLevel->world;
    buildObsidianFrame(ow, {5, 70, 5}, PortalAxis::X, 2, 3, true);
    assert(ow.setBlock({5, 70, 5}, Blocks::FIRE.defaultState(), 3));

    auto item = std::make_shared<ItemEntity>(
        ow, Vec3{4.5, 70.1, 5.5}, itemStack("minecraft:emerald", 3), Vec3{});
    ow.addEntity(item);

    worlds.levels().push_back(std::move(owLevel));
    worlds.levels().push_back(std::move(netherLevel));

    // Tick twice: tick 1 enters portal, tick 2 handlePortal attempts teleport.
    worlds.tick();
    worlds.tick();

    // Destination was ungenerated: entity stays in Overworld, nothing in Nether,
    // no portal POIs created in Nether.
    assert(ow.entities.size() == 1);
    assert(ow.entities[0] == item);
    assert(!item->isRemoved());
    assert(nether.entities.empty());
    assert(nether.pois().countInRange({0, 70, 0}, 64,
                                      poiTypeMask(PoiTypeId::NetherPortal),
                                      PoiOccupancy::Any) == 0);
    assert(!nether.isChunkLoaded(0, 0));
  }
}

std::vector<std::uint8_t> makeFullAirChunkNbt(const ChunkTicketPos &position) {
  auto add = [](nbt::Tag &parent, const std::string &k, nbt::Tag val) {
    parent.compound.emplace(k, std::move(val));
  };
  auto number = [](int type, std::int64_t v) {
    nbt::Tag t;
    t.type = type;
    t.number = v;
    return t;
  };
  auto str = [](const std::string &s) {
    nbt::Tag t;
    t.type = 8;
    t.text = s;
    return t;
  };
  nbt::RootTag root;
  root.name = "Level";
  root.value.type = 10;
  add(root.value, "DataVersion", number(3, 4786));
  add(root.value, "xPos", number(3, position.x));
  add(root.value, "zPos", number(3, position.z));
  add(root.value, "Status", str("minecraft:full"));
  add(root.value, "isLightOn", number(1, 1));
  nbt::Tag air;
  air.type = 10;
  add(air, "Name", str("minecraft:air"));
  nbt::Tag palette;
  palette.type = 9;
  palette.listElementType = 10;
  palette.list.push_back(std::move(air));
  nbt::Tag states;
  states.type = 10;
  add(states, "palette", std::move(palette));
  nbt::Tag section;
  section.type = 10;
  add(section, "Y", number(1, 4));
  add(section, "block_states", std::move(states));
  nbt::Tag sections;
  sections.type = 9;
  sections.listElementType = 10;
  sections.list.push_back(std::move(section));
  add(root.value, "sections", std::move(sections));
  return nbt::writeRoot(root);
}

struct InMemoryTestChunkStore final : RawChunkStore {
  StoreReadResult readChunk(const ChunkTicketPos &position) override {
    if (position.x >= -3 && position.x <= 3 && position.z >= -2 &&
        position.z <= 4) {
      return {StoreReadStatus::Found, makeFullAirChunkNbt(position), {},
              std::nullopt};
    }
    return {StoreReadStatus::Missing, {}, "not generated", std::nullopt};
  }
  StoreWriteResult writeChunk(const ChunkTicketPos &,
                              std::span<const std::uint8_t>, int) override {
    return {true, false, {}};
  }
};

void testCrossDimensionTeleportScaleTicketAndCooldown() {
  // Full 2-dimension setup via ServerWorldSet + RawChunkStore-backed WorldChunkManager
  // on Nether to verify TicketType::Portal placement and 1:8 / 8:1 teleportation.
  ServerWorldSet worlds;

  auto owLevel = std::make_unique<DimensionLevel>();
  owLevel->stem = {"minecraft:overworld", "minecraft:overworld",
                   overworldDimensionType()};
  owLevel->world = std::make_unique<World>();
  owLevel->world->setDimension(owLevel->stem.type, owLevel->stem.id);
  for (int cx = 4; cx <= 6; ++cx)
    for (int cz = 9; cz <= 11; ++cz)
      owLevel->world->loadChunk(cx, cz);

  auto netherLevel = std::make_unique<DimensionLevel>();
  netherLevel->stem = {"minecraft:the_nether", "minecraft:the_nether",
                       netherDimensionType()};
  netherLevel->world = std::make_unique<World>(0, 256);
  netherLevel->world->setDimension(netherLevel->stem.type, netherLevel->stem.id);
  auto netherStore = std::make_shared<InMemoryTestChunkStore>();
  netherLevel->chunkManager = std::make_unique<WorldChunkManager>(
      *netherLevel->world, netherStore, 3, 10,
      [](const ChunkTicketPos &) -> EntityChunkDataResult {
        return {EntityChunkDataResult::Status::Empty, {}};
      });

  // Load 5x5+ chunk area around Nether destination (chunk (0, 1) -> block (10, 70, 20))
  // so WorldChunkManager fullRing(2) entity ticking readiness is satisfied.
  for (int cx = -2; cx <= 2; ++cx) {
    for (int cz = -1; cz <= 3; ++cz) {
      netherLevel->chunkManager->setForced({cx, cz}, true);
    }
  }
  bool allLoaded = false;
  for (int iter = 0; iter < 500; ++iter) {
    netherLevel->chunkManager->pump();
    allLoaded = true;
    for (int cx = -2; cx <= 2; ++cx)
      for (int cz = -1; cz <= 3; ++cz)
        if (!netherLevel->world->isChunkLoaded(cx, cz))
          allLoaded = false;
    if (allLoaded)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(allLoaded);

  World &ow = *owLevel->world;
  World &nether = *netherLevel->world;
  buildFloor(ow, 78, 83, 69, 158, 162, Blocks::STONE.defaultState());
  buildObsidianFrame(ow, {81, 70, 160}, PortalAxis::X, 2, 3, true);
  assert(ow.setBlock({81, 70, 160}, Blocks::FIRE.defaultState(), 3));

  buildFloor(nether, 5, 16, 69, 15, 25, Blocks::NETHERRACK.defaultState());

  auto item = std::make_shared<ItemEntity>(
      ow, Vec3{80.5, 70.1, 160.5}, itemStack("minecraft:gold_ingot", 4),
      Vec3{0.2, 0.0, 0.0});
  ow.addEntity(item);
  const auto uuid = item->uuid;
  assert((uuid != std::array<std::int32_t, 4>{}));

  WorldChunkManager *netherMgr = netherLevel->chunkManager.get();
  worlds.levels().push_back(std::move(owLevel));
  worlds.levels().push_back(std::move(netherLevel));

  // Tick 1: item overlaps Overworld portal -> marks insidePortalThisTick.
  worlds.tick();
  assert(ow.entities.size() == 1);
  assert(nether.entities.empty());

  // Tick 2: handlePortal sees insidePortalThisTick -> queues teleport -> creates
  // exit portal near Nether (10, 70, 20) via 1:8 coordinate scale!
  worlds.tick();
  assert(ow.entities.empty());
  assert(nether.entities.size() == 1);
  assert(nether.entities[0] == item);
  assert(!item->isRemoved());
  assert(item->uuid == uuid);
  assert(item->deltaMovement.lengthSqr() == 0.0);
  assert(item->portalCooldown == 300);

  // Expected Nether exit around (10, 70, 20) (inside 16 blocks of 80/8, 160/8).
  assert(std::abs(item->position.x - 10.0) <= 18.0);
  assert(std::abs(item->position.z - 20.0) <= 18.0);
  assert(std::abs(item->position.y - 70.0) <= 4.0);
  assert(nether.pois().countInRange({10, 70, 20}, 20,
                                    poiTypeMask(PoiTypeId::NetherPortal),
                                    PoiOccupancy::Any) == 6);

  // Verify Portal ticket was installed on Nether destination chunk.
  const ChunkTicketPos exitChunk{static_cast<int>(std::floor(item->position.x)) >> 4,
                                 static_cast<int>(std::floor(item->position.z)) >> 4};
  const auto demand = netherMgr->ticketSources().query(exitChunk);
  assert(demand.loadingLevel <= ticket_levels::FullChunkLevel - 3);

  // Standing inside the Nether portal resets cooldown instead of bouncing back!
  worlds.tick();
  assert(nether.entities.size() == 1);
  assert(ow.entities.empty());
  assert(item->portalCooldown == 300);

  // Manually move outside portal, cool down completely, then return to Overworld!
  const Pos portalPos{static_cast<int>(std::floor(item->position.x)),
                      static_cast<int>(std::floor(item->position.y)),
                      static_cast<int>(std::floor(item->position.z))};
  item->setPos(Vec3{portalPos.x + 0.5, portalPos.y + 0.1, portalPos.z + 3.0});
  item->deltaMovement = {};
  for (int t = 0; t < 305; ++t) {
    worlds.tick();
  }
  assert(item->portalCooldown == 0);
  assert(nether.entities.size() == 1);

  // Step back into the Nether portal -> links to existing Overworld portal (8:1)!
  item->setPos(Vec3{portalPos.x + 0.5, portalPos.y + 0.1, portalPos.z + 0.5});
  worlds.tick(); // enter
  worlds.tick(); // teleport back
  assert(nether.entities.empty());
  assert(ow.entities.size() == 1);
  assert(std::abs(item->position.x - 80.5) <= 2.0);
  assert(std::abs(item->position.z - 160.5) <= 2.0);
  assert(item->portalCooldown == 300);
}

void testPrimedTntCrossDimensionTeleport() {
  ServerWorldSet worlds;
  auto owLevel = std::make_unique<DimensionLevel>();
  owLevel->stem = {"minecraft:overworld", "minecraft:overworld",
                   overworldDimensionType()};
  owLevel->world = std::make_unique<World>();
  owLevel->world->setDimension(owLevel->stem.type, owLevel->stem.id);
  owLevel->world->loadChunk(0, 0);

  auto netherLevel = std::make_unique<DimensionLevel>();
  netherLevel->stem = {"minecraft:the_nether", "minecraft:the_nether",
                       netherDimensionType()};
  netherLevel->world = std::make_unique<World>(0, 256);
  netherLevel->world->setDimension(netherLevel->stem.type, netherLevel->stem.id);
  netherLevel->world->loadChunk(0, 0);

  World &ow = *owLevel->world;
  World &nether = *netherLevel->world;

  buildObsidianFrame(ow, {8, 70, 8}, PortalAxis::X, 2, 3, true);
  assert(ow.setBlock({8, 70, 8}, Blocks::FIRE.defaultState(), 3));

  buildObsidianFrame(nether, {2, 70, 2}, PortalAxis::Z, 2, 3, true);
  assert(nether.setBlock({2, 70, 2}, Blocks::FIRE.defaultState(), 3));

  auto tnt = std::make_shared<PrimedTnt>(ow, Vec3{7.5, 70.1, 8.5}, Vec3{});
  tnt->yaw = 15.0f;
  tnt->fuse = 40;
  ow.addEntity(tnt);

  worlds.levels().push_back(std::move(owLevel));
  worlds.levels().push_back(std::move(netherLevel));

  worlds.tick();
  worlds.tick();
  assert(ow.entities.empty());
  assert(nether.entities.size() == 1);
  assert(nether.entities[0] == tnt);
  // Source portal is axis=x, exit portal is axis=z -> 90 degree yaw rotation!
  assert(std::abs(tnt->yaw - 105.0f) < 1e-4f);
  assert(tnt->portalCooldown == 300);
}

} // namespace

int main() {
  testBlockPropertiesAndShapes();
  testPortalShapeBoundsAndIgnition();
  testPortalForcerSearchAndCreation();
  testUngeneratedDestinationAbortsWithoutPortalOrTeleport();
  testCrossDimensionTeleportScaleTicketAndCooldown();
  testPrimedTntCrossDimensionTeleport();
  std::cout << "All portal tests passed.\n";
  return 0;
}
