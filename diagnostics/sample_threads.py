import re,sys
txt=open(sys.argv[1]).read()
call=txt[txt.index('Call graph:'):txt.index('Total number in stack')]
for t in re.split(r'\n    (?=\d+ Thread_)',call)[1:]:
    head=t.split('\n',1)[0]; total=int(head.split()[0])
    P=[]
    for l in t.split('\n')[1:]:
        m=re.match(r'^([ +!:|]*)(\d+) (.+?)  \(in ([^)]+)\)',l)
        if m: P.append((len(m.group(1)),int(m.group(2)),m.group(3)))
    leaf={}
    for i,(d,n,s) in enumerate(P):
        child=0
        for d2,n2,_ in P[i+1:]:
            if d2<=d: break
            if d2==d+2: child+=n2
        if n-child>0: leaf[s]=leaf.get(s,0)+n-child
    wait=lambda s: any(k in s for k in ('psynch','mach_msg','semaphore_wait','semwait','ulock','kevent','workq'))
    busy=sum(v for s,v in leaf.items() if not wait(s))
    if busy*100/total>8:
        print('%-46s busy %3.0f%%'%(head[:46],busy*100/total), [ (v,s[:44]) for v,s in sorted(((v,s) for s,v in leaf.items()),reverse=True)[:int(sys.argv[2]) if len(sys.argv)>2 else 5]])
