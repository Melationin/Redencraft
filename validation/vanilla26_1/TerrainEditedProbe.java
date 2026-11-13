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

public final class TerrainEditedProbe {
  private TerrainEditedProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 1)
      throw new IllegalArgumentException("usage: <cpp-edited-region-dir>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    Path directory = Path.of(args[0]);
    RegionStorageInfo info = new RegionStorageInfo(
        "terrain-w2b", net.minecraft.world.level.Level.OVERWORLD, "region");
    Path file = directory.resolve("r.-1.-1.mca");
    try (RegionFile region = new RegionFile(info, file, directory, false);
         DataInputStream in = region.getChunkDataInputStream(new ChunkPos(-1, -1))) {
      if (in == null) throw new IllegalStateException("edited chunk missing");
      CompoundTag root = NbtIo.read(in);
      if (root.getIntOr("DataVersion", 0) != 4786 ||
          root.getIntOr("xPos", 0) != -1 || root.getIntOr("zPos", 0) != -1 ||
          !root.getStringOr("Status", "").equals("minecraft:full"))
        throw new IllegalStateException("edited core fields changed");
      if (root.getBooleanOr("isLightOn", true))
        throw new IllegalStateException("edited chunk retained light correctness");
      if (!root.getCompoundOrEmpty("Heightmaps").isEmpty())
        throw new IllegalStateException("edited chunk retained old heightmaps");
      if (!root.contains("unknown_compound") ||
          !root.contains("block_entities"))
        throw new IllegalStateException("unedited fields were lost");
      ListTag sections = root.getListOrEmpty("sections");
      if (sections.isEmpty())
        throw new IllegalStateException("sections missing");
      CompoundTag states = sections.getCompoundOrEmpty(0)
          .getCompoundOrEmpty("block_states");
      ListTag palette = states.getListOrEmpty("palette");
      if (palette.size() != 17)
        throw new IllegalStateException("edited palette size is " + palette.size());
      System.out.println("JAVA_READ_CPP_EDIT_OK");
    }
  }
}
