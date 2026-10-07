#!/usr/bin/env python3
"""audit_texture_uploads.py + --extract also writes <extract>.seq: one line per completed upload (U ev path bp psm w h bw x y) and per TEX0 (T ev path tbp cbp psm cld)."""
import struct,sys,argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("capture")
parser.add_argument("--cbp",type=int,default=12543)
parser.add_argument("--tbp",type=int,default=12512)
parser.add_argument("--extract",help="Write complete host transfers until the first matching TEX0")
parser.add_argument("--all-transfers",action="store_true",help="Do not stop at the first matching TEX0")
parser.add_argument("--tex0",type=lambda value:int(value,0),help="Stop extraction only at this exact TEX0 value")
parser.add_argument("--trace-after",type=int,help="Log registers and completed transfers after this transfer ordinal")
parser.add_argument("--trace-bp",type=int,help="Log complete transfers to this destination base pointer with event offsets")
args=parser.parse_args()
LOG=open(args.extract+".seq","w") if args.extract else None
out=open(args.extract,"wb") if args.extract else None
state={};paths={};active=None;number=0;texnumber=0;transfer_number=0;event_number=0;event_path=0;event_pos=0

def reg(addr,value):
 global active,number,texnumber
 if args.trace_after is not None and transfer_number>=args.trace_after and addr in (6,7,0x50,0x51,0x52,0x53,0x1c):
  print(f"event={event_number} path={event_path} pos={event_pos} transfer={transfer_number} register={addr:02x} value={value:016x}")
 if addr==0x53 and active and active['bp']==args.tbp and number<12:
  print('texture transfer',active['psm'],'expected_remaining',active['bytes'],'received',len(active['data']));number+=1
 if addr in (6,7) and value>>37&16383==args.cbp and texnumber<16:
  print('TEX0 target',hex(value),'CLD',value>>61&7,'TBP',value&16383);texnumber+=1
 if out and not args.all_transfers and addr in (6,7) and (value==args.tex0 if args.tex0 is not None else value>>37&16383==args.cbp and value&16383==args.tbp):
  print(f"STOP exact_tex0={value:016x} event={event_number} path={event_path} transfers={transfer_number}")
  out.close();raise SystemExit
 (LOG.write(f'T {event_number} {event_path} {value&16383} {value>>37&16383} {(value>>20)&63} {value>>61&7}\n') if LOG and addr in (6,7) else None)
 state[addr]=value
 if addr==0x53:
  bb=state.get(0x50,0);wh=state.get(0x52,0);psm=bb>>56&63
  bpp={0:32,1:24,2:16,10:16,19:8,20:4}.get(psm,0)
  active={'bp':bb>>32&16383,'psm':psm,'bytes':((wh&4095)*(wh>>32&4095)*bpp+7)//8,'data':bytearray(),'bw':bb>>48&63,'x':state.get(0x51,0)>>32&2047,'y':state.get(0x51,0)>>48&2047,'w':wh&4095,'h':wh>>32&4095} if value&3==0 else None
 if addr==0x54: image(struct.pack('<Q',value))
def image(data):
 global number,transfer_number
 if active and active['bytes']:
  if not active['data']:
   active['start_event']=event_number;active['start_path']=event_path;active['start_pos']=event_pos
  active['data']+=data
  if len(active['data'])>=active['bytes'] and active['bytes']:
   transfer_number+=1
   LOG and LOG.write(f"U {event_number} {event_path} {active['bp']} {active['psm']} {active['w']} {active['h']} {active['bw']} {active['x']} {active['y']}\n")
   if args.trace_after is not None and transfer_number>=args.trace_after:
    print(f"transfer={transfer_number} event={event_number} path={event_path} pos={event_pos} psm={active['psm']} bp={active['bp']} bw={active['bw']} size={active['w']}x{active['h']}")
   if args.trace_bp is not None and active['bp']==args.trace_bp:
    print(f"target_transfer={transfer_number} start_event={active['start_event']} start_path={active['start_path']} start_pos={active['start_pos']} end_event={event_number} end_pos={event_pos} psm={active['psm']} bp={active['bp']} bw={active['bw']} xy={active['x']},{active['y']} size={active['w']}x{active['h']} bytes={active['bytes']}")
   if out:
    out.write(struct.pack('<8I',active['psm'],active['bp'],active['bw'],active['x'],active['y'],active['w'],active['h'],active['bytes']));out.write(active['data'][:active['bytes']])
   if active['bp']==args.cbp and number<4:
    print('palette incoming',active['psm'],active['bytes'],[hex(x) for x in struct.unpack('<'+str(active['bytes']//4)+'I',active['data'][:active['bytes']])][:24]);number+=1
   active['bytes']=0
with open(args.capture,'rb') as f:
 while h:=f.read(12):
  kind,path,size=struct.unpack('<III',h);data=f.read(size);event_number+=1;event_path=path;event_pos=0
  if kind==2:reg(path,struct.unpack('<Q',data)[0])
  elif kind==3:image(data)
  elif kind==1:
   p=paths.setdefault(path,{'left':0,'reg':0})
   for pos in range(0,len(data)-15,16):
    event_pos=pos
    lo,hi=struct.unpack_from('<QQ',data,pos)
    if not p['left']:
     p.update(left=lo&32767,fmt=lo>>58&3,nreg=lo>>60&15 or 16,desc=hi,reg=0);continue
    if p['fmt']==0:
     d=p['desc']>>(p['reg']*4)&15
     if d==14:reg(hi&255,lo)
     elif d in (6,7):reg(d,lo)
     p['reg']+=1
     if p['reg']==p['nreg']:p['reg']=0;p['left']-=1
    elif p['fmt']==1:
     for v in (lo,hi):
      if not p['left']:break
      reg(p['desc']>>(p['reg']*4)&15,v);p['reg']+=1
      if p['reg']==p['nreg']:p['reg']=0;p['left']-=1
    else:image(data[pos:pos+16]);p['left']-=1

if out:
 out.close()
 if args.tex0 is not None and not args.all_transfers:
  raise SystemExit(f"Requested exact TEX0 {args.tex0:016x} was not found; extraction is not a matching prefix")
