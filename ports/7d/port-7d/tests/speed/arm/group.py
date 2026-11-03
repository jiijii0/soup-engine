import re,collections,sys
# group.py <prof-output> <file.c> start:end:name ...
c=collections.Counter(); groups=[(int(a),int(b),n) for a,b,n in (g.split(':',2) for g in sys.argv[3:])]
for l in open(sys.argv[1]):
    m=re.search(r'([\d.]+)M cyc\s+[\d.]+%\s+(\S+?)(?::(\d+))?(\s|$)',l)
    if not m: continue
    v=float(m.group(1)); f=m.group(2); n=int(m.group(3) or 0)
    g=next((name for a,b,name in groups if a<=n<=b and f==sys.argv[2]),f'{f}:{n}')
    c[g]+=v
t=sum(c.values())
for k,v in c.most_common(12): print(f'  {v:6.2f}M {100*v/t:5.1f}%  {k}')
