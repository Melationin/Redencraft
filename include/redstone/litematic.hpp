#pragma once
// Litematica（.litematic）投影文件导入接口，用于将外部红石电路或建筑快照离线载入 LoadedWorld。
// 存在原因：在缺乏实时游戏内加载/建造工具时，提供可审计、无静默猜测的投影测试与调试输入通道。
#include "redstone/storage.hpp"
#include <functional>
#include <map>

namespace redstone {
// 投影导入配置项：用于在不改动源投影文件的情况下控制偏移放置、世界高度裁剪与状态校验严格度。
struct LitematicOptions {
  Pos offset{0, 64, 0};
  int minY = -64, maxY = 320;
  // 调用方可用于按官方方块状态 ID 校验完整的方块状态与属性组合。
  std::function<void(const BlockState &)> validateState;
  // 预览导入开关：用于包含本模拟器尚未实现方块的投影。启用后所有未实现方块状态
  // 都会替换为 smooth_stone 并丢弃其方块实体，同时在 LitematicImport::substitutedBlocks
  // 中记录替换详情，确保替换过程绝不静默发生。默认关闭，未实现方块直接报错而非猜测。
  bool substituteUnsupported = false;
};
// 投影导入结果及兼容处理审计清单。
// 存在原因：不仅返回构建好的 LoadedWorld，还如实汇报被跳过、替换或重生成 UUID 的内容，
// 防止调用方对投影差异一无所知。
struct LitematicImport {
  LoadedWorld world;
  std::size_t replacedAnvils{}, skippedSigns{}, skippedSignBlockEntities{};
  std::size_t regeneratedEntityUuids{}, regionCount{};
  std::size_t skippedUnsupportedBlockEntities{};
  std::size_t skippedUnsupportedEntities{};
  // 名称 -> 被丢弃的待执行计划刻数量，确保宽松模式导入时绝不会静默丢弃计划刻。
  std::map<std::string, std::size_t> skippedUnsupportedBlockTicks;
  std::map<std::string, std::size_t> skippedUnsupportedFluidTicks;
  // 源方块名称 -> 被替换为 smooth_stone 的格数。
  std::map<std::string, std::size_t> substitutedBlocks;
  Pos min, maxExclusive;
  int sourceDataVersion{};
};
// 快照式导入：不触发 onPlace/邻居更新通知，也不凭空制造计划刻。
// 方块、方块实体与计划刻坐标均相对于子区域最小角，实体坐标相对于子区域 Position。
// 当前仅接受未旋转、未镜像的 Version 6/7、SubVersion 1 格式。
LitematicImport loadLitematic(const nbt::Tag &root,
                              const LitematicOptions &options = {});
// 从磁盘路径读取 gzip 压缩的 .litematic 文件并委托给 loadLitematic(const nbt::Tag &, ...) 解析。
LitematicImport loadLitematic(const std::filesystem::path &path,
                              const LitematicOptions &options = {});
} // namespace redstone
