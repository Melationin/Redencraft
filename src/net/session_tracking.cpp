#include "net/session_internal.hpp"

namespace redstone::net {
void Session::blockUpdate(Pos p, const BlockState &) {
  if (phase_ == Phase::Play && hasChunk(p))
    changedBlocks_.push_back({static_cast<std::uint64_t>(packSectionPos(p)),
                              static_cast<std::uint32_t>(changedBlocks_.size()),
                              packSectionLocal(p)});
}
void Session::flushBlockUpdates() {
  if (changedBlocks_.empty())
    return;
  // ChunkHolder.broadcastChanges accumulates one SectionPos -> ShortSet map and
  // emits a single block_update when a section only has one change, otherwise
  // one section_blocks_update. The states are read from the current section at
  // broadcast time, like ClientboundSectionBlocksUpdatePacket's constructor.
  auto changes = std::exchange(changedBlocks_, {});
  // TODO: vanilla's cross-chunk order depends on ChunkHolder identity hashes.
  // Use coordinate order across chunks and vanilla's bottom-up order within each.
  std::sort(changes.begin(), changes.end(), [](const auto &a, const auto &b) {
    constexpr auto signs = (1ull << 63) | (1ull << 41) | (1ull << 19);
    if (a.section != b.section)
      return (a.section ^ signs) < (b.section ^ signs);
    return a.order < b.order;
  });
  SectionChanges locals;
  for (std::size_t i = 0; i < changes.size();) {
    const auto section = static_cast<std::int64_t>(changes[i].section);
    std::size_t j = i + 1;
    while (j < changes.size() && changes[j].section == changes[i].section)
      ++j;
    const auto first = changes[i].local;
    const Pos firstPos = unpackSectionBlock(section, first);
    if (j - i == 1) {
      sendBlockUpdate(firstPos);
      i = j;
      continue;
    }
    // Every position in a section belongs to the same chunk.
    if (hasChunk(firstPos)) {
      locals.reset();
      for (auto k = i; k < j; ++k)
        locals.add(changes[k].local);
      if (locals.size() == 1) {
        sendBlockUpdate(firstPos);
        i = j;
        continue;
      }
      Writer out;
      out.i64(section);
      out.varInt(locals.size());
      try {
        locals.forEach([&](std::uint16_t local) {
          const auto stateId = data_.registry.stateId(
              world_->getBlock(unpackSectionBlock(section, local)));
          out.varLong((static_cast<std::int64_t>(stateId) << 12) | local);
        });
      } catch (const ProtocolError &) {
        disconnect("Unsupported block state for protocol 775");
        return;
      }
      send("section_blocks_update", std::move(out));
    }
    i = j;
  }
  // Reuse capacity unless a callback recorded another change during broadcast.
  if (changedBlocks_.empty()) {
    changes.clear();
    changedBlocks_.swap(changes);
  }
}
void Session::sendBlockUpdate(Pos p) {
  if (phase_ != Phase::Play || !hasChunk(p))
    return;
  try {
    Writer out;
    out.position(p);
    out.varInt(data_.registry.stateId(world_->getBlock(p)));
    send("block_update", std::move(out));
  } catch (const ProtocolError &) {
    disconnect("Unsupported block state for protocol 775");
  }
}
void Session::blockEvent(const BlockEvent &event) {
  const Vec3 origin{double(event.pos.x), double(event.pos.y), double(event.pos.z)};
  if (phase_ != Phase::Play || (position_ - origin).lengthSqr() >= 64.0 * 64.0)
    return;
  const auto block = data_.blockIds.find(event.block.str());
  if (block == data_.blockIds.end()) {
    disconnect("Unsupported block event for protocol 775");
    return;
  }
  Writer out;
  out.position(event.pos);
  out.u8(static_cast<std::uint8_t>(event.type));
  out.u8(static_cast<std::uint8_t>(event.data));
  out.varInt(block->second);
  send("block_event", std::move(out));
}
void Session::explosion(Vec3 center, float radius, int blockCount) {
  if (phase_ != Phase::Play || (position_ - center).lengthSqr() >= 64.0 * 64.0)
    return;
  const auto &ids = data_.explosionEffectIds;
  Writer out;
  out.f64(center.x);
  out.f64(center.y);
  out.f64(center.z);
  out.f32(radius);
  out.i32(blockCount);
  // TODO: protocol players are not simulated entities and have no server-side
  // explosion knockback yet.
  out.bool_(false);
  out.varInt(ids.at(radius < 2.0f ? "explosion" : "explosion_emitter"));
  out.varInt(ids.at("sound") + 1); // Registry holder, 0 reserved for inline.
  out.varInt(2);
  // Level.DEFAULT_EXPLOSION_BLOCK_PARTICLES, each with weight 1.
  out.varInt(ids.at("poof"));
  out.f32(0.5f);
  out.f32(1.0f);
  out.varInt(1);
  out.varInt(ids.at("smoke"));
  out.f32(1.0f);
  out.f32(1.0f);
  out.varInt(1);
  send("explode", std::move(out));
}
void Session::fallingBlockLanded(int entityId, Pos pos, const BlockState &) {
  // FallingBlockEntity.sendToTrackingPlayers precedes discard. Ordinary chunk
  // dirty broadcasts remain in their existing earlier tick phase.
  if (trackedEntities_.contains(entityId))
    sendBlockUpdate(pos);
}

bool Session::hasManagedChunk(int chunkX, int chunkZ) const {
  return availableChunks_.contains({chunkX, chunkZ});
}

bool Session::hasChunk(Pos p) const {
  return sentChunks_.contains({chunkCoord(p.x), chunkCoord(p.z)});
}
bool Session::chunkVisible(std::pair<int, int> chunk) const {
  if (!chunkCenter_)
    return false;
  return std::abs(chunk.first - chunkCenter_->first) <= options_.viewDistance &&
         std::abs(chunk.second - chunkCenter_->second) <= options_.viewDistance;
}
void Session::queuePendingChunk(std::pair<int, int> chunk) {
  if (sentChunks_.contains(chunk) || !availableChunks_.contains(chunk) ||
      !chunkVisible(chunk) ||
      !world_->isChunkLoaded(chunk.first, chunk.second) ||
      std::find(pendingChunks_.begin(), pendingChunks_.end(), chunk) !=
          pendingChunks_.end())
    return;
  pendingChunks_.push_back(chunk);
}
void Session::expireChunk(std::pair<int, int> chunk) {
  const auto token = chunkGenerations_.find(chunk);
  if (token != chunkGenerations_.end())
    token->second->store(false, std::memory_order_release);
  chunkGenerations_.erase(chunk);
  sentChunkBlockLight_.erase(chunk);
  sentChunkSkyLight_.erase(chunk);
  awaitingLightChunks_.erase(chunk);
}
void Session::refreshChunkView() {
  const std::pair center{chunkCoord(position_.x), chunkCoord(position_.z)};
  if (chunkCenter_ == center)
    return;
  chunkCenter_ = center;
  Writer out;
  out.varInt(center.first);
  out.varInt(center.second);
  send("set_chunk_cache_center", out);
  const auto visible = [&](std::pair<int, int> p) {
    return std::abs(p.first - center.first) <= options_.viewDistance &&
           std::abs(p.second - center.second) <= options_.viewDistance;
  };
  for (auto it = sentChunks_.begin(); it != sentChunks_.end();) {
    if (visible(*it)) {
      ++it;
      continue;
    }
    out = {};
    // FriendlyByteBuf.writeChunkPos = ChunkPos.toLong, x in the low bits.
    out.i32(it->second);
    out.i32(it->first);
    send("forget_level_chunk", out);
    expireChunk(*it);
    it = sentChunks_.erase(it);
  }
  pendingChunks_.clear();
  awaitingLightChunks_.clear();
  for (auto p : availableChunks_)
    if (visible(p) && !sentChunks_.contains(p) &&
        world_->isChunkLoaded(p.first, p.second))
      pendingChunks_.push_back(p);
  std::sort(pendingChunks_.begin(), pendingChunks_.end(), [&](auto a, auto b) {
    const auto distance = [&](auto p) {
      const auto dx = p.first - center.first, dz = p.second - center.second;
      return dx * dx + dz * dz;
    };
    if (distance(a) != distance(b))
      return distance(a) < distance(b);
    return a < b;
  });
  // TODO: vanilla rounded tracking-view boundary and client settings radius;
  // this fixed square is only the visualization view, not simulation loading.
}
void Session::sendChunkBatch() {
  if (inSendChunkBatch_ || closed())
    return;
  inSendChunkBatch_ = true;
  struct Guard {
    bool &flag;
    ~Guard() { flag = false; }
  } guard{inSendChunkBatch_};
  // A chunk can join after the worker completed but before the next world tick
  // drains publications. This is a nonblocking ownership handoff, never a wait
  // for propagation, and keeps the join packet order chunk-before-position.
  if (world_->lightingEnabled() && world_->lightService())
    world_->lightService()->drainPublished();
  if ((!options_.unlimitedNetwork && awaitingChunkBatch_) ||
      pendingChunks_.empty() || closed())
    return;
  if (world_->lightingEnabled() && !options_.sendChunksBeforeLightReady) {
    const auto *service = world_->lightService();
    bool ready = false;
    for (const auto chunk : pendingChunks_) {
      const auto block =
          service->latestLayerChunk(LightLayer::Block, chunk.first, chunk.second);
      const auto sky =
          service->latestLayerChunk(LightLayer::Sky, chunk.first, chunk.second);
      if (block && block->protocolReady &&
          (!block->skyLightEnabled || (sky && sky->protocolReady))) {
        ready = true;
        break;
      }
    }
    if (!ready) {
      for (const auto chunk : pendingChunks_)
        if (world_->isChunkLoaded(chunk.first, chunk.second))
          awaitingLightChunks_.insert(chunk);
      pendingChunks_.clear();
      return;
    }
  }
  send("chunk_batch_start");
  const int chunkPacket =
      data_.packet(phaseName(phase_), "clientbound", "level_chunk_with_light");
  int count = 0;
  std::size_t bytes = 0;
  while (!pendingChunks_.empty() &&
         (options_.unlimitedNetwork ||
          (count < chunksPerBatch_ && bytes < 2 * 1024 * 1024))) {
    const auto [x, z] = pendingChunks_.front();
    pendingChunks_.erase(pendingChunks_.begin());
    if (!world_->isChunkLoaded(x, z))
      continue;
    std::shared_ptr<const LightChunkSnapshot> blockLight;
    std::shared_ptr<const LightChunkSnapshot> skyLight;
    if (world_->lightingEnabled()) {
      const auto *service = world_->lightService();
      auto candidateBlock =
          service->latestLayerChunk(LightLayer::Block, x, z);
      auto candidateSky = service->latestLayerChunk(LightLayer::Sky, x, z);
      if (options_.sendChunksBeforeLightReady) {
        if (candidateBlock && candidateBlock->protocolReady)
          blockLight = std::move(candidateBlock);
        if (candidateSky && candidateSky->protocolReady)
          skyLight = std::move(candidateSky);
      } else {
        if (!candidateBlock || !candidateBlock->protocolReady ||
            (candidateBlock->skyLightEnabled &&
             (!candidateSky || !candidateSky->protocolReady))) {
          awaitingLightChunks_.insert({x, z});
          continue;
        }
        blockLight = std::move(candidateBlock);
        skyLight = std::move(candidateSky);
      }
    }
    // Snapshot on the tick thread (the world is not thread safe), then let the
    // writer thread run the palette/bit packing. The batch-start ordering is
    // preserved because queue items are drained in order.
    auto snapshot = prepareChunkSnapshot(world_->snapshotChunk(x, z),
                                         data_.registry, blockLight, skyLight);
    for (const auto &section : snapshot.sections)
      bytes += section.palette.size() * sizeof(BlockState::Id) +
               section.indices8.size() + section.indices16.size() * 2 + 64;
    const auto &registry = data_.registry;
    const auto retainedBytes = snapshot.retainedBytes();
    const auto generation = std::make_shared<std::atomic<bool>>(true);
    // A submitted chunk belongs to the FIFO batch counted by chunk_batch_start
    // and chunk_batch_finished. It must not be cancelled independently. A later
    // forget_level_chunk removes it in order; only dynamic light updates use the
    // lifecycle token for cancellation.
    queueDeferred([snapshot = std::move(snapshot), chunkPacket, x, z,
                   &registry]() {
      return frame(chunkPacket, encodeChunk(snapshot, x, z, registry));
    }, retainedBytes);
    if (closed())
      return;
    sentChunks_.emplace(x, z);
    chunkGenerations_[{x, z}] = generation;
    if (blockLight)
      sentChunkBlockLight_[{x, z}] = lightLayerVersion(*blockLight);
    if (skyLight)
      sentChunkSkyLight_[{x, z}] = lightLayerVersion(*skyLight);
    awaitingLightChunks_.erase({x, z});
    ++count;
  }
  Writer out;
  out.varInt(count);
  send("chunk_batch_finished", out);
  awaitingChunkBatch_ = true;
}
void Session::lightPublished(
    std::shared_ptr<const LightChunkSnapshot> snapshot) {
  try {
    if (closed() || phase_ != Phase::Play || !snapshot ||
        !snapshot->protocolReady || !snapshot->layer)
      return;
    const std::pair key{snapshot->chunkX, snapshot->chunkZ};
    if (awaitingLightChunks_.contains(key)) {
      const auto *service = world_->lightService();
      const auto block = service
                             ? service->latestLayerChunk(LightLayer::Block,
                                                         key.first, key.second)
                             : nullptr;
      const auto sky = service
                           ? service->latestLayerChunk(LightLayer::Sky,
                                                       key.first, key.second)
                           : nullptr;
      if (block && block->protocolReady &&
          (!block->skyLightEnabled || (sky && sky->protocolReady))) {
        awaitingLightChunks_.erase(key);
        queuePendingChunk(key);
      }
      return;
    }
    if (!sentChunks_.contains(key))
      return;
    auto &byLayer = *snapshot->layer == LightLayer::Block
                        ? sentChunkBlockLight_
                        : sentChunkSkyLight_;
    auto previous = byLayer.find(key);
    ProtocolLightUpdate update;
    if (previous == byLayer.end()) {
      if (!options_.sendChunksBeforeLightReady)
        return;
      update = prepareLayerLightUpdate(snapshot, *snapshot->layer, nullptr);
      byLayer.emplace(key, lightLayerVersion(*snapshot));
    } else {
      if (snapshot->revision <= previous->second.revision)
        return;
      if (snapshot->lifecycleEpoch != previous->second.lifecycleEpoch ||
          snapshot->chunkX != previous->second.chunkX ||
          snapshot->chunkZ != previous->second.chunkZ)
        throw ProtocolError("stale light layer lifecycle reached the session");
      update = snapshot->deltaBaseRevision == previous->second.revision
                   ? prepareLayerLightUpdate(snapshot, *snapshot->layer,
                                             &previous->second)
                   : prepareLayerLightUpdate(snapshot, *snapshot->layer);
      previous->second = lightLayerVersion(*snapshot);
    }
    if (update.sky.dataSections.empty() &&
        update.sky.emptySections.empty() &&
        update.block.dataSections.empty() &&
        update.block.emptySections.empty())
      return;
    const auto generation = chunkGenerations_.find(key);
    if (generation == chunkGenerations_.end())
      return;
    const auto token = generation->second;
    const int packetId =
        data_.packet(phaseName(phase_), "clientbound", "light_update");
    const auto retained = update.retainedBytes();
    queueDeferred([update = std::move(update), packetId, token]() {
      if (!token->load(std::memory_order_acquire))
        return Bytes{};
      return frame(packetId, encodeLightUpdate(update));
    }, retained, [token] {
      return token->load(std::memory_order_acquire);
    });
  } catch (const ProtocolError &) {
    disconnect("Light update failed for this connection");
  }
}
void Session::lightChunkLifecycle(int chunkX, int chunkZ, bool loaded) {
  if (closed() || phase_ != Phase::Play)
    return;
  const std::pair key{chunkX, chunkZ};
  if (!loaded) {
    const bool hadSentChunk = sentChunks_.contains(key);
    expireChunk(key);
    std::erase(pendingChunks_, key);
    awaitingLightChunks_.erase(key);
    if (hadSentChunk) {
      sentChunks_.erase(key);
      try {
        Writer out;
        // FriendlyByteBuf.writeChunkPos = ChunkPos.toLong, x low bits.
        out.i32(chunkZ);
        out.i32(chunkX);
        send("forget_level_chunk", out);
      } catch (const ProtocolError &) {
        disconnect("Explicit chunk unload failed for this connection");
      }
    }
    return;
  }
  if (!availableChunks_.contains(key) || !chunkVisible(key) ||
      !world_->isChunkLoaded(chunkX, chunkZ))
    return;
  const auto *service = world_->lightService();
  const auto block = service
                         ? service->latestLayerChunk(LightLayer::Block,
                                                     chunkX, chunkZ)
                         : nullptr;
  const auto sky = service
                       ? service->latestLayerChunk(LightLayer::Sky, chunkX,
                                                   chunkZ)
                       : nullptr;
  if (options_.sendChunksBeforeLightReady ||
      (block && block->protocolReady &&
       (!block->skyLightEnabled || (sky && sky->protocolReady)))) {
    queuePendingChunk(key);
    if (options_.unlimitedNetwork)
      sendChunkBatch();
  } else
    awaitingLightChunks_.insert(key);
}

void Session::setManagedResidencyProvider(
    std::function<bool(int, int)> resident,
    std::function<std::vector<std::pair<int, int>>()> chunks) {
  managedResidency_ = std::move(resident);
  managedChunks_ = std::move(chunks);
  if (closed())
    return;
  if (phase_ != Phase::Play) {
    // Provider installation commonly happens during Handshake. Do not encode
    // Play packets before the configuration transition. The first join() syncs
    // the current resident snapshot.
    return;
  }
  syncManagedResidency();
}

void Session::syncManagedResidency() {
  if (!managedChunks_)
    return;
  std::set<std::pair<int, int>> snapshot;
  for (const auto &chunk : managedChunks_())
    if (!managedResidency_ ||
        managedResidency_(chunk.first, chunk.second))
      snapshot.insert(chunk);
  for (const auto &chunk : managedPendingResidency_)
    snapshot.insert(chunk);
  managedPendingResidency_.clear();
  availableChunks_ = std::move(snapshot);
}

void Session::managedChunkResidency(int chunkX, int chunkZ, bool resident) {
  if (closed())
    return;
  const std::pair key{chunkX, chunkZ};
  if (phase_ != Phase::Play) {
    if (resident)
      managedPendingResidency_.insert(key);
    else
      managedPendingResidency_.erase(key);
    return;
  }
  if (resident) {
    availableChunks_.insert(key);
    if (chunkCenter_ && chunkVisible(key) &&
        world_->isChunkLoaded(chunkX, chunkZ)) {
      queuePendingChunk(key);
      if (options_.unlimitedNetwork)
        sendChunkBatch();
    }
    return;
  }
  availableChunks_.erase(key);
  expireChunk(key);
  std::erase(pendingChunks_, key);
  awaitingLightChunks_.erase(key);
  if (sentChunks_.erase(key) != 0) {
    try {
      Writer out;
      out.i32(chunkZ);
      out.i32(chunkX);
      send("forget_level_chunk", out);
    } catch (const ProtocolError &) {
      disconnect("Explicit chunk unload failed for this connection");
    }
  }
}


void Session::sendEntityDataValues(
    int entityId, std::span<const EntityDataEntry> entries) {
  if (entries.empty())
    return;
  Writer values;
  values.varInt(entityId);
  for (const auto &entry : entries) {
    values.u8(entry.id);
    values.varInt(static_cast<int>(entry.serializer));
    switch (entry.serializer) {
    case EntityDataSerializer::Byte:
      values.u8(static_cast<std::uint8_t>(std::get<std::int8_t>(entry.value)));
      break;
    case EntityDataSerializer::Int:
      values.varInt(std::get<int>(entry.value));
      break;
    case EntityDataSerializer::Long:
      values.varLong(std::get<std::int64_t>(entry.value));
      break;
    case EntityDataSerializer::Float:
      values.f32(std::get<float>(entry.value));
      break;
    case EntityDataSerializer::String:
      values.string(std::get<std::string>(entry.value));
      break;
    case EntityDataSerializer::OptionalComponent: {
      const auto &opt = std::get<std::optional<std::string>>(entry.value);
      values.bool_(opt.has_value());
      if (opt.has_value())
        nbtText(values, *opt);
      break;
    }
    case EntityDataSerializer::ItemStack:
      stack(values, std::get<ItemStack>(entry.value));
      break;
    case EntityDataSerializer::Boolean:
      values.bool_(std::get<bool>(entry.value));
      break;
    case EntityDataSerializer::Rotations: {
      const auto &r = std::get<Rotations3f>(entry.value);
      values.f32(r.x);
      values.f32(r.y);
      values.f32(r.z);
      break;
    }
    case EntityDataSerializer::BlockPos:
      values.position(std::get<Pos>(entry.value));
      break;
    case EntityDataSerializer::OptionalBlockPos: {
      const auto &opt = std::get<std::optional<Pos>>(entry.value);
      values.bool_(opt.has_value());
      if (opt.has_value())
        values.position(*opt);
      break;
    }
    case EntityDataSerializer::Direction:
      values.varInt(static_cast<int>(std::get<Direction>(entry.value)));
      break;
    case EntityDataSerializer::OptionalLivingEntityReference: {
      const auto &opt = std::get<std::optional<EntityUuid>>(entry.value);
      values.bool_(opt.has_value());
      if (opt.has_value()) {
        for (std::int32_t word : *opt)
          values.i32(word);
      }
      break;
    }
    case EntityDataSerializer::BlockState:
      values.varInt(
          data_.registry.stateId(std::get<BlockState>(entry.value)));
      break;
    case EntityDataSerializer::OptionalBlockState: {
      const auto &opt = std::get<std::optional<BlockState>>(entry.value);
      values.varInt(opt.has_value() ? data_.registry.stateId(*opt) : 0);
      break;
    }
    case EntityDataSerializer::OptionalUnsignedInt: {
      const auto &opt = std::get<std::optional<std::uint32_t>>(entry.value);
      values.varInt(opt.has_value() ? static_cast<int>(*opt + 1u) : 0);
      break;
    }
    case EntityDataSerializer::Pose:
      values.varInt(static_cast<int>(std::get<Pose>(entry.value)));
      break;
    case EntityDataSerializer::HumanoidArm:
      values.varInt(std::get<int>(entry.value));
      break;
    case EntityDataSerializer::Vector3: {
      const auto &v = std::get<Vector3f>(entry.value);
      values.f32(v.x);
      values.f32(v.y);
      values.f32(v.z);
      break;
    }
    default:
      throw ProtocolError("unsupported EntityDataSerializer in tracking");
    }
  }
  values.u8(255);
  send("set_entity_data", std::move(values));
}

void Session::sendPassengers(const Entity &e, TrackedEntity &tracked) {
  std::vector<int> passengerIds;
  const auto passengers = e.getPassengers();
  passengerIds.reserve(passengers.size());
  for (const auto &p : passengers) {
    if (p)
      passengerIds.push_back(p->id);
  }
  if (passengerIds == tracked.lastPassengerIds)
    return;
  tracked.lastPassengerIds = std::move(passengerIds);
  Writer out;
  out.varInt(e.id);
  out.varInt(static_cast<int>(tracked.lastPassengerIds.size()));
  for (int pid : tracked.lastPassengerIds)
    out.varInt(pid);
  send("set_passengers", std::move(out));
}

void Session::sendTakeItemEntity(int collectorId, int targetId, int amount) {
  if (phase_ != Phase::Play)
    return;
  try {
    Writer out;
    out.varInt(targetId);
    out.varInt(collectorId);
    out.varInt(amount);
    send("take_item_entity", std::move(out));
  } catch (const ProtocolError &) {
    disconnect("Take item entity packet failed");
  }
}

void Session::sendEntitySpawn(const Entity &e, TrackedEntity &tracked) {
  const auto *player = dynamic_cast<const ServerPlayer *>(&e);
  const Uuid entityUuid =
      player ? offlineUuid(player->username)
             : offlineUuid("entity_" + std::to_string(e.id));
  if (player && !knownPlayerInfo_.contains(e.id)) {
    Writer info;
    info.u8(0x0D); // ADD_PLAYER | UPDATE_GAME_MODE | UPDATE_LISTED
    info.varInt(1);
    uuid(info, entityUuid);
    info.string(player->username);
    info.varInt(0); // empty properties
    info.varInt(player->spectator ? 3 : (player->creative ? 1 : 0));
    info.bool_(true);
    send("player_info_update", std::move(info));
    knownPlayerInfo_[e.id] = entityUuid;
  }
  Writer out;
  out.varInt(e.id);
  uuid(out, entityUuid);
  out.varInt(data_.registry.entityTypeId(e.typeName()));
  out.f64(e.position.x);
  out.f64(e.position.y);
  out.f64(e.position.z);
  lpVector(out, e.deltaMovement);
  out.u8(packDegrees(e.pitch));
  out.u8(packDegrees(e.yaw));
  out.u8(0);
  const auto *falling = dynamic_cast<const FallingBlockEntity *>(&e);
  out.varInt(falling ? data_.registry.stateId(falling->blockState) : 0);
  send("bundle_delimiter");
  send("add_entity", std::move(out));

  // ServerEntity pairs the spawn with the non-default synced data.
  auto initialValues = e.entityData().getNonDefaultValues();
  sendEntityDataValues(e.id, initialValues);
  tracked.lastDataRevision = e.entityData().revision();
  send("bundle_delimiter");
  sendPassengers(e, tracked);

  tracked.base = e.position;
  tracked.lastMovement = e.deltaMovement;
  tracked.lastMotionRevision = e.motionRevision;
  tracked.yRot = packDegrees(e.yaw);
  tracked.xRot = packDegrees(e.pitch);
  tracked.tickCount = 0;
  tracked.teleportDelay = 0;
  tracked.wasOnGround = e.onGround;
}

void Session::sendEntityChanges(const Entity &e, TrackedEntity &tracked) {
  // ServerEntity.sendChanges: movement is encoded as a 1/4096 delta against the
  // last sent position (VecDeltaCodec), rotation as 1/256 turns, and a
  // full entity_position_sync only when the delta overflows a short or the
  // ground state changes.
  sendPassengers(e, tracked);
  const int interval = e.type().updateInterval > 0 ? e.type().updateInterval : 3;
  const auto dirtyData = e.entityData().dirtySince(tracked.lastDataRevision);
  const bool dirty = !dirtyData.empty();
  if (tracked.tickCount % interval == 0 || dirty || e.hurtMarked ||
      e.motionRevision != tracked.lastMotionRevision) {
    tracked.lastMotionRevision = e.motionRevision;
    const auto motionDiff = (e.deltaMovement - tracked.lastMovement).lengthSqr();
    // All currently supported entity types track deltas. Send motion before
    // the position packet, even when that packet is a full position sync.
    if (e.hurtMarked || motionDiff > 1e-7 ||
        (motionDiff > 0 && e.deltaMovement.lengthSqr() == 0)) {
      tracked.lastMovement = e.deltaMovement;
      Writer out;
      out.varInt(e.id);
      lpVector(out, e.deltaMovement);
      send("set_entity_motion", std::move(out));
    }
    const std::uint8_t yRot = packDegrees(e.yaw);
    const std::uint8_t xRot = packDegrees(e.pitch);
    const bool rotationChanged =
        std::abs(int(yRot) - int(tracked.yRot)) >= 1 ||
        std::abs(int(xRot) - int(tracked.xRot)) >= 1;
    const Vec3 offset = e.position - tracked.base;
    const bool positionChanged = offset.lengthSqr() >= 7.6293945e-6;
    const bool position = positionChanged || tracked.tickCount % 60 == 0;
    const auto encode = [](double value) {
      const double scaled = value * 4096.0;
      const double integral = std::floor(scaled);
      return static_cast<std::int64_t>(integral) + (scaled - integral >= 0.5);
    };
    const std::int64_t xa = encode(e.position.x) - encode(tracked.base.x);
    const std::int64_t ya = encode(e.position.y) - encode(tracked.base.y);
    const std::int64_t za = encode(e.position.z) - encode(tracked.base.z);
    const bool deltaTooBig = xa < -32768 || xa > 32767 || ya < -32768 ||
                             ya > 32767 || za < -32768 || za > 32767;
    ++tracked.teleportDelay;
    bool sentPosition = false, sentRotation = false;
    if (deltaTooBig || tracked.teleportDelay > 400 ||
        tracked.wasOnGround != e.onGround) {
      tracked.wasOnGround = e.onGround;
      tracked.teleportDelay = 0;
      Writer out;
      out.varInt(e.id);
      out.f64(e.position.x);
      out.f64(e.position.y);
      out.f64(e.position.z);
      out.f64(e.deltaMovement.x);
      out.f64(e.deltaMovement.y);
      out.f64(e.deltaMovement.z);
      out.f32(e.yaw);
      out.f32(e.pitch);
      out.bool_(e.onGround);
      send("entity_position_sync", std::move(out));
      sentPosition = sentRotation = true;
    } else if (position && rotationChanged) {
      Writer out;
      out.varInt(e.id);
      out.i16(static_cast<std::int16_t>(xa));
      out.i16(static_cast<std::int16_t>(ya));
      out.i16(static_cast<std::int16_t>(za));
      out.u8(yRot);
      out.u8(xRot);
      out.bool_(e.onGround);
      send("move_entity_pos_rot", std::move(out));
      sentPosition = sentRotation = true;
    } else if (position) {
      Writer out;
      out.varInt(e.id);
      out.i16(static_cast<std::int16_t>(xa));
      out.i16(static_cast<std::int16_t>(ya));
      out.i16(static_cast<std::int16_t>(za));
      out.bool_(e.onGround);
      send("move_entity_pos", std::move(out));
      sentPosition = true;
    } else if (rotationChanged) {
      Writer out;
      out.varInt(e.id);
      out.u8(yRot);
      out.u8(xRot);
      out.bool_(e.onGround);
      send("move_entity_rot", std::move(out));
      sentRotation = true;
    }
    if (sentPosition)
      tracked.base = e.position;
    if (sentRotation) {
      tracked.yRot = yRot;
      tracked.xRot = xRot;
    }
    if (dirty) {
      sendEntityDataValues(e.id, dirtyData);
      tracked.lastDataRevision = e.entityData().revision();
    }
  }
  ++tracked.tickCount;
}

void Session::syncEntities() {
  // ChunkMap.TrackedEntity only updates entities inside the player's tracking
  // view; the entity section storage makes that a small per-column scan.
  ++syncGeneration_;
  std::vector<Entity *> candidates;
  if (chunkCenter_) {
    const int range = options_.viewDistance;
    candidates = world_->entitiesInSectionRange(
        chunkCenter_->first - range, chunkCenter_->first + range,
        chunkCenter_->second - range, chunkCenter_->second + range);
  } else {
    candidates.reserve(world_->entities.size());
    for (const auto &ptr : world_->entities)
      candidates.push_back(ptr.get());
  }
  for (Entity *candidate : candidates) {
    if (!candidate)
      continue;
    candidate->syncDataFields();
    const auto &e = *candidate;
    if (e.id == options_.playerId || e.isRemoved() ||
        !world_->isChunkLoaded(chunkCoord(e.position.x),
                              chunkCoord(e.position.z)) ||
        !sentChunks_.contains(
            {chunkCoord(e.position.x), chunkCoord(e.position.z)}))
      continue;
    const auto [tracked, inserted] = trackedEntities_.try_emplace(e.id);
    if (inserted) {
      sendEntitySpawn(e, tracked->second);
      tracked->second.lastSeenGeneration = syncGeneration_;
    } else {
      tracked->second.lastSeenGeneration = syncGeneration_;
      sendEntityChanges(e, tracked->second);
    }
  }
  Writer removed;
  int count = 0;
  for (const auto &[id, tracked] : trackedEntities_)
    if (tracked.lastSeenGeneration != syncGeneration_)
      ++count;
  if (count) {
    std::vector<Uuid> removedPlayers;
    removed.varInt(count);
    for (const auto &[id, tracked] : trackedEntities_)
      if (tracked.lastSeenGeneration != syncGeneration_) {
        removed.varInt(id);
        if (auto it = knownPlayerInfo_.find(id); it != knownPlayerInfo_.end()) {
          removedPlayers.push_back(it->second);
          knownPlayerInfo_.erase(it);
        }
      }
    send("remove_entities", std::move(removed));
    if (!removedPlayers.empty()) {
      Writer infoRemove;
      infoRemove.varInt(static_cast<int>(removedPlayers.size()));
      for (const auto &u : removedPlayers)
        uuid(infoRemove, u);
      send("player_info_remove", std::move(infoRemove));
    }
  }
  std::erase_if(trackedEntities_, [&](const auto &entry) {
    return entry.second.lastSeenGeneration != syncGeneration_;
  });
}
void Session::broadcastEntities() {
  if (phase_ != Phase::Play)
    return;
  try {
    syncEntities();
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
void Session::entityAdded(const Entity &entity) {
  if (phase_ != Phase::Play || entity.id == options_.playerId ||
      entity.isRemoved() ||
      !world_->isChunkLoaded(chunkCoord(entity.position.x),
                            chunkCoord(entity.position.z)) ||
      !sentChunks_.contains({chunkCoord(entity.position.x),
                             chunkCoord(entity.position.z)}))
    return;
  try {
    const auto [tracked, inserted] = trackedEntities_.try_emplace(entity.id);
    if (inserted) {
      sendEntitySpawn(entity, tracked->second);
      tracked->second.lastSeenGeneration = syncGeneration_;
    }
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
void Session::entityRemoved(int entityId) {
  if (phase_ != Phase::Play)
    return;
  const auto tracked = trackedEntities_.find(entityId);
  if (tracked == trackedEntities_.end())
    return;
  try {
    Writer out;
    out.varInt(1);
    out.varInt(entityId);
    send("remove_entities", std::move(out));
    trackedEntities_.erase(tracked);
    if (auto it = knownPlayerInfo_.find(entityId);
        it != knownPlayerInfo_.end()) {
      Writer infoRemove;
      infoRemove.varInt(1);
      uuid(infoRemove, it->second);
      send("player_info_remove", std::move(infoRemove));
      knownPlayerInfo_.erase(it);
    }
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
} // namespace redstone::net
