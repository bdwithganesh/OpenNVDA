#import "../../drivers/NVMTLDriver/NVMTLBufferVa.h"
#include <assert.h>
#include <stdio.h>

@interface TestBuffer : NSObject
@property void *contents;
@property NSUInteger length;
@end
@implementation TestBuffer
@end

static uint8_t pages[8192];
static uint64_t firstVa = 0x2c00000000ULL;
static unsigned lookups;
static uint64_t lookup(const void *cpu, uint64_t length) {
    lookups++;
    uintptr_t offset = (uintptr_t)cpu - (uintptr_t)pages;
    return offset <= sizeof pages && length <= sizeof pages - offset ? firstVa + offset : 0;
}
static uint64_t resolve(TestBuffer *b, uint64_t offset, uint64_t length) {
    return nvBufferMappedVa((id<MTLBuffer>)b, offset, length, lookup);
}
int main(void) {
    @autoreleasepool {
        TestBuffer *a = [TestBuffer new], *b = [TestBuffer new];
        a.contents = b.contents = pages;
        a.length = b.length = sizeof pages;
        // Reproduce the old lookup: two owners resolve to the first VA.
        assert(resolve(a, 0, 4096) == resolve(b, 0, 4096));
        nvBufferSetMapping(a, firstVa, sizeof pages);
        const uint64_t ownVa = firstVa + 0x10000;
        nvBufferSetMapping(b, ownVa, sizeof pages);
        lookups = 0;
        assert(resolve(a, 0, 4096) != resolve(b, 0, 4096));
        const uint64_t cachedTextureVa = resolve(b, 0, 4096);
        a = nil;
        firstVa = 0; // first mapping is gone, as after nvHeapUnwrap
        assert(resolve(b, 0, 4096) == cachedTextureVa);
        assert(cachedTextureVa == ownVa);
        TestBuffer *plane = [TestBuffer new];
        plane.contents = pages + 768; plane.length = sizeof pages - 768;
        nvBufferSetMapping(plane, ownVa + 768, plane.length);
        assert(resolve(plane, 32, 4096) == ownVa + 800);
        assert(resolve(b, sizeof pages - 1, 1) == ownVa + sizeof pages - 1);
        assert(resolve(b, sizeof pages - 1, 2) == 0);
        assert(resolve(b, UINT64_MAX, 2) == 0);
        assert(resolve(b, 1, UINT64_MAX) == 0);
        assert(lookups == 0); // owned ranges never borrow another mapping
        TestBuffer *heap = [TestBuffer new];
        heap.contents = pages; heap.length = sizeof pages;
        firstVa = ownVa + 0x10000;
        assert(resolve(heap, 64, 128) == firstVa + 64);
        assert(resolve(heap, sizeof pages, 1) == 0);
        puts("PASS: duplicate wraps, cached VA lifetime, plane offsets, bounds, heap fallback");
    }
}
