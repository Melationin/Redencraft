import net.minecraft.SharedConstants;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.nbt.NbtIo;
import net.minecraft.nbt.NbtOps;
import net.minecraft.nbt.Tag;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;

import java.nio.file.Path;

public final class FallingBlockCodecProbe {
  private FallingBlockCodecProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 2)
      throw new IllegalArgumentException(
          "usage: FallingBlockCodecProbe <cpp-nbt> <official-rewritten-nbt>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    BlockState sand = Blocks.SAND.defaultBlockState();
    Tag officialState = BlockState.CODEC.encodeStart(NbtOps.INSTANCE, sand)
                            .getOrThrow();
    CompoundTag officialCompound = (CompoundTag) officialState;
    if (!officialCompound.getStringOr("Name", "").equals("minecraft:sand") ||
        officialCompound.contains("Properties"))
      throw new IllegalStateException(
          "official sand codec shape changed: " + officialState);

    CompoundTag root = NbtIo.read(Path.of(args[0]));
    if (root.getIntOr("DataVersion", 0) != 4786)
      throw new IllegalStateException("C++ entity DataVersion mismatch");
    ListTag entities = root.getListOrEmpty("Entities");
    if (entities.size() != 4)
      throw new IllegalStateException("C++ mixed entity count=" + entities.size());
    if (!entities.getCompoundOrEmpty(0).getStringOr("id", "")
            .equals("minecraft:item") ||
        !entities.getCompoundOrEmpty(1).getStringOr("id", "")
            .equals("minecraft:falling_block") ||
        !entities.getCompoundOrEmpty(2).getStringOr("id", "")
            .equals("minecraft:tnt") ||
        !entities.getCompoundOrEmpty(3).getStringOr("id", "")
            .equals("minecraft:item"))
      throw new IllegalStateException("C++ mixed entity order changed");

    CompoundTag falling = entities.getCompoundOrEmpty(1);
    int[] uuid = falling.getIntArray("UUID").orElseThrow();
    if (uuid.length != 4 || uuid[0] != 51 || uuid[1] != 52 ||
        uuid[2] != 53 || uuid[3] != 54)
      throw new IllegalStateException("C++ falling block UUID mismatch");
    if (falling.getIntOr("Time", -1) != 23 ||
        falling.getByteOr("DropItem", (byte)-1) != 0 ||
        falling.getByteOr("CancelDrop", (byte)-1) != 1 ||
        falling.getByteOr("HurtEntities", (byte)-1) != 0 ||
        falling.getFloatOr("FallHurtAmount", -1.0f) != 0.0f ||
        falling.getIntOr("FallHurtMax", -1) != 40)
      throw new IllegalStateException("C++ falling block fields mismatch");
    ListTag motion = falling.getListOrEmpty("Motion");
    if (motion.size() != 3 || motion.getDoubleOr(1, Double.NaN) != -0.04)
      throw new IllegalStateException("C++ falling block motion mismatch");

    Tag cppState = falling.get("BlockState");
    if (cppState == null || !cppState.equals(officialState))
      throw new IllegalStateException(
          "C++ falling block state differs from official codec\ncpp=" +
          cppState + "\nofficial=" + officialState);
    BlockState parsed =
        BlockState.CODEC.parse(NbtOps.INSTANCE, cppState).getOrThrow();
    if (!parsed.equals(sand))
      throw new IllegalStateException("official codec rejected C++ sand state");
    falling.put("BlockState", officialState);
    NbtIo.write(root, Path.of(args[1]));
    System.out.println("JAVA_26_1_FALLING_CODEC_OK official=" + officialState +
                       " order=item,falling,tnt,item fields=1");
  }
}
