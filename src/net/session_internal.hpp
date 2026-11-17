#pragma once
// 协议会话子系统内部辅助工具：封装物品支持过滤、台阶替换判定、NBT/UUID 编解码、
// 区块区段坐标打包、fastutil 风格区段变更集、LpVec3 向量压缩以及方块标签包替换。
#include "redstone/net/session.hpp"
#include "redstone/fluid_flow.hpp"
#include "redstone/item.hpp"
#include "redstone/item_use.hpp"
#include "redstone/piston.hpp"
#include "redstone/redstone.hpp"
#include "redstone/shapes.hpp"
#include "redstone/storage.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <numbers>
#include <sstream>
#include <utility>

namespace redstone::net {
namespace session_internal {
inline bool isBucketItem(std::string_view name) {
  const Item *item = itemByName(name);
  return item && item->behavior() == ItemBehaviorKind::Bucket;
}
inline std::string_view placementBlockNameForItem(std::string_view itemName) {
  const Item *item = itemByName(itemName);
  if (item && !item->primaryBlockName().empty())
    return item->primaryBlockName();
  return itemName;
}
// 判断物品是否可在当前服务端创造模式中实际使用或放置（过滤未支持方块与空气）。
inline bool isCreativeItemSupported(const Item &item) {
  if (item.id() == 0)
    return false;
  if (!item.primaryBlockName().empty() &&
      item.behavior() != ItemBehaviorKind::SolidBucket) {
    return isSupportedBlock(std::string(item.primaryBlockName())) ||
           item.compostChance() > 0.0f;
  }
  return true;
}
// 判定目标位置现有方块是否可被手持物品直接替换放置；特别复刻原版台阶合并为双台阶时
// 对点击面与半高命中点 hitY 的判定规则。
inline bool canReplaceForPlacement(const BlockState &state, const ItemStack &held,
                            double hitY, Direction face,
                            bool replacingClickedOnBlock) {
  if (state.block().isSlab()) {
    if (state.getView("type") == "double" || held.id != state.block().name())
      return false;
    if (!replacingClickedOnBlock)
      return true;
    const bool above = hitY > 0.5;
    const bool bottomSlab = state.getView("type") == "bottom";
    const bool horizontal = face == Direction::North ||
                            face == Direction::South ||
                            face == Direction::East || face == Direction::West;
    return bottomSlab ? (face == Direction::Up || (above && horizontal))
                      : (face == Direction::Down || (!above && horizontal));
  }
  return isReplaceable(state);
}
inline bool isAxe(std::string_view name) {
  const Item *item = itemByName(name);
  return item && item->behavior() == ItemBehaviorKind::Axe;
}
inline std::optional<BlockState> strippedLogState(const BlockState &state) {
  return blockStateForAxeStrip(state);
}
// 构造数据包查找表键，与 packets.tsv 导出格式 "<phase>/<flow>/minecraft:<name>" 对齐。
inline std::string key(std::string_view phase, std::string_view flow,
                std::string_view name) {
  return std::string(phase) + "/" + std::string(flow) +
         "/minecraft:" + std::string(name);
}
// 按制表符分割协议数据表（TSV）的单行字段。
inline std::vector<std::string> fields(const std::string &line) {
  std::vector<std::string> out;
  std::istringstream stream(line);
  for (std::string s; std::getline(stream, s, '\t');)
    out.push_back(s);
  return out;
}
// 直接写入网络格式的匿名 TAG_String 文本组件。
inline void nbtText(Writer &out, const std::string &text) {
  out.u8(8);
  out.i16(static_cast<std::int16_t>(text.size()));
  out.raw({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}
// 对应原版 Component.translatable(key, args) 的网络 NBT 编码：构造包含 "translate"
// 以及（参数非空时的）"with" 字符串标签列表的复合标签；客户端依据自身语言文件解析翻译键，
// 与原版服务端数据包完全一致。
inline void nbtTranslate(Writer &out, const std::string &key,
                  const std::vector<std::string> &args) {
  const auto string_tag = [&](std::string_view value) {
    out.i16(static_cast<std::int16_t>(value.size()));
    out.raw({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
  };
  const auto named = [&](std::uint8_t tag, std::string_view name) {
    out.u8(tag);
    string_tag(name);
  };
  out.u8(10); // 匿名根 TAG_Compound。
  named(8, "translate");
  string_tag(key);
  if (!args.empty()) {
    named(9, "with");
    out.u8(8); // TAG_String 列表元素类型
    out.i32(static_cast<std::int32_t>(args.size()));
    for (const auto &arg : args)
      string_tag(arg);
  }
  out.u8(0); // TAG_End
}
inline void uuid(Writer &out, const Uuid &id) { out.raw(id); }
inline int chunkCoord(double coordinate) {
  return static_cast<int>(std::floor(coordinate / 16));
}
// 对应原版 SectionPos.sectionRelativePos：按 (x << 8 | z << 4 | y) 打包区段内局部坐标。
inline std::uint16_t packSectionLocal(Pos p) {
  return static_cast<std::uint16_t>(((p.x & 15) << 8) | ((p.z & 15) << 4) |
                                    (p.y & 15));
}
// 对应原版 SectionPos.asLong：按 (x << 42 | z << 20 | y) 打包，各占 22/22/20 位。
inline std::int64_t packSectionPos(Pos p) {
  const std::uint64_t x = static_cast<std::uint64_t>(p.x >> 4) & 0x3ffffful;
  const std::uint64_t y = static_cast<std::uint64_t>(p.y >> 4) & 0xfffffull;
  const std::uint64_t z = static_cast<std::uint64_t>(p.z >> 4) & 0x3ffffful;
  return static_cast<std::int64_t>((x << 42) | (z << 20) | y);
}
// 将打包的区段坐标与区段内局部坐标还原为世界绝对方块坐标（含位域符号扩展）。
inline Pos unpackSectionBlock(std::int64_t section, std::uint16_t local) {
  const auto signExtend = [](std::uint64_t value, int bits) {
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    value &= mask;
    return static_cast<std::int64_t>(
        value & (std::uint64_t{1} << (bits - 1)) ? value | ~mask : value);
  };
  const auto s = static_cast<std::uint64_t>(section);
  return Pos{static_cast<int>(signExtend(s >> 42, 22) * 16 + ((local >> 8) & 15)),
             static_cast<int>(signExtend(s, 20) * 16 + (local & 15)),
             static_cast<int>(signExtend(s >> 20, 22) * 16 + ((local >> 4) & 15))};
}
inline std::uint8_t packDegrees(float degrees) {
  // 对应原版 Mth.packDegrees：先对浮点乘法结果向下取整再缩窄为单字节角度（256 步进/圈）。
  return static_cast<std::uint8_t>(
      static_cast<int>(std::floor(degrees * 256.0f / 360.0f)));
}

// 复刻原版 ChunkHolder 所用的 fastutil ShortOpenHashSet：采用线性探测，遍历时先输出 0、
// 再按倒序输出其余非零槽位；单个区段至多包含 4096 个局部位置，故最大表长为 8192 槽。
// 使用内置双定长数组交替扩容，避免每区段频繁堆分配同时保证多方块更新包的元素顺序与原版一致。
class SectionChanges {
public:
  void reset() {
    table_ = std::span(tableA_).first(32);
    std::ranges::fill(table_, std::uint16_t{0});
    size_ = 0;
    hasZero_ = false;
  }
  void add(std::uint16_t local) {
    if (local == 0) {
      if (std::exchange(hasZero_, true))
        return;
    } else if (!insert(table_, local)) {
      return;
    }
    if (size_++ < static_cast<int>(table_.size() * 3 / 4))
      return;
    auto &storage = table_.data() == tableA_.data() ? tableB_ : tableA_;
    auto grown = std::span(storage).first(table_.size() * 2);
    std::ranges::fill(grown, std::uint16_t{0});
    for (auto it = table_.rbegin(); it != table_.rend(); ++it)
      if (*it != 0)
        insert(grown, *it);
    table_ = grown;
  }
  int size() const { return size_; }
  template <typename Emit> void forEach(Emit emit) const {
    if (hasZero_)
      emit(0);
    for (auto it = table_.rbegin(); it != table_.rend(); ++it)
      if (*it != 0)
        emit(*it);
  }

private:
  static bool insert(std::span<std::uint16_t> table, std::uint16_t local) {
    const auto hash = std::uint32_t{local} * 0x9e3779b9u;
    const auto mask = table.size() - 1;
    auto slot = (hash ^ (hash >> 16)) & mask;
    while (table[slot] != 0 && table[slot] != local)
      slot = (slot + 1) & mask;
    if (table[slot] == local)
      return false;
    table[slot] = local;
    return true;
  }

  std::array<std::uint16_t, 8192> tableA_, tableB_;
  std::span<std::uint16_t> table_;
  int size_;
  bool hasZero_;
};
// 根据偏航角计算最接近的水平主朝向（对应玩家平面朝向）。
inline Direction horizontal(float yaw) {
  constexpr Direction directions[]{Direction::South, Direction::West,
                                   Direction::North, Direction::East};
  return directions[static_cast<int>(std::floor(yaw / 90.0f + 0.5f)) & 3];
}
// 对应原版 Direction.orderedByNearest：按视线向量分量大小排序返回 6 个方向，用于确定方块放置朝向。
inline std::array<Direction, 6> lookingDirections(float yaw, float pitch) {
  const double yr = yaw * std::numbers::pi / 180,
               pr = pitch * std::numbers::pi / 180;
  const double x = -std::sin(yr) * std::cos(pr), y = -std::sin(pr),
               z = std::cos(yr) * std::cos(pr);
  const auto dx = x > 0 ? Direction::East : Direction::West;
  const auto dy = y > 0 ? Direction::Up : Direction::Down;
  const auto dz = z > 0 ? Direction::South : Direction::North;
  std::array<Direction, 3> first;
  if (std::abs(x) > std::abs(z)) {
    if (std::abs(y) > std::abs(x))
      first = {dy, dx, dz};
    else if (std::abs(z) > std::abs(y))
      first = {dx, dz, dy};
    else
      first = {dx, dy, dz};
  } else {
    if (std::abs(y) > std::abs(z))
      first = {dy, dz, dx};
    else if (std::abs(x) > std::abs(y))
      first = {dz, dx, dy};
    else
      first = {dz, dy, dx};
  }
  return {first[0],           first[1],           first[2],
          opposite(first[2]), opposite(first[1]), opposite(first[0])};
}
inline std::string phaseName(Phase p) {
  switch (p) {
  case Phase::Login:
    return "login";
  case Phase::Configuration:
    return "configuration";
  case Phase::Play:
    return "play";
  case Phase::Status:
    return "status";
  default:
    return "handshake";
  }
}
// 对应原版 26.1 LpVec3 低精度三维向量线格式编解码：将实体速度 Vec3 量化为 15 位定点分量
// 并附带缩放倍率压缩至 1~6+ 字节传输，以降低实体运动同步包体积。
inline void lpVector(Writer &out, Vec3 value) {
  auto clean = [](double n) {
    return std::isnan(n) ? 0.0
                         : std::clamp(n, -1.7179869183E10, 1.7179869183E10);
  };
  const double x = clean(value.x), y = clean(value.y), z = clean(value.z);
  const double length = std::max({std::abs(x), std::abs(y), std::abs(z)});
  if (length < 3.051944088384301E-5) {
    out.u8(0);
    return;
  }
  const auto scale = static_cast<std::uint64_t>(std::ceil(length));
  const auto pack = [scale](double n) {
    return static_cast<std::uint64_t>(
        std::floor((n / scale * 0.5 + 0.5) * 32766.0 + 0.5));
  };
  const bool partial = scale > 3;
  auto bits = (scale & 3) | (partial ? 4 : 0) | (pack(x) << 3) |
              (pack(y) << 18) | (pack(z) << 33);
  out.u8(static_cast<std::uint8_t>(bits));
  out.u8(static_cast<std::uint8_t>(bits >> 8));
  out.i32(static_cast<std::int32_t>(bits >> 16));
  if (partial)
    out.varInt(static_cast<std::int32_t>(scale >> 2));
}
inline Vec3 readLpVector(Reader &in) {
  const std::uint8_t lowest = in.u8();
  if (lowest == 0)
    return {};
  const std::uint8_t middle = in.u8();
  const std::uint32_t highest = static_cast<std::uint32_t>(in.i32());
  const std::uint64_t buffer = (static_cast<std::uint64_t>(highest) << 16) |
                               (static_cast<std::uint64_t>(middle) << 8) |
                               static_cast<std::uint64_t>(lowest);
  std::uint64_t scale = lowest & 3u;
  if ((lowest & 4u) == 4u)
    scale |= static_cast<std::uint64_t>(static_cast<std::uint32_t>(in.varInt()))
             << 2;
  auto unpack = [](std::uint64_t v) {
    return std::min<double>(static_cast<double>(v & 32767u), 32766.0) * 2.0 /
               32766.0 -
           1.0;
  };
  return {unpack(buffer >> 3) * static_cast<double>(scale),
          unpack(buffer >> 18) * static_cast<double>(scale),
          unpack(buffer >> 33) * static_cast<double>(scale)};
}

// 保留原版 26.1 update_tags 数据包中除方块以外的所有注册表标签；
// 仅用 blocks.tsv 的协议 ID 替换 minecraft:block 标签载荷（绝不使用本模拟器本地 ID），
// 确保客户端与服务端对可支持方块的标签判定一致。
inline Bytes replaceBlockTagPacket(const Bytes &original, const ProtocolData &data,
                            const BlockTagRegistry &tags, int packetId) {
  auto copy = original;
  auto body = popFrame(copy);
  if (!body || !copy.empty()) throw ProtocolError("invalid update_tags frame");
  Reader in(*body);
  (void)in.varInt();
  const int count = in.varInt();
  if (count < 0 || count > 10000) throw ProtocolError("invalid tag registry count");
  std::vector<Bytes> other;
  bool foundBlock = false;
  for (int i = 0; i < count; ++i) {
    const auto start = in.pos;
    const auto registry = in.string();
    const int tagCount = in.varInt();
    if (tagCount < 0 || tagCount > 100000) throw ProtocolError("invalid tag count");
    for (int t = 0; t < tagCount; ++t) {
      (void)in.string();
      const int members = in.varInt();
      if (members < 0 || members > 100000) throw ProtocolError("invalid tag member count");
      for (int j = 0; j < members; ++j) (void)in.varInt();
    }
    if (registry == "minecraft:block") {
      if (foundBlock) throw ProtocolError("duplicate block tag registry");
      foundBlock = true;
    } else {
      other.emplace_back(body->begin() + start, body->begin() + in.pos);
    }
  }
  if (!in.empty()) throw ProtocolError("trailing update_tags payload");
  Writer out;
  out.varInt(static_cast<int>(other.size()) + 1);
  for (const auto &entry : other) out.raw(entry);
  out.string("minecraft:block");
  out.varInt(static_cast<int>(tags.entries().size()));
  for (const auto &[name, members] : tags.entries()) {
    out.string(name);
    out.varInt(static_cast<int>(members.size()));
    for (const auto &member : members) {
      const auto found = data.blockIds.find(member);
      if (found == data.blockIds.end()) throw ProtocolError("unknown block in tag: " + member);
      out.varInt(found->second);
    }
  }
  return frame(packetId, out.data);
}
} // namespace session_internal
using namespace session_internal;
} // namespace redstone::net
