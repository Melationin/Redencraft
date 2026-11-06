import pathlib, subprocess, threading, queue, time, os
root=pathlib.Path(__file__).resolve().parent
base=(root/'../../vanilla26_1').resolve()
if not (root/'eula.txt').exists() or 'eula=true' not in (root/'eula.txt').read_text(encoding='utf8'):
 raise SystemExit('Accept the Minecraft EULA in this directory before running this test.')
(root/'server.properties').write_text('server-ip=127.0.0.1\nserver-port=25598\nonline-mode=true\nspawn-protection=0\nview-distance=3\nsimulation-distance=3\nlevel-name=exact-order-world\npause-when-empty-seconds=0\n')
cp=os.pathsep.join([str(base/'versions/26.1/server-26.1.jar'),*[str(p) for p in (base/'libraries').rglob('*.jar')],str(root/'exact-agent.jar')])
java=r'C:\Program Files\Zulu\zulu-26\bin\java.exe'
p=subprocess.Popen([java,'-Xmx2G',f'-javaagent:{root / "exact-agent.jar"}','-cp',cp,'net.minecraft.server.Main','nogui'],cwd=root,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1,encoding='utf-8',errors='replace')
q=queue.Queue(); log=(root/'exact-server-trace.log').open('w',encoding='utf8')
def read():
 for line in p.stdout: print(line,end='',flush=True);log.write(line);log.flush();q.put(line)
threading.Thread(target=read,daemon=True).start()
def wait(text,timeout=240):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  if p.poll() is not None: raise RuntimeError(f'exit {p.returncode}')
  try:s=q.get(timeout=1)
  except queue.Empty:continue
  if text in s:return
 raise RuntimeError('timeout '+text)
def cmd(s):
 log.write('COMMAND: '+s+'\n');log.flush();p.stdin.write(s+'\n');p.stdin.flush();time.sleep(.2)
try:
 wait('EXACT_JSON ',600)
 lines=log.flush()
 text=(root/'exact-server-trace.log').read_text(encoding='utf8')
 line=next(line for line in text.splitlines() if 'EXACT_JSON {' in line)
 (root/'vanilla-server-order.json').write_text(line[line.index('{'):]+'\n',encoding='utf8')

finally:
 if p.poll() is None:
  cmd('stop')
  try:p.wait(timeout=60)
  except subprocess.TimeoutExpired:p.kill()
 log.close()
