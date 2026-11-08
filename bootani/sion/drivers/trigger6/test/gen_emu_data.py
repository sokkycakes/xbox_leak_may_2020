import sys,struct; sys.path.insert(0,'.')
exec(open('ctrl.py').read().split('modes={}')[0])
seen={}
for s,d in res:
    rt,rq,v,i,l,_=s
    if rt!=0xc0: continue
    if rq in (0x87,): continue
    key=(rq,v,i)
    if key in seen: continue
    if len(d)==0: continue
    seen[key]=d
out=open(sys.argv[2],'w')
out.write('/* generated from the JUA365 capture */\nstruct resp { unsigned char req; unsigned short val, idx, len; const unsigned char *data; };\n')
n=0
for (rq,v,i),d in seen.items():
    out.write('static const unsigned char r%d[] = {%s};\n'%(n,','.join(str(b) for b in d))); n+=1
out.write('static const struct resp resps[] = {\n')
for k,((rq,v,i),d) in enumerate(seen.items()):
    out.write('\t{0x%02x, 0x%04x, 0x%04x, %d, r%d},\n'%(rq,v,i,len(d),k))
out.write('};\n')
for k,v in seen.items(): print('%02x %04x %04x %d'%(k[0],k[1],k[2],len(v)))
