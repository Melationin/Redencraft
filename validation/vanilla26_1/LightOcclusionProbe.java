import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeMap;

import net.minecraft.SharedConstants;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.phys.AABB;

public final class LightOcclusionProbe {
  private static final Direction[] DIRECTIONS = Direction.values();
  private static final int SCALE = 1_000_000;

  private LightOcclusionProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 2) {
      throw new IllegalArgumentException(
          "usage: LightOcclusionProbe <names> <output>");
    }
    Set<String> wanted = new HashSet<>();
    for (String line : Files.readAllLines(Path.of(args[0]), StandardCharsets.UTF_8)) {
      line = line.strip();
      if (!line.isEmpty()) wanted.add(line);
    }
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();

    var rows = new ArrayList<String>();
    for (Block block : BuiltInRegistries.BLOCK) {
      Identifier id = BuiltInRegistries.BLOCK.getKey(block);
      if (!wanted.contains(id.toString())) continue;
      for (BlockState state : block.getStateDefinition().getPossibleStates()) {
        if (!state.useShapeForLightOcclusion()) continue;
        TreeMap<String, String> properties = new TreeMap<>();
        state.getValues().forEach(value ->
            properties.put(value.property().getName(), propertyValue(value)));
        var props = new StringBuilder();
        for (var entry : properties.entrySet()) {
          if (!props.isEmpty()) props.append(',');
          props.append(entry.getKey()).append('=').append(entry.getValue());
        }
        var fields = new ArrayList<String>();
        fields.add(id.toString());
        fields.add(props.toString());
        if (!state.canOcclude()) {
          for (int direction = 0; direction < DIRECTIONS.length; ++direction)
            fields.add("");
        } else {
          for (Direction direction : DIRECTIONS)
            fields.add(faceRectangles(state, direction));
        }
        rows.add(String.join("\t", fields));
      }
    }
    rows.sort(String::compareTo);
    Files.writeString(Path.of(args[1]), String.join("\n", rows) + "\n",
        StandardCharsets.UTF_8);
  }

  private static String faceRectangles(BlockState state, Direction direction) {
    var rectangles = new ArrayList<String>();
    for (AABB box : state.getFaceOcclusionShape(direction).toAabbs()) {
      double minA;
      double maxA;
      double minB;
      double maxB;
      switch (direction.getAxis()) {
        case X -> { minA = box.minZ; maxA = box.maxZ; minB = box.minY; maxB = box.maxY; }
        case Y -> { minA = box.minX; maxA = box.maxX; minB = box.minZ; maxB = box.maxZ; }
        case Z -> { minA = box.minX; maxA = box.maxX; minB = box.minY; maxB = box.maxY; }
        default -> throw new IllegalStateException();
      }
      int minAI = scaled(minA);
      int maxAI = scaled(maxA);
      int minBI = scaled(minB);
      int maxBI = scaled(maxB);
      if (minAI >= maxAI || minBI >= maxBI) continue;
      rectangles.add(minAI + ":" + maxAI + ":" + minBI + ":" + maxBI);
    }
    rectangles = new ArrayList<>(rectangles.stream().distinct().sorted().toList());
    return String.join(";", rectangles);
  }

  private static int scaled(double value) {
    return (int)Math.round(Math.max(0.0, Math.min(1.0, value)) * SCALE);
  }

  @SuppressWarnings({"rawtypes", "unchecked"})
  private static String propertyValue(Property.Value<?> value) {
    Property property = value.property();
    return property.getName(value.value());
  }
}
