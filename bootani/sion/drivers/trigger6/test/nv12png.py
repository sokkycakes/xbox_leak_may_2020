import sys, struct, numpy as np
from PIL import Image
d=open(sys.argv[1],'rb').read(); outdir=sys.argv[2]
S=(4<<20)+512
for slot in range(32):
    b=d[slot*S:(slot+1)*S]
    if len(b)<16: break
    m,w,h,seq=struct.unpack('<4I',b[:16])
    if m!=0x36543654: continue
    px=np.frombuffer(b[512:512+w*h*3//2],dtype=np.uint8)
    Y=px[:w*h].reshape(h,w).astype(np.float32)
    UV=px[w*h:].reshape(h//2,w//2,2).astype(np.float32)
    U=UV[:,:,0].repeat(2,0).repeat(2,1)-128; V=UV[:,:,1].repeat(2,0).repeat(2,1)-128
    img=np.clip(np.stack([Y+1.402*V, Y-0.344136*U-0.714136*V, Y+1.772*U],-1),0,255).astype(np.uint8)
    name='%s/out%d_seq%03d_%dx%d.png'%(outdir,slot//16,seq,w,h)
    Image.fromarray(img).save(name); print(name, 'center px', img[h//4, w//16], img[h//4, w*15//16])
