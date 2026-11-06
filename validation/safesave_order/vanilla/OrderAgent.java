import java.lang.classfile.*;
import java.lang.constant.*;
import java.lang.instrument.*;
import java.security.ProtectionDomain;

public class OrderAgent {
 public static void premain(String args, Instrumentation inst) {
  inst.addTransformer(new ClassFileTransformer(){
   public byte[] transform(Module mod, ClassLoader loader, String name, Class<?> cls, ProtectionDomain dom, byte[] bytes) {
    String method; int slot; String hook;
    if(name.equals("net/minecraft/server/level/ServerLevel")){method="doBlockEvent";slot=1;hook="event";}
    else if(name.equals("net/minecraft/world/level/block/piston/PistonMovingBlockEntity")){method="tick";slot=1;hook="piston";}
    else return null;
    final int parameter=slot; final String callback=hook; var cf=ClassFile.of();
    return cf.transformClass(cf.parse(bytes),ClassTransform.transformingMethodBodies(m->m.methodName().equalsString(method),new CodeTransform(){
      public void atStart(CodeBuilder b){b.aload(parameter).invokestatic(ClassDesc.of("OrderAgent"),callback,MethodTypeDesc.of(ConstantDescs.CD_void,ConstantDescs.CD_Object));}
      public void accept(CodeBuilder b,CodeElement e){b.with(e);}
    }));
   }
  });
 }
 public static void event(Object e){System.out.println("ORDER_EVENT "+e);}
 public static void piston(Object pos){System.out.println("ORDER_PISTON "+pos);}
}
