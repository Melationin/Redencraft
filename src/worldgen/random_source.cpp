#include "redstone/worldgen/random_source.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace redstone::worldgen {
namespace {

constexpr std::array<std::uint32_t, 64> kMd5K{
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
    0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
    0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
    0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
    0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
    0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
    0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
    0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

constexpr std::array<unsigned, 64> kMd5Shift{
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

std::array<std::uint8_t, 16> computeMd5(std::string_view input) noexcept {
  const std::size_t origLen = input.size();
  const std::size_t paddedLen = ((origLen + 8u) / 64u + 1u) * 64u;
  std::uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};

  auto readByte = [&](std::size_t idx) -> std::uint8_t {
    if (idx < origLen)
      return static_cast<std::uint8_t>(input[idx]);
    if (idx == origLen)
      return 0x80u;
    if (idx >= paddedLen - 8u) {
      const std::uint64_t bitLen = static_cast<std::uint64_t>(origLen) * 8ULL;
      const std::size_t shift = (idx - (paddedLen - 8u)) * 8u;
      return static_cast<std::uint8_t>((bitLen >> shift) & 0xFFu);
    }
    return 0u;
  };

  for (std::size_t chunkStart = 0; chunkStart < paddedLen; chunkStart += 64u) {
    std::uint32_t m[16]{};
    for (std::size_t i = 0; i < 16u; ++i) {
      const std::size_t base = chunkStart + i * 4u;
      m[i] = static_cast<std::uint32_t>(readByte(base)) |
             (static_cast<std::uint32_t>(readByte(base + 1u)) << 8u) |
             (static_cast<std::uint32_t>(readByte(base + 2u)) << 16u) |
             (static_cast<std::uint32_t>(readByte(base + 3u)) << 24u);
    }
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    for (std::size_t i = 0; i < 64u; ++i) {
      std::uint32_t f = 0;
      std::size_t g = 0;
      if (i < 16u) {
        f = (b & c) | ((~b) & d);
        g = i;
      } else if (i < 32u) {
        f = (d & b) | ((~d) & c);
        g = (5u * i + 1u) % 16u;
      } else if (i < 48u) {
        f = b ^ c ^ d;
        g = (3u * i + 5u) % 16u;
      } else {
        f = c ^ (b | (~d));
        g = (7u * i) % 16u;
      }
      const std::uint32_t temp = d;
      d = c;
      c = b;
      b = b + std::rotl(a + f + kMd5K[i] + m[g], kMd5Shift[i]);
      a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
  }

  std::array<std::uint8_t, 16> digest{};
  for (std::size_t i = 0; i < 4u; ++i) {
    digest[i * 4u + 0u] = static_cast<std::uint8_t>(state[i] & 0xFFu);
    digest[i * 4u + 1u] = static_cast<std::uint8_t>((state[i] >> 8u) & 0xFFu);
    digest[i * 4u + 2u] = static_cast<std::uint8_t>((state[i] >> 16u) & 0xFFu);
    digest[i * 4u + 3u] = static_cast<std::uint8_t>((state[i] >> 24u) & 0xFFu);
  }
  return digest;
}

std::int64_t bigEndianLong(const std::uint8_t *b) noexcept {
  const std::uint64_t v =
      (static_cast<std::uint64_t>(b[0]) << 56u) |
      (static_cast<std::uint64_t>(b[1]) << 48u) |
      (static_cast<std::uint64_t>(b[2]) << 40u) |
      (static_cast<std::uint64_t>(b[3]) << 32u) |
      (static_cast<std::uint64_t>(b[4]) << 24u) |
      (static_cast<std::uint64_t>(b[5]) << 16u) |
      (static_cast<std::uint64_t>(b[6]) << 8u) |
      static_cast<std::uint64_t>(b[7]);
  return static_cast<std::int64_t>(v);
}

} // namespace

std::int64_t positionalSeedHash(int x, int y, int z) noexcept {
  // Exact 32-bit int multiply for (x * 3129871) before sign-extending to 64-bit long.
  const std::int32_t mulX =
      static_cast<std::int32_t>(static_cast<std::uint32_t>(x) * 3129871u);
  std::uint64_t seed =
      static_cast<std::uint64_t>(static_cast<std::int64_t>(mulX)) ^
      (static_cast<std::uint64_t>(static_cast<std::int64_t>(z)) *
       116129781ULL) ^
      static_cast<std::uint64_t>(static_cast<std::int64_t>(y));
  seed = seed * seed * 42317861ULL + seed * 11ULL;
  return static_cast<std::int64_t>(seed) >> 16;
}

std::int32_t javaStringHashCode(std::string_view input) noexcept {
  std::uint32_t h = 0;
  for (unsigned char c : input)
    h = h * 31u + static_cast<std::uint32_t>(c);
  return static_cast<std::int32_t>(h);
}

Seed128bit Seed128bit::mixed() const noexcept {
  return Seed128bit{random_support::mixStafford13(seedLo),
                    random_support::mixStafford13(seedHi)};
}

namespace random_support {

std::int64_t mixStafford13(std::int64_t z) noexcept {
  std::uint64_t u = static_cast<std::uint64_t>(z);
  u = (u ^ (u >> 30u)) * 0xBF58476D1CE4E5B9ULL;
  u = (u ^ (u >> 27u)) * 0x94D049BB133111EBULL;
  u = u ^ (u >> 31u);
  return static_cast<std::int64_t>(u);
}

Seed128bit upgradeSeedTo128bitUnmixed(std::int64_t legacySeed) noexcept {
  const std::uint64_t low = static_cast<std::uint64_t>(legacySeed) ^ kSilverRatio64;
  const std::uint64_t high = low + kGoldenRatio64;
  return Seed128bit{static_cast<std::int64_t>(low),
                    static_cast<std::int64_t>(high)};
}

Seed128bit upgradeSeedTo128bit(std::int64_t legacySeed) noexcept {
  return upgradeSeedTo128bitUnmixed(legacySeed).mixed();
}

Seed128bit seedFromHashOf(std::string_view input) noexcept {
  const auto digest = computeMd5(input);
  return Seed128bit{bigEndianLong(digest.data()),
                    bigEndianLong(digest.data() + 8)};
}

} // namespace random_support

double MarsagliaPolarGaussian::nextGaussian(RandomSource &source) {
  if (haveNextNextGaussian_) {
    haveNextNextGaussian_ = false;
    return nextNextGaussian_;
  }
  double x = 0.0;
  double y = 0.0;
  double radiusSquared = 0.0;
  do {
    x = 2.0 * source.nextDouble() - 1.0;
    y = 2.0 * source.nextDouble() - 1.0;
    radiusSquared = x * x + y * y;
  } while (radiusSquared >= 1.0 || radiusSquared == 0.0);
  const double multiplier =
      std::sqrt(-2.0 * std::log(radiusSquared) / radiusSquared);
  nextNextGaussian_ = y * multiplier;
  haveNextNextGaussian_ = true;
  return x * multiplier;
}

Xoroshiro128PlusPlus::Xoroshiro128PlusPlus(std::int64_t seedLo,
                                           std::int64_t seedHi) noexcept
    : seedLo_(seedLo), seedHi_(seedHi) {
  if ((seedLo_ | seedHi_) == 0) {
    seedLo_ = static_cast<std::int64_t>(random_support::kGoldenRatio64);
    seedHi_ = static_cast<std::int64_t>(random_support::kSilverRatio64);
  }
}

XoroshiroRandomSource::XoroshiroRandomSource(std::int64_t seed) noexcept
    : rng_(random_support::upgradeSeedTo128bit(seed)) {}

XoroshiroRandomSource::XoroshiroRandomSource(Seed128bit seed) noexcept
    : rng_(seed) {}

XoroshiroRandomSource::XoroshiroRandomSource(std::int64_t seedLo,
                                             std::int64_t seedHi) noexcept
    : rng_(seedLo, seedHi) {}

XoroshiroRandomSource XoroshiroRandomSource::fork() noexcept {
  const std::int64_t lo = nextLong();
  const std::int64_t hi = nextLong();
  return XoroshiroRandomSource(lo, hi);
}

PositionalRandomFactory XoroshiroRandomSource::forkPositional() {
  const std::int64_t lo = nextLong();
  const std::int64_t hi = nextLong();
  return PositionalRandomFactory::xoroshiro(lo, hi);
}

void XoroshiroRandomSource::setSeed(std::int64_t seed) {
  rng_ = Xoroshiro128PlusPlus(random_support::upgradeSeedTo128bit(seed));
  gaussian_.reset();
}

std::int32_t XoroshiroRandomSource::nextInt() {
  return static_cast<std::int32_t>(static_cast<std::uint64_t>(rng_.nextLong()) &
                                   0xFFFFFFFFULL);
}

std::int32_t XoroshiroRandomSource::nextInt(std::int32_t bound) {
  if (bound <= 0)
    throw std::invalid_argument("Bound must be positive");
  const std::uint64_t uBound = static_cast<std::uint32_t>(bound);
  std::uint64_t randomBits = static_cast<std::uint32_t>(nextInt());
  std::uint64_t multiplied = randomBits * uBound;
  std::uint64_t fractional = multiplied & 0xFFFFFFFFULL;
  if (fractional < uBound) {
    const std::uint32_t threshold =
        static_cast<std::uint32_t>(~static_cast<std::uint32_t>(bound) + 1u) %
        static_cast<std::uint32_t>(bound);
    while (fractional < threshold) {
      randomBits = static_cast<std::uint32_t>(nextInt());
      multiplied = randomBits * uBound;
      fractional = multiplied & 0xFFFFFFFFULL;
    }
  }
  return static_cast<std::int32_t>(multiplied >> 32u);
}

std::int64_t XoroshiroRandomSource::nextLong() { return rng_.nextLong(); }

bool XoroshiroRandomSource::nextBoolean() {
  return (static_cast<std::uint64_t>(rng_.nextLong()) & 1ULL) != 0ULL;
}

std::uint64_t XoroshiroRandomSource::nextBits(int bits) noexcept {
  return static_cast<std::uint64_t>(rng_.nextLong()) >> (64 - bits);
}

float XoroshiroRandomSource::nextFloat() {
  return static_cast<float>(nextBits(24)) * kFloatUnit;
}

double XoroshiroRandomSource::nextDouble() {
  return static_cast<double>(nextBits(53)) * kDoubleUnit;
}

double XoroshiroRandomSource::nextGaussian() {
  return gaussian_.nextGaussian(*this);
}

void XoroshiroRandomSource::consumeCount(int rounds) {
  for (int i = 0; i < rounds; ++i)
    (void)rng_.nextLong();
}

LegacyRandomSource::LegacyRandomSource(std::int64_t seed) noexcept {
  LegacyRandomSource::setSeed(seed);
}

LegacyRandomSource LegacyRandomSource::fork() noexcept {
  return LegacyRandomSource(nextLong());
}

PositionalRandomFactory LegacyRandomSource::forkPositional() {
  return PositionalRandomFactory::legacy(nextLong());
}

void LegacyRandomSource::setSeed(std::int64_t seed) {
  seed_ = (static_cast<std::uint64_t>(seed) ^ kMultiplier) & kModulusMask;
  gaussian_.reset();
}

std::int32_t LegacyRandomSource::next(int bits) noexcept {
  seed_ = (seed_ * kMultiplier + kIncrement) & kModulusMask;
  return static_cast<std::int32_t>(seed_ >> (48 - bits));
}

std::int32_t LegacyRandomSource::nextInt() { return next(32); }

std::int32_t LegacyRandomSource::nextInt(std::int32_t bound) {
  if (bound <= 0)
    throw std::invalid_argument("Bound must be positive");
  if ((bound & (bound - 1)) == 0) {
    const std::int64_t sample = static_cast<std::int64_t>(next(31));
    return static_cast<std::int32_t>((static_cast<std::int64_t>(bound) * sample) >>
                                     31);
  }
  std::int32_t sample = 0;
  std::int32_t modulo = 0;
  std::int32_t check = 0;
  do {
    sample = next(31);
    modulo = sample % bound;
    check = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(sample - modulo) +
        static_cast<std::uint32_t>(bound - 1));
  } while (check < 0);
  return modulo;
}

std::int64_t LegacyRandomSource::nextLong() {
  const std::int64_t upper = static_cast<std::int64_t>(next(32));
  const std::int64_t lower = static_cast<std::int64_t>(next(32));
  return static_cast<std::int64_t>(
      (static_cast<std::uint64_t>(upper) << 32u) +
      static_cast<std::uint64_t>(lower));
}

bool LegacyRandomSource::nextBoolean() { return next(1) != 0; }

float LegacyRandomSource::nextFloat() {
  return static_cast<float>(next(24)) * kFloatUnit;
}

double LegacyRandomSource::nextDouble() {
  const std::int64_t upper = static_cast<std::int64_t>(next(26));
  const std::int64_t lower = static_cast<std::int64_t>(next(27));
  const std::int64_t combined = (upper << 27) + lower;
  return static_cast<double>(combined) * kDoubleUnit;
}

double LegacyRandomSource::nextGaussian() {
  return gaussian_.nextGaussian(*this);
}

AnyRandomSource WorldgenRandom::fork() noexcept {
  if (kind_ == Kind::Xoroshiro) {
    const std::int64_t lo = xoro_.nextLong();
    const std::int64_t hi = xoro_.nextLong();
    return AnyRandomSource::xoroshiro(lo, hi);
  }
  return AnyRandomSource::legacy(nextLong());
}

PositionalRandomFactory WorldgenRandom::forkPositional() {
  if (kind_ == Kind::Xoroshiro) {
    const std::int64_t lo = xoro_.nextLong();
    const std::int64_t hi = xoro_.nextLong();
    return PositionalRandomFactory::xoroshiro(lo, hi);
  }
  return PositionalRandomFactory::legacy(nextLong());
}

void WorldgenRandom::setSeed(std::int64_t seed) {
  gaussian_.reset();
  if (kind_ == Kind::Xoroshiro) {
    xoro_ = Xoroshiro128PlusPlus(random_support::upgradeSeedTo128bit(seed));
    return;
  }
  legacySeed_ =
      (static_cast<std::uint64_t>(seed) ^ LegacyRandomSource::kMultiplier) &
      LegacyRandomSource::kModulusMask;
}

std::int32_t WorldgenRandom::next(int bits) noexcept {
  ++count_;
  if (kind_ == Kind::Legacy) {
    legacySeed_ = (legacySeed_ * LegacyRandomSource::kMultiplier +
                   LegacyRandomSource::kIncrement) &
                  LegacyRandomSource::kModulusMask;
    return static_cast<std::int32_t>(legacySeed_ >> (48 - bits));
  }
  const std::uint64_t v = static_cast<std::uint64_t>(xoro_.nextLong());
  return static_cast<std::int32_t>(
      static_cast<std::uint32_t>(v >> (64 - bits)));
}

std::int32_t WorldgenRandom::nextInt() { return next(32); }

std::int32_t WorldgenRandom::nextInt(std::int32_t bound) {
  if (bound <= 0)
    throw std::invalid_argument("Bound must be positive");
  if ((bound & (bound - 1)) == 0) {
    const std::int64_t sample = static_cast<std::int64_t>(next(31));
    return static_cast<std::int32_t>(
        (static_cast<std::int64_t>(bound) * sample) >> 31);
  }
  std::int32_t sample = 0;
  std::int32_t modulo = 0;
  std::int32_t check = 0;
  do {
    sample = next(31);
    modulo = sample % bound;
    check = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(sample - modulo) +
        static_cast<std::uint32_t>(bound - 1));
  } while (check < 0);
  return modulo;
}

std::int64_t WorldgenRandom::nextLong() {
  const std::int64_t upper = static_cast<std::int64_t>(next(32));
  const std::int64_t lower = static_cast<std::int64_t>(next(32));
  return static_cast<std::int64_t>(
      (static_cast<std::uint64_t>(upper) << 32u) +
      static_cast<std::uint64_t>(lower));
}

bool WorldgenRandom::nextBoolean() { return next(1) != 0; }

float WorldgenRandom::nextFloat() {
  return static_cast<float>(next(24)) * kFloatUnit;
}

double WorldgenRandom::nextDouble() {
  const std::int64_t upper = static_cast<std::int64_t>(next(26));
  const std::int64_t lower = static_cast<std::int64_t>(next(27));
  const std::int64_t combined = (upper << 27) + lower;
  return static_cast<double>(combined) * kDoubleUnit;
}

double WorldgenRandom::nextGaussian() {
  return gaussian_.nextGaussian(*this);
}

namespace worldgen_random {

std::int64_t setDecorationSeed(RandomSource &random, std::int64_t worldSeed,
                               int minBlockX, int minBlockZ) {
  random.setSeed(worldSeed);
  const std::int64_t xScale = random.nextLong() | 1LL;
  const std::int64_t zScale = random.nextLong() | 1LL;
  const std::int64_t result = static_cast<std::int64_t>(
      (static_cast<std::uint64_t>(minBlockX) *
           static_cast<std::uint64_t>(xScale) +
       static_cast<std::uint64_t>(minBlockZ) *
           static_cast<std::uint64_t>(zScale)) ^
      static_cast<std::uint64_t>(worldSeed));
  random.setSeed(result);
  return result;
}

void setFeatureSeed(RandomSource &random, std::int64_t decorationSeed,
                    int index, int step) {
  const std::int64_t result = static_cast<std::int64_t>(
      static_cast<std::uint64_t>(decorationSeed) +
      static_cast<std::uint64_t>(index) +
      10000ULL * static_cast<std::uint64_t>(step));
  random.setSeed(result);
}

void setLargeFeatureSeed(RandomSource &random, std::int64_t worldSeed,
                         int chunkX, int chunkZ) {
  random.setSeed(worldSeed);
  const std::int64_t xScale = random.nextLong();
  const std::int64_t zScale = random.nextLong();
  const std::int64_t result = static_cast<std::int64_t>(
      (static_cast<std::uint64_t>(chunkX) *
       static_cast<std::uint64_t>(xScale)) ^
      (static_cast<std::uint64_t>(chunkZ) *
       static_cast<std::uint64_t>(zScale)) ^
      static_cast<std::uint64_t>(worldSeed));
  random.setSeed(result);
}

void setLargeFeatureWithSalt(RandomSource &random, std::int64_t worldSeed,
                             int x, int z, int salt) {
  const std::int64_t result = static_cast<std::int64_t>(
      static_cast<std::uint64_t>(x) * 341873128712ULL +
      static_cast<std::uint64_t>(z) * 132897987541ULL +
      static_cast<std::uint64_t>(worldSeed) +
      static_cast<std::uint64_t>(salt));
  random.setSeed(result);
}

} // namespace worldgen_random

} // namespace redstone::worldgen
