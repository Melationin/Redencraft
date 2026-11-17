#pragma once
// 对齐 Minecraft Java Edition 26.1 的确定性世界生成随机源体系：
// 包含 `RandomSupport`（Stafford13 混洗、128-bit 种子升级与 UTF-8 MD5 哈希）、
// `Xoroshiro128PlusPlus` / `XoroshiroRandomSource`、经典 48-bit LCG `LegacyRandomSource`
// 以及 `WorldgenRandom` 区块装饰/结构盐值派生工具。
// 之所以必须 100% 按 Java 位宽与常量复刻，是因为同一世界种子下，生物群系、噪声偏移、
// 矿脉与结构坐标的 1 ULP 或 1 比特差异都会导致与原版产生连锁地形分歧。

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace redstone::worldgen {

// 原版 `Mth.getSeed(int x, int y, int z)`：严格保留 `x * 3129871` 先 32 位整型溢出再拓宽为 64 位的语义。
std::int64_t positionalSeedHash(int x, int y, int z) noexcept;

// 原版 Java `String.hashCode()`（供 `LegacyPositionalRandomFactory.fromHashOf` 使用）。
std::int32_t javaStringHashCode(std::string_view input) noexcept;

struct Seed128bit {
  std::int64_t seedLo = 0;
  std::int64_t seedHi = 0;

  constexpr Seed128bit xorWith(std::int64_t lo, std::int64_t hi) const noexcept {
    return Seed128bit{seedLo ^ lo, seedHi ^ hi};
  }
  constexpr Seed128bit xorWith(const Seed128bit &other) const noexcept {
    return xorWith(other.seedLo, other.seedHi);
  }
  Seed128bit mixed() const noexcept;
  friend bool operator==(const Seed128bit &, const Seed128bit &) = default;
};

namespace random_support {

inline constexpr std::uint64_t kSilverRatio64 = 0x6A09E667F3BCC909ULL;
inline constexpr std::uint64_t kGoldenRatio64 = 0x9E3779B97F4A7C15ULL;

std::int64_t mixStafford13(std::int64_t z) noexcept;
Seed128bit upgradeSeedTo128bitUnmixed(std::int64_t legacySeed) noexcept;
Seed128bit upgradeSeedTo128bit(std::int64_t legacySeed) noexcept;
// 计算输入字符串的 UTF-8 MD5 128-bit 摘要，并按大端序高低 8 字节拼为 `Seed128bit`（对齐 Guava `Longs.fromBytes`）。
Seed128bit seedFromHashOf(std::string_view input) noexcept;

} // namespace random_support

class RandomSource;
class AnyRandomSource;

// 按空间坐标或名称派生独立随机源的工厂（对应 `PositionalRandomFactory`）。
// 作为极轻量 24 字节值类型直接按值传递与返回，无虚表开销、无堆分配。
class PositionalRandomFactory final {
public:
  enum class Kind : std::uint8_t { Xoroshiro, Legacy };

  PositionalRandomFactory() noexcept = default;

  static PositionalRandomFactory xoroshiro(std::int64_t seedLo,
                                           std::int64_t seedHi) noexcept {
    return PositionalRandomFactory(Kind::Xoroshiro, seedLo, seedHi);
  }
  static PositionalRandomFactory legacy(std::int64_t seed) noexcept {
    return PositionalRandomFactory(Kind::Legacy, seed, 0);
  }

  inline AnyRandomSource at(int x, int y, int z) const noexcept;
  inline AnyRandomSource fromHashOf(std::string_view name) const noexcept;
  inline AnyRandomSource fromSeed(std::int64_t seed) const noexcept;

  Kind kind() const noexcept { return kind_; }
  std::int64_t seedLo() const noexcept { return seedLo_; }
  std::int64_t seedHi() const noexcept { return seedHi_; }

private:
  PositionalRandomFactory(Kind kind, std::int64_t seedLo,
                          std::int64_t seedHi) noexcept
      : kind_(kind), seedLo_(seedLo), seedHi_(seedHi) {}

  Kind kind_ = Kind::Xoroshiro;
  std::int64_t seedLo_ = 0;
  std::int64_t seedHi_ = 0;
};

// 世界生成统一随机源接口（对齐 26.1 `net.minecraft.util.RandomSource`）。
class RandomSource {
public:
  // 注意：`DOUBLE_UNIT` 在原版中是 `float` 字面量 `1.110223E-16F` 提升为 `double`，不可替换为 `0x1.0p-53`。
  static constexpr float kFloatUnit = 5.9604645E-8F;
  static constexpr double kDoubleUnit = static_cast<double>(1.110223E-16F);

  virtual ~RandomSource() = default;

  virtual PositionalRandomFactory forkPositional() = 0;
  virtual void setSeed(std::int64_t seed) = 0;

  virtual std::int32_t nextInt() = 0;
  virtual std::int32_t nextInt(std::int32_t bound) = 0;
  virtual std::int64_t nextLong() = 0;
  virtual bool nextBoolean() = 0;
  virtual float nextFloat() = 0;
  virtual double nextDouble() = 0;
  virtual double nextGaussian() = 0;

  virtual void consumeCount(int rounds) {
    for (int i = 0; i < rounds; ++i)
      (void)nextInt();
  }

  std::int32_t nextIntBetweenInclusive(std::int32_t minInclusive,
                                       std::int32_t maxInclusive) {
    return nextInt(maxInclusive - minInclusive + 1) + minInclusive;
  }
};

// Marsaglia 极坐标高斯分布状态（与原版同名工具类一致，支持 `setSeed` 时 `reset()`）。
class MarsagliaPolarGaussian {
public:
  void reset() noexcept { haveNextNextGaussian_ = false; }
  double nextGaussian(RandomSource &source);

private:
  double nextNextGaussian_ = 0.0;
  bool haveNextNextGaussian_ = false;
};

// 原版 `Xoroshiro128PlusPlus` 核心状态机（当 128 位种子全零时回退为固定非零常对）。
class Xoroshiro128PlusPlus {
public:
  Xoroshiro128PlusPlus() noexcept = default;
  explicit Xoroshiro128PlusPlus(Seed128bit seed) noexcept
      : Xoroshiro128PlusPlus(seed.seedLo, seed.seedHi) {}
  Xoroshiro128PlusPlus(std::int64_t seedLo, std::int64_t seedHi) noexcept;

  std::int64_t nextLong() noexcept {
    const std::uint64_t s0 = static_cast<std::uint64_t>(seedLo_);
    std::uint64_t s1 = static_cast<std::uint64_t>(seedHi_);
    const std::uint64_t result = std::rotl(s0 + s1, 17) + s0;
    s1 ^= s0;
    seedLo_ = static_cast<std::int64_t>(std::rotl(s0, 49) ^ s1 ^ (s1 << 21u));
    seedHi_ = static_cast<std::int64_t>(std::rotl(s1, 28));
    return static_cast<std::int64_t>(result);
  }
  std::int64_t seedLo() const noexcept { return seedLo_; }
  std::int64_t seedHi() const noexcept { return seedHi_; }

private:
  std::int64_t seedLo_ = 0;
  std::int64_t seedHi_ = 0;
};

// 26.1 现代主世界/下界/末地噪声默认随机源（`XoroshiroRandomSource.java`）。
class XoroshiroRandomSource final : public RandomSource {
public:
  explicit XoroshiroRandomSource(std::int64_t seed) noexcept;
  explicit XoroshiroRandomSource(Seed128bit seed) noexcept;
  XoroshiroRandomSource(std::int64_t seedLo, std::int64_t seedHi) noexcept;

  XoroshiroRandomSource fork() noexcept;
  PositionalRandomFactory forkPositional() override;
  void setSeed(std::int64_t seed) override;

  std::int32_t nextInt() override;
  std::int32_t nextInt(std::int32_t bound) override;
  std::int64_t nextLong() override;
  bool nextBoolean() override;
  float nextFloat() override;
  double nextDouble() override;
  double nextGaussian() override;
  void consumeCount(int rounds) override;

  const Xoroshiro128PlusPlus &engine() const noexcept { return rng_; }

private:
  std::uint64_t nextBits(int bits) noexcept;

  Xoroshiro128PlusPlus rng_;
  MarsagliaPolarGaussian gaussian_;
};

// 传统 48 位 Java LCG 随机源（用于 `BlendedNoise`、旧版下界生物群系噪声及部分结构/地物算法）。
class LegacyRandomSource final : public RandomSource {
public:
  static constexpr std::uint64_t kModulusMask = 0xFFFFFFFFFFFFULL;
  static constexpr std::uint64_t kMultiplier = 25214903917ULL;
  static constexpr std::uint64_t kIncrement = 11ULL;

  explicit LegacyRandomSource(std::int64_t seed) noexcept;

  std::int32_t next(int bits) noexcept;

  LegacyRandomSource fork() noexcept;
  PositionalRandomFactory forkPositional() override;
  void setSeed(std::int64_t seed) override;

  std::int32_t nextInt() override;
  std::int32_t nextInt(std::int32_t bound) override;
  std::int64_t nextLong() override;
  bool nextBoolean() override;
  float nextFloat() override;
  double nextDouble() override;
  double nextGaussian() override;

  std::uint64_t rawSeed() const noexcept { return seed_; }

private:
  std::uint64_t seed_ = 0;
  MarsagliaPolarGaussian gaussian_;
};

// 无堆分配值语义随机源：同时内联承载 `XoroshiroRandomSource` 或 `LegacyRandomSource`，
// 供 `PositionalRandomFactory` 与多态分支按值返回与栈上采样使用。
class AnyRandomSource final : public RandomSource {
public:
  using Kind = PositionalRandomFactory::Kind;

  AnyRandomSource(const XoroshiroRandomSource &src) noexcept
      : kind_(Kind::Xoroshiro), xoro_(src.engine()) {}
  AnyRandomSource(const LegacyRandomSource &src) noexcept
      : kind_(Kind::Legacy), legacySeed_(src.rawSeed()) {}

  static AnyRandomSource xoroshiro(std::int64_t seed) noexcept {
    return AnyRandomSource(
        Kind::Xoroshiro,
        Xoroshiro128PlusPlus(random_support::upgradeSeedTo128bit(seed)), 0ULL);
  }
  static AnyRandomSource xoroshiro(Seed128bit seed) noexcept {
    return AnyRandomSource(Kind::Xoroshiro, Xoroshiro128PlusPlus(seed), 0ULL);
  }
  static AnyRandomSource xoroshiro(std::int64_t seedLo,
                                   std::int64_t seedHi) noexcept {
    return AnyRandomSource(Kind::Xoroshiro,
                           Xoroshiro128PlusPlus(seedLo, seedHi), 0ULL);
  }
  static AnyRandomSource legacy(std::int64_t seed) noexcept {
    return AnyRandomSource(
        Kind::Legacy, Xoroshiro128PlusPlus(),
        (static_cast<std::uint64_t>(seed) ^ LegacyRandomSource::kMultiplier) &
            LegacyRandomSource::kModulusMask);
  }

  Kind kind() const noexcept { return kind_; }
  const Xoroshiro128PlusPlus &xoroEngine() const noexcept { return xoro_; }
  std::uint64_t legacyRawSeed() const noexcept { return legacySeed_; }

  AnyRandomSource fork() noexcept {
    if (kind_ == Kind::Xoroshiro) {
      const std::int64_t lo = nextLong();
      const std::int64_t hi = nextLong();
      return xoroshiro(lo, hi);
    }
    return legacy(nextLong());
  }

  PositionalRandomFactory forkPositional() override {
    if (kind_ == Kind::Xoroshiro) {
      const std::int64_t lo = nextLong();
      const std::int64_t hi = nextLong();
      return PositionalRandomFactory::xoroshiro(lo, hi);
    }
    return PositionalRandomFactory::legacy(nextLong());
  }

  void setSeed(std::int64_t seed) override {
    gaussian_.reset();
    if (kind_ == Kind::Xoroshiro) {
      xoro_ = Xoroshiro128PlusPlus(random_support::upgradeSeedTo128bit(seed));
    } else {
      legacySeed_ =
          (static_cast<std::uint64_t>(seed) ^ LegacyRandomSource::kMultiplier) &
          LegacyRandomSource::kModulusMask;
    }
  }

  std::int32_t nextLegacy(int bits) noexcept {
    legacySeed_ = (legacySeed_ * LegacyRandomSource::kMultiplier +
                   LegacyRandomSource::kIncrement) &
                  LegacyRandomSource::kModulusMask;
    return static_cast<std::int32_t>(legacySeed_ >> (48 - bits));
  }

  std::int32_t nextInt() override {
    if (kind_ == Kind::Xoroshiro) {
      return static_cast<std::int32_t>(
          static_cast<std::uint64_t>(xoro_.nextLong()) & 0xFFFFFFFFULL);
    }
    return nextLegacy(32);
  }

  std::int32_t nextInt(std::int32_t bound) override {
    if (bound <= 0)
      throw std::invalid_argument("Bound must be positive");
    if (kind_ == Kind::Xoroshiro) {
      const std::uint64_t uBound = static_cast<std::uint32_t>(bound);
      std::uint64_t randomBits = static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(xoro_.nextLong()) & 0xFFFFFFFFULL);
      std::uint64_t multiplied = randomBits * uBound;
      std::uint64_t fractional = multiplied & 0xFFFFFFFFULL;
      if (fractional < uBound) {
        const std::uint32_t threshold =
            static_cast<std::uint32_t>(~static_cast<std::uint32_t>(bound) +
                                       1u) %
            static_cast<std::uint32_t>(bound);
        while (fractional < threshold) {
          randomBits = static_cast<std::uint32_t>(
              static_cast<std::uint64_t>(xoro_.nextLong()) & 0xFFFFFFFFULL);
          multiplied = randomBits * uBound;
          fractional = multiplied & 0xFFFFFFFFULL;
        }
      }
      return static_cast<std::int32_t>(multiplied >> 32u);
    }
    if ((bound & (bound - 1)) == 0) {
      const std::int64_t sample = static_cast<std::int64_t>(nextLegacy(31));
      return static_cast<std::int32_t>(
          (static_cast<std::int64_t>(bound) * sample) >> 31);
    }
    std::int32_t sample = 0;
    std::int32_t modulo = 0;
    std::int32_t check = 0;
    do {
      sample = nextLegacy(31);
      modulo = sample % bound;
      check = static_cast<std::int32_t>(
          static_cast<std::uint32_t>(sample - modulo) +
          static_cast<std::uint32_t>(bound - 1));
    } while (check < 0);
    return modulo;
  }

  std::int64_t nextLong() override {
    if (kind_ == Kind::Xoroshiro)
      return xoro_.nextLong();
    const std::int64_t upper = static_cast<std::int64_t>(nextLegacy(32));
    const std::int64_t lower = static_cast<std::int64_t>(nextLegacy(32));
    return static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(upper) << 32u) +
        static_cast<std::uint64_t>(lower));
  }

  bool nextBoolean() override {
    if (kind_ == Kind::Xoroshiro)
      return (static_cast<std::uint64_t>(xoro_.nextLong()) & 1ULL) != 0ULL;
    return nextLegacy(1) != 0;
  }

  float nextFloat() override {
    if (kind_ == Kind::Xoroshiro) {
      return static_cast<float>(
                 static_cast<std::uint64_t>(xoro_.nextLong()) >> 40u) *
             kFloatUnit;
    }
    return static_cast<float>(nextLegacy(24)) * kFloatUnit;
  }

  double nextDouble() override {
    if (kind_ == Kind::Xoroshiro) {
      return static_cast<double>(
                 static_cast<std::uint64_t>(xoro_.nextLong()) >> 11u) *
             kDoubleUnit;
    }
    const std::int64_t upper = static_cast<std::int64_t>(nextLegacy(26));
    const std::int64_t lower = static_cast<std::int64_t>(nextLegacy(27));
    const std::int64_t combined = (upper << 27) + lower;
    return static_cast<double>(combined) * kDoubleUnit;
  }

  double nextGaussian() override { return gaussian_.nextGaussian(*this); }

  void consumeCount(int rounds) override {
    if (kind_ == Kind::Xoroshiro) {
      for (int i = 0; i < rounds; ++i)
        (void)xoro_.nextLong();
      return;
    }
    for (int i = 0; i < rounds; ++i)
      (void)nextLegacy(32);
  }

private:
  AnyRandomSource(Kind kind, const Xoroshiro128PlusPlus &xoro,
                  std::uint64_t legacySeed) noexcept
      : kind_(kind), xoro_(xoro), legacySeed_(legacySeed) {}

  Kind kind_;
  Xoroshiro128PlusPlus xoro_;
  std::uint64_t legacySeed_ = 0;
  MarsagliaPolarGaussian gaussian_;
};

inline AnyRandomSource PositionalRandomFactory::at(int x, int y,
                                                   int z) const noexcept {
  if (kind_ == Kind::Xoroshiro) {
    return AnyRandomSource::xoroshiro(positionalSeedHash(x, y, z) ^ seedLo_,
                                      seedHi_);
  }
  return AnyRandomSource::legacy(positionalSeedHash(x, y, z) ^ seedLo_);
}

inline AnyRandomSource
PositionalRandomFactory::fromHashOf(std::string_view name) const noexcept {
  if (kind_ == Kind::Xoroshiro) {
    const Seed128bit hash = random_support::seedFromHashOf(name);
    return AnyRandomSource::xoroshiro(hash.xorWith(seedLo_, seedHi_));
  }
  return AnyRandomSource::legacy(
      static_cast<std::int64_t>(javaStringHashCode(name)) ^ seedLo_);
}

inline AnyRandomSource
PositionalRandomFactory::fromSeed(std::int64_t seed) const noexcept {
  if (kind_ == Kind::Xoroshiro) {
    return AnyRandomSource::xoroshiro(seed ^ seedLo_, seed ^ seedHi_);
  }
  return AnyRandomSource::legacy(seed);
}

// 装饰阶段与大型结构使用的种子派生辅助方法（对齐 `WorldgenRandom.java`）。
// 直接内联底层 RNG 状态，零堆分配、零 `dynamic_cast`。
class WorldgenRandom final : public RandomSource {
public:
  using Kind = PositionalRandomFactory::Kind;

  explicit WorldgenRandom(const AnyRandomSource &source) noexcept
      : kind_(source.kind()), xoro_(source.xoroEngine()),
        legacySeed_(source.legacyRawSeed()) {}
  explicit WorldgenRandom(const XoroshiroRandomSource &source) noexcept
      : kind_(Kind::Xoroshiro), xoro_(source.engine()) {}
  explicit WorldgenRandom(const LegacyRandomSource &source) noexcept
      : kind_(Kind::Legacy), legacySeed_(source.rawSeed()) {}

  static WorldgenRandom xoroshiro(std::int64_t seed) noexcept {
    return WorldgenRandom(AnyRandomSource::xoroshiro(seed));
  }
  static WorldgenRandom legacy(std::int64_t seed) noexcept {
    return WorldgenRandom(AnyRandomSource::legacy(seed));
  }

  AnyRandomSource fork() noexcept;
  PositionalRandomFactory forkPositional() override;
  void setSeed(std::int64_t seed) override;

  std::int32_t nextInt() override;
  std::int32_t nextInt(std::int32_t bound) override;
  std::int64_t nextLong() override;
  bool nextBoolean() override;
  float nextFloat() override;
  double nextDouble() override;
  double nextGaussian() override;

  std::int32_t next(int bits) noexcept;
  std::int32_t count() const noexcept { return count_; }

private:
  Kind kind_ = Kind::Xoroshiro;
  Xoroshiro128PlusPlus xoro_;
  std::uint64_t legacySeed_ = 0;
  MarsagliaPolarGaussian gaussian_;
  std::int32_t count_ = 0;
};

namespace worldgen_random {

std::int64_t setDecorationSeed(RandomSource &random, std::int64_t worldSeed,
                               int minBlockX, int minBlockZ);
void setFeatureSeed(RandomSource &random, std::int64_t decorationSeed,
                    int index, int step);
void setLargeFeatureSeed(RandomSource &random, std::int64_t worldSeed,
                         int chunkX, int chunkZ);
void setLargeFeatureWithSalt(RandomSource &random, std::int64_t worldSeed,
                             int x, int z, int salt);

} // namespace worldgen_random

} // namespace redstone::worldgen
