#include "redstone/nbt.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <set>
#include <zlib.h>
namespace redstone::nbt {
const Tag *Tag::get(const std::string &key) const {
  auto found = compound.find(key);
  return found == compound.end() ? nullptr : &found->second;
}
Tag *Tag::get(const std::string &key) {
  auto found = compound.find(key);
  return found == compound.end() ? nullptr : &found->second;
}
namespace {
constexpr std::size_t kMaxCollectionEntries = 16'000'000;
constexpr std::size_t kMaxNodes = 4'000'000;

struct Reader {
  std::span<const uint8_t> d;
  size_t p = 0, nodes = 0;
  uint64_t read(int n) {
    if (n < 0 || size_t(n) > d.size() - p)
      throw Error("truncated NBT");
    uint64_t v = 0;
    while (n--)
      v = (v << 8) | d[p++];
    return v;
  }
  std::string str() {
    auto n = read(2);
    if (n > d.size() - p)
      throw Error("truncated NBT string");
    std::string s(reinterpret_cast<const char *>(d.data() + p), size_t(n));
    p += size_t(n);
    return s;
  }
  size_t count(size_t width = 1) {
    auto n = static_cast<int32_t>(read(4));
    if (n < 0 || size_t(n) > (d.size() - p) / width ||
        static_cast<size_t>(n) > kMaxCollectionEntries)
      throw Error("invalid NBT length");
    return size_t(n);
  }
  Tag payload(uint8_t t, int depth) {
    if (depth > 64 || ++nodes > kMaxNodes)
      throw Error("NBT complexity limit exceeded");
    Tag v;
    v.type = t;
    switch (t) {
    case 1:
      v.number = static_cast<int8_t>(read(1));
      break;
    case 2:
      v.number = static_cast<int16_t>(read(2));
      break;
    case 3:
      v.number = static_cast<int32_t>(read(4));
      break;
    case 4:
      v.number = std::bit_cast<int64_t>(read(8));
      break;
    case 5: {
      auto x = static_cast<uint32_t>(read(4));
      v.real = std::bit_cast<float>(x);
      break;
    }
    case 6: {
      auto x = read(8);
      v.real = std::bit_cast<double>(x);
      break;
    }
    case 7: {
      auto n = count();
      v.bytes.reserve(n);
      while (n--)
        v.bytes.push_back(static_cast<int8_t>(read(1)));
      break;
    }
    case 8:
      v.text = str();
      break;
    case 9: {
      auto e = static_cast<uint8_t>(read(1));
      auto n = count();
      if (e > 12 || (e == 0 && n))
        throw Error("invalid NBT list type");
      v.listElementType = e;
      v.list.reserve(n);
      while (n--)
        v.list.push_back(payload(e, depth + 1));
      break;
    }
    case 10: {
      for (;;) {
        auto e = static_cast<uint8_t>(read(1));
        if (!e)
          break;
        if (e > 12)
          throw Error("invalid NBT tag type");
        auto name = str();
        auto [position, inserted] =
            v.compound.emplace(name, payload(e, depth + 1));
        if (!inserted)
          throw Error("duplicate NBT key");
        v.compoundOrder.push_back(std::move(name));
      }
      break;
    }
    case 11: {
      auto n = count(4);
      v.ints.reserve(n);
      while (n--)
        v.ints.push_back(static_cast<int32_t>(read(4)));
      break;
    }
    case 12: {
      auto n = count(8);
      v.longs.reserve(n);
      while (n--)
        v.longs.push_back(std::bit_cast<int64_t>(read(8)));
      break;
    }
    default:
      throw Error("invalid NBT tag type");
    }
    return v;
  }
};
} // namespace
RootTag parseRoot(std::span<const uint8_t> data) {
  Reader r{data};
  auto t = r.read(1);
  if (t != 10)
    throw Error("NBT root must be a compound");
  RootTag result;
  result.name = r.str();
  result.value = r.payload(10, 0);
  if (r.p != data.size())
    throw Error("trailing NBT data");
  return result;
}
Tag parse(std::span<const uint8_t> data) { return parseRoot(data).value; }

namespace {
void u16(std::vector<uint8_t> &out, std::size_t value) {
  if (value > 0xffff)
    throw Error("NBT string too long");
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}
void u32(std::vector<uint8_t> &out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<uint8_t>(value >> shift));
}
void u64(std::vector<uint8_t> &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<uint8_t>(value >> shift));
}
void str(std::vector<uint8_t> &out, const std::string &value) {
  if (value.size() > std::numeric_limits<std::uint16_t>::max())
    throw Error("NBT string too long");
  u16(out, value.size());
  out.insert(out.end(), value.begin(), value.end());
}
void collectionCount(std::vector<uint8_t> &out, std::size_t count) {
  if (count > kMaxCollectionEntries ||
      count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    throw Error("NBT collection too large");
  u32(out, static_cast<std::uint32_t>(count));
}
void payload(std::vector<uint8_t> &out, const Tag &tag, int depth,
             std::size_t &nodes) {
  if (depth > 64 || ++nodes > kMaxNodes)
    throw Error("NBT write complexity limit exceeded");
  switch (tag.type) {
  case 1:
    out.push_back(static_cast<uint8_t>(tag.number));
    break;
  case 2:
    out.push_back(static_cast<uint8_t>(tag.number >> 8));
    out.push_back(static_cast<uint8_t>(tag.number));
    break;
  case 3:
    u32(out, static_cast<uint32_t>(tag.number));
    break;
  case 4:
    u64(out, static_cast<uint64_t>(tag.number));
    break;
  case 5:
    u32(out, std::bit_cast<uint32_t>(static_cast<float>(tag.real)));
    break;
  case 6:
    u64(out, std::bit_cast<uint64_t>(tag.real));
    break;
  case 7:
    collectionCount(out, tag.bytes.size());
    for (int8_t value : tag.bytes)
      out.push_back(static_cast<uint8_t>(value));
    break;
  case 8:
    str(out, tag.text);
    break;
  case 9: {
    uint8_t elementType = tag.listElementType;
    if (!elementType && !tag.list.empty())
      elementType = tag.list.front().type;
    if (elementType > 12 || (elementType == 0 && !tag.list.empty()))
      throw Error("invalid NBT list type");
    for (const Tag &value : tag.list)
      if (value.type != elementType)
        throw Error("heterogeneous NBT list");
    out.push_back(elementType);
    collectionCount(out, tag.list.size());
    for (const Tag &value : tag.list)
      payload(out, value, depth + 1, nodes);
    break;
  }
  case 10: {
    // 快路径：compoundOrder 覆盖全部键且无重复时直接顺序写出，省掉去重集合。
    bool orderedUnique = tag.compoundOrder.size() == tag.compound.size();
    for (const std::string &name : tag.compoundOrder) {
      if (!tag.compound.contains(name)) {
        orderedUnique = false;
        break;
      }
    }
    if (orderedUnique) {
      for (const std::string &name : tag.compoundOrder) {
        const Tag &value = tag.compound.at(name);
        if (value.type == 0 || value.type > 12)
          throw Error("invalid NBT compound tag");
        out.push_back(value.type);
        str(out, name);
        payload(out, value, depth + 1, nodes);
      }
      out.push_back(0);
      break;
    }
    std::set<std::string> written;
    auto emit = [&](const std::string &name, const Tag &value) {
      if (value.type == 0 || value.type > 12)
        throw Error("invalid NBT compound tag");
      out.push_back(value.type);
      str(out, name);
      payload(out, value, depth + 1, nodes);
      written.insert(name);
    };
    for (const std::string &name : tag.compoundOrder) {
      if (written.contains(name))
        continue;
      const auto found = tag.compound.find(name);
      if (found != tag.compound.end())
        emit(name, found->second);
    }
    for (const auto &[name, value] : tag.compound)
      if (!written.contains(name))
        emit(name, value);
    out.push_back(0);
    break;
  }
  case 11:
    collectionCount(out, tag.ints.size());
    for (int32_t value : tag.ints)
      u32(out, static_cast<uint32_t>(value));
    break;
  case 12:
    collectionCount(out, tag.longs.size());
    for (int64_t value : tag.longs)
      u64(out, static_cast<uint64_t>(value));
    break;
  default:
    throw Error("invalid NBT tag type");
  }
}
} // namespace

std::vector<uint8_t> writeRoot(const RootTag &root) {
  if (root.value.type != 10)
    throw Error("NBT root must be a compound");
  std::vector<uint8_t> out;
  out.reserve(std::size_t{1} << 16);
  out.push_back(10);
  str(out, root.name);
  std::size_t nodes = 0;
  payload(out, root.value, 0, nodes);
  return out;
}
std::vector<uint8_t> compress(std::span<const uint8_t> data, int bits) {
  if (data.size() > std::numeric_limits<uInt>::max())
    throw Error("NBT too large to compress");
  z_stream z{};
  if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, bits, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK)
    throw Error("deflate initialization failed");
  struct DeflateGuard {
    z_stream *stream;
    ~DeflateGuard() { deflateEnd(stream); }
  } guard{&z};
  z.next_in = const_cast<Bytef *>(data.data());
  z.avail_in = static_cast<uInt>(data.size());
  std::vector<uint8_t> out;
  std::array<uint8_t, 16384> buffer{};
  for (;;) {
    z.next_out = buffer.data();
    z.avail_out = static_cast<uInt>(buffer.size());
    const int rc = deflate(&z, Z_FINISH);
    const std::size_t produced = buffer.size() - z.avail_out;
    out.insert(out.end(), buffer.begin(), buffer.begin() + produced);
    if (rc == Z_STREAM_END)
      break;
    if (rc != Z_OK)
      throw Error("invalid deflate stream");
  }
  return out;
}

std::vector<uint8_t> decompress(std::span<const uint8_t> data, int bits,
                                size_t limit) {
  if (data.size() > std::numeric_limits<uInt>::max())
    throw Error("compressed NBT too large");
  z_stream z{};
  if (inflateInit2(&z, bits) != Z_OK)
    throw Error("inflate initialization failed");
  struct InflateGuard {
    z_stream* stream;
    ~InflateGuard() { inflateEnd(stream); }
  } guard{&z};
  z.next_in = const_cast<Bytef *>(data.data());
  z.avail_in = static_cast<uInt>(data.size());
  std::vector<uint8_t> out;
  uint8_t buf[16384];
  int rc;
  do {
    z.next_out = buf;
    z.avail_out = sizeof(buf);
    rc = inflate(&z, Z_NO_FLUSH);
    auto n = sizeof(buf) - z.avail_out;
    if (n > limit - out.size()) {
      throw Error("decompressed NBT exceeds limit");
    }
    out.insert(out.end(), buf, buf + n);
    if (rc != Z_OK && rc != Z_STREAM_END) {
      throw Error("invalid compressed NBT");
    }
  } while (rc != Z_STREAM_END);
  bool trailing = z.avail_in != 0;
  if (trailing)
    throw Error("trailing compressed data");
  return out;
}
} // namespace redstone::nbt
