#pragma once
// 多维度服务端世界管理：封装各维度的运行时实例（DimensionLevel）与跨维度协同入口（ServerWorldSet）。
#include "redstone/dimension.hpp"
#include "redstone/engine.hpp"
#include "redstone/storage.hpp"
#include "redstone/world_chunk_manager.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {

namespace worldgen {
class ChunkGenerator;
class WorldgenRegistries;
} // namespace worldgen

// 单个维度在运行时的完整上下文，对应原版的 ServerLevel + LevelChunk 缓存。
// 存在原因：每个维度拥有独立的 World 模拟核心、region 目录、WorldChunkManager、加载票与派生种子，
// 需要集中绑定在一起以便 ServerWorldSet 按维度隔离并统一调度。
struct DimensionLevel {
  DimensionStem stem;
  std::filesystem::path regionDirectory;
  std::unique_ptr<World> world;
  std::unique_ptr<WorldChunkManager> chunkManager;
  // 该维度的 26.1 噪声生成器；未挂载时缺失区块不生成。
  std::shared_ptr<worldgen::ChunkGenerator> generator;
  std::vector<PersistentTicketRecord> initialTickets;
  std::uint64_t seed = 0;
  bool blockTagsLoaded = false;
  std::size_t blockTagPackCount = 0;
};

// 打开多维度世界目录时的配置项，用于控制区块管理、视距、内存策略与维度过滤。
struct ServerWorldOptions {
  bool withChunkManagers = true;
  int viewDistance = 3;
  int simulationDistance = 10;
  int maxNeighborUpdates = 1000000;
  std::vector<std::filesystem::path> extraBlockTagPacks;
  // 可选的维度 ID 过滤列表；为空时加载世界元数据中列出的全部维度。
  std::vector<std::string> dimensionsFilter;
  // 26.1 worldgen 参数目录（含 noise/、density_function/、noise_settings/ 等）。
  // 为空表示不启用世界生成；缺失区块保持原来的 Missing 语义。
  std::filesystem::path worldgenDirectory;
};

// 统一持有共享的服务端元数据（level.dat 中的 Time、GameRules、WorldClocks）以及多维度 26.1 世界目录下的所有活跃 DimensionLevel。
// 存在原因：原版 MinecraftServer 在所有维度之上共享全局 gameTime、WorldClocks、数据包标签与游戏规则，
// 必须在服务器层面统一推进各维度刻循环、排空异步任务、处理跨维度传送门转移并在关服时一致性落盘。
class ServerWorldSet {
public:
  ServerWorldSet() = default;
  ~ServerWorldSet() noexcept;
  ServerWorldSet(const ServerWorldSet &) = delete;
  ServerWorldSet &operator=(const ServerWorldSet &) = delete;
  ServerWorldSet(ServerWorldSet &&) noexcept = default;
  ServerWorldSet &operator=(ServerWorldSet &&) noexcept = default;

  // 打开世界目录：加载元数据与数据包，按需为各维度构造 World / WorldChunkManager 并恢复持久化加载票。
  static ServerWorldSet open(const std::filesystem::path &worldDir,
                             const ServerWorldOptions &options = {});

  const std::filesystem::path &worldDir() const noexcept { return worldDir_; }
  WorldMetadata &metadata() noexcept { return metadata_; }
  const WorldMetadata &metadata() const noexcept { return metadata_; }
  const WorldDataPackSelection &dataPackSelection() const noexcept {
    return packSelection_;
  }
  const std::vector<std::filesystem::path> &blockTagPacks() const noexcept {
    return blockTagPacks_;
  }

  const std::vector<std::unique_ptr<DimensionLevel>> &levels() const noexcept {
    return levels_;
  }
  std::vector<std::unique_ptr<DimensionLevel>> &levels() noexcept {
    return levels_;
  }

  DimensionLevel *find(std::string_view dimensionId);
  const DimensionLevel *find(std::string_view dimensionId) const;
  DimensionLevel *overworld();
  const DimensionLevel *overworld() const;

  // 为所有活跃维度统一启用后台异步光照计算。
  void enableLighting(LightWorkerOptions options = {});
  // 推进一次服务端全局刻：更新 WorldClocks、同步各维度 gameTime、驱动区块或世界刻，并结算跨维度传送门转移。
  void tick(bool runsNormally = true, bool tickChunks = true);
  // 排空所有维度区块管理器的异步完成队列与光照发布队列，确保加载/光照结果及时生效。
  void pumpAll();
  // 关服存盘流程：关闭并刷盘所有维度区块管理器，随后保存全局元数据、世界时钟与加载票。
  void flushAndSaveOnShutdown();
  // 仅保存当前共享元数据（level.dat、world_clocks.dat）及各维度的持久化加载票（chunk_tickets.dat）。
  void saveMetadataAndTickets();
  const std::shared_ptr<PeriodicCleanupManager> &cleanupManager() const noexcept {
    return cleanupManager_;
  }

private:
  std::filesystem::path worldDir_;
  WorldMetadata metadata_;
  WorldDataPackSelection packSelection_;
  std::vector<std::filesystem::path> blockTagPacks_;
  std::shared_ptr<PeriodicCleanupManager> cleanupManager_ =
      std::make_shared<PeriodicCleanupManager>();
  // 生成器持有该注册表的引用，必须比 levels_ 活得久（声明在前、析构在后）。
  std::shared_ptr<const worldgen::WorldgenRegistries> worldgenRegistries_;
  std::vector<std::unique_ptr<DimensionLevel>> levels_;
};

// 基于共享的世界主种子，确定性地为指定维度派生各自的 RNG 种子（主世界直接沿用 worldSeed）。
std::uint64_t deriveDimensionSeed(std::uint64_t worldSeed,
                                  std::string_view dimensionId);

} // namespace redstone
