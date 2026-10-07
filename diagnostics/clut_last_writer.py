#!/usr/bin/env python3
"""For every indexed TEX0 with CLD!=0, find which upload last wrote the CLUT block (CBP).

Input: the .seq file from audit_texture_sequence.py (works for recomp raw GIF captures and
pcsx2_dump_to_events.py output). 'palette_upload' = last writer is a CT32 upload <=256 texels;
'overwritten_by_other' = a larger texture upload covered the CBP block after the palette.
PCSX2 Level_00 reference (2026-10-07): 4692/4692 palette_upload. Block math = GS page/block tables.
"""
import sys,collections
T32=[[0,1,4,5,16,17,20,21],[2,3,6,7,18,19,22,23],[8,9,12,13,24,25,28,29],[10,11,14,15,26,27,30,31]]
T16=[[0,2,8,10],[1,3,9,11],[4,6,12,14],[5,7,13,15],[16,18,24,26],[17,19,25,27],[20,22,28,30],[21,23,29,31]]
G={0:(64,32,8,8,T32),1:(64,32,8,8,T32),2:(64,64,16,8,T16),10:(64,64,16,8,T16),19:(128,64,16,16,T32),20:(128,128,32,16,T16)}
def blocks(bp,bw,psm,x,y,w,h):
  pw,ph,bwid,bh,tab=G[psm]; ppr=max(1,(bw*64)//pw); out=set()
  for by in range(y//bh,(y+h-1)//bh+1):
    for bx in range(x//bwid,(x+w-1)//bwid+1):
      px,py=(bx*bwid)//pw,(by*bh)//ph
      out.add((bp+(py*ppr+px)*32+tab[by%(ph//bh)][bx%(pw//bwid)])&0x3fff)
  return out
last={};n=0;res=collections.Counter();ex=[]
for l in open(sys.argv[1]):
  e=l.split()
  if e[0]=='U':
    n+=1;bp,psm,w,h,bw,x,y=map(int,e[3:10])
    if psm in G:
      for b in blocks(bp,bw,psm,x,y,w,h): last[b]=(n,bp,psm,w,h,x,y)
  else:
    tbp,cbp,psm,cld=map(int,e[3:7])
    if psm in (19,20) and cld:
      wr=last.get(cbp)
      if wr is None: res['unknown']+=1
      elif wr[3]*wr[4]<=256 and wr[2]==0: res['palette_upload']+=1
      else:
        res['overwritten_by_other']+=1
        if len(ex)<8: ex.append(dict(event=e[1],tbp=tbp,cbp=cbp,writer=wr))
runs=[];c=0
for l in open(sys.argv[1]):
  if l[0]=='U': c+=1
  elif c: runs.append(c);c=0
print(dict(res)); print('max uploads without a TEX0 in between:',max(runs+[c]))
for x in ex: print(' ',x)
