import sys, struct; sys.path.insert(0,'.')
from usbpcap import packets
P=sys.argv[1]; pend={}; res=[]
for p in packets(P):
    if p['xt']!=2: continue
    if p['typ']=='S':
        pend[p['id']]=struct.unpack('<BBHHH',p['setup'])+(p['data'],)
    else:
        s=pend.pop(p['id'],None)
        if s: res.append((s,p['data']))
modes={}
for s,d in res:
    if s[0]==0xc0 and s[1]==0x89:
        modes.setdefault(s[2],b'')
        modes[s[2]]+=d
    if s[0]==0xc0 and s[1] in (0xb1,0xb3,0xcc,0xa5,0xb0):
        print(hex(s[1]),hex(s[3]),d.hex())
for out,b in modes.items():
    print('output',out,len(b))
    seen=set()
    for i in range(0,len(b),32):
        e=b[i:i+32]
        if len(e)<32: break
        f=struct.unpack('<IHHHHHHHHHHHHBBBB',e)
        print(' clk%7d %3dHz h %4d %4d %4d %3d v %4d %4d %4d %3d | %04x %04x %04x %d %d %d %d'%f)
