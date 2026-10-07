#!/usr/bin/env python3
"""Exercise production IRQ pending coalescing without replacing dispatch logic."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'tools/PS2Recomp/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp').read_text()
a=s.index('void EeScheduler::queueInvocation(');z=s.index('\n[[noreturn]]',a);body=s[a:z]
head=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <utility>
#include <cstdlib>
#include <cstdio>
enum class GuestInvocationKind {Interrupt,Alarm,GsCallback,RpcCallback,SyscallOverride,ExitHandler,HleCall,SifCommand};
struct GuestInvocation {GuestInvocationKind kind;uint64_t tag=0,sequence=0;struct {uint32_t pc=0;} context;};
struct GuestThread {std::deque<GuestInvocation> invocations;};
struct EeScheduler {uint64_t m_invocationSequence=0;std::deque<GuestInvocation> m_pendingInvocations;std::atomic<bool> m_checkpointPending{false};GuestThread* currentThread(){return nullptr;} void assertExecutor(){};void queueInvocation(GuestInvocation);};
'''
tail=r'''
int main(){
 EeScheduler s;
 for(int i=0;i<100000;++i) s.queueInvocation({GuestInvocationKind::Interrupt,1});
 assert(s.m_pendingInvocations.size()==1 && s.m_invocationSequence==1 && s.m_checkpointPending);
 s.queueInvocation({GuestInvocationKind::Interrupt,2});
 s.queueInvocation({GuestInvocationKind::Interrupt,(1ull<<63)|1});
 assert(s.m_pendingInvocations.size()==3); // distinct handler and INTC/DMAC namespace
 for(int i=0;i<100;++i)s.queueInvocation({GuestInvocationKind::SifCommand,1});
 assert(s.m_pendingInvocations.size()==103); // each packet must still be delivered
 s.m_pendingInvocations.pop_front(); // handler is now active, not pending
 for(int i=0;i<100000;++i)s.queueInvocation({GuestInvocationKind::Interrupt,1});
 assert(s.m_pendingInvocations.size()==103); // one re-delivery while active
}
'''
with tempfile.TemporaryDirectory() as directory:
 d=Path(directory);(d/'test.cpp').write_text(head+body+tail)
 subprocess.run(['clang++','-std=c++20','-O2',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('PASS: repeated IRQs latch once; distinct handlers, active re-delivery and SIF packets preserved')
