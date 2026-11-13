import net.minecraft.SharedConstants;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.nbt.NbtIo;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.storage.RegionFile;
import net.minecraft.world.level.chunk.storage.RegionStorageInfo;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;

public final class TerrainFixtureProbe {
  private TerrainFixtureProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 1)
      throw new IllegalArgumentException("usage: <region-dir>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    Path directory = Path.of(args[0]);
    Files.createDirectories(directory);
    CompoundTag root = new CompoundTag();
    root.putInt("DataVersion", 4786);
    root.putInt("xPos", -1);
    root.putInt("zPos", -1);
    root.putString("Status", "minecraft:full");
    root.putBoolean("isLightOn", true);

    CompoundTag section = new CompoundTag();
    section.putByte("Y", (byte) 0);
    CompoundTag blockStates = new CompoundTag();
    ListTag palette = new ListTag();
    CompoundTag stone = new CompoundTag();
    stone.putString("Name", "minecraft:stone");
    palette.add(stone);
    blockStates.put("palette", palette);
    section.put("block_states", blockStates);
    CompoundTag biomes = new CompoundTag();
    ListTag biomePalette = new ListTag();
    CompoundTag plains = new CompoundTag();
    plains.putString("Name", "minecraft:plains");
    biomePalette.add(plains);
    biomes.put("palette", biomePalette);
    section.put("biomes", biomes);
    section.putByteArray("BlockLight", new byte[2048]);
    section.putByteArray("SkyLight", new byte[2048]);
    ListTag sections = new ListTag();
    sections.add(section);
    root.put("sections", sections);

    CompoundTag heightmap = new CompoundTag();
    heightmap.putLongArray("WORLD_SURFACE", new long[]{1L, 2L});
    root.put("Heightmaps", heightmap);
    ListTag empty = new ListTag();
    root.put("block_ticks", empty.copy());
    root.put("fluid_ticks", empty.copy());
    ListTag entities = new ListTag();
    CompoundTag chest = new CompoundTag();
    chest.putString("id", "minecraft:chest");
    chest.putInt("x", -1);
    chest.putInt("y", 64);
    chest.putInt("z", -1);
    entities.add(chest);
    root.put("block_entities", entities);
    root.put("unknown_empty", new ListTag());
    CompoundTag unknown = new CompoundTag();
    unknown.putLong("keep_long_width", 0x123456789abcdefL);
    root.put("unknown_compound", unknown);

    RegionStorageInfo info =
        new RegionStorageInfo("terrain-w2b", net.minecraft.world.level.Level.OVERWORLD, "region");
    Path file = directory.resolve("r.-1.-1.mca");
    try (RegionFile region = new RegionFile(info, file, directory, false)) {
      try (DataOutputStream out = region.getChunkDataOutputStream(new ChunkPos(-1, -1))) {
        NbtIo.write(root, out);
      }
      region.flush();
    }
    try (RegionFile region = new RegionFile(info, file, directory, false);
         DataInputStream in = region.getChunkDataInputStream(new ChunkPos(-1, -1))) {
      if (in == null) throw new IllegalStateException("generated chunk is missing");
      CompoundTag read = NbtIo.read(in);
      if (read.getIntOr("DataVersion", 0) != 4786)
        throw new IllegalStateException("DataVersion mismatch");
      System.out.println("JAVA_FULL_FIXTURE_OK");
    }
  }
}
