"""Test how far we can step post-rebuild before connection drops."""
import json, socket, subprocess, time
subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.5)
p = subprocess.Popen([r'F:/Projects/SuperMarioWorldRecomp/build/bin-x64-Oracle/smw.exe','--paused'],
                    cwd=r'F:/Projects/SuperMarioWorldRecomp',
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.5)
s = socket.socket(); s.connect(('127.0.0.1',4377)); f = s.makefile('r'); f.readline()
def cmd(l):
    s.sendall((l+'\n').encode()); return json.loads(f.readline())

import sys
side = sys.argv[1] if len(sys.argv) > 1 else 'both'
n = 0
try:
    for i in range(500):
        if side in ('recomp', 'both'): cmd('step 1')
        if side in ('oracle', 'both'): cmd('emu_step 1')
        n = i + 1
except Exception as e:
    print(f'side={side} CRASH at step {n}: {type(e).__name__}: {e}')
print(f'side={side} reached {n} steps')
s.close(); p.terminate(); time.sleep(0.5)
subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
