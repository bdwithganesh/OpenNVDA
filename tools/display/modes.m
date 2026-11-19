// modes: list every display and the modes CoreGraphics offers (incl. HiDPI duplicates), mark the current one.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
int main(void) {
    CGDirectDisplayID ids[16]; uint32_t n = 0; CGGetOnlineDisplayList(16, ids, &n);
    NSDictionary *opt = @{(id)kCGDisplayShowDuplicateLowResolutionModes: @YES};
    for (uint32_t i = 0; i < n; i++) {
        CGDisplayModeRef cur = CGDisplayCopyDisplayMode(ids[i]);
        printf("display %u vendor %04x model %04x main %d: current %zux%zu px %zux%zu\n", ids[i], CGDisplayVendorNumber(ids[i]),
               CGDisplayModelNumber(ids[i]), CGDisplayIsMain(ids[i]), CGDisplayModeGetWidth(cur), CGDisplayModeGetHeight(cur),
               CGDisplayModeGetPixelWidth(cur), CGDisplayModeGetPixelHeight(cur));
        NSArray *m = CFBridgingRelease(CGDisplayCopyAllDisplayModes(ids[i], (__bridge CFDictionaryRef)opt));
        for (id x in m) { CGDisplayModeRef r = (__bridge CGDisplayModeRef)x;
            printf("  %s %5zux%-5zu px %5zux%-5zu %.0f Hz usable %d\n", CGDisplayModeGetIODisplayModeID(r) == CGDisplayModeGetIODisplayModeID(cur) && CGDisplayModeGetPixelWidth(r) == CGDisplayModeGetPixelWidth(cur) && CGDisplayModeGetWidth(r) == CGDisplayModeGetWidth(cur) ? "*" : " ",
                   CGDisplayModeGetWidth(r), CGDisplayModeGetHeight(r), CGDisplayModeGetPixelWidth(r), CGDisplayModeGetPixelHeight(r),
                   CGDisplayModeGetRefreshRate(r), CGDisplayModeIsUsableForDesktopGUI(r)); }
        CGDisplayModeRelease(cur);
    }
}
