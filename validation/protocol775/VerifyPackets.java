import java.nio.file.*;
import java.io.*;
import io.netty.buffer.Unpooled;
import net.minecraft.core.RegistryAccess;
import net.minecraft.network.RegistryFriendlyByteBuf;
import net.minecraft.network.protocol.game.GameProtocols;

/** Reads u32 BE length + uncompressed packet id/payload, no TCP framing. */
public class VerifyPackets {
  public static void verify(Path path, RegistryAccess access) throws Exception {
    var codec=GameProtocols.CLIENTBOUND_TEMPLATE.bind(RegistryFriendlyByteBuf.decorator(access)).codec();
    int count=0;
    try(var input=new DataInputStream(Files.newInputStream(path))) {
      while(input.available()>0) {
        int size=input.readInt();
        if(size<1||size>8388608)throw new IOException("Invalid record length "+size);
        byte[] raw=input.readNBytes(size);if(raw.length!=size)throw new EOFException();
        var b=Unpooled.wrappedBuffer(raw);
        try {
          var packet=codec.decode(b);
          if(b.isReadable()) throw new IOException("Trailing "+b.readableBytes()+" bytes after "+packet.type());
          if(packet instanceof net.minecraft.network.protocol.game.ClientboundLevelChunkWithLightPacket chunk) {
            var sections=chunk.getChunkData().getReadBuffer();
            try {
              var factory=net.minecraft.world.level.chunk.PalettedContainerFactory.create(access);
              for(int n=0;n<24;n++) new net.minecraft.world.level.chunk.LevelChunkSection(factory).read(sections);
              if(sections.isReadable())throw new IOException("Trailing section bytes: "+sections.readableBytes());
            } finally {sections.release();}
          }
          System.out.println("Decoded "+count+" "+packet.type()+" ("+size+" bytes)");
        } catch(Exception e) { throw new IOException("Record "+count+", first byte="+(raw[0]&255),e); }
        finally {b.release();}
        count++;
      }
    }
    if(count==0)throw new IOException("No packets to validate");
    System.out.println("VERIFIED "+count+" official protocol775 play packets");
  }
}
