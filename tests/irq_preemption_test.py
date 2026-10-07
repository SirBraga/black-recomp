#!/usr/bin/env python3
"""Verify production checkpoints preserve an active ISR until its return."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'tools/PS2Recomp/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp').read_text()
def method(start, end):
    return source[source.index(start):source.index(end, source.index(start))]
helper = method('    bool interruptInvocationActive(', '    constexpr int KE_OK')
checkpoint = method('bool EeScheduler::checkpointDue(', '\nvoid EeScheduler::accountCycles(')
preemption = method('void EeScheduler::applyPendingPreemption()', '\nvoid EeScheduler::processPendingEvents()')
head = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
enum class GuestInvocationKind { Interrupt, SifCommand };
struct GuestInvocation { GuestInvocationKind kind; };
struct GuestThread { int id=1,currentPriority=20; std::vector<GuestInvocation> invocations; };
struct EeScheduler {
 GuestThread thread; int m_currentThreadId=1;
 bool m_rescheduleRequested=false,m_timeSliceExpired=false,ready=true;
 std::atomic<bool> m_checkpointPending{false},m_stopRequested{false};
 std::atomic<uint64_t> m_nextDeadlineCycle{0}; uint64_t m_eeCycle=100,m_sliceEndCycle=50;
 int enqueued=0; bool enqueuedFront=false;
 void assertExecutor(){}; void accountCycles(uint32_t cycles){m_eeCycle+=cycles;}
 GuestThread* currentThread(){return m_currentThreadId?&thread:nullptr;}
 bool hasReadyAtOrAbovePriority(int){return ready;}
 void renewTimeSlice(){m_sliceEndCycle=m_eeCycle+50;}
 void enqueueReady(GuestThread&,bool front){++enqueued;enqueuedFront=front;}
 bool checkpointDue(uint32_t) noexcept; void applyPendingPreemption();
};
'''
tail = r'''
int main(){
 EeScheduler s; s.thread.invocations.push_back({GuestInvocationKind::Interrupt});
 assert(!s.checkpointDue(1)); // an expired slice cannot switch away from the ISR
 assert(!s.m_rescheduleRequested && !s.m_timeSliceExpired);
 s.m_rescheduleRequested=true; // iSignalSema woke a higher-priority thread
 s.applyPendingPreemption();
 assert(s.m_currentThreadId==1 && s.enqueued==0 && s.m_rescheduleRequested);
 s.m_checkpointPending=true; assert(s.checkpointDue(1)); // hardware events still run
 s.m_checkpointPending=false; s.m_nextDeadlineCycle=s.m_eeCycle+1;
 assert(s.checkpointDue(1)); // timer deadline remains observable inside the ISR
 s.m_checkpointPending=false; s.m_nextDeadlineCycle=0;
 s.thread.invocations.pop_back(); // guest ISR has returned
 s.applyPendingPreemption();
 assert(s.m_currentThreadId==0 && s.enqueued==1 && s.enqueuedFront && !s.m_rescheduleRequested);
 EeScheduler ordinary; assert(ordinary.checkpointDue(1));
 assert(ordinary.m_timeSliceExpired && ordinary.m_rescheduleRequested);
 ordinary.applyPendingPreemption(); assert(ordinary.enqueued==1 && !ordinary.enqueuedFront);
 EeScheduler sif; sif.thread.invocations.push_back({GuestInvocationKind::SifCommand});
 assert(sif.checkpointDue(1)); // unrelated callbacks keep normal preemption
}
'''
with tempfile.TemporaryDirectory() as directory:
    d=Path(directory); (d/'test.cpp').write_text(head+helper+checkpoint+preemption+tail)
    subprocess.run(['clang++','-std=c++20','-O2',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
print('PASS: ISR defers thread dispatch until return; events, ordinary slices and SIF remain active')
