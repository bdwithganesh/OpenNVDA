// setmode W H [PXW]: switch the main display to the "looks like" W x H mode (pixel width PXW if given, else the
// largest), permanently (it stays after reboot/login). Prints the new mode. Exit 1 if no such mode.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: setmode W H [PIXELW]\n"); return 2; }
    size_t w = strtoul(argv[1], 0, 10), h = strtoul(argv[2], 0, 10), pw = argc > 3 ? strtoul(argv[3], 0, 10) : 0;
    CGDirectDisplayID d = CGMainDisplayID();
    NSDictionary *opt = @{(id)kCGDisplayShowDuplicateLowResolutionModes: @YES};
    NSArray *m = CFBridgingRelease(CGDisplayCopyAllDisplayModes(d, (__bridge CFDictionaryRef)opt));
    CGDisplayModeRef best = NULL;
    for (id x in m) {
        CGDisplayModeRef r = (__bridge CGDisplayModeRef)x;
        if (CGDisplayModeGetWidth(r) != w || CGDisplayModeGetHeight(r) != h || !CGDisplayModeIsUsableForDesktopGUI(r)) continue;
        if (pw ? CGDisplayModeGetPixelWidth(r) == pw : (!best || CGDisplayModeGetPixelWidth(r) > CGDisplayModeGetPixelWidth(best))) best = r;
    }
    if (!best) { printf("no %zux%zu mode\n", w, h); return 1; }
    CGDisplayConfigRef c; CGBeginDisplayConfiguration(&c);
    CGConfigureDisplayWithDisplayMode(c, d, best, NULL);
    CGError e = CGCompleteDisplayConfiguration(c, kCGConfigurePermanently);
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(d);
    printf("set %zux%zu px %zux%zu err %d; now %zux%zu px %zux%zu\n", w, h, CGDisplayModeGetPixelWidth(best),
           CGDisplayModeGetPixelHeight(best), e, CGDisplayModeGetWidth(cur), CGDisplayModeGetHeight(cur),
           CGDisplayModeGetPixelWidth(cur), CGDisplayModeGetPixelHeight(cur));
    CGDisplayModeRelease(cur);
    return e != kCGErrorSuccess;
}
