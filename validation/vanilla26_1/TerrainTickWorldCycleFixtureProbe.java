import net.minecraft.SharedConstants;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.nbt.NbtIo;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.storage.RegionFile;
import net.minecraft.world.level.chunk.storage.RegionStorageInfo;

import java.io.DataOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;

public final class TerrainTickWorldCycleFixtureProbe {
  private TerrainTickWorldCycleFixtureProbe() {}

  private static CompoundTag tick(String id, int x, int y, int z, int delay,
                                  int priority) {
    CompoundTag tag = new CompoundTag();
    tag.putString("i", id);
    tag.putInt("x", x);
    tag.putInt("y", y);
    tag.putInt("z", z);
    tag.putInt("t", delay);
    tag.putInt("p", priority);
    return tag;
  }

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
    CompoundTag states = new CompoundTag();
    ListTag palette = new ListTag();
    CompoundTag stone = new CompoundTag();
    stone.putString("Name", "minecraft:stone");
    palette.add(stone);
    states.put("palette", palette);
    section.put("block_states", states);
    ListTag sections = new ListTag();
    sections.add(section);
    root.put("sections", sections);

    ListTag blocks = new ListTag();
    blocks.add(tick("minecraft:stone", -16, 0, -16, 5, 1));
    blocks.add(tick("minecraft:stone", -16, 0, -16, 5, 1));
    blocks.add(tick("minecraft:stone", -15, 0, -16, 5, 1));
    root.put("block_ticks", blocks);
    ListTag fluids = new ListTag();
    fluids.add(tick("minecraft:water", -14, 0, -16, 7, -1));
    root.put("fluid_ticks", fluids);

    CompoundTag unknown = new CompoundTag();
    unknown.putLong("preserved", 0x123456789abcdefL);
    root.put("tick_cycle_extension", unknown);

    RegionStorageInfo info = new RegionStorageInfo(
        "tick-cycle-w3a", net.minecraft.world.level.Level.OVERWORLD, "region");
    Path file = directory.resolve("r.-1.-1.mca");
    try (RegionFile region = new RegionFile(info, file, directory, false);
         DataOutputStream out =
             region.getChunkDataOutputStream(new ChunkPos(-1, -1))) {
      NbtIo.write(root, out);
      region.flush();
    }
    System.out.println("JAVA_TICK_WORLD_FIXTURE_OK");
  }
}
