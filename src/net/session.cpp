#include "net/session_internal.hpp"

namespace redstone::net {
ProtocolData::ProtocolData(const std::filesystem::path &directory)
    : registry(directory) {
  std::ifstream ids(directory / "packet_ids.tsv");
  if (!ids)
    throw ProtocolError("missing packet_ids.tsv");
  for (std::string line; std::getline(ids, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto f = fields(line);
    if (f.size() == 4)
      packetIds.emplace(f[0] + "/" + f[1] + "/" + f[2], std::stoi(f[3]));
  }
  std::ifstream blocks(directory / "blocks.tsv");
  if (!blocks)
    throw ProtocolError("missing blocks.tsv");
  for (std::string line; std::getline(blocks, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto f = fields(line);
    if (f.size() == 2)
      blockIds.emplace(f[1], std::stoi(f[0]));
  }
  std::ifstream registries(directory / "registry_ids.tsv");
  bool overworld = false;
  for (std::string line; std::getline(registries, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto f = fields(line);
    if (f.size() != 3)
      continue;
    if (f[0] == "minecraft:dimension_type") {
      const int id = std::stoi(f[1]);
      const std::string normalized = normalizeDimensionId(f[2]);
      dimensionTypeIds.emplace(normalized, id);
      if (normalized == "minecraft:overworld") {
        dimensionId = id;
        overworld = true;
      }
    }
  }
  if (!overworld)
    throw ProtocolError("missing overworld dimension registry ID");
  std::ifstream effects(directory / "explosion_effects.tsv");
  for (std::string line; std::getline(effects, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto f = fields(line);
    if (f.size() == 2)
      explosionEffectIds.emplace(f[0], std::stoi(f[1]));
  }
  for (const auto *name : {"explosion", "explosion_emitter", "poof", "smoke", "sound"})
    if (!explosionEffectIds.contains(name))
      throw ProtocolError("missing explosion effect ID in explosion_effects.tsv");
  std::ifstream config(directory / "configuration.bin", std::ios::binary);
  if (!config)
    throw ProtocolError(
        "missing configuration.bin; run validation/protocol775/export.ps1");
  Bytes bytes((std::istreambuf_iterator<char>(config)), {});
  if (bytes.size() > 16 * 1024 * 1024)
    throw ProtocolError("configuration data too large");
  const auto registryDataId = packetIds.find(
      key("configuration", "clientbound", "registry_data"));
  if (registryDataId == packetIds.end())
    throw ProtocolError("missing registry_data packet ID");
  bool worldClockRegistryFrame = false;
  bool timelineRegistryFrame = false;
  Reader reader(bytes);
  while (!reader.empty()) {
    int length = reader.i32();
    if (length <= 0 || length > static_cast<int>(maxFrameSize))
      throw ProtocolError("invalid configuration packet length");
    auto body = reader.raw(length);
    Reader packetReader(body);
    int id = packetReader.varInt();
    if (id == registryDataId->second) {
      Reader registry(body.subspan(packetReader.pos));
      const std::string registryName = registry.string();
      if (registryName == "minecraft:timeline") {
        if (timelineRegistryFrame)
          throw ProtocolError("duplicate timeline registry_data frame");
        timelineRegistryFrame = true;
      }
      if (registryName == "minecraft:world_clock") {
        if (worldClockRegistryFrame)
          throw ProtocolError("duplicate world_clock registry_data frame");
        worldClockRegistryFrame = true;
        const int count = registry.varInt();
        if (count != 2)
          throw ProtocolError("world_clock registry must contain two entries");
        std::array<bool, 2> seen{};
        for (int holderId = 0; holderId < count; ++holderId) {
          const std::string entry = registry.string();
          int target = -1;
          if (entry == "minecraft:overworld")
            target = 0;
          else if (entry == "minecraft:the_end")
            target = 1;
          else
            throw ProtocolError("unsupported world_clock registry entry: " +
                                entry);
          if (seen[target])
            throw ProtocolError("duplicate world_clock registry entry: " +
                                entry);
          seen[target] = true;
          // The current official WorldClock.DIRECT_CODEC is an empty compound.
          // Optional<Tag> is present and encoded as TAG_Compound followed by
          // its immediate TAG_End. Reject richer profiles instead of silently
          // assigning an ID for a codec this server does not implement.
          if (!registry.bool_() || registry.u8() != 10 ||
              registry.u8() != 0)
            throw ProtocolError("unsupported world_clock registry payload");
          worldClockIds[target] = holderId;
        }
        if (!seen[0] || !seen[1] || !registry.empty())
          throw ProtocolError("invalid world_clock registry frame");
      }
    }
    configuration.push_back(frame(id, body.subspan(packetReader.pos)));
  }
  if (!worldClockRegistryFrame)
    throw ProtocolError(
        "configuration is missing the world_clock registry frame");
  if (!timelineRegistryFrame)
    throw ProtocolError(
        "configuration is missing the timeline registry frame");
}
int ProtocolData::packet(std::string_view phase, std::string_view flow,
                         std::string_view name) const {
  auto it = packetIds.find(key(phase, flow, name));
  if (it == packetIds.end())
    throw ProtocolError("unknown packet " + key(phase, flow, name));
  return it->second;
}

Uuid offlineUuid(std::string_view name) {
  // UUID.nameUUIDFromBytes("OfflinePlayer:" + name), MD5 with RFC4122 v3 bits.
  std::string text = "OfflinePlayer:" + std::string(name);
  Bytes bytes(text.begin(), text.end());
  const std::uint64_t bitLength = bytes.size() * 8;
  bytes.push_back(0x80);
  while (bytes.size() % 64 != 56)
    bytes.push_back(0);
  for (int i = 0; i < 8; ++i)
    bytes.push_back(static_cast<std::uint8_t>(bitLength >> (8 * i)));
  constexpr int shift[]{7, 12, 17, 22, 5, 9,  14, 20,
                        4, 11, 16, 23, 6, 10, 15, 21};
  std::array<std::uint32_t, 4> h{0x67452301, 0xefcdab89, 0x98badcfe,
                                 0x10325476};
  for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
    std::uint32_t words[16]{};
    for (int i = 0; i < 64; ++i)
      words[i / 4] |= std::uint32_t(bytes[offset + i]) << (8 * (i % 4));
    auto [a, b, c, d] = h;
    for (int i = 0; i < 64; ++i) {
      std::uint32_t f;
      int g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const auto k = static_cast<std::uint32_t>(
          std::floor(std::abs(std::sin(double(i + 1))) * 4294967296.0));
      auto temp = d;
      d = c;
      c = b;
      b += std::rotl(a + f + k + words[g], shift[(i / 16) * 4 + i % 4]);
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
  }
  Uuid result{};
  for (int i = 0; i < 16; ++i)
    result[i] = static_cast<std::uint8_t>(h[i / 4] >> (8 * (i % 4)));
  result[6] = (result[6] & 15) | 0x30;
  result[8] = (result[8] & 63) | 0x80;
  return result;
}

int ProtocolData::dimensionTypeId(std::string_view id) const {
  if (!id.empty()) {
    try {
      const std::string normalized = normalizeDimensionId(id);
      if (auto it = dimensionTypeIds.find(normalized);
          it != dimensionTypeIds.end()) {
        return it->second;
      }
    } catch (const std::invalid_argument &) {
    }
  }
  return dimensionId;
}

Session::Session(World &world, const ProtocolData &data, ServerOptions options)
    : world_(&world), data_(data), options_(std::move(options)),
      currentDimensionId_(options_.dimensionId.empty()
                                ? world.dimensionId()
                                : normalizeDimensionId(options_.dimensionId)),
      currentDimensionTypeId_(
          options_.dimensionTypeId.empty()
              ? (world.dimension().id.empty() ? currentDimensionId_
                                              : world.dimension().id)
              : normalizeDimensionId(options_.dimensionTypeId)),
      position_(options_.spawn) {
  options_.viewDistance = std::clamp(options_.viewDistance, 2, maxViewDistance);
  availableChunks_.insert(options_.chunks.begin(), options_.chunks.end());
  if (options_.unlimitedNetwork) {
    chunksPerBatch_ = 64;
    chunkBatchLimit_ = 4096;
  } else if (availableChunks_.size() > 64) {
    chunksPerBatch_ = chunkBatchLimit_ = 2;
  }
}
Session::~Session() { detachPlayerEntity(); }

void Session::attachPlayerEntity() {
  if (playerEntity_ && !playerEntity_->isRemoved())
    return;
  auto player = std::make_shared<ServerPlayer>(
      *world_, position_, username_.empty() ? std::string("Player") : username_);
  for (std::size_t i = 0; i < 4; ++i) {
    const std::uint32_t word =
        (static_cast<std::uint32_t>(uuid_[i * 4]) << 24) |
        (static_cast<std::uint32_t>(uuid_[i * 4 + 1]) << 16) |
        (static_cast<std::uint32_t>(uuid_[i * 4 + 2]) << 8) |
        static_cast<std::uint32_t>(uuid_[i * 4 + 3]);
    player->uuid[i] = static_cast<std::int32_t>(word);
  }
  player->yaw = yaw_;
  player->pitch = pitch_;
  player->onItemPickup = [this](const ItemStack &item) -> int {
    if (item.empty() || phase_ != Phase::Play)
      return 0;
    int remaining = item.count;
    for (int slot = 0; slot < 36 && remaining > 0; ++slot) {
      if (inventory_[slot].id == item.id && inventory_[slot].count < 64) {
        const int add = std::min(remaining, 64 - inventory_[slot].count);
        ItemStack updated = inventory_[slot];
        updated.count += add;
        setSlot(slot, std::move(updated));
        remaining -= add;
      }
    }
    for (int slot = 0; slot < 36 && remaining > 0; ++slot) {
      if (inventory_[slot].empty()) {
        const int add = std::min(remaining, 64);
        ItemStack updated = item;
        updated.count = add;
        setSlot(slot, std::move(updated));
        remaining -= add;
      }
    }
    return item.count - remaining;
  };
  playerEntity_ = player;
  world_->addEntityWithId(std::move(player), options_.playerId);
}

void Session::detachPlayerEntity() {
  if (!playerEntity_)
    return;
  if (!playerEntity_->isRemoved() && world_) {
    playerEntity_->stopRiding();
    playerEntity_->stopSleeping();
    world_->removeEntity(playerEntity_);
  }
  playerEntity_.reset();
}
void Session::setWorldClockSource(const WorldClocks &clocks,
                                  bool advanceTime) {
  worldClocks_ = &clocks;
  advanceTime_ = advanceTime;
}
void Session::writeWorldClock(Writer &out,
                              BuiltinWorldClock clock) const {
  const std::size_t index = static_cast<std::size_t>(clock);
  const WorldClockState &state = worldClocks_->state(clock);
  // ClientboundSetTimePacket / ClockNetworkState: registry holder identity,
  // VarLong total, partial, then the effective rate. A paused clock and the
  // global advance_time rule both advertise zero; tick-freeze is represented
  // separately by the existing ticking-state packet and does not change this.
  out.varInt(data_.worldClockIds[index]);
  out.varLong(state.totalTicks);
  out.f32(state.partialTick);
  out.f32(state.paused || !advanceTime_ ? 0.0f : state.rate);
}
void Session::sendSetTime() {
  Writer out;
  out.i64(world_->gameTime());
  if (!worldClocks_) {
    out.varInt(0);
    send("set_time", std::move(out));
    return;
  }
  out.varInt(2);
  writeWorldClock(out, BuiltinWorldClock::Overworld);
  writeWorldClock(out, BuiltinWorldClock::TheEnd);
  send("set_time", std::move(out));
}
void Session::sendWorldClockUpdate(BuiltinWorldClock clock) {
  if (!worldClocks_ || phase_ != Phase::Play)
    return;
  Writer out;
  out.i64(world_->gameTime());
  out.varInt(1);
  writeWorldClock(out, clock);
  send("set_time", std::move(out));
}
void Session::packet(int id, const Writer &payload) {
  if (closed())
    return;
  // Framing is a full payload copy; leave it to the writer thread.
  enqueue(OutPacket{payload.data, id, {}});
}
void Session::packet(int id, Writer &&payload) {
  if (!closed())
    enqueue(OutPacket{std::move(payload.data), id, {}});
}
void Session::enqueue(OutPacket item) {
  if (closed())
    return;
  const std::size_t charge = outPacketCharge(item);
  if (!options_.unlimitedNetwork &&
      (output_.size() >= maxQueuedOutputPackets ||
       charge > maxQueuedOutputBytes ||
       outputBytes_ > maxQueuedOutputBytes - charge)) {
    output_.clear();
    outputBytes_ = 0;
    error_ = "Connection output backlog exceeded";
    phase_ = Phase::Closed;
    if (diagnostic)
      diagnostic(error_);
    return;
  }
  outputBytes_ += charge;
  output_.push_back(std::move(item));
}
void Session::queue(Bytes bytes) {
  enqueue(OutPacket{std::move(bytes), -1, {}});
}
void Session::queueDeferred(std::function<Bytes()> producer,
                            std::size_t retainedBytes,
                            std::function<bool()> stillValid) {
  enqueue(OutPacket{{}, -1, std::move(producer), retainedBytes,
                    std::move(stillValid)});
}
void Session::send(std::string_view name, const Writer &payload) {
  if (closed())
    return;
  packet(data_.packet(phaseName(phase_), "clientbound", name), payload);
}
void Session::send(std::string_view name, Writer &&payload) {
  if (!closed())
    packet(data_.packet(phaseName(phase_), "clientbound", name),
           std::move(payload));
}
std::vector<OutPacket> Session::takeOutputItems() {
  outputBytes_ = 0;
  return std::exchange(output_, {});
}
std::vector<Bytes> Session::takeOutput() {
  std::vector<OutPacket> items = takeOutputItems();
  std::vector<Bytes> packets;
  packets.reserve(items.size());
  for (auto &item : items) {
    if (item.stillValid && !item.stillValid())
      continue;
    Bytes produced = item.deferred ? item.deferred()
                      : item.packetId >= 0 ? frame(item.packetId, item.bytes)
                                           : std::move(item.bytes);
    if (!produced.empty())
      packets.push_back(std::move(produced));
  }
  return packets;
}
void Session::disconnect(const std::string &reason) {
  if (closed())
    return;
  detachPlayerEntity();
  closeOpenContainer(false);
  error_ = reason;
  Writer payload;
  if (phase_ == Phase::Login) {
    payload.string("{\"text\":\"" + reason + "\"}");
    send("login_disconnect", payload);
  } else if (phase_ == Phase::Play || phase_ == Phase::Configuration) {
    nbtText(payload, reason);
    send("disconnect", payload);
  }
  phase_ = Phase::Closed;
  if (diagnostic)
    diagnostic(reason);
}
void Session::receive(std::span<const std::uint8_t> bytes) {
  if (closed())
    return;
  try {
    if (input_.size() + bytes.size() > 2 * maxFrameSize)
      throw ProtocolError("input backlog exceeded");
    input_.insert(input_.end(), bytes.begin(), bytes.end());
    int count = 0;
    while (auto body = popFrame(input_)) {
      if (++count > 4096)
        throw ProtocolError("too many packets in one read");
      Reader reader(*body);
      handle(reader);
      if (closed())
        break;
    }
  } catch (const std::exception &) {
    // Avoid copying attacker-controlled parser strings into JSON/NBT errors.
    disconnect("Invalid or unsupported protocol packet");
  }
}
void Session::handle(Reader &in) {
  const int id = in.varInt();
  if (phase_ == Phase::Handshake) {
    if (id != 0)
      throw ProtocolError("expected handshake");
    const int version = in.varInt();
    (void)in.string(255 * 3);
    (void)in.i16();
    const int next = in.varInt();
    if (!in.empty() || (next != 1 && next != 2))
      throw ProtocolError("invalid handshake");
    phase_ = next == 1 ? Phase::Status : Phase::Login;
    if (next == 2 && version != 775)
      disconnect("Requires Minecraft 26.1 protocol 775");
  } else if (phase_ == Phase::Status) {
    if (id == 0 && in.empty()) {
      Writer out;
      out.string("{\"version\":{\"name\":\"26.1 "
                 "C++\",\"protocol\":775},\"players\":{\"max\":8,\"online\":0},"
                 "\"description\":{\"text\":\"C++ redstone "
                 "simulator\"},\"enforcesSecureChat\":false}");
      send("status_response", out);
    } else if (id == 1) {
      Writer out;
      out.i64(in.i64());
      if (!in.empty())
        throw ProtocolError("bad ping");
      send("pong_response", out);
      phase_ = Phase::Closed;
    } else
      throw ProtocolError("bad status packet");
  } else if (phase_ == Phase::Login) {
    if (id == 0 && username_.empty()) {
      username_ = in.string(16);
      if (username_.empty() ||
          !std::all_of(username_.begin(), username_.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_';
          }))
        throw ProtocolError("invalid username");
      (void)in.raw(16);
      if (!in.empty())
        throw ProtocolError("bad hello");
      uuid_ = offlineUuid(username_);
      Writer out;
      uuid(out, uuid_);
      out.string(username_);
      out.varInt(0);
      send("login_finished", out);
    } else if (id == 3 && !username_.empty() && in.empty()) {
      phase_ = Phase::Configuration;
      // Offer no known packs: all registry values in the exported stream are
      // present. This also avoids trusting a different patch client's cache.
      Writer out;
      out.varInt(0);
      send("select_known_packs", out);
      awaitingKnownPacks_ = true;
    } else
      throw ProtocolError("unexpected login packet");
  } else if (phase_ == Phase::Configuration) {
    if (id ==
        data_.packet("configuration", "serverbound", "select_known_packs")) {
      if (!awaitingKnownPacks_ || in.varInt() != 0 || !in.empty())
        throw ProtocolError("bad known packs reply");
      awaitingKnownPacks_ = false;
      for (const auto &bytes : data_.configuration) {
        auto copy = bytes;
        const auto body = popFrame(copy);
        if (!body) throw ProtocolError("invalid configuration frame");
        Reader packet(*body);
        const int packetId = packet.varInt();
        if (world_->customBlockTags() && packetId == data_.packet("configuration", "clientbound", "update_tags"))
          queue(replaceBlockTagPacket(bytes, data_, *world_->customBlockTags(), packetId));
        else
          queue(bytes);
      }
      configuredBlockTagRevision_ = world_->blockTagRevision();
      send("finish_configuration");
      finishSent_ = true;
    } else if (id == 3) {
      if (!finishSent_ || !in.empty())
        throw ProtocolError("early configuration finish");
      phase_ = Phase::Play;
      syncManagedResidency();
      join();
      if (configuredBlockTagRevision_ != world_->blockTagRevision())
        sendBlockTagUpdate();
    } else if (id != 0 && id != 2 && id != 5)
      throw ProtocolError("unsupported configuration packet");
    // Client information, brand payload and optional pong need no world state.
  } else if (phase_ == Phase::Play)
    play(id, in);
}

void Session::positionPacket(Writer &out, Vec3 pos, Vec3 motion, float yaw,
                             float pitch) {
  out.f64(pos.x);
  out.f64(pos.y);
  out.f64(pos.z);
  out.f64(motion.x);
  out.f64(motion.y);
  out.f64(motion.z);
  out.f32(yaw);
  out.f32(pitch);
  out.i32(0); // no relative flags
}
void Session::writeCommonPlayerSpawnInfo(Writer &out, const std::string &dimId,
                                         int dimTypeRegistryId,
                                         std::int64_t hashedSeed, bool isFlat,
                                         int seaLevel) const {
  out.varInt(dimTypeRegistryId);
  out.string(dimId);
  out.i64(hashedSeed);
  out.u8(1);
  out.u8(255);
  out.bool_(false);
  out.bool_(isFlat);
  out.bool_(false);
  out.varInt(0);
  out.varInt(seaLevel);
}

void Session::resetDimensionViewState() {
  for (auto &[pos, token] : chunkGenerations_)
    if (token)
      token->store(false, std::memory_order_release);
  chunkGenerations_.clear();
  trackedEntities_.clear();
  knownPlayerInfo_.clear();
  changedBlocks_.clear();
  sentChunks_.clear();
  pendingChunks_.clear();
  awaitingLightChunks_.clear();
  availableChunks_.clear();
  managedPendingResidency_.clear();
  sentChunkBlockLight_.clear();
  sentChunkSkyLight_.clear();
  chunkCenter_.reset();
  awaitingChunkBatch_ = false;
  joinPositionPending_ = false;
}

void Session::join() {
  const int height = world_->maxY() - world_->minY();
  if (height < kDimensionMinHeight || (height & 15) != 0 ||
      (world_->minY() & 15) != 0)
    throw ProtocolError("network dimension height must be section-aligned");
  Writer out;
  out.i32(options_.playerId);
  out.bool_(false);
  std::vector<std::string> dims;
  dims.reserve(options_.availableDimensions.size() + 1);
  bool hasCurrent = false;
  for (const auto &raw : options_.availableDimensions) {
    const std::string normalized = normalizeDimensionId(raw);
    if (std::find(dims.begin(), dims.end(), normalized) == dims.end())
      dims.push_back(normalized);
    if (normalized == currentDimensionId_)
      hasCurrent = true;
  }
  if (!hasCurrent)
    dims.insert(dims.begin(), currentDimensionId_);
  out.varInt(static_cast<int>(dims.size()));
  for (const auto &dim : dims)
    out.string(dim);
  out.varInt(8);
  out.varInt(options_.viewDistance);
  out.varInt(options_.viewDistance);
  out.bool_(false);
  out.bool_(true);
  out.bool_(false);
  writeCommonPlayerSpawnInfo(
      out, currentDimensionId_, data_.dimensionTypeId(currentDimensionTypeId_),
      options_.hashedSeed, options_.isFlat, options_.seaLevel);
  out.bool_(false);
  send("login", out);
  out = {};
  out.u8(0x0d);
  out.varInt(1);
  uuid(out, uuid_);
  out.string(username_);
  out.varInt(0);
  out.varInt(1);
  out.bool_(true);
  send("player_info_update", out);
  out = {};
  out.u8(15);
  out.f32(0.05f);
  out.f32(0.1f);
  send("player_abilities", out);
  out = {};
  out.string(currentDimensionId_);
  out.position({int(std::floor(position_.x)), int(std::floor(position_.y)),
                int(std::floor(position_.z))});
  out.f32(0);
  out.f32(0);
  send("set_default_spawn_position", out);
  out = {};
  out.varInt(options_.viewDistance);
  send("set_chunk_cache_radius", out);
  sendCommands();
  if (commandContext_ && commandContext_->ticks) {
    // ServerTickRateManager.updateJoiningPlayer.
    sendTickingState(*commandContext_->ticks);
    sendTickingStep(*commandContext_->ticks);
  }
  refreshChunkView();
  out = {};
  out.u8(13);
  out.f32(0);
  send("game_event", out);
  sendChunkBatch();
  if (awaitingLightChunks_.empty())
    finishJoin();
  else
    joinPositionPending_ = true;
}
void Session::finishJoin() {
  joinPositionPending_ = false;
  respawning_ = false;
  Writer out;
  sendSetTime();
  constexpr const char *initial[]{
      "smooth_stone",        "sticky_piston",  "slime_block",
      "white_carpet",        "redstone_block", "observer",
      "dead_tube_coral_fan", "honey_block",    "tnt"};
  for (int i = 0; i < 9; ++i)
    setSlot(i, itemStack(initial[i], 64));
  if (options_.spawnPlayerEntity && !playerEntity_)
    attachPlayerEntity();
  out = {};
  out.varInt(pendingTeleportId_);
  positionPacket(out, position_, {}, yaw_, pitch_);
  send("player_position", out);
  broadcastEntities();
  // TODO: recipes, commands, achievements, player avatars and
  // player physics/pushes. This endpoint is a creative editor of loaded chunks.
}

void Session::finishRespawn() {
  joinPositionPending_ = false;
  respawning_ = false;
  Writer out;
  out.varInt(pendingTeleportId_);
  positionPacket(out, position_, {}, yaw_, pitch_);
  send("player_position", out);
  broadcastEntities();
}

void Session::switchDimension(World &targetWorld, const DimensionStem &stem,
                              Vec3 spawnPos, float yaw, float pitch) {
  const int height = targetWorld.maxY() - targetWorld.minY();
  if (height < kDimensionMinHeight || (height & 15) != 0 ||
      (targetWorld.minY() & 15) != 0)
    throw ProtocolError("network dimension height must be section-aligned");
  if (openContainer_)
    closeOpenContainer(true);
  if (playerEntity_ && !playerEntity_->isRemoved()) {
    playerEntity_->stopRiding();
    playerEntity_->stopSleeping();
    world_->detachEntityForDimensionTransfer(playerEntity_);
  }
  resetDimensionViewState();
  world_ = &targetWorld;
  currentDimensionId_ =
      stem.id.empty() ? targetWorld.dimensionId() : normalizeDimensionId(stem.id);
  currentDimensionTypeId_ = !stem.type.id.empty()
                                ? normalizeDimensionId(stem.type.id)
                                : (!stem.typeId.empty()
                                       ? normalizeDimensionId(stem.typeId)
                                       : normalizeDimensionId(targetWorld.dimension().id));
  position_ = spawnPos;
  yaw_ = yaw;
  pitch_ = pitch;
  if (playerEntity_) {
    playerEntity_->yaw = yaw;
    playerEntity_->pitch = pitch;
    playerEntity_->setPos(spawnPos);
    world_->addEntityWithId(playerEntity_, options_.playerId);
  }
  teleportPending_ = true;
  ++pendingTeleportId_;
  respawning_ = true;
  if (phase_ != Phase::Play)
    return;
  if (world_->customBlockTags() &&
      configuredBlockTagRevision_ != world_->blockTagRevision()) {
    sendBlockTagUpdate();
    configuredBlockTagRevision_ = world_->blockTagRevision();
  }
  Writer out;
  writeCommonPlayerSpawnInfo(
      out, currentDimensionId_, data_.dimensionTypeId(currentDimensionTypeId_),
      options_.hashedSeed, options_.isFlat, options_.seaLevel);
  out.u8(3); // ClientboundRespawnPacket.KEEP_ALL_DATA
  send("respawn", out);
  out = {};
  out.string(currentDimensionId_);
  out.position({int(std::floor(position_.x)), int(std::floor(position_.y)),
                int(std::floor(position_.z))});
  out.f32(0);
  out.f32(0);
  send("set_default_spawn_position", out);
  out = {};
  out.u8(15);
  out.f32(0.05f);
  out.f32(0.1f);
  send("player_abilities", out);
  sendSetTime();
  out = {};
  out.varInt(options_.viewDistance);
  send("set_chunk_cache_radius", out);
  if (managedResidency_) {
    syncManagedResidency();
  } else {
    const int cx0 = chunkCoord(position_.x);
    const int cz0 = chunkCoord(position_.z);
    for (int dx = -options_.viewDistance; dx <= options_.viewDistance; ++dx)
      for (int dz = -options_.viewDistance; dz <= options_.viewDistance; ++dz)
        if (world_->isChunkLoaded(cx0 + dx, cz0 + dz))
          availableChunks_.emplace(cx0 + dx, cz0 + dz);
  }
  refreshChunkView();
  out = {};
  out.u8(13);
  out.f32(0);
  send("game_event", out);
  sendChunkBatch();
  if (awaitingLightChunks_.empty())
    finishRespawn();
  else
    joinPositionPending_ = true;
}
void Session::stack(Writer &out, const ItemStack &item) const {
  if (item.empty()) {
    out.varInt(0);
    return;
  }
  out.varInt(item.count);
  out.varInt(data_.registry.itemId(item.id));
  if (!item.components) {
    out.varInt(0);
    out.varInt(0);
    return;
  }
  const ItemComponentData &c = *item.components;
  // Serialize primitive DataComponentType IDs matching 26.1 DataComponents.java.
  std::vector<std::pair<int, Writer>> encoded;
  if (c.maxStackSize) {
    Writer w;
    w.varInt(*c.maxStackSize);
    encoded.emplace_back(1, std::move(w));
  }
  if (c.maxDamage) {
    Writer w;
    w.varInt(*c.maxDamage);
    encoded.emplace_back(2, std::move(w));
  }
  if (c.damage > 0) {
    Writer w;
    w.varInt(c.damage);
    encoded.emplace_back(3, std::move(w));
  }
  if (c.unbreakable) {
    encoded.emplace_back(4, Writer{});
  }
  if (c.repairCost) {
    Writer w;
    w.varInt(*c.repairCost);
    encoded.emplace_back(19, std::move(w));
  }
  if (c.dyedColor) {
    Writer w;
    w.i32(static_cast<std::int32_t>(*c.dyedColor));
    encoded.emplace_back(44, std::move(w));
  }
  out.varInt(static_cast<int>(encoded.size()));
  out.varInt(0);
  for (const auto &[cid, w] : encoded) {
    out.varInt(cid);
    out.raw(w.data);
  }
}
void Session::setSlot(int slot, ItemStack item) {
  inventory_.at(slot) = std::move(item);
  Writer out;
  out.varInt(slot);
  stack(out, inventory_[slot]);
  send("set_player_inventory", out);
}
bool Session::canInteract(Pos p) const {
  if (teleportPending_ || !world_->isLoaded(p) || !hasChunk(p))
    return false;
  Vec3 eye = position_ + Vec3{0, 1.62, 0};
  const auto dx = std::max({double(p.x) - eye.x, eye.x - (p.x + 1.0), 0.0});
  const auto dy = std::max({double(p.y) - eye.y, eye.y - (p.y + 1.0), 0.0});
  const auto dz = std::max({double(p.z) - eye.z, eye.z - (p.z + 1.0), 0.0});
  return dx * dx + dy * dy + dz * dz <
         36; // base 4.5 + creative .5 + server margin 1
}

void Session::playerInteract(Reader &in) {
  const int targetId = in.varInt();
  const int hand = in.varInt();
  (void)readLpVector(in);
  const bool secondary = in.bool_();
  if (!in.empty() || !playerEntity_ || playerEntity_->isRemoved() ||
      teleportPending_)
    return;
  if (hand != 0 || secondary)
    return;
  for (const auto &ptr : world_->entities) {
    if (ptr && ptr->id == targetId && !ptr->isRemoved()) {
      if ((ptr->position - playerEntity_->position).lengthSqr() > 36.0)
        return;
      if (dynamic_cast<const Minecart *>(ptr.get()) ||
          dynamic_cast<const AbstractBoat *>(ptr.get())) {
        playerEntity_->startRiding(*ptr);
      }
      return;
    }
  }
}

void Session::play(int id, Reader &in) {
  const auto is = [&](std::string_view name) {
    return id == data_.packet("play", "serverbound", name);
  };
  if (is("accept_teleportation")) {
    if (in.varInt() == pendingTeleportId_)
      teleportPending_ = false;
    if (!in.empty())
      throw ProtocolError("bad teleport ack");
  } else if (is("keep_alive")) {
    if (!keepAlive_ || in.i64() != *keepAlive_ || !in.empty())
      throw ProtocolError("bad keepalive");
    keepAlive_.reset();
  } else if (is("ping_request")) {
    Writer out;
    out.i64(in.i64());
    send("pong_response", out);
  } else if (is("move_player_pos") || is("move_player_pos_rot") ||
             is("move_player_rot") || is("move_player_status_only")) {
    Vec3 pos = position_;
    float yaw = yaw_, pitch = pitch_;
    if (is("move_player_pos") || is("move_player_pos_rot")) {
      pos.x = in.f64();
      pos.y = in.f64();
      pos.z = in.f64();
    }
    if (is("move_player_rot") || is("move_player_pos_rot")) {
      yaw = in.f32();
      pitch = in.f32();
    }
    const std::uint8_t moveFlags = in.u8();
    const bool onGroundNow = (moveFlags & 1) != 0;
    if (!in.empty() || !std::isfinite(pos.x) || !std::isfinite(pos.y) ||
        !std::isfinite(pos.z) || !std::isfinite(yaw) || !std::isfinite(pitch) ||
        std::abs(pos.x) > 29999984 || std::abs(pos.z) > 29999984 ||
        std::abs(pos.y) > 20000000)
      throw ProtocolError("invalid movement");
    if (!teleportPending_) {
      position_ = pos;
      yaw_ = std::remainder(yaw, 360.0f);
      pitch_ = std::clamp(pitch, -90.0f, 90.0f);
      if (playerEntity_ && !playerEntity_->isRemoved()) {
        playerEntity_->applyClientMovement(*world_, position_, yaw_, pitch_,
                                           onGroundNow);
      }
    }
  } else if (is("set_carried_item")) {
    int slot = in.i16();
    if (slot < 0 || slot > 8 || !in.empty())
      throw ProtocolError("invalid held slot");
    selectedSlot_ = slot;
  } else if (is("set_creative_mode_slot"))
    creativeSlot(in);
  else if (is("player_action"))
    playerAction(in);
  else if (is("use_item_on"))
    useItemOn(in);
  else if (is("pick_item_from_block")) {
    Pos p = in.position();
    (void)in.bool_();
    if (!in.empty())
      throw ProtocolError("invalid pick");
    auto item = blockItem(world_->getBlock(p));
    if (canInteract(p) && !item.empty())
      setSlot(selectedSlot_, itemStack(item));
  } else if (is("use_item")) {
    useItem(in);
  } else if (is("interact")) {
    playerInteract(in);
  } else if (is("chat_command")) {
    const std::string line = in.string();
    if (!in.empty())
      throw ProtocolError("invalid chat command");
    runCommand(line);
  } else if (is("command_suggestion")) {
    const int requestId = in.varInt();
    const std::string line = in.string(32500);
    if (!in.empty())
      throw ProtocolError("invalid command suggestion request");
    sendSuggestions(requestId, line);
  } else if (is("player_input")) {
    const auto flags = in.u8();
    if (!in.empty())
      throw ProtocolError("bad player input");
    sneaking_ = (flags & 32) != 0;
    if (playerEntity_ && !playerEntity_->isRemoved()) {
      playerEntity_->setShiftKeyDown(sneaking_);
      if (sneaking_ && playerEntity_->isPassenger())
        playerEntity_->stopRiding();
    }
  } else if (is("chunk_batch_received")) {
    const float desired = in.f32();
    if (!in.empty() || !std::isfinite(desired) || desired <= 0)
      throw ProtocolError("invalid chunk batch acknowledgement");
    awaitingChunkBatch_ = false;
    chunksPerBatch_ =
        static_cast<int>(std::clamp(desired, 1.0f, float(chunkBatchLimit_)));
  } else if (is("container_click")) {
    containerClick(in);
  } else if (is("container_close")) {
    containerClose(in);
  } else if (is("client_tick_end") || is("player_loaded") ||
             is("player_abilities") || is("client_information") ||
             is("custom_payload") || is("swing") || is("player_command") ||
             is("chat_session_update") || is("pong")) {
    // These packets do not modify this creative editor's simulated blocks.
    // TODO: player actions, chat/session state and sprint.
  } else if (diagnostic)
    diagnostic("TODO: unsupported play packet ignored");
}

void Session::sendTickTime(double milliseconds, std::int64_t tick, std::size_t samples) {
  if (phase_ != Phase::Play || !std::isfinite(milliseconds) || milliseconds < 0)
    return;
  try {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "Tick " << tick << ": " << std::fixed << std::setprecision(3)
         << milliseconds << " ms (1s avg, " << samples << " ticks)";
    Writer out;
    // ComponentSerialization accepts a network NBT StringTag for literal text.
    nbtText(out, text.str());
    out.bool_(false); // Ordinary chat, as requested by the user.
    send("system_chat", out);
  } catch (const ProtocolError &) {
    disconnect("Tick timing packet failed");
  }
}
void Session::attachCommands(const command::Dispatcher *dispatcher,
                             const command::Context *context) {
  commands_ = dispatcher;
  commandContext_ = context;
}
void Session::sendCommandMessage(const command::Message &message) {
  Writer out;
  nbtTranslate(out, message.key, message.args);
  out.bool_(false); // ClientboundSystemChatPacket(component, overlay=false).
  send("system_chat", out);
}
void Session::runCommand(std::string_view line) {
  if (!commands_ || !commandContext_)
    return;
  if (!line.empty() && line.front() == '/')
    line.remove_prefix(1);
  command::Source source;
  source.permissionLevel = options_.commandPermissionLevel;
  source.name = username_;
  if (commands_->run(line, source, *commandContext_) < 0) {
    // The client parses against the exported tree before sending, so this is a
    // malformed or unknown line: brigadier would answer with a syntax error.
    Writer out;
    nbtText(out, "Unknown or incomplete command");
    out.bool_(false);
    send("system_chat", out);
    return;
  }
  // TODO: broadcast=true lines go to every player with the command permission in
  // vanilla; this server has a single local player, so they only echo here.
  for (const auto &message : source.messages)
    sendCommandMessage(message);
}
void Session::sendBlockTagUpdate() {
  if (phase_ != Phase::Play || !world_->customBlockTags()) return;
  for (const auto &bytes : data_.configuration) {
    auto copy = bytes;
    const auto body = popFrame(copy);
    if (!body) throw ProtocolError("invalid configuration frame");
    Reader packet(*body);
    if (packet.varInt() == data_.packet("configuration", "clientbound", "update_tags")) {
      queue(replaceBlockTagPacket(bytes, data_, *world_->customBlockTags(),
                                  data_.packet("play", "clientbound", "update_tags")));
      return;
    }
  }
  throw ProtocolError("missing configuration update_tags");
}
void Session::sendSuggestions(int requestId, std::string_view line) {
  if (!commands_ || !commandContext_ || phase_ != Phase::Play)
    return;
  try {
    // ServerGamePacketListenerImpl.handleCustomCommandSuggestions skips a leading
    // slash with StringReader.skip, which leaves the string intact, so every
    // replacement offset is still relative to the text the client sent.
    const bool slash = !line.empty() && line.front() == '/';
    const std::string_view body = slash ? line.substr(1) : line;
    command::Source source;
    source.permissionLevel = options_.commandPermissionLevel;
    source.name = username_;
    auto completion = commands_->suggestions(body, source);
    if (slash)
      ++completion.start;
    Writer out;
    out.varInt(requestId);
    out.varInt(completion.start);
    out.varInt(completion.length);
    out.varInt(static_cast<std::int32_t>(completion.texts.size()));
    for (const auto &text : completion.texts) {
      out.string(text);
      out.bool_(false); // Optional<Component> tooltip: none.
    }
    send("command_suggestions", out);
  } catch (const ProtocolError &) {
    disconnect("Command suggestion packet failed");
  }
}
void Session::sendCommands() {
  if (!commands_ || phase_ != Phase::Play)
    return;
  try {
    Writer out;
    const auto tree = encodeCommandTree(commands_->root());
    out.raw(tree);
    send("commands", out);
  } catch (const ProtocolError &) {
    disconnect("Command tree packet failed");
  }
}
void Session::sendTickingState(const TickRateManager &ticks) {
  if (phase_ != Phase::Play)
    return;
  Writer out;
  out.f32(ticks.tickrate());
  out.bool_(ticks.isFrozen());
  send("ticking_state", out);
}
void Session::sendTickingStep(const TickRateManager &ticks) {
  if (phase_ != Phase::Play)
    return;
  Writer out;
  out.varInt(ticks.frozenTicksToRun());
  send("ticking_step", out);
}

void Session::tick() {
  if (closed())
    return;
  try {
    ++connectionTicks_;
    if (dropSpam_ > 0)
      --dropSpam_;
    if (phase_ != Phase::Play) {
      if (connectionTicks_ > 600)
        disconnect("Login timed out");
      return;
    }
    if (keepAlive_ && connectionTicks_ - keepAliveSent_ > 600) {
      disconnect("Keepalive timed out");
      return;
    }
    if (!keepAlive_ && connectionTicks_ - keepAliveSent_ >= 200) {
      keepAlive_ = connectionTicks_;
      keepAliveSent_ = connectionTicks_;
      Writer out;
      out.i64(*keepAlive_);
      send("keep_alive", out);
    }
    if (connectionTicks_ % 20 == 0)
      sendSetTime();
    if (playerEntity_ && !playerEntity_->isRemoved()) {
      if (playerEntity_->hurtMarked) {
        Writer out;
        out.varInt(playerEntity_->id);
        lpVector(out, playerEntity_->deltaMovement);
        send("set_entity_motion", std::move(out));
        playerEntity_->hurtMarked = false;
      }
      if (!teleportPending_ &&
          (playerEntity_->position - position_).lengthSqr() > 1.0e-6) {
        position_ = playerEntity_->position;
      }
    }
    refreshChunkView();
    syncOpenContainer();
    sendChunkBatch();
    if (joinPositionPending_ && !sentChunks_.empty()) {
      if (respawning_)
        finishRespawn();
      else
        finishJoin();
    }
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
} // namespace redstone::net
