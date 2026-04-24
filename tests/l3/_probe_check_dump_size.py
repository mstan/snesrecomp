"""Check what dump_ram / emu_read_wram return for a 0x200-byte range."""
import socket, json, subprocess, time
subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.5)
p = subprocess.Popen([r'F:/Projects/SuperMarioWorldRecomp/build/bin-x64-Oracle/smw.exe','--paused'],
                    cwd=r'F:/Projects/SuperMarioWorldRecomp',
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)
s = socket.socket(); s.connect(('127.0.0.1',4377)); f = s.makefile('r'); f.readline()
def cmd(l):
    s.sendall((l+'\n').encode()); return json.loads(f.readline())
r = cmd('dump_ram 0x200 0x200')
print('recomp keys:', list(r.keys()))
print('recomp hex len (bytes):', len(r.get('hex','').replace(' ','')) // 2)
o = cmd('emu_read_wram 0x200 0x200')
print('oracle keys:', list(o.keys()))
print('oracle hex len (bytes):', len(o.get('hex','').replace(' ','')) // 2)
s.close(); p.terminate(); time.sleep(0.5)
subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
