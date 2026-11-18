#include "redstone/worldgen/generator_factory.hpp"

#include "redstone/worldgen/biome_source.hpp"
#include "redstone/worldgen/noise_chunk_generator.hpp"
#include "redstone/worldgen/noise_router.hpp"

#include <stdexcept>
#include <string>

namespace redstone::worldgen {
namespace {

std::shared_ptr<ChunkGenerator>
makeNoiseGenerator(const WorldgenRegistries &registries,
                   std::string_view settingsId,
                   std::shared_ptr<const BiomeSource> biomeSource,
                   const DimensionTypeSpec &type, std::string_view dimensionId) {
  const NoiseGeneratorSettings *settings =
      registries.findNoiseSettings(settingsId);
  if (!settings) {
    throw std::runtime_error("missing 26.1 noise settings '" +
                             std::string(settingsId) + "' for dimension " +
                             std::string(dimensionId));
  }
  return std::make_shared<NoiseBasedChunkGenerator>(
      registries, *settings, std::move(biomeSource), type.minY, type.height);
}

} // namespace

std::shared_ptr<ChunkGenerator>
createVanillaChunkGenerator(const WorldgenRegistries &registries,
                            std::string_view dimensionId,
                            const DimensionTypeSpec &type) {
  const std::string id = normalizeDimensionId(dimensionId);
  if (id == "minecraft:overworld") {
    return makeNoiseGenerator(
        registries, "minecraft:overworld",
        std::make_shared<MultiNoiseBiomeSource>(
            MultiNoiseBiomeSource::createOverworld()),
        type, id);
  }
  if (id == "minecraft:overworld_caves") {
    return makeNoiseGenerator(
        registries, "minecraft:caves",
        std::make_shared<MultiNoiseBiomeSource>(
            MultiNoiseBiomeSource::createOverworld()),
        type, id);
  }
  if (id == "minecraft:the_nether") {
    return makeNoiseGenerator(
        registries, "minecraft:nether",
        std::make_shared<MultiNoiseBiomeSource>(
            MultiNoiseBiomeSource::createNether()),
        type, id);
  }
  if (id == "minecraft:the_end") {
    return makeNoiseGenerator(registries, "minecraft:end",
                              std::make_shared<TheEndBiomeSource>(), type, id);
  }
  return nullptr;
}

} // namespace redstone::worldgen
