import net.minecraft.SharedConstants;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.nbt.NbtIo;
import net.minecraft.server.Bootstrap;

import java.nio.file.Path;

public final class MinecartCodecProbe {
  private MinecartCodecProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 2)
      throw new IllegalArgumentException(
          "usage: MinecartCodecProbe <cpp-nbt> <official-rewritten-nbt>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    CompoundTag root = NbtIo.read(Path.of(args[0]));
    if (root.getIntOr("DataVersion", 0) != 4786)
      throw new IllegalStateException("C++ entity DataVersion mismatch");
    ListTag entities = root.getListOrEmpty("Entities");
    if (entities.size() != 4 ||
        !entities.getCompoundOrEmpty(0).getStringOr("id", "")
            .equals("minecraft:item") ||
        !entities.getCompoundOrEmpty(1).getStringOr("id", "")
            .equals("minecraft:minecart") ||
        !entities.getCompoundOrEmpty(2).getStringOr("id", "")
            .equals("minecraft:tnt") ||
        !entities.getCompoundOrEmpty(3).getStringOr("id", "")
            .equals("minecraft:falling_block"))
      throw new IllegalStateException("C++ mixed entity order mismatch");
    CompoundTag cart = entities.getCompoundOrEmpty(1);
    int[] uuid = cart.getIntArray("UUID").orElseThrow();
    if (uuid.length != 4 || uuid[0] != 71 || uuid[1] != 72 ||
        uuid[2] != 73 || uuid[3] != 74)
      throw new IllegalStateException("C++ minecart UUID mismatch");
    if (cart.getByteOr("FlippedRotation", (byte)-1) != 1 ||
        cart.getByteOr("HasTicked", (byte)-1) != 0 ||
        cart.contains("DisplayState") || cart.contains("DisplayOffset"))
      throw new IllegalStateException("C++ minecart flags/display mismatch");
    ListTag motion = cart.getListOrEmpty("Motion");
    if (motion.size() != 3 || motion.getDoubleOr(0, Double.NaN) != 0.125 ||
        motion.getDoubleOr(1, Double.NaN) != 0.02 ||
        motion.getDoubleOr(2, Double.NaN) != 0.0625)
      throw new IllegalStateException("C++ minecart motion mismatch");
    NbtIo.write(root, Path.of(args[1]));
    System.out.println("JAVA_26_1_MINECART_CODEC_OK order=item,minecart,tnt,falling "
                       + "uuid/flags/motion=1");
  }
}
