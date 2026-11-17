#pragma once
// 对齐 Minecraft Java Edition 26.1 的含水层（`Aquifer.java`）与巨型矿脉（`OreVeinifier.java`）。
// 用途：在 `NoiseChunk` 为每个格点求出实体密度后：
// 1. 当 `finalDensity <= 0` 时，按 16×12×16 含水层网格采样附近水位与阻隔噪声，在地下洞穴生成局部水体/岩浆海与隔水石障；
// 2. 当处于实体岩石内部时，由 `OreVeinifier` 按 `veinToggle`/`veinRidged`/`veinGap` 生成铜/花岗岩或深层铁/凝灰岩矿脉。

#include "redstone/chunk_lifecycle.hpp"
#include "redstone/engine_types.hpp"
#include "redstone/worldgen/noise_router.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace redstone::worldgen {

class NoiseChunk;

inline constexpr int kDimensionMinY = -2032;
inline constexpr int kWayBelowMinY = kDimensionMinY << 4; // -32512

struct FluidStatus {
  int fluidLevel = 0;
  BlockState fluidType;

  BlockState at(int blockY) const noexcept;

  friend bool operator==(const FluidStatus &, const FluidStatus &) = default;
};

using FluidPicker =
    std::function<FluidStatus(int blockX, int blockY, int blockZ)>;

using BlockStateFiller =
    std::function<std::optional<BlockState>(const DensityFunctionContext &ctx)>;

class Aquifer {
public:
  virtual ~Aquifer() = default;

  virtual std::optional<BlockState>
  computeSubstance(const DensityFunctionContext &ctx, double density) = 0;

  virtual bool shouldScheduleFluidUpdate() const noexcept = 0;

  static std::unique_ptr<Aquifer> create(
      NoiseChunk &noiseChunk, ChunkTicketPos chunkPos, const NoiseRouter &router,
      const PositionalRandomFactory &aquiferRandom, int minBlockY,
      int yBlockSize, FluidPicker globalFluidPicker);

  static std::unique_ptr<Aquifer> createDisabled(FluidPicker globalFluidPicker);
};

// 26.1 巨型矿脉填充器（`OreVeinifier.java`）：在 y=[0,50] 生成铜/花岗岩矿脉，在 y=[-60,-8] 生成深层铁/凝灰岩矿脉。
BlockStateFiller createOreVeinifier(const DensityNodePtr &veinToggle,
                                    const DensityNodePtr &veinRidged,
                                    const DensityNodePtr &veinGap,
                                    const PositionalRandomFactory &oreRandom);

} // namespace redstone::worldgen
