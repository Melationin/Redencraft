#include "redstone/nbt.hpp"
#include <bit>
#include <cstring>
#include <limits>
#include <zlib.h>
namespace redstone::nbt {
const Tag *Tag::get(const std::string &key) const {
  auto i = compound.find(key);
  return i == compound.end() ? nullptr : &i->second;
}
namespace {
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
    if (n < 0 || size_t(n) > (d.size() - p) / width || n > 16000000)
      throw Error("invalid NBT length");
    return size_t(n);
  }
  Tag payload(uint8_t t, int depth) {
    if (depth > 64 || ++nodes > 4000000)
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
        if (!v.compound.emplace(std::move(name), payload(e, depth + 1)).second)
          throw Error("duplicate NBT key");
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
Tag parse(std::span<const uint8_t> data) {
  Reader r{data};
  auto t = r.read(1);
  if (t != 10)
    throw Error("NBT root must be a compound");
  r.str();
  auto result = r.payload(10, 0);
  if (r.p != data.size())
    throw Error("trailing NBT data");
  return result;
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
