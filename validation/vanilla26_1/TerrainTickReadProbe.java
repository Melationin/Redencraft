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

public final class TerrainTickReadProbe {
  private TerrainTickReadProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 1)
      throw new IllegalArgumentException("usage: <region-dir>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    Path directory = Path.of(args[0]);
    RegionStorageInfo info = new RegionStorageInfo(
        "tick-cycle-w3a", net.minecraft.world.level.Level.OVERWORLD, "region");
    Path file = directory.resolve("r.-1.-1.mca");
    try (RegionFile region = new RegionFile(info, file, directory, false);
         DataInputStream in =
             region.getChunkDataInputStream(new ChunkPos(-1, -1))) {
      if (in == null)
        throw new IllegalStateException("saved tick chunk missing");
      CompoundTag root = NbtIo.read(in);
      ListTag blocks = root.getListOrEmpty("block_ticks");
      ListTag fluids = root.getListOrEmpty("fluid_ticks");
      if (blocks.size() != 3 || fluids.size() != 1)
        throw new IllegalStateException(
            "unexpected tick sizes blocks=" + blocks.size() +
            " fluids=" + fluids.size());
      for (int i = 0; i < blocks.size(); ++i) {
        CompoundTag tick = blocks.getCompoundOrEmpty(i);
        if (tick.getIntOr("t", -999) != 5 ||
            tick.getIntOr("p", -999) != 1 ||
            tick.contains("SubTick"))
          throw new IllegalStateException("invalid saved block tick");
      }
      CompoundTag fluid = fluids.getCompoundOrEmpty(0);
      if (fluid.getIntOr("t", -999) != 7 ||
          fluid.getIntOr("p", -999) != -1 ||
          fluid.contains("SubTick"))
        throw new IllegalStateException("invalid saved fluid tick");
      if (!root.contains("tick_cycle_extension"))
        throw new IllegalStateException("unknown NBT extension was lost");
      System.out.println("JAVA_READ_CPP_TICKS_OK");
    }
  }
}
