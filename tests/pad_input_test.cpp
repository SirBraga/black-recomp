#include "runtime/ps2_pad.h"
#include "raylib.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

static bool keys[512]{};
static bool connected = false;
static float axes[6]{};
extern "C" bool IsKeyDown(int key) { return keys[key]; }
extern "C" bool IsGamepadAvailable(int) { return connected; }
extern "C" bool IsGamepadButtonDown(int, int) { return false; }
extern "C" float GetGamepadAxisMovement(int, int axis) { return axes[axis]; }

int main()
{
    setenv("PS2X_PAD_SCRIPT", "1:ls_up+rs_right+cross:1,2:ls_left+ls_right:1", 1);
    double now = 0;
    PSPadBackend::setGuestClock([&] { return now; });
    PSPadBackend pad;
    uint8_t data[32];
    auto read = [&] { assert(pad.readState(0, 0, data, sizeof(data))); };
    read();
    assert(data[4] == 128 && data[5] == 128 && data[6] == 128 && data[7] == 128);
    assert(data[2] == 255 && data[3] == 255);
    keys[KEY_W] = keys[KEY_J] = true;
    read();
    assert(data[7] == 1 && data[4] == 1 && data[2] == 0xef);
    now = 1.5;
    read();
    assert(data[7] == 1 && data[4] == 255 && data[3] == 0xbf);
    keys[KEY_A] = true;
    now = 2.5;
    read();
    assert(data[6] == 128 && data[7] == 1 && data[4] == 1 && data[3] == 255);
    now = 3;
    read();
    assert(data[6] == 1 && data[7] == 1 && data[4] == 1);
    keys[KEY_D] = keys[KEY_S] = true;
    read();
    assert(data[6] == 128 && data[7] == 128);
    connected = true;
    axes[GAMEPAD_AXIS_LEFT_X] = 1;
    axes[GAMEPAD_AXIS_LEFT_Y] = -1;
    axes[GAMEPAD_AXIS_RIGHT_Y] = 1;
    read();
    assert(data[6] == 255 && data[7] == 1 && data[5] == 255 && data[4] == 128);
    assert(data[2] == 255 && data[3] == 255);
    assert(!pad.readState(0, 0, data, 31));
    assert(!pad.readState(0, 0, nullptr, 32));
    std::puts("pad input: PASS (keyboard, script, cancellation, release, gamepad)");
}
