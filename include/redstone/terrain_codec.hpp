#pragma once
// 区块 NBT 编解码与局部原地补丁接口：负责 Anvil 区段调色板位打包解析、区块校验，以及面向后台存盘线程的零克隆 NBT 增量写回。

#include "redstone/block.hpp"
#include "redstone/heightmap.hpp"
#include "redstone/nbt.hpp"
#include "redstone/region.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace redstone::terrain {

// 区分区块缺失、结构损坏与含未支持方块等结果，供区块生命周期管理层决定跳过、降级还是报错。
enum class DecodeStatus {
  Ok,
  Missing,
  Corrupt,
  Unsupported,
};

// 区段调色板中的单个方块状态条目，同时保留原始 NBT 名称与属性以便诊断不支持的状态。
struct PaletteEntry {
  std::string name;
  std::map<std::string, std::string> properties;
  BlockState state;
  bool supported = false;
};

// 还原 Anvil 区段 block_states 的调色板与 64 位整数位打包存储（4096 格），避免直接展开成庞大对象数组。
struct PackedBlockStates {
  int bitsPerEntry = 0; // 0 表示该区段仅含单一调色板值且无 data 位流。
  std::vector<PaletteEntry> palette;
  std::vector<std::uint64_t> data;
  std::size_t cell(std::size_t index) const;
  static constexpr std::size_t CellCount = 4096;
};

// 还原 Anvil 区段 biomes 的 4x4x4 Quart 生物群系调色板（64 格，位宽 0..6）。
struct PackedBiomes {
  int bitsPerEntry = 0; // 0 表示单值生物群系且无 data 位流。
  std::vector<std::string> palette;
  std::vector<std::uint64_t> data;
  std::size_t cell(std::size_t index) const;
  static constexpr std::size_t CellCount = 64;
};

// 单个 16x16x16 区块区段的解码结果，按需携带方块状态、4x4x4 生物群系与方块/天空光照半字节数组。
struct Section {
  std::int8_t y = 0;
  std::optional<PackedBlockStates> blockStates;
  std::optional<PackedBiomes> biomes;
  std::optional<std::array<std::uint8_t, 2048>> blockLight;
  std::optional<std::array<std::uint8_t, 2048>> skyLight;
};

// 轻量化的延迟刻快照，用于在解码/存盘线程与主线程模拟引擎之间传递 block_ticks / fluid_ticks。
struct SavedTick {
  std::string id;
  int x = 0, y = 0, z = 0;
  int delay = 0;
  int priority = 0;
};

// 紧凑的移动活塞（minecraft:piston）方块实体快照，使跨线程加载/存盘无需传递完整 nbt::Tag DOM。
struct MovingPistonEntity {
  int x = 0, y = 0, z = 0;
  BlockState movedState;
  std::uint8_t facing = 0;
  float progressO = 0.0f;
  bool extending = false;
  bool source = false;
};

// 控制区块解码时的所有权保留策略与未实现方块容忍度。
struct DecodeOptions {
  // 测试夹具或调试调用方可以要求保留原始 NBT DOM；线上受管加载路径仅消费紧凑记录，并将根 NBT 留在工作线程持有。
  bool retainRawBlockEntities = false;
  // 只读离线基准测试可允许解码已注册但模拟行为尚未实现的 26.1 方块状态；
  // 生产加载与存盘路径必须保持 false，从而拒绝包含未支持方块的区块，避免静默丢失行为语义。
  bool allowUnimplementedBlocks = false;
  // 默认仅接受 `minecraft:full` 完整区块，兼容原有“非满块拒绝”契约；
  // 世界生成期（ProtoChunk 恢复与继续推进）设为 false 以解码任意 12 阶段状态。
  bool requireFullStatus = true;
};

// 单个区块的完整解码载体；同时保留 root 以便后续存盘直接在其上打补丁，而不丢失未解析的原版字段。
struct DecodedChunk {
  DecodeStatus status = DecodeStatus::Corrupt;
  std::string detail;
  std::int32_t chunkX = 0, chunkZ = 0;
  std::string statusName;
  bool isLightOn = false;
  std::int64_t inhabitedTime = 0;
  std::vector<Section> sections;
  std::vector<std::uint64_t> carvingMask;
  std::vector<std::vector<std::int16_t>> postProcessing;
  std::map<HeightmapType, std::vector<std::uint64_t>> heightmaps;
  std::vector<SavedTick> blockTicks;
  std::vector<SavedTick> fluidTicks;
  std::vector<MovingPistonEntity> movingPistons;
  bool hasUnsupportedBlockEntities = false;
  std::string blockEntityError;
  std::vector<nbt::Tag> blockEntities;
  std::vector<nbt::Tag> entities;
  nbt::RootTag root;
};

// 从未压缩的区块 NBT 字节流或指定 region 目录解码单个区块。
DecodedChunk decodeChunkNbt(std::span<const std::uint8_t> nbtBytes,
                            DecodeOptions options = {});
DecodedChunk decodeChunkAt(const std::filesystem::path &regionDirectory,
                           int chunkX, int chunkZ,
                           DecodeOptions options = {});

// 面向测试夹具等常规路径的单格方块修改描述。
struct BlockEdit {
  std::int8_t sectionY = 0;
  std::uint16_t localIndex = 0;
  BlockState state;
};

// 主线程向后台存盘线程传递的紧凑修改记录。sectionY 与 localIndex 共同定位所属区块中的一个网格单元，
// stateId 为进程内稳定的 BlockState 注册表 ID。8 字节包含全部字段与对齐填充；
// 存在原因：在区块高频存盘快照中消除冗余字符串/状态拷贝，显著降低跨线程传输与按位置去重开销。
struct CompactBlockEdit {
  std::int8_t sectionY = 0;
  std::uint16_t localIndex = 0;
  BlockState::Id stateId = 0;
};
static_assert(sizeof(CompactBlockEdit) == 8,
              "compact save edit must remain one 8-byte record");
// 将 (sectionY, localIndex) 打包为单一 20 位整型键，便于快照构建时高效排序与去重。
inline std::uint32_t compactCellKey(const CompactBlockEdit &edit) {
  return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(edit.sectionY))
          << 12) |
         edit.localIndex;
}
inline CompactBlockEdit compactEdit(const BlockEdit &edit) {
  return {edit.sectionY, edit.localIndex, edit.state.id()};
}

// 汇报原地方块补丁应用结果（修改的格子数、重打包的区段数以及错误信息）。
struct PatchResult {
  bool edited = false;
  std::string error;
  std::size_t changedCells = 0;
  std::size_t repackedSections = 0;
};

// 原地修改调用方持有的已解码区块 NBT 根节点。修改按区段分组，同一单元以最后一次修改为准，
// 且每个受影响的区段仅重新打包一次。绝不克隆整棵根节点。
// nbt::Tag 可表达的结构与数值均会保留；但对于 DOM 无法无损表达的载荷（例如非规范 IEEE NaN 载荷），
// 不保证输出逐字节完全一致。传入的 root 必须来自一次成功的 decodeChunkNbt 或 decodeChunkAt 调用。
PatchResult patchBlockStates(nbt::RootTag &root,
                             std::span<const BlockEdit> edits);
// 生产环境存盘专用路径：仅在应用每个目标单元时按需解析 stateId，全程不物化 BlockEdit 向量。
PatchResult patchBlockStatesCompact(
    nbt::RootTag &root, std::span<const CompactBlockEdit> edits);

// 只读存盘前校验：复用与 patchBlockStates / decode 完全相同的 DataVersion/FULL、区段调色板、
// 位打包存储及已支持状态规则；既不分配已解码区段载荷，也不会放宽支持范围。
// `allowRegisteredUnsupported` 只放宽“已注册但未实现模拟”的方块状态（世界生成产出），
// 未注册名称、损坏调色板等仍然拒绝。
bool validateChunkRootForSave(const nbt::RootTag &root, std::string &error,
                              bool allowRegisteredUnsupported = false);

// 描述区块 block_ticks 与 fluid_ticks 的覆盖补丁。
struct TickListPatch {
  // nullopt 表示保持对应已保存列表不变；若为已赋值的空 vector，则会显式清空该列表。
  std::optional<std::vector<SavedTick>> blockTicks;
  std::optional<std::vector<SavedTick>> fluidTicks;
};
// 仅原地替换已持有区块根节点中的 block_ticks / fluid_ticks，其余所有 NBT 字段保持原样。
// 存盘时绝不在 NBT 中凭空捏造子刻顺序（sub-tick order）。
bool patchChunkTicks(nbt::RootTag &root, const TickListPatch &patch,
                     std::string &error);

// 用紧凑的移动活塞列表替换 block_entities（空 span 也会精确写为空列表），其余根字段保持不变。
// 在真正修改所持有的 root 之前会先执行完整校验，保证失败时不产生部分修改。
bool patchMovingPistonEntities(
    nbt::RootTag &root,
    std::span<const MovingPistonEntity> entities,
    std::string &error);

// 单个区段的光照更新载荷（分别对应 2048 字节半字节数组）。
struct SectionLightPatch {
  std::int8_t y = 0;
  std::optional<std::array<std::uint8_t, 2048>> blockLight;
  std::optional<std::array<std::uint8_t, 2048>> skyLight;
};

// 整区块的光照补丁载荷，由光照工作线程或区块存盘快照生成。
struct ChunkLightPatch {
  bool isLightOn = false;
  std::vector<SectionLightPatch> sections;
};

// 按照原版 26.1 SerializableChunkData 规则原地替换 isLightOn 与各区段 BlockLight/SkyLight 半字节数组
// （全零的空光照层会被省略，不写入 NBT）。
bool patchChunkLight(
    nbt::RootTag &root,
    const ChunkLightPatch &patch,
    std::string &error);

// 仅用于测试夹具兼容性：编辑前会完整克隆一次根节点。
// 生产存盘路径必须直接对自持有的 root 调用 patchBlockStates / patchBlockStatesCompact。
struct EditResult {
  bool edited = false;
  std::string error;
  nbt::RootTag root;
};
EditResult setBlockState(const DecodedChunk &chunk, std::int8_t sectionY,
                         std::uint16_t localIndex, const BlockState &state);
// 直接将已持有的 root 移动进兼容结果对象中，避免复制整棵根节点。
EditResult setBlockState(DecodedChunk &&chunk, std::int8_t sectionY,
                         std::uint16_t localIndex, const BlockState &state);
// 将区块 NBT 根节点序列化为字节流，用于写入 Anvil 扇区。
std::vector<std::uint8_t> encodeChunkNbt(const nbt::RootTag &root);
std::vector<std::uint8_t> encodeChunkNbt(const DecodedChunk &chunk);

} // namespace redstone::terrain
