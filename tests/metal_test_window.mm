#import <Cocoa/Cocoa.h>

static NSWindow *testWindow;

extern "C" void *black_test_window_create() {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
    testWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 640, 448)
                                             styleMask:(NSWindowStyleMaskTitled |
                                                        NSWindowStyleMaskClosable)
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
    if (!testWindow) return nullptr;
    testWindow.title = @"Black GS Metal presentation test";
    [testWindow makeKeyAndOrderFront:nil];
    return (__bridge void *)testWindow;
}

extern "C" void black_test_window_destroy(void *window) {
    if (!window || (__bridge NSWindow *)window != testWindow) return;
    [testWindow close];
    testWindow = nil;
}
