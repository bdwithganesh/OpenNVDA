// A foreign CPU range can be wrapped more than once, each at a different
// GPU VA. Keep that VA on the buffer that owns the mapping; a CPU-address
// search can select a different wrapper whose lifetime ends sooner.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdint.h>

typedef struct { uint64_t va, length; } NVBufferMapping;
static char kNVBufferMappingKey;

static inline void nvBufferSetMapping(id buffer, uint64_t va, uint64_t length) {
    const NVBufferMapping mapping = {va, length};
    objc_setAssociatedObject(buffer, &kNVBufferMappingKey,
        [NSValue valueWithBytes:&mapping objCType:@encode(NVBufferMapping)],
        OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static inline uint64_t nvBufferMappedVa(id<MTLBuffer> buffer, uint64_t offset, uint64_t length,
                                       uint64_t (*lookup)(const void *, uint64_t)) {
    NSValue *value = objc_getAssociatedObject(buffer, &kNVBufferMappingKey);
    if (value) {
        NVBufferMapping mapping;
        [value getValue:&mapping];
        if (offset > mapping.length || length > mapping.length - offset ||
            offset > UINT64_MAX - mapping.va) return 0;
        return mapping.va + offset;
    }
    if (offset > buffer.length || length > buffer.length - offset) return 0;
    const uint8_t *cpu = buffer.contents;
    return cpu ? lookup(cpu + offset, length) : 0;
}
