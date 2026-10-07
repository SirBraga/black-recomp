#include "runtime/ps2_memory.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main()
{
    setenv("PS2X_DMA_COOPERATIVE", "1", 1);
    PS2Memory memory;
    assert(memory.initialize());
    memory.writeIORegister(0x1000E000u, 1u);
    unsigned gifPackets = 0u, vuCalls = 0u;
    bool observedBothActive = false;
    memory.setGifPacketCallback([&](const uint8_t *, uint32_t bytes)
    {
        assert(bytes == 16u);
        ++gifPackets;
        // Nested polls must not consume the transfer whose callback is running.
        observedBothActive |= (memory.readIORegister(0x1000A000u) & 0x100u) &&
                              (memory.readIORegister(0x10009000u) & 0x100u);
    });
    memory.setVu1MscalCallback([&](uint32_t pc, uint32_t, uint32_t)
    {
        assert(pc == vuCalls * 8u);
        ++vuCalls;
        assert(memory.readIORegister(0x10009000u) & 0x100u);
        memory.processPendingTransfers(); // Exercise reentrant service guard.
    });

    // GIF END chain with two complete zero-loop EOP packets.
    memory.write64(0x1000u, (7ull << 28u) | 2u);
    memory.write64(0x1008u, 0u);
    for (uint32_t address : {0x1010u, 0x1020u})
    {
        memory.write64(address, 0x8000u);
        memory.write64(address + 8u, 0xfu);
    }
    memory.writeIORegister(0x1000A030u, 0x1000u);
    memory.writeIORegister(0x1000A000u, 0x105u);
    assert(gifPackets == 1u && memory.hasPendingGifTransfer());
    assert(memory.consumeCompletedDmacCauses().empty());

    // VIF1 END chain: two VU executions, then enough NOPs for several prefixes.
    memory.write64(0x2000u, (7ull << 28u) | 16u);
    memory.write64(0x2008u, 0u);
    for (uint32_t offset = 0u; offset < 256u; offset += 4u)
        memory.write32(0x2010u + offset, 0u);
    memory.write32(0x2010u, 0x14000000u);
    memory.write32(0x2014u, 0x14000001u);
    memory.writeIORegister(0x10009030u, 0x2000u);
    memory.writeIORegister(0x10009000u, 0x105u);
    assert(observedBothActive && gifPackets == 2u && vuCalls == 1u);
    assert(memory.consumeCompletedDmacCauses() == std::vector<uint32_t>{2u});
    unsigned polls = 0u;
    while (memory.readIORegister(0x10009000u) & 0x100u)
        assert(++polls < 128u);
    assert(vuCalls == 2u && polls >= 1u);
    assert(memory.consumeCompletedDmacCauses() == std::vector<uint32_t>{1u});
    assert(memory.pollDmaRegisters() == 0);
    assert(memory.consumeCompletedDmacCauses().empty());
    std::puts("PASS: concurrent GIF/VIF1 STR, command/EOP progress, reentrant callbacks, and one completion IRQ per channel");
}
