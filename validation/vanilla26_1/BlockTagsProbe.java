import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.SharedConstants;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.tags.TagLoader;

// Use Mojang's tag loader to resolve nested entries exactly as the vanilla pack.
public final class BlockTagsProbe {
  public static void main(String[] args) throws Exception {
    if (args.length != 1) throw new IllegalArgumentException("expected output path");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    if (!SharedConstants.getCurrentVersion().name().equals("26.1"))
      throw new IllegalStateException("requires official 26.1");
    try (var resources = new MultiPackResourceManager(PackType.SERVER_DATA,
            List.of(ServerPacksSource.createVanillaPackSource()))) {
      var layers = RegistryLayer.createRegistryAccess();
      var tags = TagLoader.loadTagsForExistingRegistries(resources, layers.getLayer(RegistryLayer.STATIC));
      for (var tag : tags) tag.apply();
      var rows = new ArrayList<String>();
      BuiltInRegistries.BLOCK.getTags().forEach(tag -> {
        var members = new ArrayList<String>();
        tag.forEach(holder -> members.add(BuiltInRegistries.BLOCK.getKey(holder.value()).toString()));
        members.sort(String::compareTo);
        rows.add(tag.key().location().toString() + "\t" + String.join(",", members));
      });
      rows.sort(String::compareTo);
      Files.writeString(Path.of(args[0]), String.join("\n", rows) + "\n", StandardCharsets.UTF_8);
    }
    net.minecraft.util.Util.shutdownExecutors();
  }
}
