"""Print an address range from an `objdump -d -M intel` listing.

  objdump -d -M intel PPJDD.EXE > ppjdd.asm
  python asmrange.py ppjdd.asm 40c580 40ca60
"""
import sys,re
lines=open(sys.argv[1]).read().splitlines()
lo,hi=int(sys.argv[2],16),int(sys.argv[3],16)
for l in lines:
    m=re.match(r'\s+([0-9a-f]+):\t(?:[0-9a-f]{2} )+\s*\t(.*)',l)
    if m:
        a=int(m.group(1),16)
        if lo<=a<hi: print('%06x  %s'%(a,m.group(2)))
