import struct
def blocks(path):
    data=open(path,'rb').read(); off=0
    while off < len(data):
        bt,bl=struct.unpack_from('<II',data,off)
        yield bt,data[off+8:off+bl-4]
        off+=bl
def packets(path):
    n=0
    for bt,body in blocks(path):
        if bt!=6: continue
        iface,th,tl,cap,orig=struct.unpack_from('<IIIII',body,0)
        pkt=body[20:20+cap]
        (uid,typ,xt,ep,dev,bus,fs,fd,sec,usec,status,length,lencap)=struct.unpack_from('<QcBBBHbbqiiII',pkt,0)
        setup=pkt[40:48]
        n+=1
        yield dict(n=n,id=uid,ts=sec+usec/1e6,typ=typ.decode(),xt=xt,ep=ep,dev=dev,status=status,length=length,setup=setup if fs==0 else None,data=pkt[64:])
