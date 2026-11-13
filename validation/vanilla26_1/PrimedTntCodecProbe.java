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

public final class PrimedTntCodecProbe {
  private PrimedTntCodecProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 2)
      throw new IllegalArgumentException(
          "usage: PrimedTntCodecProbe <cpp-nbt> <official-rewritten-nbt>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();

    BlockState defaultState = Blocks.TNT.defaultBlockState();
    Tag officialState =
        BlockState.CODEC.encodeStart(NbtOps.INSTANCE, defaultState).getOrThrow();
    CompoundTag officialCompound = (CompoundTag) officialState;
    if (!officialCompound.getStringOr("Name", "").equals("minecraft:tnt") ||
        !officialCompound.getCompoundOrEmpty("Properties")
            .getStringOr("unstable", "")
            .equals("false"))
      throw new IllegalStateException(
          "official default TNT codec shape changed: " + officialState);

    CompoundTag root = NbtIo.read(Path.of(args[0]));
    if (root.getIntOr("DataVersion", 0) != 4786)
      throw new IllegalStateException("C++ entity DataVersion mismatch");
    int[] position = root.getIntArray("Position").orElseThrow();
    if (position.length != 2 || position[0] != -1 || position[1] != -1)
      throw new IllegalStateException("C++ entity Position mismatch");
    ListTag entities = root.getListOrEmpty("Entities");
    if (entities.size() != 3)
      throw new IllegalStateException("C++ mixed entity count=" + entities.size());

    CompoundTag first = entities.getCompoundOrEmpty(0);
    CompoundTag tnt = entities.getCompoundOrEmpty(1);
    CompoundTag last = entities.getCompoundOrEmpty(2);
    if (!first.getStringOr("id", "").equals("minecraft:item") ||
        !tnt.getStringOr("id", "").equals("minecraft:tnt") ||
        !last.getStringOr("id", "").equals("minecraft:item"))
      throw new IllegalStateException("C++ mixed entity order changed");
    int[] firstUuid = first.getIntArray("UUID").orElseThrow();
    int[] tntUuid = tnt.getIntArray("UUID").orElseThrow();
    int[] lastUuid = last.getIntArray("UUID").orElseThrow();
    if (firstUuid.length != 4 || firstUuid[0] != 1 ||
        tntUuid.length != 4 || tntUuid[0] != 31 ||
        lastUuid.length != 4 || lastUuid[0] != 41)
      throw new IllegalStateException("C++ mixed UUID/order mismatch");
    if (tnt.getShortOr("fuse", (short)-1) != 47)
      throw new IllegalStateException("C++ TNT fuse mismatch");
    ListTag motion = tnt.getListOrEmpty("Motion");
    if (motion.size() != 3 || motion.getDoubleOr(0, Double.NaN) != 0.0 ||
        motion.getDoubleOr(1, Double.NaN) != 0.0 ||
        motion.getDoubleOr(2, Double.NaN) != 0.0625)
      throw new IllegalStateException("C++ TNT motion mismatch");

    Tag cppState = tnt.get("block_state");
    if (cppState == null || !cppState.equals(officialState))
      throw new IllegalStateException(
          "C++ block_state differs from official codec\ncpp=" + cppState +
          "\nofficial=" + officialState);
    BlockState parsed =
        BlockState.CODEC.parse(NbtOps.INSTANCE, cppState).getOrThrow();
    if (!parsed.equals(defaultState))
      throw new IllegalStateException("official codec rejected C++ default state");
    tnt.put("block_state", officialState);
    NbtIo.write(root, Path.of(args[1]));
    System.out.println("JAVA_26_1_TNT_CODEC_OK official=" + officialState +
                       " order=item,tnt,item uuid/fuse/motion=1");
    System.out.println("JAVA_26_1_TNT_REWRITE=" + args[1]);
  }
}
