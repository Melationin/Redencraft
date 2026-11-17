#pragma once
// 协议 775 线格式（Wire Format）底层编解码原语：定义字节流 Writer/Reader、包帧封装、
// 协议注册表映射、线程安全的冻结区块/光照快照以及异步延迟发包描述符 OutPacket。
#include "redstone/engine.hpp"
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace redstone::command {
struct Node;
}
namespace redstone::net {
using Bytes = std::vector<std::uint8_t>;
// 协议解析/校验失败时抛出的异常（如非法 VarInt、截断载荷或越界数量），用于隔离单条异常连接。
struct ProtocolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// 协议字节流写入器：按 Minecraft 线格式（大端整数、VarInt/VarLong、UTF-8 字符串及打包坐标）追加编码数据。
struct Writer {
  Bytes data;
  void u8(std::uint8_t);
  void bool_(bool);
  void i16(std::int16_t);
  void i32(std::int32_t);
  void i64(std::int64_t);
  void f32(float);
  void f64(double);
  void varInt(std::int32_t);
  void varLong(std::int64_t);
  void string(std::string_view);
  void position(Pos);
  void raw(std::span<const std::uint8_t>);
};
// 协议字节流读取器：基于只读 span 零拷贝解码入站数据包，越界或格式错误时抛出 ProtocolError。
struct Reader {
  explicit Reader(std::span<const std::uint8_t> input) : data(input) {}
  std::span<const std::uint8_t> data;
  std::size_t pos = 0;
  std::size_t remaining() const { return data.size() - pos; }
  bool empty() const { return remaining() == 0; }
  std::uint8_t u8();
  bool bool_();
  std::int16_t i16();
  std::int32_t i32();
  std::int64_t i64();
  float f32();
  double f64();
  std::int32_t varInt();
  std::int64_t varLong();
  std::string string(std::size_t maxBytes = 32767 * 3);
  Pos position();
  std::span<const std::uint8_t> raw(std::size_t);
};
// Minecraft 的 Varint21FrameDecoder 最多允许 3 个长度前缀字节，单帧上限为 2097151 字节。
inline constexpr std::size_t maxFrameSize = 2097151;
Bytes frame(int packetId, std::span<const std::uint8_t> payload);
// 从入站缓冲中消费恰好一个完整协议帧并返回包体（含包 ID）；
// 若长度前缀或载荷尚不完整则保持 input 原封不动。本服务端不协商压缩。
std::optional<Bytes> popFrame(Bytes &input);

// 协议注册表映射：将原版导出的协议数值 ID 与本模拟器内部状态相互转换。
class Registry {
public:
  struct StateFlags {
    bool air, fluid, blocksMotion, leaves;
  };
  explicit Registry(const std::filesystem::path &directory);
  int stateId(const BlockState &) const;
  const BlockState &stateById(int) const;
  int itemId(const std::string &) const;
  std::string itemName(int) const;
  int entityTypeId(const std::string &) const;
  int blockEntityTypeId(const std::string &) const;
  int biomeId(const std::string &) const;
  int stateBits() const;
  int biomeBits() const;
  StateFlags stateFlags(int stateId) const;

private:
  // 按本模拟器 BlockState::Id（以及按协议 stateId）索引的稠密查找表，
  // 确保热路径封包过程无需构造任何方块名或属性字符串。
  std::vector<int> protocolByLocal_;
  std::vector<StateFlags> flagsByProtocol_;
  std::map<int, BlockState> states_;
  std::map<std::string, int> stateIds_, items_, entities_, blockEntities_,
      biomes_;
  std::map<int, std::string> itemNames_;
  std::map<int, StateFlags> flags_;
};
// 同步兼容编码入口（不含包 ID）；后台写线程异步编码必须改用在世界线程创建的
// prepareChunkSnapshot 及其冻结快照重载，避免跨线程访问可变世界状态。
Bytes encodeChunk(const ChunkSnapshot &, int chunkX, int chunkZ,
                  const Registry &);
// 编码 ClientboundCommandsPacket 包体：按广度优先顺序输出节点条目并在末尾附加根节点索引；
// 当前仅支持 /tick 与 /time 所需字面量以及 brigadier:float、minecraft:time、minecraft:resource 参数。
Bytes encodeCommandTree(const redstone::command::Node &root);
Bytes encodeMovingPistonNbt(const PistonSnapshot &);
// 在世界线程冻结的不可变区块协议快照：区段内已预转换为协议状态 ID，且方块实体 NBT 已转为
// 独立拥有的字节数组，使写线程编码区块包时完全不触碰可变状态注册表或世界数据。
struct ProtocolChunkSnapshot {
  struct BlockEntity {
    Pos pos;
    Bytes nbt;
  };
  int minY = 0, maxY = 0;
  int airId = 0, stateBits = 0, biomeId = 0, biomeBits = 0, pistonTypeId = 0;
  std::vector<SectionSnapshot> sections;
  std::vector<std::vector<int>> sectionBiomes;
  std::vector<BlockEntity> blockEntities;
  // 独立发布的单层光照视图。区块数据包仅在写线阶段拼接各自的位掩码与数据字段，
  // 绝不创建合并光照区段或额外拷贝光照页。
  std::shared_ptr<const LightChunkSnapshot> blockLight;
  std::shared_ptr<const LightChunkSnapshot> skyLight;
  std::size_t retainedBytes() const noexcept;
};
ProtocolChunkSnapshot prepareChunkSnapshot(
    ChunkSnapshot, const Registry &,
    std::shared_ptr<const LightChunkSnapshot> blockLight = {},
    std::shared_ptr<const LightChunkSnapshot> skyLight = {});
Bytes encodeChunk(const ProtocolChunkSnapshot &, int chunkX, int chunkZ,
                  const Registry &);

// 主线程基于不可变已发布光照层筛选变更区段，写线程再打包引用的 2048 字节光照数组；
// 缺失层、全零存储、满/混合存储以及需要清空客户端旧数据的层在协议上保持严格区分。
struct ProtocolLightSections {
  std::vector<int> dataSections;
  std::vector<int> emptySections;
};
struct ProtocolLightUpdate {
  int chunkX = 0;
  int chunkZ = 0;
  std::shared_ptr<const LightChunkSnapshot> snapshot;
  ProtocolLightSections sky;
  ProtocolLightSections block;
  std::size_t retainedBytes() const noexcept;
};
ProtocolLightUpdate prepareLightUpdate(
    std::shared_ptr<const LightChunkSnapshot> current,
    std::shared_ptr<const LightChunkSnapshot> previous = {});
// 单层光照的网络同步基准版本：刻意不持有任何光照页指针或字节缓冲，仅使用生命周期纪元与版本号
// 拒绝过期工作线程结果，并判定脏区段集合是否仍覆盖客户端当前基准。
struct LightLayerVersion {
  int chunkX = 0;
  int chunkZ = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t revision = 0;
};
inline LightLayerVersion lightLayerVersion(const LightChunkSnapshot &view) {
  return {view.chunkX, view.chunkZ, view.minBlockSectionY,
          view.blockSectionCount, view.lifecycleEpoch, view.revision};
}
// 生产环境增量更新路径：仅读取请求的独立发布光照层，其中 changedSections 来自该层工作线程的写入集合。
ProtocolLightUpdate prepareLayerLightUpdate(
    std::shared_ptr<const LightChunkSnapshot> current, LightLayer layer,
    const LightLayerVersion *previous = nullptr);
// 编码 ClientboundLightUpdatePacket 完整包体：先写入 VarInt 区块坐标 x/z，再写入光照数据。
Bytes encodeLightUpdate(const ProtocolLightUpdate &);
// 一条入队的下行客户端数据包：可以是已帧化字节/未帧化载荷，也可以是由写线程执行的延迟编码闭包。
struct OutPacket {
  Bytes bytes;                     // 已帧化字节；若 packetId >= 0 则为待帧化载荷
  int packetId = -1;               // >= 0 时由写线程执行 frame(packetId, bytes)
  std::function<Bytes()> deferred; // 完全在工作/写线程生成的包体（如区块数据包）
  std::size_t retainedBytes = 0;   // deferred 闭包捕获的快照/存储估算内存占用
  // 主线程设置的取消令牌，用于在区块卸载或重载代际更替时跳过过期任务；
  // 已取消的条目不产生任何字节且绝不访问实时 World 状态。
  std::function<bool()> stillValid;
};
inline constexpr std::size_t maxQueuedOutputBytes = 16 * 1024 * 1024;
inline constexpr std::size_t maxQueuedOutputPackets = 65536;
// 在编码前按实际保留的内存容量计费，而非仅按最终网络线长计费；
// 未标明大小的延迟生产者默认按至少 4096 字节计费，区块生产者则提供实际快照内存占用，
// 溢出时饱和到上限 + 1 以触发队列拒绝入队。
inline std::size_t outPacketCharge(const OutPacket &packet) noexcept {
  std::size_t result = sizeof(OutPacket) + 6;
  const auto add = [&](std::size_t amount) {
    if (amount > maxQueuedOutputBytes ||
        result > maxQueuedOutputBytes - amount)
      result = maxQueuedOutputBytes + 1;
    else
      result += amount;
  };
  add(packet.bytes.capacity());
  add(packet.deferred && !packet.retainedBytes ? 4096 : packet.retainedBytes);
  return result;
}
} // namespace redstone::net
