// Prints the pipeline reflection Metal hands back (arguments and bindings of
// each stage with every property), for a render or compute pipeline built
// from a metallib. Run on a Mac with Apple's driver for the reference and on
// the RTX for ours.
//   metal_refl_dump <lib> <vertex> <fragment>      render pipeline (BGRA8 target)
//   metal_refl_dump <lib> <kernel>                  compute pipeline
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>

static void show(id a, const char *sel) {
    SEL s = sel_registerName(sel);
    if (![a respondsToSelector:s]) return;
    const char *rt = [a methodSignatureForSelector:s].methodReturnType;
    if (rt[0] == '@') printf(" %s=%s", sel, [[((id (*)(id, SEL))objc_msgSend)(a, s) description] UTF8String]);
    else if (rt[0] == 'B' || rt[0] == 'c') printf(" %s=%d", sel, ((BOOL (*)(id, SEL))objc_msgSend)(a, s));
    else printf(" %s=%lu", sel, ((unsigned long (*)(id, SEL))objc_msgSend)(a, s));
}

static void dumpObj(id a, int depth) {
    printf("%*s%s:", depth * 2, "", class_getName([a class]));
    static const char *common[] = {"name", "type", "index", "access", "isActive", "isUsed", "isArgument", NULL};
    static const char *buf[] = {"arrayLength", "bufferAlignment", "bufferDataSize", "bufferDataType", NULL};
    static const char *tg[] = {"threadgroupMemoryAlignment", "threadgroupMemoryDataSize", NULL};
    static const char *tex[] = {"arrayLength", "textureType", "textureDataType", "isDepthTexture", NULL};
    for (const char **c = common; *c; c++) show(a, *c);
    const unsigned long t = ((unsigned long (*)(id, SEL))objc_msgSend)(a, @selector(type));
    const char **more = t == MTLBindingTypeBuffer ? buf : t == MTLBindingTypeThreadgroupMemory ? tg
                      : t == MTLBindingTypeTexture ? tex : NULL;
    for (const char **c = more; c && *c; c++) show(a, *c);
    printf("\n");
    SEL st = sel_registerName("bufferStructType");
    if (t == MTLBindingTypeBuffer && [a respondsToSelector:st]) {
        MTLStructType *ty = ((id (*)(id, SEL))objc_msgSend)(a, st);
        for (MTLStructMember *m in ty.members)
            printf("%*smember %s offset %lu type %lu\n", depth * 2 + 4, "", m.name.UTF8String, (unsigned long)m.offset,
                   (unsigned long)m.dataType);
    }
}

static void dumpList(const char *what, NSArray *l) {
    printf("%s (%lu):\n", what, (unsigned long)l.count);
    for (id a in l) dumpObj(a, 1);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) { fprintf(stderr, "usage: %s lib vertex fragment | lib kernel\n", argv[0]); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
        if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
        if (argc == 3) {
            MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
            cd.computeFunction = [lib newFunctionWithName:@(argv[2])];
            MTLComputePipelineReflection *r = nil;
            id ps = [dev newComputePipelineStateWithDescriptor:cd
                                                       options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
                                                    reflection:&r error:&e];
            printf("compute pipeline %s, reflection %s\n", ps ? "made" : "nil", r ? class_getName([r class]) : "nil");
            if ([r respondsToSelector:@selector(arguments)]) dumpList("arguments", ((id (*)(id, SEL))objc_msgSend)(r, @selector(arguments)));
            if ([r respondsToSelector:@selector(bindings)]) dumpList("bindings", r.bindings);
            return 0;
        }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@(argv[2])];
        rd.fragmentFunction = [lib newFunctionWithName:@(argv[3])];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        MTLRenderPipelineReflection *r = nil;
        id ps = [dev newRenderPipelineStateWithDescriptor:rd options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
                                               reflection:&r error:&e];
        printf("render pipeline %s (%s), reflection %s\n", ps ? "made" : "nil", e.localizedDescription.UTF8String ?: "",
               r ? class_getName([r class]) : "nil");
        if (!r) return 1;
        dumpList("vertexArguments", ((id (*)(id, SEL))objc_msgSend)(r, @selector(vertexArguments)));
        dumpList("fragmentArguments", ((id (*)(id, SEL))objc_msgSend)(r, @selector(fragmentArguments)));
        if ([r respondsToSelector:@selector(vertexBindings)]) dumpList("vertexBindings", r.vertexBindings);
        if ([r respondsToSelector:@selector(fragmentBindings)]) dumpList("fragmentBindings", r.fragmentBindings);
        return 0;
    }
}
