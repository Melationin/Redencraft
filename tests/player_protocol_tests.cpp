#include "redstone/net/session.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace redstone;
using namespace redstone::net;

namespace {

void lpVector(Writer &out, Vec3 value) {
  auto clean = [](double n) {
    return std::isnan(n) ? 0.0
                         : std::clamp(n, -1.7179869183E10, 1.7179869183E10);
  };
  const double x = clean(value.x), y = clean(value.y), z = clean(value.z);
  const double length = std::max({std::abs(x), std::abs(y), std::abs(z)});
  if (length < 3.051944088384301E-5) {
    out.u8(0);
    return;
  }
  const auto scale = static_cast<std::uint64_t>(std::ceil(length));
  const auto pack = [scale](double n) {
    return static_cast<std::uint64_t>(
        std::floor((n / scale * 0.5 + 0.5) * 32766.0 + 0.5));
  };
  const bool partial = scale > 3;
  auto bits = (scale & 3) | (partial ? 4 : 0) | (pack(x) << 3) |
              (pack(y) << 18) | (pack(z) << 33);
  out.u8(static_cast<std::uint8_t>(bits));
  out.u8(static_cast<std::uint8_t>(bits >> 8));
  out.i32(static_cast<std::int32_t>(bits >> 16));
  if (partial)
    out.varInt(static_cast<std::int32_t>(scale >> 2));
}

struct CapturedPacket {
  int id = -1;
  Bytes payload;
};

std::vector<CapturedPacket> drainPackets(Session &session) {
  std::vector<CapturedPacket> packets;
  for (auto &bytes : session.takeOutput()) {
    auto body = popFrame(bytes);
    assert(body && bytes.empty());
    Reader in(*body);
    const int id = in.varInt();
    Bytes payload(body->begin() + static_cast<std::ptrdiff_t>(in.pos),
                  body->end());
    packets.push_back({id, std::move(payload)});
  }
  return packets;
}

void joinPlayer(Session &session, const ProtocolData &data,
                const std::string &name) {
  Writer out;
  out.varInt(775);
  out.string("localhost");
  out.i16(25565);
  out.varInt(2);
  session.receive(frame(0, out.data));

  out = {};
  out.string(name);
  out.raw(Uuid{});
  session.receive(frame(0, out.data));
  (void)drainPackets(session);

  session.receive(frame(3, {}));
  (void)drainPackets(session);

  out = {};
  out.varInt(0);
  session.receive(frame(7, out.data));
  (void)drainPackets(session);

  session.receive(frame(3, {}));
  assert(session.phase() == Phase::Play);
  (void)drainPackets(session);

  out = {};
  out.varInt(1);
  session.receive(
      frame(data.packet("play", "serverbound", "accept_teleportation"),
            out.data));
}

void wireObservers(World &world, std::vector<Session *> &sessions) {
  world.entityAddedObserver = [&](const Entity &entity) {
    for (Session *s : sessions)
      if (!s->closed())
        s->entityAdded(entity);
  };
  world.entityRemovedObserver = [&](int entityId) {
    for (Session *s : sessions)
      if (!s->closed())
        s->entityRemoved(entityId);
  };
  world.entityTrackingBroadcastObserver = [&]() {
    for (Session *s : sessions)
      if (!s->closed())
        s->broadcastEntities();
  };
  world.entityTakenObserver = [&](int collectorId, int targetId, int amount) {
    for (Session *s : sessions)
      if (!s->closed())
        s->sendTakeItemEntity(collectorId, targetId, amount);
  };
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "missing assets directory argument\n";
    return 1;
  }
  const ProtocolData data(argv[1]);
  const int addEntityId = data.packet("play", "clientbound", "add_entity");
  const int playerInfoUpdateId =
      data.packet("play", "clientbound", "player_info_update");
  const int playerInfoRemoveId =
      data.packet("play", "clientbound", "player_info_remove");
  const int takeItemId = data.packet("play", "clientbound", "take_item_entity");
  const int setPassengersId =
      data.packet("play", "clientbound", "set_passengers");
  const int setInventoryId =
      data.packet("play", "clientbound", "set_player_inventory");

  World world;
  for (int cx = -1; cx <= 1; ++cx) {
    for (int cz = -1; cz <= 1; ++cz) {
      world.loadChunk(cx, cz);
      for (int lx = 0; lx < 16; ++lx)
        for (int lz = 0; lz < 16; ++lz)
          world.setBlock({cx * 16 + lx, 63, cz * 16 + lz},
                         defaultState("stone"), 2);
    }
  }
  world.setBlock({6, 64, 6}, defaultState("oak_pressure_plate"), 3);

  std::vector<Session *> activeSessions;
  wireObservers(world, activeSessions);

  ServerOptions optsA;
  optsA.spawn = {4.5, 64.0, 4.5};
  optsA.chunks = {{0, 0}};
  optsA.playerId = world.allocateEntityId();
  optsA.spawnPlayerEntity = true;

  ServerOptions optsB = optsA;
  optsB.playerId = world.allocateEntityId();
  optsB.spawn = {5.5, 64.0, 4.5};

  Session sessionA(world, data, optsA);
  Session sessionB(world, data, optsB);
  activeSessions = {&sessionA, &sessionB};

  joinPlayer(sessionA, data, "Alice");
  assert(sessionA.playerEntity() != nullptr);
  assert(sessionA.playerEntity()->id == optsA.playerId);

  joinPlayer(sessionB, data, "Bob");
  assert(sessionB.playerEntity() != nullptr);
  assert(sessionB.playerEntity()->id == optsB.playerId);

  world.tick();
  sessionA.tick();
  sessionB.tick();

  // Alice must receive player_info_update + add_entity for Bob (NOT Alice).
  auto pktsA = drainPackets(sessionA);
  bool aliceSawBobInfo = false;
  bool aliceSawBobSpawn = false;
  bool aliceSawSelfSpawn = false;
  for (const auto &p : pktsA) {
    if (p.id == playerInfoUpdateId)
      aliceSawBobInfo = true;
    if (p.id == addEntityId) {
      Reader r(p.payload);
      const int eid = r.varInt();
      if (eid == optsB.playerId)
        aliceSawBobSpawn = true;
      if (eid == optsA.playerId)
        aliceSawSelfSpawn = true;
    }
  }
  assert(aliceSawBobInfo && aliceSawBobSpawn && !aliceSawSelfSpawn);

  // Move Alice onto oak pressure plate via move_player_pos_rot.
  {
    Writer mv;
    mv.f64(6.5);
    mv.f64(64.0);
    mv.f64(6.5);
    mv.f32(0.0f);
    mv.f32(0.0f);
    mv.u8(1); // onGround = true
    sessionA.receive(
        frame(data.packet("play", "serverbound", "move_player_pos_rot"), mv.data));
    assert(world.getBlock({6, 64, 6}).getView("powered") == "true");
  }

  // Spawn an ItemEntity near Alice and verify take_item_entity + inventory update.
  {
    auto item = std::make_shared<ItemEntity>(
        world, Vec3{6.5, 64.0, 6.5}, itemStack("minecraft:redstone", 5));
    item->pickupDelay = 0;
    world.addEntity(item);
    const int droppedId = item->id;
    world.tick();
    sessionA.tick();
    auto pkts = drainPackets(sessionA);
    bool gotTake = false;
    bool gotSlot = false;
    for (const auto &p : pkts) {
      if (p.id == takeItemId) {
        Reader r(p.payload);
        assert(r.varInt() == droppedId);
        assert(r.varInt() == optsA.playerId);
        assert(r.varInt() == 5);
        assert(r.empty());
        gotTake = true;
      }
      if (p.id == setInventoryId)
        gotSlot = true;
    }
    assert(gotTake && gotSlot);
  }

  // Interact with a Minecart to ride, verify set_passengers, then sneak to dismount.
  {
    auto cart = std::make_shared<Minecart>(world, Vec3{6.5, 64.0625, 7.5});
    world.addEntity(cart);
    world.tick();
    (void)drainPackets(sessionA);
    (void)drainPackets(sessionB);

    Writer interact;
    interact.varInt(cart->id);
    interact.varInt(0);
    lpVector(interact, Vec3{0.0, 0.5, 0.0});
    interact.bool_(false);
    sessionA.receive(
        frame(data.packet("play", "serverbound", "interact"), interact.data));
    assert(sessionA.playerEntity()->isPassenger());

    world.tick();
    sessionA.tick();
    sessionB.tick();
    auto pktsB = drainPackets(sessionB);
    bool bobSawRider = false;
    for (const auto &p : pktsB) {
      if (p.id == setPassengersId) {
        Reader r(p.payload);
        if (r.varInt() == cart->id && r.varInt() == 1 &&
            r.varInt() == optsA.playerId)
          bobSawRider = true;
      }
    }
    assert(bobSawRider);

    Writer sneak;
    sneak.u8(32); // sneaking bit
    sessionA.receive(
        frame(data.packet("play", "serverbound", "player_input"), sneak.data));
    assert(!sessionA.playerEntity()->isPassenger());
  }

  // Test switchDimension re-attaches playerEntity to targetWorld cleanly.
  {
    World netherWorld(netherDimensionType(), 1000000, "minecraft:the_nether");
    netherWorld.loadChunk(0, 0);
    DimensionStem netherStem{"minecraft:the_nether", "minecraft:the_nether",
                           netherDimensionType()};
    sessionA.switchDimension(netherWorld, netherStem, {8.5, 65.0, 8.5});
    assert(&sessionA.world() == &netherWorld);
    bool inNether = false;
    for (const auto &e : netherWorld.entities)
      if (e.get() == sessionA.playerEntity().get())
        inNether = true;
    assert(inNether);
  }

  // Disconnecting Bob removes Bob's ServerPlayer and notifies sessions tracking Bob.
  {
    World closeWorld;
    closeWorld.loadChunk(0, 0);
    std::vector<Session *> two;
    wireObservers(closeWorld, two);
    ServerOptions s1, s2;
    s1.chunks = s2.chunks = {{0, 0}};
    s1.spawn = s2.spawn = {4.5, 64.0, 4.5};
    s1.playerId = closeWorld.allocateEntityId();
    s2.playerId = closeWorld.allocateEntityId();
    s1.spawnPlayerEntity = s2.spawnPlayerEntity = true;
    Session sa(closeWorld, data, s1);
    Session sb(closeWorld, data, s2);
    two = {&sa, &sb};
    joinPlayer(sa, data, "Observer");
    joinPlayer(sb, data, "Leaver");
    closeWorld.tick();
    (void)drainPackets(sa);

    two.pop_back();
    closeWorld.removeEntity(sb.playerEntity());
    closeWorld.tick();
    auto pkts = drainPackets(sa);
    bool gotInfoRemove = false;
    for (const auto &p : pkts) {
      if (p.id == playerInfoRemoveId)
        gotInfoRemove = true;
    }
    assert(gotInfoRemove);
  }

  return 0;
}
