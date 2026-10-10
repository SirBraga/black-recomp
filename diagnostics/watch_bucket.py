"""lldb script: catch whoever overwrites the game's render-queue bucket table (the freeze where qsort is called
with a garbage compare function, see PS2_PROJECT_STATE.md). A hardware watchpoint sits on bucket 0's compare
pointer, which only the constructor writes; when anything else writes there the host backtrace (the recompiled
function names carry the guest addresses) and the registers go to the log and the guest RAM is saved.

  lldb -b -o "command script import ps2recomp/diagnostics/watch_bucket.py" -o run -- ps2EntryRunner SLUS_213.76
  (JogarBlack.command does it with BLACK_WATCH=1)
"""
import lldb, os

# BLACK_WATCH_ADDR=<guest address> watches something else; BLACK_WATCH_ARM=<symbol> arms it when that function is
# first called (any recompiled function works: its first argument is the guest RAM). BLACK_WATCH_HITS=<n> keeps the
# watchpoint for n writes instead of one.
WATCH_GUEST = int(os.environ.get('BLACK_WATCH_ADDR', '0x4C9C00'), 0)  # default: [D_0040F4C0] + 0x14 + 0xCA58 + 0x14
state = {}

def setup(frame, location, internal):
    target = frame.GetThread().GetProcess().GetTarget()
    state['rdram'] = frame.FindRegister('x0').GetValueAsUnsigned()
    location.GetBreakpoint().SetEnabled(False)
    error = lldb.SBError()
    watch = target.WatchAddress(state['rdram'] + WATCH_GUEST, 4, False, True, error)
    print('[watch] guest RAM at 0x%x, watchpoint %s %s' % (state['rdram'], 'set' if watch.IsValid() else 'FAILED', error), flush=True)
    if watch.IsValid():
        lldb.debugger.HandleCommand('watchpoint command add -F watch_bucket.hit %d' % watch.GetID())
    return False

def hit(frame, watch, internal):
    thread = frame.GetThread()
    process = thread.GetProcess()
    print('[watch] WRITE to the bucket table, thread %s' % thread.GetName(), flush=True)
    for index in range(min(thread.GetNumFrames(), 20)):
        print('[watch]   #%d %s' % (index, thread.GetFrameAtIndex(index).GetFunctionName()), flush=True)
    registers = ' '.join('%s=%x' % (name, frame.FindRegister(name).GetValueAsUnsigned()) for name in ('pc', 'x0', 'x1', 'x2', 'x3', 'x8', 'x9', 'x10', 'x11'))
    print('[watch]   ' + registers, flush=True)
    error = lldb.SBError()
    data = process.ReadMemory(state['rdram'], 0x2000000, error)
    path = os.environ.get('BLACK_WATCH_RAM', 'watch-fault.ram')
    if error.Success():
        open(path, 'wb').write(data)
        print('[watch]   guest RAM saved to ' + path, flush=True)
    state['hits'] = state.get('hits', 0) + 1
    if state['hits'] >= int(os.environ.get('BLACK_WATCH_HITS', '1')):
        watch.SetEnabled(False)
    return False

def __lldb_init_module(debugger, internal):
    target = debugger.GetSelectedTarget()
    breakpoint = target.BreakpointCreateByName(os.environ.get('BLACK_WATCH_ARM', 'sub_001AF580_0x1af580'))
    breakpoint.SetScriptCallbackFunction('watch_bucket.setup')
    print('[watch] waiting for the bucket table to be set up (level load)', flush=True)
