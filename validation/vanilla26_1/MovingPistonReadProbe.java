import net.minecraft.SharedConstants;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.nbt.NbtIo;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.storage.RegionFile;
import net.minecraft.world.level.chunk.storage.RegionStorageInfo;

import java.io.DataInputStream;
import java.nio.file.Path;

public final class MovingPistonReadProbe {
  private MovingPistonReadProbe() {}

  private static CompoundTag section(CompoundTag root, int sectionY) {
    ListTag sections = root.getListOrEmpty("sections");
    for (int i = 0; i < sections.size(); ++i) {
      CompoundTag section = sections.getCompoundOrEmpty(i);
      if (section.getByteOr("Y", (byte) 99) == (byte) sectionY)
        return section;
    }
    throw new IllegalStateException("section missing: " + sectionY);
  }

  private static String blockAt(CompoundTag root, int x, int y, int z) {
    CompoundTag states = section(root, y >> 4).getCompoundOrEmpty("block_states");
    ListTag palette = states.getListOrEmpty("palette");
    if (palette.isEmpty())
      throw new IllegalStateException("empty block palette");
    int cell = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15);
    int value = 0;
    if (palette.size() > 1) {
      int bits = Math.max(4, 32 - Integer.numberOfLeadingZeros(palette.size() - 1));
      long[] data = states.getLongArray("data").orElseThrow();
      int perLong = 64 / bits;
      value = (int)((data[cell / perLong] >>> ((cell % perLong) * bits)) &
                    ((1L << bits) - 1));
    }
    return palette.getCompoundOrEmpty(value).getStringOr("Name", "");
  }

  private static CompoundTag entityAt(CompoundTag root, int x, int y, int z) {
    ListTag entities = root.getListOrEmpty("block_entities");
    for (int i = 0; i < entities.size(); ++i) {
      CompoundTag entity = entities.getCompoundOrEmpty(i);
      if (entity.getIntOr("x", 0) == x && entity.getIntOr("y", 0) == y &&
          entity.getIntOr("z", 0) == z)
        return entity;
    }
    throw new IllegalStateException("piston block entity missing at " + x + "," + y + "," + z);
  }

  private static void requireEntity(CompoundTag root, int x, int y, int z,
                                    String moved, int source) {
    CompoundTag entity = entityAt(root, x, y, z);
    if (!entity.getStringOr("id", "").equals("minecraft:piston") ||
        entity.getByteOr("facing", (byte) -1) != 5 ||
        entity.getByteOr("extending", (byte) 0) != 1 ||
        entity.getByteOr("source", (byte) -1) != source ||
        entity.getFloatOr("progress", -1.0f) != 0.0f ||
        entity.getByteOr("keepPacked", (byte) -1) != 0 ||
        !entity.getCompoundOrEmpty("components").isEmpty() ||
        !entity.getCompoundOrEmpty("blockState").getStringOr("Name", "").equals(moved))
      throw new IllegalStateException("invalid moving piston entity at " + x + "," + y + "," + z);
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 2 || !(args[0].equals("mid") || args[0].equals("complete")))
      throw new IllegalArgumentException("usage: <mid|complete> <region-dir>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    Path directory = Path.of(args[1]);
    RegionStorageInfo info = new RegionStorageInfo(
        "moving-piston-w3b1", net.minecraft.world.level.Level.OVERWORLD, "region");
    try (RegionFile region = new RegionFile(
             info, directory.resolve("r.-1.-1.mca"), directory, false);
         DataInputStream in = region.getChunkDataInputStream(new ChunkPos(-1, -1))) {
      if (in == null)
        throw new IllegalStateException("piston chunk missing");
      CompoundTag root = NbtIo.read(in);
      if (root.getIntOr("DataVersion", 0) != 4786)
        throw new IllegalStateException("DataVersion mismatch");
      ListTag entities = root.getListOrEmpty("block_entities");
      if (args[0].equals("mid")) {
        if (entities.size() != 2)
          throw new IllegalStateException("mid entity count=" + entities.size());
        requireEntity(root, -13, -58, -14, "minecraft:piston_head", 1);
        requireEntity(root, -12, -58, -14, "minecraft:stone", 0);
        if (!blockAt(root, -13, -58, -14).equals("minecraft:moving_piston") ||
            !blockAt(root, -12, -58, -14).equals("minecraft:moving_piston"))
          throw new IllegalStateException("mid terrain is not moving_piston");
        System.out.println("JAVA_READ_CPP_MOVING_PISTON_MID_OK");
      } else {
        if (!entities.isEmpty())
          throw new IllegalStateException("completed chunk retained piston entities");
        if (!blockAt(root, -13, -58, -14).equals("minecraft:piston_head") ||
            !blockAt(root, -12, -58, -14).equals("minecraft:stone"))
          throw new IllegalStateException("completed terrain mismatch");
        System.out.println("JAVA_READ_CPP_MOVING_PISTON_COMPLETE_OK");
      }
    }
  }
}
