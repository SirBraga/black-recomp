import re,sys
txt=open(sys.argv[1]).read()
call=txt[txt.index('Call graph:'):txt.index('Total number in stack')]
for t in re.split(r'\n    (?=\d+ Thread_)',call)[1:]:
    head=t.split('\n',1)[0]; total=int(head.split()[0])
    if total<1000: continue
    P=[]
    for l in t.split('\n')[1:]:
        m=re.match(r'^([ +!:|]*)(\d+) (.+?)  \(in ([^)]+)\)',l)
        if m: P.append((len(m.group(1)),int(m.group(2)),m.group(3)))
    agg={}
    stack=[]
    for d,n,s in P:
        while stack and stack[-1][0]>=d: stack.pop()
        if any(k in s for k in ('__psynch_cvwait','__psynch_mutexwait','semaphore_wait_trap','__semwait')):
            # nearest non-system ancestors
            names=[x[1] for x in stack if not any(k in x[1] for k in ('pthread','std::__1','_pthread','condition_variable','mutex','unique_lock','thread_start','__thread_proxy','void*','decay','invoke'))][-3:]
            key=s.split('(')[0][2:14]+' <- '+' <- '.join(n.split('(')[0][-46:] for n in reversed(names))
            agg[key]=agg.get(key,0)+n
        stack.append((d,s))
    if agg:
        print(head[:44])
        for k,v in sorted(agg.items(),key=lambda x:-x[1])[:5]: print('   %5.1f%% %s'%(100*v/total,k[:170]))
