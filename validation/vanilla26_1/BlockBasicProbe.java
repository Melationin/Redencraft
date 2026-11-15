import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.*;
import net.minecraft.world.level.block.piston.MovingPistonBlock;
import net.minecraft.world.level.block.piston.PistonBaseBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.VoxelShape;

// Read-only official 26.1 registry & state-shape probe.
public final class BlockBasicProbe {
  private static String fmt(double v) {
    double scaled = v * 16.0;
    long rounded = Math.round(scaled);
    if (Math.abs(scaled - rounded) < 1e-9) {
      return Long.toString(rounded);
    }
    return Double.toString(scaled);
  }

  private static String fmtFloat(float v) {
    if (v == (long) v) {
      return Long.toString((long) v);
    }
    return Float.toString(v);
  }

  private static String encodeShape(VoxelShape shape) {
    if (shape.isEmpty()) return "";
    List<AABB> boxes = shape.toAabbs();
    var sb = new StringBuilder();
    for (int i = 0; i < boxes.size(); ++i) {
      if (i > 0) sb.append(';');
      AABB b = boxes.get(i);
      sb.append(fmt(b.minX)).append(',')
        .append(fmt(b.minY)).append(',')
        .append(fmt(b.minZ)).append(',')
        .append(fmt(b.maxX)).append(',')
        .append(fmt(b.maxY)).append(',')
        .append(fmt(b.maxZ));
    }
    return sb.toString();
  }

  @SuppressWarnings({"rawtypes", "unchecked"})
  private static String encodeProperties(Block block) {
    BlockState def = block.defaultBlockState();
    var parts = new ArrayList<String>();
    for (Property prop : block.getStateDefinition().getProperties()) {
      var vals = new ArrayList<String>();
      for (Object v : prop.getPossibleValues()) {
        vals.add(prop.getName((Comparable) v));
      }
      String defVal = prop.getName((Comparable) def.getValue(prop));
      parts.add(prop.getName() + "=" + String.join("|", vals) + "=" + defVal);
    }
    return String.join(";", parts);
  }

  private static String encodeFlags(Block block) {
    var flags = new ArrayList<String>();
    if (block instanceof AirBlock) flags.add("air_like");
    if (block instanceof BaseRailBlock) flags.add("rail");
    if (block instanceof LiquidBlock) flags.add("liquid");
    if (block instanceof CarpetBlock) flags.add("carpet");
    if (block instanceof WallBlock) flags.add("wall");
    if (block instanceof FenceBlock) flags.add("fence");
    if (block instanceof FenceGateBlock) flags.add("fence_gate");
    if (block instanceof StairBlock) flags.add("stairs");
    if (block instanceof SlabBlock) flags.add("slab");
    if (block instanceof DoorBlock) flags.add("door");
    if (block instanceof TrapDoorBlock) flags.add("trapdoor");
    if (block instanceof ButtonBlock) flags.add("button");
    if (block instanceof BasePressurePlateBlock) flags.add("pressure_plate");
    if (block instanceof SignBlock) flags.add("sign");
    if (block instanceof LeavesBlock) flags.add("leaves");
    if (block instanceof SaplingBlock) flags.add("sapling");
    if (block instanceof FallingBlock) flags.add("falling");
    if (block instanceof BaseCoralWallFanBlock) flags.add("coral_wall_fan");
    if (block instanceof BaseCoralFanBlock || block instanceof CoralFanBlock) flags.add("coral_fan");
    if (block instanceof SnowyBlock) flags.add("snowy");
    if (block instanceof FlowerBlock) flags.add("flower");
    if (block instanceof VegetationBlock) flags.add("vegetation");
    if (block instanceof DryVegetationBlock) flags.add("dry_vegetation");
    if (block instanceof RotatedPillarBlock) flags.add("rotated_pillar");
    if (block instanceof BedBlock) flags.add("bed");
    if (block instanceof AbstractChestBlock) flags.add("chest");
    if (block instanceof ShulkerBoxBlock) flags.add("shulker_box");
    if (block instanceof PistonBaseBlock) flags.add("piston");
    if (block instanceof MovingPistonBlock) flags.add("moving_piston");
    if (block instanceof CopperBulbBlock) flags.add("copper_bulb");
    if (block instanceof AbstractCandleBlock) flags.add("candle");
    if (block instanceof AbstractCauldronBlock) flags.add("cauldron");
    if (block instanceof AbstractBannerBlock) flags.add("banner");
    if (block instanceof AbstractSkullBlock) flags.add("skull");
    if (block instanceof SimpleWaterloggedBlock) flags.add("waterlogged_host");
    return String.join(",", flags);
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 2) {
      throw new IllegalArgumentException("usage: BlockBasicProbe <basics.tsv> <states.tsv>");
    }
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    if (!SharedConstants.getCurrentVersion().name().equals("26.1")) {
      throw new IllegalStateException("requires official 26.1");
    }

    var blockRows = new ArrayList<String>();
    var stateRows = new ArrayList<String>();
    int stateId = 0;

    for (Block block : BuiltInRegistries.BLOCK) {
      int id = BuiltInRegistries.BLOCK.getId(block);
      String name = BuiltInRegistries.BLOCK.getKey(block).toString();
      BlockState def = block.defaultBlockState();
      String push = def.getPistonPushReaction().name().toLowerCase(Locale.ROOT);
      boolean conductor = def.isRedstoneConductor(EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
      boolean liquid = block instanceof LiquidBlock;
      boolean liquidContainer = block instanceof LiquidBlockContainer;
      boolean replaceable = def.canBeReplaced();
      boolean ignitedByLava = def.ignitedByLava();
      String instrument = def.instrument().getSerializedName();
      boolean forceSolid = def.blocksMotion() && def.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO).isEmpty();

      blockRows.add(String.join("\t",
          Integer.toString(id),
          name,
          fmtFloat(block.defaultDestroyTime()),
          fmtFloat(block.getExplosionResistance()),
          fmtFloat(block.getFriction()),
          fmtFloat(block.getSpeedFactor()),
          push,
          conductor ? "1" : "0",
          liquid ? "1" : "0",
          liquidContainer ? "1" : "0",
          forceSolid ? "1" : "0",
          replaceable ? "1" : "0",
          ignitedByLava ? "1" : "0",
          instrument,
          encodeFlags(block),
          encodeProperties(block)));

      for (BlockState s : block.getStateDefinition().getPossibleStates()) {
        String col = encodeShape(s.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO));
        String out = encodeShape(s.getShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO));
        stateRows.add(String.join("\t",
            Integer.toString(stateId++),
            s.isRandomlyTicking() ? "1" : "0",
            s.blocksMotion() ? "1" : "0",
            s.isRedstoneConductor(EmptyBlockGetter.INSTANCE, BlockPos.ZERO) ? "1" : "0",
            col,
            out));
      }
    }

    Files.writeString(Path.of(args[0]), String.join("\n", blockRows) + "\n", StandardCharsets.UTF_8);
    Files.writeString(Path.of(args[1]), String.join("\n", stateRows) + "\n", StandardCharsets.UTF_8);
    System.out.println("Exported " + blockRows.size() + " blocks and " + stateRows.size() + " states");
  }
}
