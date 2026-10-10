import struct,sys
U1={0:'ADDx',1:'ADDy',2:'ADDz',3:'ADDw',4:'SUBx',5:'SUBy',6:'SUBz',7:'SUBw',8:'MADDx',9:'MADDy',10:'MADDz',11:'MADDw',12:'MSUBx',13:'MSUBy',14:'MSUBz',15:'MSUBw',16:'MAXx',17:'MAXy',18:'MAXz',19:'MAXw',20:'MINIx',21:'MINIy',22:'MINIz',23:'MINIw',24:'MULx',25:'MULy',26:'MULz',27:'MULw',28:'MULq',29:'MAXi',30:'MULi',31:'MINIi',32:'ADDq',33:'MADDq',34:'ADDi',35:'MADDi',36:'SUBq',37:'MSUBq',38:'SUBi',39:'MSUBi',40:'ADD',41:'MADD',42:'MUL',43:'MAX',44:'SUB',45:'MSUB',46:'OPMSUB',47:'MINI'}
U2={0:'ADDAx',1:'ADDAy',2:'ADDAz',3:'ADDAw',4:'SUBAx',5:'SUBAy',6:'SUBAz',7:'SUBAw',8:'MADDAx',9:'MADDAy',10:'MADDAz',11:'MADDAw',12:'MSUBAx',13:'MSUBAy',14:'MSUBAz',15:'MSUBAw',16:'ITOF0',17:'ITOF4',18:'ITOF12',19:'ITOF15',20:'FTOI0',21:'FTOI4',22:'FTOI12',23:'FTOI15',24:'MULAx',25:'MULAy',26:'MULAz',27:'MULAw',28:'MULAq',29:'ABS',30:'MULAi',31:'CLIP',32:'ADDAq',33:'MADDAq',34:'ADDAi',35:'MADDAi',36:'SUBAq',37:'MSUBAq',38:'SUBAi',39:'MSUBAi',40:'ADDA',41:'MADDA',42:'MULA',44:'SUBA',45:'MSUBA',46:'OPMULA',47:'NOP'}
L1={0x30:'IADD',0x31:'ISUB',0x32:'IADDI',0x34:'IAND',0x35:'IOR'}
L2={0x30:'MOVE',0x31:'MR32',0x34:'LQI',0x35:'SQI',0x36:'LQD',0x37:'SQD',0x38:'DIV',0x39:'SQRT',0x3A:'RSQRT',0x3B:'WAITQ',0x3C:'MTIR',0x3D:'MFIR',0x3E:'ILWR',0x3F:'ISWR',0x40:'RNEXT',0x41:'RGET',0x42:'RINIT',0x43:'RXOR',0x64:'MFP',0x68:'XTOP',0x69:'XITOP',0x6C:'XGKICK'}
LO={0:'LQ',1:'SQ',4:'ILW',5:'ISW',8:'IADDIU',9:'ISUBIU',0x10:'FCEQ',0x11:'FCSET',0x12:'FCAND',0x13:'FCOR',0x14:'FSEQ',0x15:'FSSET',0x16:'FSAND',0x17:'FSOR',0x18:'FMEQ',0x1A:'FMAND',0x1B:'FMOR',0x1C:'FCGET',0x20:'B',0x21:'BAL',0x24:'JR',0x25:'JALR',0x28:'IBEQ',0x29:'IBNE',0x2C:'IBLTZ',0x2D:'IBGTZ',0x2E:'IBLEZ',0x2F:'IBGEZ'}
def dm(d): return ''.join(c for c,b in zip('xyzw',(8,4,2,1)) if d&b)
def dis(mem,start,n):
    for i in range(n):
        pc=start+i*8; lo,up=struct.unpack_from('<II',mem,pc)
        d=(up>>21)&15; ft=(up>>16)&31; fs=(up>>11)&31; fd=(up>>6)&31; op=up&63
        if op>=0x3C:
            o2=((up>>4)&0x7C)|(up&3); us='%s.%s ft=vf%d fs=vf%d'%(U2.get(o2,'u2_%02x'%o2),dm(d),ft,fs)
        else: us='%s.%s fd=vf%d fs=vf%d ft=vf%d'%(U1.get(op,'u1_%02x'%op),dm(d),fd,fs,ft)
        fl=''.join(c for c,b in zip('IEMDT',(31,30,29,28,27)) if up>>b&1)
        if up>>31&1: ls='I=%g'%struct.unpack('<f',struct.pack('<I',lo))[0]
        else:
            lop=lo>>25; ld=(lo>>21)&15; it=(lo>>16)&31; is_=(lo>>11)&31; id_=(lo>>6)&31; imm11=lo&0x7FF
            if imm11&0x400: simm=imm11-0x800
            else: simm=imm11
            if lop==0x40:
                f=lo&63
                if f>=0x3C:
                    o2=((lo>>4)&0x7C)|(lo&3); ls='%s.%s t=%d s=%d ftf=%d fsf=%d'%(L2.get(o2,'l2_%02x'%o2),dm(ld),it,is_,(lo>>23)&3,(lo>>21)&3)
                else: ls='%s d=vi%d s=vi%d t=vi%d imm5=%d'%(L1.get(f,'l1_%02x'%f),id_,is_,it,id_)
            elif lop in (0x20,0x21,0x28,0x29,0x2C,0x2D,0x2E,0x2F): ls='%s t=vi%d s=vi%d -> %04x'%(LO[lop],it,is_,(pc+8+simm*8)&0xFFFF)
            elif lop in LO: ls='%s.%s t=%d s=%d imm11=%d imm24=%06x imm15=%04x'%(LO[lop],dm(ld),it,is_,simm,lo&0xFFFFFF,((lo>>10)&0x7800)|(lo&0x7FF))
            else: ls='l_%02x %08x'%(lop,lo)
            if lo==0x8000033C: ls='NOP'
        print('%04x: %-5s %-34s | %s'%(pc,fl,us,ls))
        if 'E' in fl and i<n-1: n=min(n,i+2)
if __name__=='__main__':
    dis(open(sys.argv[1],'rb').read(),int(sys.argv[2],16),int(sys.argv[3]))
