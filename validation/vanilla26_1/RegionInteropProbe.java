import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;

import net.minecraft.SharedConstants;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.storage.RegionFile;
import net.minecraft.world.level.chunk.storage.RegionStorageInfo;

public final class RegionInteropProbe {
  private RegionInteropProbe() {}

  private static RegionStorageInfo info() {
    return new RegionStorageInfo("ticket-w2a",
        net.minecraft.world.level.Level.OVERWORLD, "region");
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 3)
      throw new IllegalArgumentException(
          "usage: <read|write> <cpp-region-dir> <java-region-dir>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    RegionStorageInfo info = info();
    if (args[0].equals("write")) {
      Path java = Path.of(args[2]);
      Files.createDirectories(java);
      Path file = java.resolve("r.-1.-1.mca");
      try (RegionFile region = new RegionFile(info, file, java, false)) {
        for (ChunkPos pos : new ChunkPos[]{
            new ChunkPos(-1, -1), new ChunkPos(-1, 0)}) {
          try (DataOutputStream out = region.getChunkDataOutputStream(pos)) {
            CompoundTag tag = new CompoundTag();
            tag.putInt("probe", pos.x() * 10 + pos.z());
            tag.putString("name", "java-w2a");
            net.minecraft.nbt.NbtIo.write(tag, out);
          }
        }
        region.flush();
      }
      System.out.println("JAVA_WRITE_OK");
      return;
    }
    if (!args[0].equals("read"))
      throw new IllegalArgumentException("operation must be read or write");

    Path cpp = Path.of(args[1]);
    for (ChunkPos pos : new ChunkPos[]{
        new ChunkPos(-1, -1), new ChunkPos(-1, 0)}) {
      Path file = cpp.resolve(
          "r." + pos.getRegionX() + "." + pos.getRegionZ() + ".mca");
      try (RegionFile region = new RegionFile(info, file, cpp, false)) {
        if (!region.doesChunkExist(pos))
          throw new IllegalStateException("missing " + pos);
        try (DataInputStream in = region.getChunkDataInputStream(pos)) {
          if (in == null)
            throw new IllegalStateException("unreadable " + pos);
          CompoundTag tag = net.minecraft.nbt.NbtIo.read(in);
          System.out.printf("JAVA_READ_CPP\t%d\t%d\t%d\t%s%n",
              pos.x(), pos.z(), tag.getIntOr("probe", -1),
              tag.getStringOr("name", ""));
        }
      }
    }
    System.out.println("JAVA_READ_OK");
  }
}
