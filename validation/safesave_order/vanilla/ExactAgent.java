import java.lang.classfile.*;
import java.lang.classfile.instruction.ReturnInstruction;
import java.lang.constant.*;
import java.lang.instrument.*;
import java.security.ProtectionDomain;
import java.lang.reflect.Field;
import java.util.*;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.FullChunkStatus;
import net.minecraft.world.level.BlockEventData;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.piston.PistonBaseBlock;
import net.minecraft.world.level.block.piston.PistonMovingBlockEntity;

public class ExactAgent {
 static int phase=0, elapsed=0; static long baseTime; static ServerLevel level;
 static final int[] CHUNKS={-1,0,1,511,512}, COORDS={1,-1,17,15,8193,8191};
 static final List<String> events=new ArrayList<>(), completion=new ArrayList<>(), states=new ArrayList<>();
 static Field progress,progressO,last;
 static void initFields(){try { progress=PistonMovingBlockEntity.class.getDeclaredField("progress"); progressO=PistonMovingBlockEntity.class.getDeclaredField("progressO"); last=PistonMovingBlockEntity.class.getDeclaredField("lastTicked");progress.setAccessible(true);progressO.setAccessible(true);last.setAccessible(true); } catch(Exception e){throw new RuntimeException(e);} }
 public static void premain(String args, Instrumentation inst) {
  inst.addTransformer(new ClassFileTransformer(){
   public byte[] transform(Module m,ClassLoader l,String name,Class<?> c,ProtectionDomain p,byte[] bytes) {
    String method,hook; int slot; boolean atReturn;
    if(name.equals("net/minecraft/server/MinecraftServer")){method="tickServer";hook="server";slot=0;atReturn=true;}
    else if(name.equals("net/minecraft/server/level/ServerLevel")){method="doBlockEvent";hook="event";slot=1;atReturn=false;}
    else if(name.equals("net/minecraft/world/level/block/piston/PistonMovingBlockEntity")){method="tick";hook="piston";slot=3;atReturn=false;}
    else return null;
    System.err.println("EXACT_TRANSFORM " + name + " " + method);
    final String callback=hook;final int arg=slot;final boolean end=atReturn;var cf=ClassFile.of();
    return cf.transformClass(cf.parse(bytes),ClassTransform.transformingMethodBodies(mm->mm.methodName().equalsString(method),new CodeTransform(){
     void call(CodeBuilder b){b.aload(arg).invokestatic(ClassDesc.of("ExactAgent"),callback,MethodTypeDesc.of(ConstantDescs.CD_void,ConstantDescs.CD_Object));}
     public void atStart(CodeBuilder b){if(!end)call(b);}
     public void accept(CodeBuilder b,CodeElement e){if(end&&e instanceof ReturnInstruction)call(b);b.with(e);}
    }));
   }
  },true);
  try { for (Class<?> c : inst.getAllLoadedClasses()) if (c.getName().equals("net.minecraft.world.level.block.piston.PistonMovingBlockEntity")) { System.err.println("EXACT_RETRANSFORM "+c); inst.retransformClasses(c); } } catch(Exception ex) { throw new RuntimeException(ex); }
 }
 public static void event(Object object){if(phase!=2)return;var e=(BlockEventData)object;if(e.block()!=Blocks.NOTE_BLOCK)return;var p=e.pos();if(p.getY()==82&&p.getZ()==4)events.add("["+p.getX()+",82,4,"+e.paramA()+","+e.paramB()+"]");}
 public static void piston(Object object){if(phase!=2)return;var e=(PistonMovingBlockEntity)object;var p=e.getBlockPos();if(p.getY()!=84||p.getZ()!=-3)return;try{if(progress.getFloat(e)>=1)completion.add("["+p.getX()+",84,-3]");}catch(Exception ex){throw new RuntimeException(ex);}}
 public static void server(Object object){try {var server=(MinecraftServer)object;var l=server.overworld();if(l==null||phase==3)return;
  if(phase==0){initFields();level=l;for(int x:CHUNKS)for(int z:new int[]{-1,0})l.setChunkForced(x,z,true);phase=1;System.out.println("EXACT_WAITING_FOR_CHUNKS");return;}
  if(phase==1){for(int x:CHUNKS)for(int z:new int[]{-1,0})if(!l.getChunk(x,z).getFullStatus().isOrAfter(FullChunkStatus.BLOCK_TICKING)||!l.areEntitiesLoaded(ChunkPos.pack(x,z)))return;
   int n=0;for(int b:new int[]{0,16,8192})for(int x:new int[]{b,b-1,b+1,b-2}){var p=new BlockPos(x,82,4);l.setBlock(p,Blocks.NOTE_BLOCK.defaultBlockState(),2);int data=++n;l.blockEvent(p,Blocks.NOTE_BLOCK,0,data);l.blockEvent(p,Blocks.NOTE_BLOCK,0,data);}
   baseTime=l.getGameTime();for(int x:COORDS){var p=new BlockPos(x,84,-3);var s=Blocks.MOVING_PISTON.defaultBlockState().setValue(PistonBaseBlock.FACING,Direction.EAST);l.setBlock(p,s,2);var e=new PistonMovingBlockEntity(p,s,Blocks.SLIME_BLOCK.defaultBlockState(),Direction.EAST,true,false);progress.setFloat(e,.5f);progressO.setFloat(e,0f);last.setLong(e,baseTime);l.setBlockEntity(e);} phase=2;System.out.println("EXACT_SETUP_DONE "+baseTime);return;}
  if(phase==2){var row=new ArrayList<String>();for(int x:COORDS){var p=new BlockPos(x,84,-3);var e=l.getBlockEntity(p);String be="null,null,null";if(e instanceof PistonMovingBlockEntity moving)be=progress.getFloat(moving)+","+progressO.getFloat(moving)+","+(moving.getLastTicked()-baseTime);row.add("["+x+",84,-3,"+be+",\""+BuiltInRegistries.BLOCK.getKey(l.getBlockState(p).getBlock())+"\"]");}states.add("["+String.join(",",row)+"]");if(++elapsed==3){phase=3;System.out.println("EXACT_JSON {\"fixture_events\":["+String.join(",",events)+"],\"moving_entities\":["+String.join(",",states)+"],\"moving_completion_order\":["+String.join(",",completion)+"]}");}}
 }catch(Throwable t){phase=3;t.printStackTrace();System.out.println("EXACT_FAILED "+t);}}
}
