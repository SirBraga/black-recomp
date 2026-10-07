#!/usr/bin/env python3
"""Keep exactly the GPU events processed up to a TEX0 snapshot, including its partial packet."""
import argparse,json,struct
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('stream');p.add_argument('registers');p.add_argument('output');args=p.parse_args()
state=json.load(open(args.registers));events=state['capture_events'];prefix=state['trigger_packet_bytes'];total=0
assert events>0 and prefix>0 and state["capture_current_event"], "Capture ended before snapshot event"
with open(args.stream,'rb') as source,open(args.output,'wb') as output:
 for i in range(events):
  h=source.read(12);assert len(h)==12
  kind,path,size=struct.unpack('<III',h);data=source.read(size);assert len(data)==size
  total+=12+size
  if i==events-1:
   assert kind==1 and prefix<=size and prefix%16==0
   size=prefix;data=data[:prefix]
  output.write(struct.pack('<III',kind,path,size));output.write(data)
assert total==state['capture_bytes']
print('PASS:',events,'events; final GIF packet retained through byte',prefix)
