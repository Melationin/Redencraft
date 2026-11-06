import java.util.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.ticks.*;

public class VanillaOrderProbe {
  static LevelTicks<Block> scheduler() {
    var ticks=new LevelTicks<Block>(key->true);
    for(int x:new int[]{-1,0,1,511,512}) ticks.addContainer(new ChunkPos(x,0),new LevelChunkTicks<>());
    return ticks;
  }
  static void add(LevelTicks<Block> t,int x,int y,int z,long time,TickPriority p,long order){ t.schedule(new ScheduledTick<>(Blocks.STONE,new BlockPos(x,y,z),time,p,order)); }
  static String coords(List<BlockPos> list){var out=new ArrayList<String>();for(var p:list)out.add("["+p.getX()+","+p.getY()+","+p.getZ()+"]");return "["+String.join(",",out)+"]";}
  static List<BlockPos> drain(LevelTicks<Block> t,long now){var order=new ArrayList<BlockPos>();t.tick(now,65536,(p,b)->order.add(p));return order;}
  public static void main(String[] args){
    SharedConstants.tryDetectVersion(); Bootstrap.bootStrap();
    var fixture=scheduler();int n=0;
    for(int b:new int[]{0,16,8192})for(int x:new int[]{b,b-1,b+1,b-2}){add(fixture,x,82,5,2,n%2==0?TickPriority.HIGH:TickPriority.NORMAL,2L*n);n++;}
    var overdue=scheduler();add(overdue,0,82,5,5,TickPriority.LOW,0);add(overdue,1,82,5,6,TickPriority.EXTREMELY_HIGH,1);add(overdue,16,82,5,8,TickPriority.NORMAL,2);add(overdue,8192,82,5,8,TickPriority.NORMAL,3);
    var negative=scheduler();add(negative,16,82,5,10,TickPriority.NORMAL,0);add(negative,-1,82,5,10,TickPriority.NORMAL,-2);
    var dedup=new LevelChunkTicks<Block>();
    dedup.schedule(new ScheduledTick<>(Blocks.STONE,new BlockPos(2,87,2),50,TickPriority.LOW,999));
    dedup.schedule(new ScheduledTick<>(Blocks.STONE,new BlockPos(2,87,2),4,TickPriority.HIGH,100));
    dedup.schedule(new ScheduledTick<>(Blocks.STONE,new BlockPos(3,87,2),60,TickPriority.NORMAL,1000));
    var saved=dedup.getAll().sorted(Comparator.comparingInt(t->t.pos().getX())).toList();var entries=new ArrayList<String>();
    for(var t:saved)entries.add("["+t.pos().getX()+","+t.pos().getY()+","+t.pos().getZ()+","+t.triggerTick()+","+t.priority().getValue()+","+t.subTickOrder()+"]");
    System.out.println("{\"fixture_ticks\":"+coords(drain(fixture,2))+",\"dedup_ticks\":["+String.join(",",entries)+"],\"overdue_ticks\":"+coords(drain(overdue,11))+",\"negative_orders\":"+coords(drain(negative,10))+"}");
  }
}
