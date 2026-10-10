import struct,sys,os,collections
path=sys.argv[1]; PX,PY=float(sys.argv[2]),float(sys.argv[3]); tail=int(sys.argv[4]) if len(sys.argv)>4 else 60_000_000
size=os.path.getsize(path)
st={'prim':0,'rgbaq':(0,0,0,0,1.0),'st':(0.0,0.0),'uv':(0,0),'tex0':0,'ofx':0,'ofy':0,'frame':0,'test':0,'alpha':0,'fogcol':0,'prmodecont':1,'prmode':0,'zbuf':0,'scissor':0}
verts=[]; hits=collections.deque(maxlen=400); ntri=0
def setprim(v):
    st['prim']=v&0x7FF; verts.clear()
def inside(a,b,c):
    def s(p,q,r): return (q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0])
    P=(PX,PY); d1=s(a,b,P); d2=s(b,c,P); d3=s(c,a,P)
    return not((d1<0 or d2<0 or d3<0) and (d1>0 or d2>0 or d3>0))
def vertex(x,y,z,f,kick):
    global ntri
    sx=(x-st['ofx'])/16.0; sy=(y-st['ofy'])/16.0
    verts.append((sx,sy,z,st['st'],st['rgbaq'],st['uv'],f,st.get('q',0)))
    t=st['prim']&7
    if t==3:
        if len(verts)==3:
            tri=tuple(verts); verts.clear()
        else: return
    elif t==4:
        if len(verts)>3: verts.pop(0)
        if len(verts)<3: return
        tri=tuple(verts)
    elif t==5:
        if len(verts)>3: verts.pop(1)
        if len(verts)<3: return
        tri=tuple(verts)
    elif t==6:
        if len(verts)==2:
            a,b=verts; verts.clear()
            if kick and min(a[0],b[0])<=PX<=max(a[0],b[0]) and min(a[1],b[1])<=PY<=max(a[1],b[1]):
                hits.append(('sprite',dict(st),(a,b),ev))
        return
    else:
        if len(verts)>2: verts.pop(0)
        return
    if kick:
        ntri+=1
        if inside(tri[0],tri[1],tri[2]): hits.append(('tri',dict(st),tri,ev))
def reg(a,v):
    if a==0: setprim(v)
    elif a==1: st['rgbaq']=(v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255,struct.unpack('<f',struct.pack('<I',(v>>32)&0xFFFFFFFF))[0])
    elif a==2: st['st']=struct.unpack('<ff',struct.pack('<Q',v))
    elif a==3: st['uv']=(v&0x3FFF,(v>>16)&0x3FFF)
    elif a in (4,0xC): vertex(v&0xFFFF,(v>>16)&0xFFFF,(v>>32)&0xFFFFFF,(v>>56)&255,a==4)
    elif a in (5,0xD): vertex(v&0xFFFF,(v>>16)&0xFFFF,(v>>32)&0xFFFFFFFF,0,a==5)
    elif a==6: st['tex0']=v
    elif a==0x14: st['tex1']=v
    elif a==0x34: st['mip1']=v
    elif a==0x36: st['mip2']=v
    elif a==0x08: st['clamp']=v
    elif a==0x18: st['ofx']=v&0xFFFF; st['ofy']=(v>>32)&0xFFFF
    elif a==0x4C: st['frame']=v
    elif a==0x47: st['test']=v
    elif a==0x42: st['alpha']=v
    elif a==0x3D: st['fogcol']=v
    elif a==0x1A: st['prmodecont']=v&1
    elif a==0x1B: st['prmode']=v
    elif a==0x4E: st['zbuf']=v
    elif a==0x40: st['scissor']=v
paths={}
ev=0
with open(path,'rb') as f:
    while True:
        h=f.read(12)
        if len(h)<12: break
        kind,p,sz=struct.unpack('<III',h); ev+=1
        if f.tell()<size-tail and kind!=2:
            # still track state-setting A+D cheaply? skip data
            f.seek(sz,1); paths.pop(p,None); continue
        data=f.read(sz)
        if len(data)<sz: break
        if kind==2:
            continue
        if kind!=1: continue
        ps=paths.setdefault(p,{'left':0})
        pos=0
        while pos+16<=len(data):
            if ps['left']==0:
                lo,hi=struct.unpack_from('<QQ',data,pos); pos+=16
                nloop=lo&0x7FFF; eop=(lo>>15)&1; pre=(lo>>46)&1; prim=(lo>>47)&0x7FF; flg=(lo>>58)&3; nreg=(lo>>60)&15 or 16
                ps.update(flg=flg,nreg=nreg,regs=[(hi>>(4*i))&15 for i in range(nreg)],idx=0)
                if pre and flg!=3 and nloop: setprim(prim)
                if flg==0: ps['left']=nloop*nreg
                elif flg==1: ps['left']=nloop*nreg; ps['odd']=(nloop*nreg)&1
                else: ps['left']=nloop
                if nloop==0: ps['left']=0
                continue
            flg=ps['flg']
            if flg==0:
                lo,hi=struct.unpack_from('<QQ',data,pos); pos+=16
                r=ps['regs'][ps['idx']%ps['nreg']]; ps['idx']+=1; ps['left']-=1
                if r==0: setprim(lo)
                elif r==1:
                    q=st['rgbaq'][4]; st['rgbaq']=(lo&255,(lo>>32)&255,hi&255,(hi>>32)&255,st.get('q',1.0))
                elif r==2:
                    s,t=struct.unpack('<ff',struct.pack('<Q',lo)); qq=struct.unpack('<f',struct.pack('<I',hi&0xFFFFFFFF))[0]; st['st']=(s,t); st['q']=qq
                elif r==3: st['uv']=(lo&0x3FFF,(lo>>32)&0x3FFF)
                elif r==4: vertex(lo&0xFFFF,(lo>>32)&0xFFFF,(hi>>4)&0xFFFFFF,(hi>>36)&255,not (hi>>47)&1)
                elif r==5: vertex(lo&0xFFFF,(lo>>32)&0xFFFF,hi&0xFFFFFFFF,0,not (hi>>47)&1)
                elif r==0xE: reg(hi&0xFF,lo)
                elif r==0xF: pass
                else: reg(r,lo)
            elif flg==1:
                for k in range(2):
                    if ps['left']==0: break
                    v=struct.unpack_from('<Q',data,pos+8*k)[0]
                    r=ps['regs'][ps['idx']%ps['nreg']]; ps['idx']+=1; ps['left']-=1
                    if r not in (0xE,0xF): reg(r,v)
                pos+=16
            else:
                pos+=16; ps['left']-=1
print('events',ev,'tris in tail',ntri,'hits',len(hits))
last=list(hits)[-int(sys.argv[5]) if len(sys.argv)>5 else -14:]
for kind,s,tri,e in last:
    pr=s['prim']; t0=s['tex0']
    print(f"{kind} ev={e} prim={pr:#05x} type={pr&7} iip={(pr>>3)&1} tme={(pr>>4)&1} fge={(pr>>5)&1} abe={(pr>>6)&1} fst={(pr>>8)&1} tex0 tbp={t0&0x3FFF:#x} psm={(t0>>20)&63:#x} tw={(t0>>26)&15} th={(t0>>30)&15} tfx={(t0>>35)&3} cbp={(t0>>37)&0x3FFF:#x} fbp={s['frame']&0x1FF:#x} alpha={s['alpha']:#x} test={s['test']:#x}")
    t1=s.get('tex1',0); m1=s.get('mip1',0)
    print('      tex1 lcm=%d mxl=%d mmag=%d mmin=%d mtba=%d L=%d K=%d | miptbp1 %s | clamp=%#x'%(t1&1,(t1>>2)&7,(t1>>5)&1,(t1>>6)&7,(t1>>9)&1,(t1>>19)&3,((t1>>32)&0xFFF)-(0x1000 if (t1>>43)&1 else 0),[(hex((m1>>(20*i))&0x3FFF),(m1>>(20*i+14))&63) for i in range(3)],s.get('clamp',0)))
    print('      '+' | '.join('(%.0f,%.0f) z=%x q=%.5f uv=(%.2f,%.2f) c=%s f=%d'%(v[0],v[1],v[2],v[7],v[3][0]/v[7] if v[7] else 0,v[3][1]/v[7] if v[7] else 0,v[4][:4],v[6]) for v in tri))
