// icon_alpha_probe: count visible pixels of an app icon through several paths, to find where
// the blank Dock icons on the RTX lose their content.
//   ws   NSWorkspace iconForFile (IconServices, may render in iconservicesagent)
//   icns the bundle's CFBundleIconFile .icns decoded directly by ImageIO (no IconServices)
//   car  ISIcon via -[NSWorkspace iconForContentType:] for the generic app type (reference glyph)
// ICON_PNG_DIR=<dir> also writes each 128x128 bitmap as a PNG.
// Output per path: alpha>8 pixel count and coloured (non-grey) count at 128x128.
// Build: clang -fobjc-arc -framework AppKit -framework UniformTypeIdentifiers icon_alpha_probe.m -o icon_alpha_probe
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

static void count(NSString *tag, NSString *what, NSImage *img)
{
    if (!img) { printf("%-4s %-40s nil\n", tag.UTF8String, what.UTF8String); return; }
    const int S = 128;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:S pixelsHigh:S
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:S * 4 bitsPerPixel:32];
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    [img drawInRect:NSMakeRect(0, 0, S, S) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1.0];
    [NSGraphicsContext restoreGraphicsState];
    const unsigned char *p = rep.bitmapData;
    int vis = 0, col = 0;
    for (int i = 0; i < S * S; i++, p += 4) {
        if (p[3] > 8) vis++;
        int mx = MAX(p[0], MAX(p[1], p[2])), mn = MIN(p[0], MIN(p[1], p[2]));
        if (p[3] > 8 && mx - mn > 24) col++;
    }
    const char *dir = getenv("ICON_PNG_DIR");
    if (dir) {
        NSString *f = [NSString stringWithFormat:@"%s/%@-%@.png", dir, tag, what.lastPathComponent.stringByDeletingPathExtension];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:f atomically:YES];
    }
    printf("%-4s %-40s reps=%lu vis=%d col=%d\n", tag.UTF8String, what.lastPathComponent.UTF8String,
           (unsigned long)img.representations.count, vis, col);
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        for (int i = 1; i < argc; i++) {
            NSString *path = [NSString stringWithUTF8String:argv[i]];
            count(@"ws", path, [[NSWorkspace sharedWorkspace] iconForFile:path]);
            NSBundle *b = [NSBundle bundleWithPath:path];
            NSString *icf = [b objectForInfoDictionaryKey:@"CFBundleIconFile"];
            NSString *icns = nil;
            if (icf) {
                icns = [b pathForResource:icf ofType:icf.pathExtension.length ? nil : @"icns"];
            }
            if (icns) count(@"icns", icns, [[NSImage alloc] initWithContentsOfFile:icns]);
            else printf("icns %-40s none\n", path.lastPathComponent.UTF8String);
        }
        count(@"car", @"UTTypeApplicationBundle",
              [[NSWorkspace sharedWorkspace] iconForContentType:UTTypeApplicationBundle]);
    }
    return 0;
}
