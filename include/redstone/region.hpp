#pragma once
// Minecraft Anvil 地区文件（r.x.z.mca）与外部超限区块文件（c.x.z.mcc）的底层扇区级读写接口。
// 存在原因：将 4KiB 扇区分配、压缩编解码与外部大区块文件管理从地形/实体/POI 解析中解耦，
// 供 region、entities 与 poi 三类存储目录共用同一套可靠的原版容器读写逻辑。

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace redstone::region {

// 区分区块缺失与文件损坏/压缩不受支持，防止上层将磁盘损坏误判为未生成区块。
enum class ReadStatus {
  Found,
  Missing,
  Corrupt,
  Unsupported,
};

// 描述目标区块在地区目录中的文件定位及槽位索引（0..1023），避免负坐标整除逻辑在各处重复。
struct ChunkSlot {
  int chunkX = 0;
  int chunkZ = 0;
  int regionX = 0;
  int regionZ = 0;
  int slot = 0;
  std::filesystem::path path;

  bool operator==(const ChunkSlot &) const = default;
};

// 区块原始 NBT 读取结果；通过状态码与诊断文本返回失败原因，而非模糊地返回空字节。
struct ReadResult {
  ReadStatus status = ReadStatus::Missing;
  std::vector<std::uint8_t> nbt;
  std::string detail;

  explicit operator bool() const { return status == ReadStatus::Found; }
};

// 区块原始 NBT 写入结果，记录是否落入外部 .mcc、分配扇区位置及非致命清理告警。
struct WriteResult {
  bool wrote = false;
  bool external = false;
  std::uint32_t sectors = 0;
  std::uint32_t sectorNumber = 0;
  int timestampSeconds = 0;
  std::filesystem::path mccPath;
  bool replacedExternal = false;
  // 目标载荷已提交落盘，但旧的 .mcc 外部文件未能删除；
  // 这是带清理告警的成功写入，绝不表示回滚信号。
  bool cleanupPending = false;
  std::string warning;
};

// 将区块坐标解析到其所属维度地区目录中的 .mca 槽位；负数区块坐标采用向下取整除法计算地区坐标。
ChunkSlot locateChunk(const std::filesystem::path &regionDirectory,
                      int chunkX, int chunkZ);

// 仅读取目标槽位所占扇区并返回解压后的原始 NBT 字节流。
// 明确区分缺失、畸形损坏、不支持的压缩方式以及外部文件故障，不调用任何世界或区块解析器。
ReadResult readChunkNbt(const std::filesystem::path &regionDirectory,
                        int chunkX, int chunkZ);

// 按 26.1 默认的 deflate（版本 2）格式写入单个区块的原始 NBT 载荷，保持其他区块槽位
// 与外部文件完好。当新载荷需要不少于 256 个扇区时写入外部 .mcc 文件，否则直接写入新分配的
// .mca 扇区。通过目录内临时文件与重命名完成外部载荷替换（不声明提供全文件系统级原子性）。
WriteResult writeChunkNbt(const std::filesystem::path &regionDirectory,
                          int chunkX, int chunkZ,
                          std::span<const std::uint8_t> nbt,
                          int timestampSeconds = 0);

} // namespace redstone::region
