#pragma once
// 原生协议 775 会话状态机与协议数据定义：管理握手、登录、配置、游戏阶段流转，
// 玩家库存/容器同步、区块与光照增量推送、实体追踪及指令下发，与底层 Socket I/O 完全解耦。
#include "redstone/command.hpp"
#include "redstone/containers.hpp"
#include "redstone/entity.hpp"
#include "redstone/net/wire.hpp"
#include <array>
#include <atomic>
#include <map>
#include <set>

namespace redstone {
class WorldClocks;
enum class BuiltinWorldClock : std::uint8_t;
}
namespace redstone::net {
// 由固定版本的原版编解码器导出的不可变协议数据（对齐 26.1 / 协议 775）。
// 所有网络注册表 ID 均严格来自该导出数据，绝不从本模拟器较小的内置注册表中推断。
struct ProtocolData {
  explicit ProtocolData(const std::filesystem::path &directory);
  Registry registry;
  std::vector<Bytes> configuration;
  std::map<std::string, int> packetIds, blockIds;
  std::map<std::string, int> explosionEffectIds;
  std::map<std::string, int> dimensionTypeIds;
  int dimensionId = 0;
  // 来自 registry_ids.tsv 的官方 26.1 world_clock 注册表 ID，顺序固定为 overworld、the_end；
  // 两者均存在于本 ProtocolData 实际向客户端下发的 configuration 数据包中。
  std::array<int, 2> worldClockIds{{-1, -1}};
  int packet(std::string_view phase, std::string_view flow,
             std::string_view name) const;
  int dimensionTypeId(std::string_view id) const;
};
using Uuid = std::array<std::uint8_t, 16>;
Uuid offlineUuid(std::string_view name);
// 客户端连接所处的协议生命周期阶段。
enum class Phase { Handshake, Status, Login, Configuration, Play, Closed };
// 协议层区块视野上限仍固定为 10；服务端加载票源视距则在 TicketSourceManager 中独立校验（2..32）。
inline constexpr int maxViewDistance = 10;
// 单个协议会话的初始化配置（出生点、预置区块、玩家实体 ID、视距、维度与初始状态开关）。
struct ServerOptions {
  Vec3 spawn{8.5, 66, 8.5};
  std::vector<std::pair<int, int>> chunks;
  int playerId = 1000000;
  // 客户端可见视距与服务端加载票源覆盖范围相互独立。
  int viewDistance = 3;
  int ticketViewDistance = 3;
  // 该玩家在 Commands.hasPermission 中对应的指令权限等级。当前原生服务端尚未接入 op 列表，
  // 因此玩家默认无管理员权限；本地控制台会单独构造服主级（owner）权限源。
  int commandPermissionLevel = 0;
  std::string dimensionId = "minecraft:overworld";
  std::string dimensionTypeId = "minecraft:overworld";
  std::vector<std::string> availableDimensions;
  std::int64_t hashedSeed = 0;
  bool isFlat = true;
  int seaLevel = 63;
  // 可选开关：是否在 World::entities 中生成权威 ServerPlayer 实体。
  // 默认关闭，避免影响只关注离线区块编辑的测试对实体 ID 的占用预期。
  bool spawnPlayerEntity = false;
  // 关闭发送队列字节/包数背压限制与区块分批吞吐上限（适合高视距本地联机调试）。
  bool unlimitedNetwork = false;
  // 区块就绪时立即下发客户端（即使光照尚未收敛），光照完成后再对仍可见区块补发 light_update。
  bool sendChunksBeforeLightReady = true;
};
// 单条连接的确定性协议状态机，与底层 Socket I/O 完全解耦，便于独立单测与无锁主循环推进。
// 所有成员调用以及 World::tick 均在服务端主线程上串行执行。
class Session {
public:
  Session(World &, const ProtocolData &, ServerOptions);
  ~Session();
  void receive(std::span<const std::uint8_t> bytes);
  void tick();
  // 若当前世界尚无活跃的玩家实体，则按 options_.playerId 为本会话挂载一个权威 ServerPlayer 实体。
  void attachPlayerEntity();
  std::shared_ptr<ServerPlayer> playerEntity() const noexcept {
    return playerEntity_;
  }
  void sendTakeItemEntity(int collectorId, int targetId, int amount);
  // 仅用于受管加载票世界（ticketed-world）：该指针在 Session 生命周期内归服务端所有。
  // 离线投影会话保持不设置，从而沿用旧版空的 set_time 映射语义。
  void setWorldClockSource(const WorldClocks &, bool advanceTime);
  // 对应 ServerClockManager.modifyClock：仅发送发生变更的那一条世界时钟记录。
  void sendWorldClockUpdate(BuiltinWorldClock);
  // 在游戏阶段（Play）向客户端同步当前世界的方块标签快照。
  void sendBlockTagUpdate();
  // 在 World::tick 的区块与实体广播阶段调用，统一推送实体增量同步包。
  void broadcastEntities();
  void entityAdded(const Entity &);
  void entityRemoved(int entityId);
  // 在单刻计时完成后调用；发包耗时本身不计入该刻的挂钟持续时间统计。
  void sendTickTime(double milliseconds, std::int64_t tick, std::size_t samples = 1);
  void blockUpdate(Pos, const BlockState &);
  void flushBlockUpdates();
  void blockEvent(const BlockEvent &);
  void explosion(Vec3 center, float radius, int blockCount);
  void fallingBlockLanded(int entityId, Pos, const BlockState &);
  // 光照工作线程向主线程投递的快照发布回调：首次发送区块会等待协议就绪的光照快照；
  // 后续更新则在本回调返回后由不可变快照异步编码，不阻塞主线程。
  void lightPublished(std::shared_ptr<const LightChunkSnapshot> snapshot);
  // 显式世界区块生命周期通知：区别于玩家视野边缘变化，可在服务端主线程回调中安全调用。
  void lightChunkLifecycle(int chunkX, int chunkZ, bool loaded);
  // 可选启用的受管世界区块驻留钩子（旧版会话从不安装）。
  // 变为驻留的区块才对网络可见；真正的受管移除会立即作废待发送队列及区块代际令牌。
  void setManagedResidencyProvider(
      std::function<bool(int, int)>,
      std::function<std::vector<std::pair<int, int>>()>);
  void managedChunkResidency(int chunkX, int chunkZ, bool resident);
  bool hasManagedChunk(int chunkX, int chunkZ) const;
  // 绑定指令树与运行时上下文：两者的宿主都在服务端主循环中，生命周期覆盖所有会话。
  void attachCommands(const command::Dispatcher *, const command::Context *);
  void sendCommands();
  const std::string &currentDimensionId() const noexcept {
    return currentDimensionId_;
  }
  const std::string &currentDimensionTypeId() const noexcept {
    return currentDimensionTypeId_;
  }
  World &world() noexcept { return *world_; }
  const World &world() const noexcept { return *world_; }
  // 发送 ClientboundRespawnPacket 并将客户端区块、实体与光照跟踪状态重置到 targetWorld
  //（D-2 阶段实现，暂不含传送门搜索与生成）。
  void switchDimension(World &targetWorld, const DimensionStem &stem,
                       Vec3 spawnPos, float yaw = 0, float pitch = 0);
  // 发送 ClientboundTickingStatePacket / ClientboundTickingStepPacket 同步服务端 tick 速率/步进状态。
  void sendTickingState(const TickRateManager &);
  void sendTickingStep(const TickRateManager &);
  // 发送单条指令反馈消息（ClientboundSystemChatPacket）。
  void sendCommandMessage(const command::Message &);
  std::vector<Bytes> takeOutput();
  // 排空尚未组帧的 OutPacket 工作项，交由网络写线程编码发送。
  std::vector<OutPacket> takeOutputItems();
  Phase phase() const { return phase_; }
  bool closed() const { return phase_ == Phase::Closed; }
  const std::string &error() const { return error_; }
  Vec3 playerPosition() const { return position_; }
  int playerId() const { return options_.playerId; }
  const std::array<ItemStack, 41> &inventory() const { return inventory_; }
  int openContainerId() const {
    return openContainer_ ? openContainer_->containerId : 0;
  }
  const ItemStack &carriedItem() const { return carriedItem_; }
  std::function<void(std::string_view)> diagnostic;

private:
  // 对应原版 ServerEntity 跟踪状态：保存 VecDeltaCodec 坐标基准、量化旋转角以及更新/传送计时器；
  // 本会话当前可见的每个实体对应一条记录。
  struct TrackedEntity {
    Vec3 base;
    Vec3 lastMovement;
    std::uint64_t lastMotionRevision = 0;
    std::uint64_t lastDataRevision = 0;
    std::uint8_t yRot = 0, xRot = 0;
    std::vector<int> lastPassengerIds;
    int tickCount = 0;
    int teleportDelay = 0;
    std::uint64_t lastSeenGeneration = 0;
    bool wasOnGround = false;
  };
  void handle(Reader &);
  void play(int, Reader &);
  void join();
  void finishJoin();
  void finishRespawn();
  void resetDimensionViewState();
  void writeCommonPlayerSpawnInfo(Writer &, const std::string &dimId,
                                    int dimTypeRegistryId,
                                    std::int64_t hashedSeed, bool isFlat,
                                    int seaLevel) const;
  void writeWorldClock(Writer &, BuiltinWorldClock) const;
  void sendSetTime();
  void send(std::string_view name, const Writer &payload = {});
  void send(std::string_view name, Writer &&payload);
  void packet(int id, const Writer &payload = {});
  void packet(int id, Writer &&payload);
  void enqueue(OutPacket packet);
  void queue(Bytes bytes);
  void queueDeferred(std::function<Bytes()> producer,
                     std::size_t retainedBytes,
                     std::function<bool()> stillValid = {});
  void disconnect(const std::string &);
  void acknowledge(int sequence);
  void sendBlockUpdate(Pos);
  void setSlot(int slot, ItemStack stack);
  void stack(Writer &, const ItemStack &) const;
  void syncEntities();
  void sendEntitySpawn(const Entity &, TrackedEntity &);
  void sendEntityChanges(const Entity &, TrackedEntity &);
  void sendEntityDataValues(int entityId,
                            std::span<const EntityDataEntry> entries);
  void sendPassengers(const Entity &, TrackedEntity &);
  // 仅刷新可用区块集合；Play 阶段的视野数据包必须由 join() 在强制登录包之后发送，
  // 绝不能在配置完成阶段提前触发。
  void syncManagedResidency();
  void refreshChunkView();
  void sendChunkBatch();
  bool chunkVisible(std::pair<int, int>) const;
  void queuePendingChunk(std::pair<int, int>);
  void expireChunk(std::pair<int, int>);
  bool hasChunk(Pos) const;
  void positionPacket(Writer &, Vec3 pos, Vec3 motion, float yaw, float pitch);
  bool canInteract(Pos) const;
  void useItemOn(Reader &);
  void useItem(Reader &);
  void playerInteract(Reader &);
  void playerAction(Reader &);
  void creativeSlot(Reader &);
  void openContainerScreen(Pos, const BlockState &);
  void closeOpenContainer(bool sendPacket);
  void syncOpenContainer();
  void sendContainerContent();
  void containerClick(Reader &);
  void containerClose(Reader &);
  void detachPlayerEntity();
  void runCommand(std::string_view line);
  void sendSuggestions(int requestId, std::string_view line);
  World *world_;
  const ProtocolData &data_;
  ServerOptions options_;
  std::shared_ptr<ServerPlayer> playerEntity_;
  std::map<int, Uuid> knownPlayerInfo_;
  std::string currentDimensionId_;
  std::string currentDimensionTypeId_;
  const command::Dispatcher *commands_ = nullptr;
  const command::Context *commandContext_ = nullptr;
  const WorldClocks *worldClocks_ = nullptr;
  bool advanceTime_ = true;
  Phase phase_ = Phase::Handshake;
  bool awaitingKnownPacks_ = false, finishSent_ = false,
       teleportPending_ = true;
  bool sneaking_ = false;
  std::uint64_t configuredBlockTagRevision_ = 0;
  Bytes input_;
  std::vector<OutPacket> output_;
  std::size_t outputBytes_ = 0;
  std::string username_, error_;
  Uuid uuid_{};
  Vec3 position_;
  float yaw_ = 0, pitch_ = 0;
  std::array<ItemStack, 41> inventory_{};
  int selectedSlot_ = 0;
  struct OpenContainer {
    int containerId = 0;
    int menuTypeId = 0;
    Pos pos{};
    ContainerKind kind = ContainerKind::Chest;
    int slotCount = 0;
    int stateId = 1;
    std::uint64_t lastSeenModCount = 0;
    int quickCraftMode = -1;
    int quickCraftEvent = 0;
    std::vector<int> quickCraftSlots;
  };
  std::optional<OpenContainer> openContainer_;
  int nextContainerId_ = 1;
  ItemStack carriedItem_;
  std::map<int, TrackedEntity> trackedEntities_;
  struct ChangedBlock {
    std::uint64_t section;
    std::uint32_t order;
    std::uint16_t local;
  };
  std::vector<ChangedBlock> changedBlocks_;
  std::set<std::pair<int, int>> availableChunks_, sentChunks_;
  std::vector<std::pair<int, int>> pendingChunks_;
  std::set<std::pair<int, int>> awaitingLightChunks_;
  std::function<bool(int, int)> managedResidency_;
  std::function<std::vector<std::pair<int, int>>()> managedChunks_;
  std::set<std::pair<int, int>> managedPendingResidency_;
  std::map<std::pair<int, int>, std::shared_ptr<std::atomic<bool>>>
      chunkGenerations_;
  // 独立维护的逐层（方块光/天空光）增量基准：仅保存层版本号、区块尺寸以及
  // 光照工作线程提供的脏区段元数据；页内容变为可变后绝不在主线程做字节级差分。
  std::map<std::pair<int, int>, LightLayerVersion> sentChunkBlockLight_;
  std::map<std::pair<int, int>, LightLayerVersion> sentChunkSkyLight_;
  std::optional<std::pair<int, int>> chunkCenter_;
  bool awaitingChunkBatch_ = false;
  bool inSendChunkBatch_ = false;
  bool joinPositionPending_ = false;
  bool respawning_ = false;
  int pendingTeleportId_ = 1;
  int chunksPerBatch_ = 9;
  int chunkBatchLimit_ = 9;
  int dropSpam_ = 0;
  std::int64_t connectionTicks_ = 0, keepAliveSent_ = 0;
  std::uint64_t syncGeneration_ = 0;
  std::optional<std::int64_t> keepAlive_;
};
} // namespace redstone::net
