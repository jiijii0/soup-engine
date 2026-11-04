"""Resident size of a module: SHF_ALLOC sections (text+data+bss). Usage: python3 resident.py build/soup7d.mo"""
import sys,re,subprocess
out=subprocess.run(['arm-none-eabi-readelf','-S','-W',sys.argv[1]],capture_output=True,text=True).stdout
t=0
for l in out.splitlines():
  m=re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+(\S*)',l)
  if m and 'A' in m.group(6): t+=int(m.group(5),16)
print(t, round(t/1024,1),'KB')
