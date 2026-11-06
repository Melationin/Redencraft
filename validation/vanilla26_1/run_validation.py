import os, pathlib, queue, subprocess, threading, time, sys
root = pathlib.Path(__file__).resolve().parent
if not (root/'eula.txt').exists() or 'eula=true' not in (root/'eula.txt').read_text(encoding='utf-8'):
 raise SystemExit('Accept the Minecraft EULA in this directory before running this test.')
(root/'server.properties').write_text('server-ip=127.0.0.1\nserver-port=25597\nonline-mode=true\nspawn-protection=0\nview-distance=3\nsimulation-distance=3\nmax-players=1\nlevel-name=carpet-validation-world\n', encoding='ascii')
java = os.environ.get('JAVA_26', r'C:\Program Files\Zulu\zulu-26\bin\java.exe')
p = subprocess.Popen([java,'-Xms512M','-Xmx2G','-jar','server.jar','nogui'], cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, encoding='utf-8', errors='replace')
lines=queue.Queue(); log=(root/'server-validation.log').open('w',encoding='utf-8')
def reader():
 for line in p.stdout:
  print(line,end='',flush=True); log.write(line); log.flush(); lines.put(line)
threading.Thread(target=reader,daemon=True).start()
def wait_for(text,timeout=300):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  if p.poll() is not None: raise RuntimeError(f'server exited {p.returncode}')
  try: line=lines.get(timeout=1)
  except queue.Empty: continue
  if text in line: return
 raise RuntimeError('Timed out waiting for '+text)
def command(s):
 print('COMMAND:',s,flush=True); log.write('COMMAND: '+s+'\n');log.flush();p.stdin.write(s+'\n');p.stdin.flush();time.sleep(.35)
try:
 wait_for('Done (')
 command('tick freeze')
 command('forceload add -16 -16 15 15')
 command('fill -3 99 -3 5 104 3 minecraft:air replace')
 command('kill @e[type=minecraft:item]')
 command('setblock 0 100 0 minecraft:sticky_piston[facing=east,extended=false]')
 command('setblock 1 100 0 minecraft:slime_block')
 command('setblock 1 101 0 minecraft:slime_block')
 command('setblock -1 100 0 minecraft:redstone_block')
 command('tick step 3')
 time.sleep(2)
 command('execute if block 0 100 0 minecraft:sticky_piston[extended=true] if block 1 100 0 minecraft:piston_head if block 2 100 0 minecraft:slime_block if block 2 101 0 minecraft:slime_block run say VALIDATION_EXTENDED_OK')
 command('setblock 1 101 0 minecraft:white_carpet')
 command('execute if block 1 101 0 minecraft:white_carpet run say VALIDATION_CARPET_PLACED')
 command('setblock -1 100 0 minecraft:air')
 command('tick step 5')
 time.sleep(2)
 command('execute if block 1 101 0 minecraft:white_carpet run say VALIDATION_CARPET_REMAINS')
 command('execute if block 0 101 0 minecraft:white_carpet run say VALIDATION_CARPET_MOVED_TO_BASE')
 command('execute if block 1 101 0 minecraft:air run say VALIDATION_CARPET_ORIGINAL_AIR')
 command('execute if block 1 100 0 minecraft:slime_block run say VALIDATION_LOWER_SLIME_RETRACTED')
 command('execute if block 1 101 0 minecraft:slime_block run say VALIDATION_UPPER_SLIME_RETRACTED')
 command('execute as @e[type=minecraft:item] run data get entity @s Item')
 command('execute if entity @e[type=minecraft:item,nbt={Item:{id:"minecraft:white_carpet"}}] run say VALIDATION_WHITE_CARPET_ITEM')
 command('say VALIDATION_END')
 wait_for('VALIDATION_END',30)
finally:
 if p.poll() is None:
  command('stop')
  try: p.wait(timeout=60)
  except subprocess.TimeoutExpired: p.kill()
 log.close()

