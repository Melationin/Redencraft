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

public final class ItemEntityReadProbe {
  private ItemEntityReadProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 2 || !(args[0].equals("mid") || args[0].equals("empty")))
      throw new IllegalArgumentException("usage: <mid|empty> <entities-region>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    Path directory = Path.of(args[1]);
    RegionStorageInfo info = new RegionStorageInfo(
        "item-w3b2a", net.minecraft.world.level.Level.OVERWORLD, "entities");
    try (RegionFile region = new RegionFile(
             info, directory.resolve("r.-1.-1.mca"), directory, false);
         DataInputStream in = region.getChunkDataInputStream(new ChunkPos(-1, -1))) {
      if (in == null)
        throw new IllegalStateException("item entity chunk missing");
      CompoundTag root = NbtIo.read(in);
      if (root.getIntOr("DataVersion", 0) != 4786 ||
          root.getIntArray("Position").orElseThrow().length != 2)
        throw new IllegalStateException("entity root envelope mismatch");
      ListTag entities = root.getListOrEmpty("Entities");
      if (args[0].equals("empty")) {
        if (!entities.isEmpty())
          throw new IllegalStateException("removed item is still persisted");
        System.out.println("JAVA_READ_CPP_EMPTY_ITEM_ENTITIES_OK");
        return;
      }
      if (entities.size() != 1)
        throw new IllegalStateException("item count=" + entities.size());
      CompoundTag item = entities.getCompoundOrEmpty(0);
      if (!item.getStringOr("id", "").equals("minecraft:item") ||
          item.getShortOr("Age", (short)-1) != 18 ||
          item.getShortOr("PickupDelay", (short)-1) != 199 ||
          item.getShortOr("Health", (short)-1) != 5 ||
          !item.getCompoundOrEmpty("Item").getStringOr("id", "")
              .equals("minecraft:stone") ||
          item.getCompoundOrEmpty("Item").getIntOr("count", 0) != 3)
        throw new IllegalStateException("item fields mismatch");
      int[] uuid = item.getIntArray("UUID").orElseThrow();
      if (uuid.length != 4 || uuid[0] != 1000665188 ||
          uuid[1] != 1491226247 || uuid[2] != -1939110731 ||
          uuid[3] != 1144620115)
        throw new IllegalStateException("item UUID mismatch");
      System.out.println("JAVA_READ_CPP_ITEM_ENTITY_MID_OK");
    }
  }
}
