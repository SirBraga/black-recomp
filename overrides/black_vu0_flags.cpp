// Black's EE code never reads the VU0 status, MAC or clip registers (no CFC2 of control registers 16-18
// anywhere in the executable), so the VU0 microprograms (the culling tests, mostly) can compute their
// flags on demand and run as directly emitted blocks, like VU1's. BLACK_VU0_EXACT_FLAGS=1 keeps the
// cycle-by-cycle flag pipeline (diagnostic).
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <cstdlib>

namespace
{
    void applyBlackVu0Flags(PS2Runtime &)
    {
        if (!std::getenv("BLACK_VU0_EXACT_FLAGS"))
            VU1Interpreter::setVu0FlagsUnobserved(true);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black VU0 flags on demand", "SLUS_213.76", 0x00100008u, 0u, applyBlackVu0Flags)
