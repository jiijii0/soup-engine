"""Run the ARM build of the develop pipeline under Unicorn and count instructions
(and an ARM946-ish cycle estimate) per function.
usage: run.py <elf> <jpeg> <stock.fs> [label]"""
import sys, re, bisect
from unicorn import *
from unicorn.arm_const import *
from elftools.elf.elffile import ELFFile
elf_path, jpg, stock = sys.argv[1:4]
f = open(elf_path, 'rb'); elf = ELFFile(f)
syms = {}; funcs = []
for s in elf.get_section_by_name('.symtab').iter_symbols():
    syms[s.name] = s['st_value']
    if s['st_info']['type'] == 'STT_FUNC' and s['st_size']:
        funcs.append((s['st_value'], s['st_value'] + s['st_size'], s.name))
funcs.sort(); starts = [a for a, _, _ in funcs]
# cycle weights per instruction from the disassembly (rough ARM946E-S model)
w = {}
for line in open(elf_path + '.dis'):
    m = re.match(r'\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)', line)
    if not m: continue
    a, op, args = int(m.group(1), 16), m.group(2), m.group(3)
    base = re.sub(r'(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)$', '', op) if op not in ('bls','bhi') else op
    c = 1
    if base.startswith(('ldm', 'pop')): c = 2 + args.count(',')
    elif base.startswith(('stm', 'push')): c = 1 + args.count(',')
    elif base.startswith('ldr'): c = 2          # load + typical use interlock
    elif base.startswith(('smull','umull','smlal','umlal')): c = 4
    elif base.startswith(('mul','mla','smul','smla')): c = 3
    elif base in ('b','bl','bx','blx') or base.startswith(('b.', 'bl')) and len(base) <= 3: c = 3
    w[a] = c
mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
try: mu.ctl_set_cpu_model(UC_CPU_ARM_926)
except Exception: pass
mu.mem_map(0, 64 << 20)
for seg in elf.iter_segments():
    if seg['p_type'] == 'PT_LOAD':
        mu.mem_write(seg['p_paddr'], seg.data())
data = open(jpg, 'rb').read(); txt = open(stock, 'rb').read()
mu.mem_write(syms['in_jpg'], data); mu.mem_write(syms['in_len'], len(data).to_bytes(4, 'little'))
mu.mem_write(syms['stock_txt'], txt); mu.mem_write(syms['stock_len'], len(txt).to_bytes(4, 'little'))
mu.reg_write(UC_ARM_REG_SP, (64 << 20) - 16)
icount = {}; ccount = {}; cache = {}; bcount = {}
def blk(uc, addr, size, _):
    k = (addr, size)
    e = cache.get(k)
    if e is None:
        i = bisect.bisect_right(starts, addr) - 1
        name = funcs[i][2] if i >= 0 and addr < funcs[i][1] else '?'
        e = cache[k] = (name, size // 4, sum(w.get(a, 1) for a in range(addr, addr + size, 4)))
    bcount[k] = bcount.get(k, 0) + 1
    icount[e[0]] = icount.get(e[0], 0) + e[1]; ccount[e[0]] = ccount.get(e[0], 0) + e[2]
mu.hook_add(UC_HOOK_BLOCK, blk)
mu.emu_start(syms['_start'], syms['halt'])
res = int.from_bytes(mu.mem_read(syms['result'], 4), 'little', signed=True)
h = int.from_bytes(mu.mem_read(syms['out_hash'], 4), 'little')
n = int.from_bytes(mu.mem_read(syms['out_len'], 4), 'little')
ti = sum(icount.values()); tc = sum(ccount.values())
label = sys.argv[4] if len(sys.argv) > 4 else ''
print(f'{label} result={res} out={n} hash={h:08x} instr={ti/1e6:.2f}M cycles~{tc/1e6:.2f}M')
for k in sorted(ccount, key=ccount.get, reverse=True)[:14]:
    print(f'   {k:24s} {icount[k]/1e6:7.2f}M instr {ccount[k]/1e6:7.2f}M cyc {100*ccount[k]/tc:5.1f}%')

import os, subprocess, collections
fn = os.environ.get('PROF_FN')
if fn:
    per_addr = collections.Counter()
    for (addr, size), cnt in bcount.items():
        if cache[(addr, size)][0] != fn: continue
        for a in range(addr, addr + size, 4): per_addr[a] += cnt * w.get(a, 1)
    out = subprocess.run(['/opt/homebrew/bin/arm-none-eabi-addr2line', '-e', elf_path, '-i'] + [hex(a) for a in per_addr],
                         capture_output=True, text=True).stdout.split('\n')
    # -i may print several lines per address (inlining); take the first per address
    res = subprocess.run(['/opt/homebrew/bin/arm-none-eabi-addr2line', '-e', elf_path] + [hex(a) for a in per_addr],
                         capture_output=True, text=True).stdout.split('\n')
    lines = collections.Counter()
    for a, l in zip(per_addr, res): lines[l.split('/')[-1]] += per_addr[a]
    tot = sum(lines.values())
    for l, c in lines.most_common(400): print(f'      {c/1e6:7.2f}M cyc {100*c/tot:5.1f}%  {l}')
