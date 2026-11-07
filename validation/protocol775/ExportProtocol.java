import java.nio.file.*;
import java.io.*;
import java.util.*;
import io.netty.buffer.Unpooled;
import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.core.*;
import net.minecraft.core.registries.*;
import net.minecraft.resources.*;
import net.minecraft.nbt.NbtOps;
import net.minecraft.network.*;
import net.minecraft.network.protocol.*;
import net.minecraft.network.protocol.configuration.*;
import net.minecraft.network.protocol.common.*;
import net.minecraft.network.protocol.game.*;
import net.minecraft.network.protocol.login.*;
import net.minecraft.network.protocol.status.*;
import net.minecraft.network.protocol.handshake.*;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.tags.*;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.properties.Property;

/** Export using unmodified official 26.1 server codecs. No world or network server. */
public class ExportProtocol {
  static Path out;
  static PrintWriter writer(String name) throws Exception { return new PrintWriter(Files.newBufferedWriter(out.resolve(name))); }
  @SuppressWarnings({"unchecked", "rawtypes"})
  static String properties(net.minecraft.world.level.block.state.BlockState s) {
    var parts=new TreeMap<String,String>();
    s.getValues().forEach(v->parts.put(v.property().getName(),((Property)v.property()).getName((Comparable)v.value())));
    return String.join(",",parts.entrySet().stream().map(e->e.getKey()+"="+e.getValue()).toList());
  }
  public static void main(String[] args) throws Exception {
    out=Path.of(args[0]); Files.createDirectories(out);
    SharedConstants.tryDetectVersion(); Bootstrap.bootStrap();
    if(SharedConstants.getCurrentVersion().protocolVersion()!=775 || !SharedConstants.getCurrentVersion().name().equals("26.1"))
      throw new IllegalStateException("Exporter requires official 26.1 protocol775 jar");
    try(var w=writer("block_states.tsv")) {
      for(var b:BuiltInRegistries.BLOCK) for(var s:b.getStateDefinition().getPossibleStates())
        w.println(Block.getId(s)+"\t"+BuiltInRegistries.BLOCK.getKey(b)+"\t"+properties(s));
    }
    try(var w=writer("items.tsv")) { for(var v:BuiltInRegistries.ITEM) w.println(BuiltInRegistries.ITEM.getId(v)+"\t"+BuiltInRegistries.ITEM.getKey(v)); }
    try(var w=writer("blocks.tsv")) { for(var v:BuiltInRegistries.BLOCK) w.println(BuiltInRegistries.BLOCK.getId(v)+"\t"+BuiltInRegistries.BLOCK.getKey(v)); }
    try(var w=writer("block_entity_types.tsv")) { for(var v:BuiltInRegistries.BLOCK_ENTITY_TYPE) w.println(BuiltInRegistries.BLOCK_ENTITY_TYPE.getId(v)+"\t"+BuiltInRegistries.BLOCK_ENTITY_TYPE.getKey(v)); }
    try(var w=writer("entities.tsv")) { for(var v:BuiltInRegistries.ENTITY_TYPE) w.println(BuiltInRegistries.ENTITY_TYPE.getId(v)+"\t"+BuiltInRegistries.ENTITY_TYPE.getKey(v)); }
    try(var w=writer("explosion_effects.tsv")) {
      for(var name:List.of("explosion", "explosion_emitter", "poof", "smoke"))
        w.println(name+"\t"+BuiltInRegistries.PARTICLE_TYPE.getId(BuiltInRegistries.PARTICLE_TYPE.getValue(Identifier.withDefaultNamespace(name))));
      w.println("sound\t"+BuiltInRegistries.SOUND_EVENT.getId(net.minecraft.sounds.SoundEvents.GENERIC_EXPLODE.value()));
    }
    try(var w=writer("entity_data_serializers.tsv")) {
      for(var f:net.minecraft.network.syncher.EntityDataSerializers.class.getFields())
        if(f.get(null) instanceof net.minecraft.network.syncher.EntityDataSerializer<?> s)
          w.println(net.minecraft.network.syncher.EntityDataSerializers.getSerializedId(s)+"\t"+f.getName());
    }
    try(var w=writer("packet_ids.tsv")) {
      for(var p:List.of(HandshakeProtocols.SERVERBOUND_TEMPLATE,StatusProtocols.CLIENTBOUND_TEMPLATE,StatusProtocols.SERVERBOUND_TEMPLATE,
          LoginProtocols.CLIENTBOUND_TEMPLATE,LoginProtocols.SERVERBOUND_TEMPLATE,ConfigurationProtocols.CLIENTBOUND_TEMPLATE,
          ConfigurationProtocols.SERVERBOUND_TEMPLATE,GameProtocols.CLIENTBOUND_TEMPLATE,GameProtocols.SERVERBOUND_TEMPLATE)) {
        var d=p.details(); d.listPackets((type,id)->w.println(d.id().id()+"\t"+d.flow().id()+"\t"+type.id()+"\t"+id));
      }
    }
    try(var resources=new MultiPackResourceManager(PackType.SERVER_DATA,List.of(ServerPacksSource.createVanillaPackSource()))) {
      var layers=RegistryLayer.createRegistryAccess();
      var staticTags=TagLoader.loadTagsForExistingRegistries(resources,layers.getLayer(RegistryLayer.STATIC));
      var context=TagLoader.buildUpdatedLookups(layers.getAccessForLoading(RegistryLayer.WORLDGEN),staticTags);
      var worldgen=RegistryDataLoader.load(resources,context,RegistryDataLoader.WORLDGEN_REGISTRIES,Runnable::run).join();
      for(var t:staticTags)t.apply();
      try(var w=writer("state_flags.tsv")) {
        for(var b:BuiltInRegistries.BLOCK) for(var s:b.getStateDefinition().getPossibleStates())
          w.println(Block.getId(s)+"\t"+(s.isAir()?1:0)+"\t"+(!s.getFluidState().isEmpty()?1:0)+"\t"+(s.blocksMotion()?1:0)+"\t"+(s.is(BlockTags.LEAVES)?1:0));
      }
      layers=layers.replaceFrom(RegistryLayer.WORLDGEN,worldgen);
      var access=layers.compositeAccess();
      BuiltInRegistries.DATA_COMPONENT_INITIALIZERS.build(access).forEach(c->c.apply());
      var emptySection=new net.minecraft.world.level.chunk.LevelChunkSection(net.minecraft.world.level.chunk.PalettedContainerFactory.create(access));
      var sectionBuffer=new FriendlyByteBuf(Unpooled.buffer());
      try {emptySection.write(sectionBuffer);byte[] bytes=new byte[sectionBuffer.readableBytes()];sectionBuffer.readBytes(bytes);Files.write(out.resolve("empty_section.bin"),bytes);}
      finally {sectionBuffer.release();}
      try(var w=writer("biomes.tsv")) { var r=access.lookupOrThrow(Registries.BIOME); for(var v:r)w.println(r.getId(v)+"\t"+r.getKey(v)); }
      var packets=new ArrayList<Packet<? super ClientConfigurationPacketListener>>();
      packets.add(new ClientboundUpdateEnabledFeaturesPacket(Set.of(Identifier.withDefaultNamespace("vanilla"))));
      RegistrySynchronization.packRegistries(access.createSerializationContext(NbtOps.INSTANCE),worldgen,Set.of(),
        (key,entries)->packets.add(new ClientboundRegistryDataPacket(key,entries)));
      packets.add(new ClientboundUpdateTagsPacket(TagNetworkSerialization.serializeTagsToNetwork(layers)));
      try(var w=writer("registry_ids.tsv")) {
        worldgen.registries().forEach(e->dumpRegistry(w,e));
      }
      try(var stream=new DataOutputStream(Files.newOutputStream(out.resolve("configuration.bin")))) {
        for(var p:packets) {
          var b=Unpooled.buffer();
          try {
            ConfigurationProtocols.CLIENTBOUND.codec().encode(b,p);
            byte[] bytes=new byte[b.readableBytes()];b.readBytes(bytes);stream.writeInt(bytes.length);stream.write(bytes);
            var copy=Unpooled.wrappedBuffer(bytes);
            try {ConfigurationProtocols.CLIENTBOUND.codec().decode(copy); if(copy.isReadable())throw new AssertionError("trailing configuration bytes");}
            finally {copy.release();}
          } finally {b.release();}
        }
      }
      System.out.println("Exported "+packets.size()+" full-data configuration packets to "+out);
      if(args.length>1) VerifyPackets.verify(Path.of(args[1]),access);
    }
    net.minecraft.util.Util.shutdownExecutors();
  }
  static <T> void dumpRegistry(PrintWriter w, RegistryAccess.RegistryEntry<T> e) {
    var r=e.value(); for(var v:r)w.println(r.key().identifier()+"\t"+r.getId(v)+"\t"+r.getKey(v));
  }
}
