#pragma once
// 世界存档、元数据、游戏规则、世界时钟、持久化加载票以及 Anvil 世界导入接口。
#include "redstone/dimension.hpp"
#include "redstone/engine.hpp"
#include "redstone/nbt.hpp"
#include "redstone/tickets.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace redstone {
// 用于在磁盘存档 / Litematica 投影与引擎内部计划刻队列之间传递单条延迟刻记录。
struct SavedTickData {
  Pos pos;
  std::string type;
  int delay{}, priority{};
  bool fluid{};
  // Litematica 会把原始的 ScheduledTick.subTickOrder 记录为 SubTick；
  // 原版区块存档格式不会记录该值，而是在加载时按已保存列表顺序重建为 -N..-1（LevelChunkTicks.unpack）。
  // 缺失表示“使用原版按区块重建的序列”，存在表示“保留投影自带的子刻顺序”，
  // 从而避免两种排序规则混用（见 LitematicaSchematic.java:1498,2834）。
  std::optional<std::int64_t> subTick;
};
// 封装当前模拟核心依赖的原版游戏规则子集，避免业务层直接耦合 game_rules.dat 的 NBT 结构。
struct WorldGameRules {
  bool advanceTime = true;
  bool tntExplodes = true;
  bool waterSourceConversion = true;
  bool lavaSourceConversion = false;
  int randomTickSpeed = 3;
};

// 表示单个世界时钟的运行时状态（总刻数、子刻小数进度、倍率与暂停标志）。
struct WorldClockState {
  std::int64_t totalTicks = 0;
  float partialTick = 0.0f;
  float rate = 1.0f;
  bool paused = false;
};
enum class BuiltinWorldClock : std::uint8_t { Overworld, TheEnd };
// 对应原版 ServerClockManager 内核及 26.1 版本 data/minecraft/world_clocks.dat SavedData 中已支持的部分。
// 存在原因：26.1 将时间系统从单纯的 gameTime 拆分为独立时钟，且存档中可能带有未知的自定义时钟或扩展字段；
// 本类内部保留完整 NBT 文档，修改已支持时钟时绝不丢弃未识别的顶层数据、时钟条目与字段。
class WorldClocks {
public:
  WorldClocks();
  static WorldClocks load(const std::filesystem::path &path);
  const WorldClockState &state(BuiltinWorldClock clock) const;
  // 仅当请求的状态与当前状态存在差异时返回 true 并标记脏状态；随后在 saveIfDirty 成功时写盘。
  bool setClock(BuiltinWorldClock clock, const WorldClockState &state);
  // 遵循原版推进顺序：MinecraftServer.tickChildren 仅在 tick-rate 管理器正常运行（runsNormally）时
  // 调用 clocks.tick，再由 ServerClockManager.tick 结合 advanceTime 规则决定是否推进时钟。
  void tick(bool runsNormally, bool advanceTime);
  bool dirty() const { return dirty_; }
  // 返回 false 表示磁盘原有字节未被触动。写入成功后会清除 dirty 标志；
  // 若发生 I/O 或编码错误则抛出异常并保持 dirty 状态。
  bool saveIfDirty(const std::filesystem::path &path);
private:
  std::array<WorldClockState, 2> states_{};
  nbt::RootTag document_;
  bool dirty_ = false;
};

// 聚合跨维度共享的世界级元数据，供多维度运行时初始化、/time 控制与存档回写共享。
struct WorldMetadata {
  std::int64_t gameTime = 0;
  std::int64_t seed = 0;
  WorldGameRules gameRules;
  WorldClocks worldClocks;
  std::vector<DimensionStem> dimensions;
  // 已解析的 level.dat 根节点始终作为唯一的可写元数据模板（以保留未建模的原版字段）；
  // 读取路径在返回前会校验 DataVersion 与 Data.Time。
  nbt::RootTag levelRoot;

  const DimensionStem *findDimension(std::string_view id) const;
};
// 加载为只读操作。setWorldMetadataTime 会原地修改 levelRoot 中的 Time；
// saveWorldMetadata 通过“临时 gzip 文件 + 重命名”写入，若原文件存在则保留为 level.dat_old，
// 任何失败都不会破坏现有的 level.dat。
void setWorldMetadataTime(WorldMetadata &metadata, std::int64_t gameTime);
void saveWorldMetadata(const std::filesystem::path &worldDir,
                       const WorldMetadata &metadata);
// 加载票数据文件路径固定为：<所选维度根目录>/data/minecraft/chunk_tickets.dat。
std::filesystem::path ticketDataPath(const std::filesystem::path &regionDirectory);
// 表示持久化在 chunk_tickets.dat 中的单条区块加载票（如 forced、portal），用于重启后恢复常加载需求。
struct PersistentTicketRecord {
  std::int32_t x = 0, z = 0;
  std::string type;
  std::int32_t level = 0;
  std::int64_t ticksLeft = 0;
};
// 文件缺失时返回空列表；若外层 SavedData 封包畸形或包含不支持的加载票类型，
// 则必须报错并在启动流程安装任何加载需求之前拒绝加载。
std::vector<PersistentTicketRecord>
loadPersistentTickets(const std::filesystem::path &path);
// 当文件不存在且列表为空时调用方可跳过写入，避免产生多余空文件。
// 写入时使用严格的 26.1 SavedDataStorage gzip 封包格式，并仅替换该目标文件。
void savePersistentTickets(const std::filesystem::path &path,
                           std::span<const PersistentTicketRecord> tickets);
// 离线整世界加载快照，用于测试夹具或中小规模存档经由 importWorld 一次性导入 World。
struct LoadedWorld {
  std::int64_t gameTime{};
  int minY = -64, maxY = 320;
  std::map<Pos, BlockState> blocks;
  std::vector<std::pair<int, int>> chunks;
  std::vector<SavedTickData> ticks;
  std::vector<nbt::Tag> blockEntities;
  std::vector<nbt::Tag> entities;
  std::unordered_map<std::uint64_t, PersistedChunkLight> persistedLights;
  std::int64_t seed = 0;
};
// 26.1 命名空间化的主世界路径（dimensions/minecraft/overworld/region）优先于旧版 world/region。
std::filesystem::path defaultRegionDirectory(const std::filesystem::path &worldDir);
// regionDir 为所选维度的 region 目录；读取其同级 entities 目录时不会触发额外地形区块生成或加载。
// worldDir 包含 level.dat 及命名空间化的 data/minecraft 持久化数据。
// 仅读取 level.dat 与命名空间内的种子、规则及世界时钟元数据，不扫描地形区块。
WorldMetadata loadWorldMetadata(const std::filesystem::path &worldDir);
// 按 level.dat 中启用的顺序（从低优先级到高优先级）解析文件数据包路径，供方块标签等数据叠加加载。
struct WorldDataPackSelection {
  std::vector<std::filesystem::path> paths;
  std::vector<std::string> diagnostics;
};
WorldDataPackSelection selectWorldDataPacks(const std::filesystem::path &worldDir,
                                            const nbt::RootTag &levelRoot);
// 仅读取已存在的 26.1 命名空间 game_rules SavedData（game_rules.dat）。
// 文件缺失时返回原版默认值；文件存在但格式畸形时报错。
WorldGameRules loadWorldGameRules(const std::filesystem::path &path);
// 控制 Anvil 世界导入时的垂直范围、非完整区块策略及区块坐标过滤条件。
struct AnvilLoadOptions {
  int minY = -64;
  int maxY = 320;
  // 默认 false：遇到非 "minecraft:full" 状态的 proto-chunk 时抛出 nbt::Error，提醒调用方区块尚未生成完整。
  // 设为 true（通过 --skip-non-full-chunks）可跳过边界处部分生成的区块，直到边界 proto-chunk 加载实现完成。
  // TODO: 直接加载非 full 状态的边界 proto-chunk，而非仅跳过它们。
  bool skipNonFullChunks = false;
  std::function<bool(int, int)> chunkFilter{};
};
// 记录流式导入 Anvil 世界后的统计信息，便于日志输出与测试断言。
struct AnvilWorldImportSummary {
  std::vector<std::pair<int, int>> loadedChunks;
  std::size_t skippedNonFullChunks = 0;
  std::size_t loadedBlockTicks = 0;
  std::size_t loadedFluidTicks = 0;
  std::size_t loadedBlockEntities = 0;
  std::size_t loadedEntities = 0;
};
LoadedWorld loadAnvilWorld(const std::filesystem::path &worldDir,
                           const std::filesystem::path &regionDir,
                           const AnvilLoadOptions &options);
LoadedWorld loadAnvilWorld(const std::filesystem::path &worldDir,
                           const std::filesystem::path &regionDir,
                           int minY = -64, int maxY = 320);
// 直接将解码后的 Anvil 区段流式写入 `world`，不在内存中构建中间产物 LoadedWorld::blocks std::map；
// 专门面向包含数千个区块的大型存档，避免巨额峰值内存与红黑树构造开销。
AnvilWorldImportSummary
loadAnvilWorldInto(const std::filesystem::path &worldDir,
                   const std::filesystem::path &regionDir, World &world,
                   const AnvilLoadOptions &options = {});
// 将离线加载的 LoadedWorld 快照批量写入运行中的 World 实例。
void importWorld(const LoadedWorld &input, World &world);
} // namespace redstone
