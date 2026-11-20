#pragma once
// 对齐 Minecraft Java Edition 26.1 的结构模板（`.nbt`）、处理器列表（`processor_list/*.json`）、
// 模板池（`template_pool/**/*.json`）与拼图展开器（`JigsawPlacement.java`）：
// 从 `assets/worldgen26_1/` 实时解析并放置村庄、掠夺者前哨站、古代城市、试炼密室、古迹废墟等 Jigsaw 结构。

#include "redstone/block.hpp"
#include "redstone/flat_hash_map.hpp"
#include "redstone/nbt.hpp"
#include "redstone/worldgen/proto_chunk.hpp"
#include "redstone/worldgen/random_source.hpp"
#include "redstone/worldgen/structure.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace picojson {
class value;
}

namespace redstone::worldgen {

class NoiseBasedChunkGenerator;
class RandomState;
class WorldGenRegion;
class WorldgenRegistries;

struct StructureBlockInfo {
  Pos pos{};
  BlockState state;
  std::optional<nbt::Tag> nbt;
};

struct JigsawBlockInfo {
  StructureBlockInfo info;
  std::string name;
  std::string target;
  std::string pool;
  std::string finalState = "minecraft:air";
  std::string joint = "rollable";
  int placementPriority = 0;
  int selectionPriority = 0;
};

struct StructureEntityInfo {
  double x = 0.0, y = 0.0, z = 0.0;
  Pos blockPos{};
  nbt::Tag nbt;
};

struct StructurePalette {
  std::vector<StructureBlockInfo> blocks;
  std::vector<JigsawBlockInfo> jigsaws;
  std::array<std::vector<JigsawBlockInfo>, 4> jigsawsByRotation;
};

struct StructureTemplate {
  Pos size{};
  std::vector<StructurePalette> palettes;
  std::vector<StructureEntityInfo> entities;

  static StructureTemplate loadFromGzipNbtFile(const std::filesystem::path &p);

  Pos rotatedSize(Rotation rotation) const noexcept;
  BoundingBox boundingBox(Pos anchor, Rotation rotation, Pos pivot,
                          Mirror mirror) const noexcept;
  std::vector<JigsawBlockInfo> getJigsaws(Pos pos, Rotation rotation) const;
  // 返回指定旋转下 jigsaw 列表的稳定引用（palette 选择规则与 getJigsaws 一致），
  // 调用方按 offset=pos 自行平移；用于避免逐次放置时复制整份 JigsawBlockInfo。
  const std::vector<JigsawBlockInfo> &jigsawsFor(Pos pos,
                                                 Rotation rotation) const;

  static Pos transformPos(Pos p, Mirror mirror, Rotation rotation,
                          Pos pivot) noexcept;
  static Pos getZeroPositionWithTransform(Pos zeroPos, Mirror mirror,
                                          Rotation rotation, int sizeX,
                                          int sizeZ) noexcept;
};

struct RulePredicate {
  enum class Kind : std::uint8_t {
    AlwaysTrue,
    BlockMatch,
    BlockStateMatch,
    TagMatch,
    RandomBlockMatch,
    RandomBlockStateMatch,
  };
  Kind kind = Kind::AlwaysTrue;
  std::string blockOrTag;
  BlockState matchState;
  float probability = 1.0f;

  bool test(const BlockState &state, RandomSource &rng) const;
};

struct ProcessorRule {
  RulePredicate inputPredicate;
  RulePredicate locationPredicate;
  BlockState outputState;
};

struct StructureProcessor {
  enum class Kind : std::uint8_t {
    Rule,
    BlockRot,
    ProtectedBlocks,
    Capped,
    BlackstoneReplace,
    LavaSubmerged,
    Nop,
  };
  Kind kind = Kind::Nop;
  std::vector<ProcessorRule> rules;
  float integrity = 1.0f;
  std::string rotTag;
  std::string protectedTag;
  int capLimit = 0;
  std::vector<StructureProcessor> delegate;
};

struct StructureProcessorList {
  std::string id;
  std::vector<StructureProcessor> processors;
};

struct StructurePoolElement {
  enum class Kind : std::uint8_t {
    Empty,
    Single,
    LegacySingle,
    List,
    Feature,
  };
  Kind kind = Kind::Empty;
  std::string location; // template ID or feature ID
  std::string processorListId = "minecraft:empty";
  std::vector<StructureProcessor> inlineProcessors;
  bool projectionRigid = true;
  std::vector<StructurePoolElement> children; // for List
};

struct StructureTemplatePool {
  struct WeightedElement {
    StructurePoolElement element;
    int weight = 1;
  };
  std::string id;
  std::string fallback = "minecraft:empty";
  std::vector<WeightedElement> elements;
};

class StructureTemplateCache {
public:
  explicit StructureTemplateCache(std::filesystem::path nbtRoot)
      : nbtRoot_(std::move(nbtRoot)) {
    cache_.reserve(256);
  }

  const StructureTemplate *getOrLoad(std::string_view templateId) const;

private:
  std::filesystem::path nbtRoot_;
  // 读多写少：命中路径只取共享锁；phmap 扁平哈希 + wyhash 异质查找，
  // 避免 std::map 的逐层字符串比较与每次 normId 分配。
  mutable std::shared_mutex mutex_;
  mutable FlatHashMap<std::string, std::shared_ptr<StructureTemplate>,
                      TransparentStringHash, std::equal_to<>>
      cache_;
};

StructureProcessorList parseProcessorListJson(const picojson::value &v,
                                              std::string_view id);
StructureTemplatePool parseTemplatePoolJson(const picojson::value &v,
                                            std::string_view id);

// 对齐 `JigsawStructure.findGenerationPoint` + `JigsawPlacement.addPieces` 生成拼图结构起始与子件列表。
std::optional<StructureStart> generateJigsawStructureStart(
    const StructureDefinition &def, const NoiseBasedChunkGenerator &gen,
    RandomState &rs, const ChunkGeneratorStructureState &structState,
    std::int64_t worldSeed, ChunkTicketPos chunkPos, int minY, int height);

// 生成特定结构（包含 `Jigsaw`、`Igloo`、`NetherFossil`、`RuinedPortal` 等；暂未移植程序迷宫部件的结构保留合法锚点并标注 TODO）。
std::optional<StructureStart> generateStructureStart(
    const StructureDefinition &def, const NoiseBasedChunkGenerator &gen,
    RandomState &rs, const ChunkGeneratorStructureState &structState,
    std::int64_t worldSeed, ChunkTicketPos chunkPos, int minY, int height);

// 在 `ChunkStatus::FEATURES` 阶段将落入本区块裁剪盒的结构块实际写入 `WorldGenRegion`。
void placeStructurePieceBlocks(const StructurePiece &piece,
                               const StructureTemplateCache &templates,
                               const WorldgenRegistries &registries,
                               WorldGenRegion &region,
                               const BoundingBox &chunkBounds,
                               RandomSource &rng);

// 直接在指定变换与处理器列表下放置结构模板（供 `FossilFeature` 等地物复用模板加载与处理器管线）。
bool placeStructureTemplateWithProcessors(
    std::string_view templateId, std::string_view processorListId,
    Pos position, Rotation rotation, Mirror mirror, Pos pivot,
    const BoundingBox &clipBounds, LiquidSettings liquidSettings,
    const StructureTemplateCache &templates,
    const WorldgenRegistries &registries, WorldGenRegion &region,
    RandomSource &rng, bool useRngForBlockRot, int flags = 18);

} // namespace redstone::worldgen
