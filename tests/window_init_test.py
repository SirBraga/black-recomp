#!/usr/bin/env python3
"""Exercise the production Raylib platform/GPU initialization sequence with a failed platform."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'tools/PS2Recomp/out/rt/_deps/raylib-src/src/rcore.c').read_text()
start = source.index('    // Initialize platform\n', source.index('void InitWindow('))
end = source.index('    // Setup default viewport', start)
sequence = source[start:end]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define TRACELOG(...) ((void)0)
static int platformResult, gpuCalls;
static bool isGpuReady;
static struct { struct { struct { int width, height; } currentFbo; } Window; } CORE;
static int InitPlatform(void) { return platformResult; }
static void rlglInit(int width, int height) { (void)width; (void)height; ++gpuCalls; }
static void initialize(void) {
'''+sequence+r'''
}
int main(void) {
    platformResult = -1;
    initialize();
    assert(gpuCalls == 0 && !isGpuReady);
    platformResult = 0;
    initialize();
    assert(gpuCalls == 1 && isGpuReady);
    puts("window init: PASS (failed platform skips GPU; successful platform initializes GPU)");
}
'''
with tempfile.TemporaryDirectory(prefix='black-window-test-') as temp:
    test = Path(temp) / 'test.c'; executable = Path(temp) / 'test'
    test.write_text(code)
    subprocess.run(['clang', '-std=c11', str(test), '-o', str(executable)], check=True)
    raise SystemExit(subprocess.run([str(executable)]).returncode)
