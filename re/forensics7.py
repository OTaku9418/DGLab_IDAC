import struct
d=open(r'D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\ue4ss\Mods\DGLab_IDAC\dlls\main.dll','rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]; opt=pe+24
so=opt+struct.unpack_from('<H',d,pe+20)[0]
secs=[]
for i in range(struct.unpack_from('<H',d,pe+6)[0]):
    o=so+i*40; nm=d[o:o+8].rstrip(b'\0').decode('latin1')
    vs,va,rs,raw=struct.unpack_from('<IIII',d,o+8); secs.append((nm,va,vs,raw,rs))
def r2o(r):
    for nm,va,vs,raw,rs in secs:
        if va<=r<va+max(vs,rs):
            o=raw+(r-va)
            if o<len(d): return o
    return None
print('=== RVA 0x2928C 附近 (L"ue" 那一处) 周围是什么 ===')
o=r2o(0x29240)
blob=d[o:o+0x120]
# decode as utf-16
txt=blob.decode('utf-16-le','replace')
print(repr(txt[:110]))
print()
print('=== 把它按 UTF-16 字符串切开看相邻字符串 ===')
i=0
base=0x29240
while i < len(blob)-1:
    # find string run
    start=i
    s=[]
    while i+1 < len(blob):
        c=blob[i]|(blob[i+1]<<8)
        if c==0: break
        s.append(c); i+=2
    if s:
        try: st=''.join(chr(c) for c in s)
        except: st='?'
        print('  RVA 0x%06X: %r' % (base+start, st[:70]))
    i+=2
