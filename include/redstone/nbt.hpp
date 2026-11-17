#pragma once
// NBT（Named Binary Tag）树形 DOM 解析、序列化以及 gzip/zlib 流编解码接口。
// 存在原因：Minecraft 存档（level.dat、Anvil 区块、POI 与实体）及 .litematic
// 投影均以大端序二进制 NBT 存储，需要独立的无损 DOM 与流压缩层供各子系统复用。
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace redstone::nbt {
// NBT 格式解析、编码或复杂度/大小校验失败时抛出的异常，用于显式暴露损坏或越界数据。
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// 通用 NBT 标签节点（DOM）。
// 存在原因：通过保留空列表元素类型与复合标签原始键序，支持读入未知扩展字段并在
// 未修改时稳定回写。
struct Tag {
  uint8_t type{};
  int64_t number{};
  double real{};
  std::string text;
  std::vector<int8_t> bytes;
  std::vector<int32_t> ints;
  std::vector<int64_t> longs;
  std::vector<Tag> list;
  // 即使列表为空也保留元素类型；保留插入顺序以保证此 DOM 能无损表示的值可稳定
  // 重新编码（不保证非规范 IEEE NaN 载荷等奇异载荷完全一致），查找仍为有序遍历。
  uint8_t listElementType{};
  std::map<std::string, Tag> compound;
  // 保留未改动复合标签的原始键序；重复键仅写出一次。写入器按 map 序追加新键，
  // 已有键则维持捕获到的原有定义顺序。
  std::vector<std::string> compoundOrder;
  // 按键查找复合标签成员，不存在时返回 nullptr（避免直接下标访问误插空节点）。
  const Tag *get(const std::string &key) const;
  Tag *get(const std::string &key);
};
// 带根名称的顶层 NBT 复合标签；某些格式（如投影或 level.dat）需要区分根标签名。
struct RootTag {
  std::string name;
  Tag value;
};
// 从完整字节流解析带根名称的 NBT 复合文档，并校验无尾部残留字节。
RootTag parseRoot(std::span<const uint8_t> data);
// parseRoot 的便捷封装：直接提取根复合标签的值，适用于不关心根名称的场景。
Tag parse(std::span<const uint8_t> data);
// 将 RootTag 序列化为标准大端序二进制 NBT 字节流。
std::vector<uint8_t> writeRoot(const RootTag &root);
// 解压 zlib/gzip 流；limit 用于限制最大输出字节数以防止恶意或损坏数据耗尽内存。
std::vector<uint8_t> decompress(std::span<const uint8_t> data, int window_bits,
                                size_t limit = 128 * 1024 * 1024);
// zlib 流写入器：window_bits=31 生成 level.dat 与 SavedDataStorage 使用的 gzip
// 封装；window_bits=15 生成标准 zlib 流。
std::vector<uint8_t> compress(std::span<const uint8_t> data, int window_bits);
} // namespace redstone::nbt
