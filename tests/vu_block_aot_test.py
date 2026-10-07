#!/usr/bin/env python3
"""Check generated straight-line VU0 blocks across scheduler budget cuts."""
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
EMITTER = ROOT / "ps2recomp/diagnostics/emit_vu_upper_aot.py"


def main():
    with tempfile.TemporaryDirectory(prefix="vu-block-aot-") as tmp:
        tmp = Path(tmp)
        image = tmp / "vu0-block-fixture.bin"
        header = tmp / "vu0-block-fixture.hpp"
        # PC 0 contains IBEQ r1,r2,+3. PC 8 is the delay slot; PC 24 stores
        # a qword and PC 32 starts XGKICK. The taken path skips the store.
        branch = (40 << 25) | (2 << 16) | (1 << 11) | 3
        store = (1 << 25) | (15 << 21) | (4 << 16) | (2 << 11)
        kick = (64 << 25) | (3 << 11) | (108 << 4) | 60
        pairs = [(0x8000033c, 0x2ff)] * 16
        pairs[0] = (branch, 0x2ff)
        pairs[3] = (store, 0x2ff)
        pairs[4] = (kick, 0x2ff)
        image.write_bytes(b"".join(struct.pack("<II", *pair) for pair in pairs))
        subprocess.run(["python3", str(EMITTER), str(image), str(header)], check=True)
        source = tmp / "block_test.cpp"
        source.write_text(r'''#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "vu0-block-fixture.hpp"
struct State { float vf[32][4]{}; int32_t vi[16]{}; uint32_t pc=0, branchTarget=0; unsigned branchDelay=0; bool branchPending=false; };
struct Host {
    State m_state{}; uint32_t m_currentUpperInstruction=0; uint8_t data[4096]{};
    uint64_t cycle=0, storeReady=0; uint32_t storeAddress=0, storeWords[4]{}; uint8_t storeMask=0;
    bool storePending=false, kickActive=false; unsigned kickStarts=0, kickCompletions=0; uint32_t kickAddress=0; std::vector<uint8_t> kickBytes;
    uint32_t microAddressMask() { return 0xfff; }
    int32_t readBranchVi(uint8_t r) { return m_state.vi[r]; }
    void queueStore(uint32_t address,const uint32_t* words,uint8_t mask) { storePending=true;storeAddress=address;storeMask=mask;storeReady=cycle+1;std::memcpy(storeWords,words,16); }
    void startXgkick(uint32_t address) { assert(!kickActive);kickActive=true;kickAddress=address*16;++kickStarts; }
    void advanceCycle() {
        ++cycle;
        if(storePending && storeReady<=cycle) { for(unsigned c=0;c<4;c++)if(storeMask&(8u>>c))std::memcpy(data+storeAddress+c*4,&storeWords[c],4);storePending=false; }
        if(kickActive) { assert(!storePending || storeReady>cycle);kickBytes.assign(data+kickAddress,data+kickAddress+16);kickActive=false;++kickCompletions; }
    }
};
struct Pair { uint32_t upper=0x2ff, lower=0; bool iBit=false; unsigned upperVfShadowReg=0; };
struct Runner {
    Host& host; unsigned budget=0, issued=0; Pair pair{};
    uint32_t visited[128]{}; unsigned visits=0;
    template<class Body> bool operator()(Body body) {
        if (issued == budget) return false;
        pair.lower = host.m_state.pc == 0 ? ((40u << 25) | (2u << 16) | (1u << 11) | 3u)
                    : host.m_state.pc == 24 ? ((1u << 25) | (15u << 21) | (4u << 16) | (2u << 11))
                    : host.m_state.pc == 32 ? ((64u << 25) | (3u << 11) | (108u << 4) | 60u) : 0x8000033cu;
        if (!body(&host, pair, host.data, sizeof(host.data))) return false;
        visited[visits++] = host.m_state.pc;
        ++issued;
        host.m_state.pc = (host.m_state.pc + 8) & 0xfff;
        if (host.m_state.branchPending) {
            if (host.m_state.branchDelay == 0) {
                host.m_state.pc = host.m_state.branchTarget & 0xfff;
                host.m_state.branchPending = false;
            } else --host.m_state.branchDelay;
        }
        host.advanceCycle();
        return true;
    }
};
int main() {
    for (unsigned slice=1; slice<=8; ++slice) {
        Host host; host.m_state.pc=8; Runner runner{host};
        unsigned guard=0;
        while (runner.issued < 15 && ++guard<100) {
            runner.budget = std::min(runner.issued + slice, 15u);
            auto scheduler = BlackVuUpperAot::makeBlockScheduler<Pair>(runner, host);
            int status = BlackVuUpperAot::executeBlock(scheduler, host.m_state.pc);
            assert(status == 1 || status == 2);
            assert(host.m_state.pc == 8 + runner.issued * 8);
        }
        assert(runner.issued == 15 && host.m_state.pc == 128);
    }
    for (bool taken : {false,true}) for (unsigned slice=1; slice<=8; ++slice) {
        Host host; Runner runner{host};
        host.m_state.vi[1]=7; host.m_state.vi[2]=taken?7:8;
        host.m_state.vi[4]=1; host.m_state.vi[3]=1;
        const uint32_t stored[4]={0x11223344u,0x55667788u,0x99aabbccu,0xddeeff00u};
        std::memcpy(host.m_state.vf[2],stored,16);
        for(unsigned n=0;n<16;n++)host.data[16+n]=static_cast<uint8_t>(0xa0+n);
        while (runner.visits < 10) {
            runner.issued=0; runner.budget=std::min(slice,10u-runner.visits);
            auto scheduler = BlackVuUpperAot::makeBlockScheduler<Pair>(runner, host);
            int status = BlackVuUpperAot::executeBlock(scheduler, host.m_state.pc);
            assert(status == 1 || status == 2);
        }
        assert(runner.visits == 10 && runner.visited[0] == 0 && runner.visited[1] == 8);
        assert(host.m_state.pc == (taken?96u:80u) && !host.m_state.branchPending);
        assert(runner.visited[0] == 0 && runner.visited[1] == 8);
        assert((runner.visited[2] == 32u) == taken);
        if (taken) for (unsigned n=0; n<runner.visits; ++n) assert(runner.visited[n] != 16 && runner.visited[n] != 24);
        else assert(runner.visited[2] == 16u && runner.visited[3] == 24u);
        assert(host.kickStarts==1 && host.kickCompletions==1 && !host.kickActive && !host.storePending);
        const uint8_t* expected=taken?reinterpret_cast<const uint8_t*>("\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf"):reinterpret_cast<const uint8_t*>(stored);
        assert(host.kickBytes.size()==16 && std::memcmp(host.kickBytes.data(),expected,16)==0);
    }
    std::puts("PASS: generated blocks resume across IBEQ delay slots and preserve a queued SQ before one XGKICK event (slice sizes 1..8)");
}
''')
        binary = tmp / "block_test"
        subprocess.run(["clang++", "-std=c++20", "-O2", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

        # VU1 uses the same conservative dispatcher; every pair still passes
        # through the existing scheduler and its budget/resume contract.
        vu1_image = tmp / "vu1-block-fixture.bin"
        vu1_header = tmp / "vu1-block-fixture.hpp"
        vu1_image.write_bytes(image.read_bytes())
        subprocess.run(["python3", str(EMITTER), str(vu1_image), str(vu1_header)], check=True)
        vu1_source = tmp / "vu1_block_test.cpp"
        vu1_source.write_text(source.read_text().replace("vu0-block-fixture.hpp", "vu1-block-fixture.hpp"))
        vu1_binary = tmp / "vu1_block_test"
        subprocess.run(["clang++", "-std=c++20", "-O2", str(vu1_source), "-o", str(vu1_binary)], check=True)
        subprocess.run([str(vu1_binary)], check=True)


if __name__ == "__main__":
    main()
