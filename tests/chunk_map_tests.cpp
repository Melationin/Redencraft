#include "redstone/engine.hpp"
#include "redstone/worldgen.hpp"
#include "redstone/worldgen/noise_chunk_generator.hpp"

#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace redstone;
using namespace redstone::worldgen;

void require(bool cond, const char *msg) {
  if (!cond) {
    std::cerr << "chunk_map_tests failure: " << msg << "\n";
    assert(false && "chunk_map_tests assertion failed");
    std::abort();
  }
}

void testHolderTransitionsAndInvalidationFlag() {
  ChunkMap map;
  const ChunkTicketPos pos{4, -7};
  auto holder = map.getOrCreateHolder(pos);
  require(holder != nullptr, "getOrCreateHolder returns non-null");
  require(holder->heldKind() == HeldChunkKind::Empty && holder->empty(),
          "new holder starts Empty");
  require(map.holderCount() == 1, "holderCount == 1");

  auto proto1 = std::make_shared<ProtoChunk>(pos, -64, 384);
  require(!proto1->isInvalidated(), "fresh proto is valid");
  holder->setProto(proto1);
  require(holder->heldKind() == HeldChunkKind::Proto && !holder->empty(),
          "holder transitions Empty -> Proto");
  require(proto1->isProtoChunk() && !proto1->isWorldChunk(), "BaseChunk kind is Proto");
  require(proto1->resizeMutex == &holder->chunkMutex(),
          "proto resizeMutex bound to ChunkHolder mutex");
  require(chunkPersistedStatus(*proto1) == ChunkStatusId::Empty,
          "chunkPersistedStatus works non-virtually on ProtoChunk");

  auto proto2 = std::make_shared<ProtoChunk>(pos, -64, 384);
  holder->setProto(proto2);
  require(proto1->isInvalidated(), "replaced proto1 is marked invalidated");
  require(!proto2->isInvalidated(), "active proto2 is valid");
  require(proto1->resizeMutex == nullptr, "replaced proto1 detached mutex");

  holder->resetChunk();
  require(proto2->isInvalidated(), "resetChunk invalidates proto2");
  require(holder->heldKind() == HeldChunkKind::Empty && holder->empty(),
          "holder returns to Empty after resetChunk");

  require(map.evictChunk(pos.x, pos.z), "evictChunk erases holder");
  require(map.holderCount() == 0 && map.findHolder(pos) == nullptr,
          "holder removed from holders_ after evict");
}

void testProtoToWorldPromotionAndZeroRescanHeightmaps() {
  World world(-64, 320);
  ChunkMap &map = world.chunkMap();
  const ChunkTicketPos pos{1, -2};

  auto proto = std::make_shared<ProtoChunk>(pos, -64, 384);
  proto->setPersistedStatus(ChunkStatusId::Full);
  proto->setDefaultBiome("minecraft:plains");
  require(proto->setBlockState(16, 70, -32, BlockState("minecraft:stone")).has_value(),
          "proto accepts stone block");
  require(proto->setBlockState(17, 72, -31, BlockState("minecraft:oak_leaves")).has_value(),
          "proto accepts oak_leaves block");
  require(proto->setBlockState(18, 60, -30, BlockState("minecraft:water")).has_value(),
          "proto accepts water block");
  proto->setNoiseBiome(4, 16, -8, "minecraft:forest");
  proto->markPosForPostprocessing(18, 60, -30);

  terrain::SavedTick bTick{"minecraft:stone", 16, 70, -32, 5, 0};
  terrain::SavedTick fTick{"minecraft:water", 18, 60, -30, 1, -1};
  proto->blockTicks().push_back(bTick);
  proto->fluidTicks().push_back(fTick);

  proto->setLightCorrect(true);
  proto->setLightSubmissionGeneration(42);
  const int secIdx = proto->sectionIndex(64);
  proto->section(secIdx).blockLight.emplace().fill(0xAB);

  // Set sentinel heightmap values via ProtoChunk::getOrCreateHeightmapUnchecked semantics:
  require(proto->getHeight(HeightmapType::WorldSurface, 0, 0) == 70,
          "proto WorldSurface at (0,0) == 70");
  require(proto->getHeight(HeightmapType::MotionBlocking, 1, 1) == 72,
          "proto MotionBlocking at (1,1) == 72");
  require(proto->getHeight(HeightmapType::MotionBlockingNoLeaves, 1, 1) < 72,
          "proto MotionBlockingNoLeaves ignores oak_leaves");

  auto holder = map.getOrCreateHolder(pos);
  holder->setProto(proto);

  std::vector<Pos> pp;
  std::vector<terrain::SavedTick> bt;
  std::vector<terrain::SavedTick> ft;
  std::vector<terrain::MovingPistonEntity> mp;
  std::vector<nbt::Tag> ent;
  auto wc = holder->promoteProtoToWorld(-64, 320, &pp, &bt, &ft, &mp, &ent);
  require(wc != nullptr, "promoteProtoToWorld returns WorldChunk");
  require(proto->isInvalidated(), "promoted ProtoChunk marked invalidated");
  require(!wc->isInvalidated(), "new WorldChunk is valid");
  require(holder->heldKind() == HeldChunkKind::World,
          "holder switched union to HeldChunkKind::World");
  require(map.findWorldChunk(pos.x, pos.z) == wc.get(),
          "main-thread worldIndex_ points to promoted WorldChunk");
  require(wc->requiredLightGeneration == 42 && wc->isLightCorrect(),
          "light generation & lightCorrect transferred");
  require(!wc->sections[static_cast<std::size_t>(secIdx)].blockLight.has_value(),
          "live WorldChunk clears ProtoChunk section blockLight");
  require(wc->persistedLight.has_value() && wc->persistedLight->sections.size() == 1,
          "persistedLight preserved serialized section light");
  require(wc->getHeight(HeightmapType::WorldSurface, 0, 0) == 70,
          "WorldChunk adopted WorldSurface heightmap without rescan");
  require(wc->getHeight(HeightmapType::MotionBlockingNoLeaves, 1, 1) < 72,
          "WorldChunk adopted MotionBlockingNoLeaves without rescan");
  require(wc->getNoiseBiome(4, 16, -8) == "minecraft:forest",
          "WorldChunk inherited custom biome");
  require(pp.size() == 1 && pp[0] == Pos{18, 60, -30},
          "postProcessing pos unpacked");
  require(bt.size() == 1 && ft.size() == 1, "saved ticks extracted");
}

void testWorldCacheInvalidationAndOfflineUnload() {
  World world(-64, 320);
  world.loadChunk(2, -3);
  const Pos stonePos{32, 64, -48};
  const Pos wheatPos{33, 64, -48};
  require(world.setBlock(stonePos, BlockState("minecraft:stone")),
          "set stone in loaded chunk");
  require(world.setBlock(wheatPos, BlockState("minecraft:wheat")),
          "set random-ticking wheat in loaded chunk");

  // Warm World::blockReadCache_ on stonePos
  require(world.getBlock(stonePos) == BlockState("minecraft:stone"),
          "warm blockReadCache_ reads stone");
  WorldChunk *wc = world.chunkMap().findWorldChunk(2, -3);
  require(wc != nullptr && wc->loaded, "worldIndex_ has loaded WorldChunk");
  require(wc->randomTickIndex != WorldChunk::kInvalidRandomTickIndex,
          "chunk registered in randomTickingChunks_");

  // Offline soft unload preserves WorldChunk inside ChunkHolder while invalidating caches.
  world.unloadChunk(2, -3);
  require(!world.isChunkLoaded(2, -3), "unloaded chunk is not in worldIndex_");
  require(world.chunkMap().findWorldChunk(2, -3) == nullptr,
          "worldIndex_ cleared on unloadChunk");
  require(!wc->isInvalidated() && !wc->loaded,
          "soft-unloaded WorldChunk remains valid inside ChunkHolder");
  require(wc->randomTickIndex == WorldChunk::kInvalidRandomTickIndex,
          "randomTickingChunks_ removed chunk on unloadChunk");
  require(world.getBlock(stonePos).isAir(),
          "blockReadCache_ invalidated on unloadChunk (reads air)");

  // Reloading offline chunk restores WorldChunk, blocks, and randomTickIndex.
  world.loadChunk(2, -3);
  require(world.chunkMap().findWorldChunk(2, -3) == wc,
          "loadChunk restores same WorldChunk into worldIndex_");
  require(wc->loaded && !wc->isInvalidated(), "reloaded chunk active");
  require(world.getBlock(stonePos) == BlockState("minecraft:stone"),
          "reloaded chunk preserves block data");
  require(wc->randomTickIndex != WorldChunk::kInvalidRandomTickIndex,
          "reloaded chunk re-registered in randomTickingChunks_");

  // Full eviction via ChunkMap marks WorldChunk invalidated and wipes caches.
  require(world.chunkMap().evictChunk(2, -3), "evictChunk removes chunk");
  require(wc->isInvalidated(), "evictChunk marks WorldChunk invalidated");
  require(world.chunkMap().findWorldChunk(2, -3) == nullptr,
          "evictChunk removes chunk from worldIndex_");
  require(world.chunkMap().findHolder(ChunkTicketPos{2, -3}) == nullptr,
          "evictChunk removes ChunkHolder from holders_");
  require(world.getBlock(stonePos).isAir(),
          "blockReadCache_ invalidated on evictChunk");
}

void testConcurrentDirectReadsWithPromotionAndEviction() {
  World world(-64, 320);
  LightWorkerOptions opts;
  opts.batchWindowMs = 0;
  world.enableLighting(opts);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        for (int cx = 0; cx < 4; ++cx) {
          for (int cz = 0; cz < 4; ++cz) {
            const LightSectionView view =
                ChunkMap::directReadSectionCallback(&world.chunkMap(), cx, cz, 4);
            if (!view.empty) {
              volatile std::size_t sz = view.palette.size();
              (void)sz;
            }
            if (auto h = world.chunkMap().findHolder(ChunkTicketPos{cx, cz})) {
              if (auto ch = h->chunkShared()) {
                volatile bool inv = ch->isInvalidated();
                (void)inv;
              }
            }
            reads.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  for (int iter = 0; iter < 60; ++iter) {
    const int cx = iter & 3;
    const int cz = (iter >> 2) & 3;
    auto proto = std::make_shared<ProtoChunk>(ChunkTicketPos{cx, cz}, -64, 384);
    proto->setBlockState((cx << 4) + 3, 64, (cz << 4) + 5,
                         BlockState("minecraft:glowstone"));
    auto holder = world.chunkMap().getOrCreateHolder(ChunkTicketPos{cx, cz});
    holder->setProto(proto);
    require(world.installGeneratedProtoChunk(proto),
            "installGeneratedProtoChunk promotes concurrently");
    require(proto->isInvalidated(), "old ProtoChunk invalidated after install");
    require(world.setBlock({(cx << 4) + 4, 64, (cz << 4) + 5},
                           BlockState("minecraft:stone")),
            "main-thread setBlock after concurrent install");
    world.chunkMap().evictChunk(cx, cz);
    const LightSectionView evictedView =
        world.chunkMap().directReadSection(cx, cz, 4);
    require(evictedView.empty, "evicted chunk directReadSection returns empty");
  }

  stop.store(true, std::memory_order_release);
  for (auto &th : readers)
    th.join();
  require(reads.load(std::memory_order_relaxed) > 0, "reader threads ran");
}

} // namespace

int main() {
  testHolderTransitionsAndInvalidationFlag();
  testProtoToWorldPromotionAndZeroRescanHeightmaps();
  testWorldCacheInvalidationAndOfflineUnload();
  testConcurrentDirectReadsWithPromotionAndEviction();
  return 0;
}
