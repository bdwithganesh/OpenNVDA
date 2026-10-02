// See NVMTLCompiler.h. v1 subsets:
//
// compute: kernel void name(device [const] T *buf [[buffer(N)]], ...,
//          uint tid [[thread_position_in_grid]]) { body }
// vertex:  vertex float4 name(device [const] T *buf [[buffer(N)]], ...,
//          uint vid [[vertex_id]]) { body; return float4(...); }
//          T in {float,float2,float3,float4,int,...2/3/4,uint,...2/3/4}.
//          Implicit OUT float4 array + vstart appended to push constants.
// fragment: fragment float4 name(...) { ... return float4(r,g,b,a); }
//          with constant floats; stage_in params accepted and ignored.
//          Parsed, not compiled (fixed raster consumes the color).
#include <os/log.h>
#import "NVMTLLog.h"
#import "NVMTLCompiler.h"
#import "NVMTLTexHw.h"
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <os/lock.h>
#include <dlfcn.h>

@implementation NVMTLKernel
@end
@implementation NVMTLFragment {
    float _color[4];
}
- (void)nvSetColor:(const float *)c { memcpy(_color, c, sizeof _color); }
- (const float *)nvColor { return _color; }
@end

#define NAKC_PATH "/usr/local/libexec/nakc"
#define GLSLANG_PATH "/usr/local/libexec/glslangValidator"

// 0.6.2: nakc in-process. Sandboxed clients (WindowServer, apps) may not
// launch /usr/local/libexec/nakc; libnakc.dylib in our bundle's Resources
// is the same compiler with main() renamed nakc_main.
#include <fcntl.h>
static NSString *nvCacheDir(void);
typedef int (*NakcMainFn)(int, char **);
static NakcMainFn nvNakcInProcess(void) {
    static NakcMainFn fn;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (getenv("NVMTL_NAKC_EXEC")) return;
        Dl_info di;
        if (!dladdr((const void *)&nvNakcInProcess, &di) || !di.dli_fname) return;
        // .../NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver -> Contents/Resources/libnakc.dylib
        NSString *res = [[[@(di.dli_fname) stringByDeletingLastPathComponent] stringByDeletingLastPathComponent]
                         stringByAppendingPathComponent:@"Resources/libnakc.dylib"];
        void *h = dlopen(res.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        if (!h) { NSLog(@"NVMTLCompiler: in-process nakc unavailable: %s", dlerror()); return; }
        fn = (NakcMainFn)dlsym(h, "nakc_main");
    });
    return fn;
}

// the compiler's size/mtime go into cache keys: nakc, or the bundled library
// when a sandbox hides nakc
static BOOL nvCompilerStat(struct stat *st) {
    if (stat(NAKC_PATH, st) == 0) return YES;
    Dl_info di;
    if (!dladdr((const void *)&nvCompilerStat, &di) || !di.dli_fname) return NO;
    NSString *lib = [[[@(di.dli_fname) stringByDeletingLastPathComponent] stringByDeletingLastPathComponent]
                     stringByAppendingPathComponent:@"Resources/libnakc.dylib"];
    return stat(lib.fileSystemRepresentation, st) == 0;
}

static NSString *runTool(NSString *path, NSArray<NSString *> *args, int *statusOut) {
    NakcMainFn nakc = [path isEqual:@NAKC_PATH] ? nvNakcInProcess() : NULL;
    if (nakc) {
        static NSLock *lock;
        static dispatch_once_t once;
        dispatch_once(&once, ^{ lock = [NSLock new]; });
        char **argv = calloc(args.count + 2, sizeof(char *));
        argv[0] = strdup("nakc");
        for (NSUInteger i = 0; i < args.count; i++) argv[i + 1] = strdup(args[i].UTF8String);
        // 0.6.12: its stderr goes to a file for the call, so a failure in a
        // sandboxed process says why (the process's stderr is often /dev/null)
        NSString *errPath = [nvCacheDir() stringByAppendingFormat:@"/.nakc-err.%d", getpid()];
        [lock lock];
        fflush(stderr);
        const int saved = dup(2);
        const int ef = open(errPath.fileSystemRepresentation, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (ef >= 0) { dup2(ef, 2); close(ef); }
        *statusOut = nakc((int)args.count + 1, argv);
        fflush(stderr);
        if (saved >= 0) { dup2(saved, 2); close(saved); }
        [lock unlock];
        for (NSUInteger i = 0; i <= args.count; i++) free(argv[i]);
        free(argv);
        NSString *msg = *statusOut ? [NSString stringWithContentsOfFile:errPath encoding:NSUTF8StringEncoding error:nil] : nil;
        unlink(errPath.fileSystemRepresentation);
        if (msg.length > 600) msg = [msg substringFromIndex:msg.length - 600];
        return *statusOut ? [NSString stringWithFormat:@"(in-process nakc failed: %@)", msg ?: @"no output"] : @"";
    }
    NSTask *t = [NSTask new];
    t.launchPath = path; t.arguments = args;
    NSPipe *p = [NSPipe pipe]; t.standardOutput = p; t.standardError = p;
    @try { [t launch]; } @catch (NSException *e) { *statusOut = -1; return e.reason; }
    [t waitUntilExit];
    *statusOut = t.terminationStatus;
    NSData *d = [p.fileHandleForReading readDataToEndOfFile];
    if (!d) d = [NSData data];
    NSString *s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
    return s ? s : @"(tool output undecodable)";
}

// Find top-level (depth-0) comma splits.
static NSArray<NSString *> *splitTop(NSString *s) {
    NSMutableArray *out = [NSMutableArray new];
    int depth = 0; NSUInteger start = 0;
    for (NSUInteger i = 0; i < s.length; i++) {
        unichar c = [s characterAtIndex:i];
        if (c == '<' || c == '(' || c == '[') depth++;
        else if (c == '>' || c == ')' || c == ']') depth--;
        else if (c == ',' && depth == 0) {
            [out addObject:[s substringWithRange:NSMakeRange(start, i - start)]];
            start = i + 1;
        }
    }
    [out addObject:[s substringWithRange:NSMakeRange(start, s.length - start)]];
    return out;
}

// Match the closing brace of the '{' at openIdx. NSNotFound if bad.
static NSUInteger matchBrace(NSString *s, NSUInteger openIdx) {
    int depth = 0;
    for (NSUInteger i = openIdx; i < s.length; i++) {
        unichar c = [s characterAtIndex:i];
        if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) return i; }
    }
    return NSNotFound;
}

// Word-boundary replace of identifier `from` with `to`.
static NSString *subIdent(NSString *s, NSString *from, NSString *to) {
    NSCharacterSet *idc = [NSCharacterSet characterSetWithCharactersInString:
                           @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_"];
    NSMutableString *o = [NSMutableString new];
    NSUInteger i = 0, n = s.length;
    while (i < n) {
        if ([s rangeOfString:from options:0 range:NSMakeRange(i, n - i)].location == i) {
            BOOL lb = i == 0 || ![idc characterIsMember:[s characterAtIndex:i - 1]];
            NSUInteger e = i + from.length;
            BOOL rb = e >= n || ![idc characterIsMember:[s characterAtIndex:e]];
            if (lb && rb) { [o appendString:to]; i = e; continue; }
        }
        [o appendFormat:@"%C", [s characterAtIndex:i++]];
    }
    return o;
}

static NSMutableString *stripComments(NSString *source) {
    NSMutableString *src = [source mutableCopy];
    NSRegularExpression *lc = [NSRegularExpression regularExpressionWithPattern:@"//[^\\n]*" options:0 error:nil];
    [lc replaceMatchesInString:src options:0 range:NSMakeRange(0, src.length) withTemplate:@""];
    NSRegularExpression *bc = [NSRegularExpression regularExpressionWithPattern:@"/\\*.*?\\*/"
        options:NSRegularExpressionDotMatchesLineSeparators error:nil];
    [bc replaceMatchesInString:src options:0 range:NSMakeRange(0, src.length) withTemplate:@""];
    return src;
}

// Metal scalar/vector type -> {glsl, wrapper}.
static NSArray *typeInfo(NSString *t) {
    static NSDictionary *m;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        m = @{@"float": @[@"float", @"FBuf"], @"float2": @[@"vec2", @"V2Buf"],
              @"float3": @[@"vec3", @"V3Buf"], @"float4": @[@"vec4", @"V4Buf"],
              @"int": @[@"int", @"IBuf"], @"int2": @[@"ivec2", @"I2Buf"],
              @"int3": @[@"ivec3", @"I3Buf"], @"int4": @[@"ivec4", @"I4Buf"],
              @"uint": @[@"uint", @"UBuf"], @"uint2": @[@"uvec2", @"U2Buf"],
              @"uint3": @[@"uvec3", @"U3Buf"], @"uint4": @[@"uvec4", @"U4Buf"]};
    });
    return m[t];
}

// Compiled kernels are kept in /var/tmp/nvmtl-cache, keyed by the GLSL text
// plus the nakc binary's size and mtime, so a compiler update drops them.
// glslang + nakc cost ~200 ms per kernel, which every new process paid before.
// 0.6.2: /var/tmp/nvmtl-cache, or the process's own temp dir when that is
// not writable (sandboxed clients: WindowServer, apps)
static NSString *nvCacheDir(void) {
    static NSString *dir;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // 0.6.12: access() only looks at the mode bits; a sandbox (the
        // WebKit GPU process) may still refuse the write. Try one for real.
        BOOL (^writable)(NSString *) = ^BOOL(NSString *d) {
            NSString *probe = [d stringByAppendingFormat:@"/.probe.%d", getpid()];
            const int fd = open(probe.fileSystemRepresentation, O_CREAT | O_WRONLY | O_TRUNC, 0600);
            if (fd < 0) return NO;
            close(fd);
            unlink(probe.fileSystemRepresentation);
            return YES;
        };
        const char *shared = "/var/tmp/nvmtl-cache";
        if (mkdir(shared, 01777) == 0) chmod(shared, 01777);
        if (writable(@(shared))) { dir = @(shared); return; }
        dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"nvmtl-cache"];
        mkdir(dir.fileSystemRepresentation, 0700);
        if (!writable(dir)) NSLog(@"NVMTLCompiler: no writable cache directory (tried %s, %@)", shared, dir);
    });
    return dir;
}
#define NVMTL_CACHE_DIR_NS nvCacheDir()
static NSString *cachePath(NSString *glsl) {
    struct stat nst;
    if (!nvCompilerStat(&nst)) return nil;
    NSString *keySrc = [NSString stringWithFormat:@"%lld.%ld|%@", (long long)nst.st_size,
                        (long)nst.st_mtimespec.tv_sec, glsl];
    NSData *d = [keySrc dataUsingEncoding:NSUTF8StringEncoding];
    unsigned char h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    NSMutableString *hex = [NSMutableString stringWithCapacity:32];
    for (int i = 0; i < 16; i++) [hex appendFormat:@"%02x", h[i]];
    return [NSString stringWithFormat:@"%@/%@.nak", nvCacheDir(), hex];
}

static NSData *compileNak(NSString *name, NSString *glsl) {
    NSString *cp = cachePath(glsl);
    if (cp) {
        NSData *hit = [NSData dataWithContentsOfFile:cp];
        if (hit.length >= 64 && *(const uint32_t *)hit.bytes == 0x314b414e) return hit;
    }
    char tmp[1024];   // 0.6.12: next to the cache, /tmp is closed to some sandboxes
    snprintf(tmp, sizeof tmp, "%s/tmp.XXXXXX", nvCacheDir().fileSystemRepresentation);
    if (!mkdtemp(tmp)) return nil;
    NSString *base = [@(tmp) stringByAppendingPathComponent:name];
    NSError *e = nil;
    if (![glsl writeToFile:[base stringByAppendingString:@".comp"] atomically:YES
                  encoding:NSUTF8StringEncoding error:&e]) {
        NSLog(@"NVMTLCompiler: %@ write failed %@", name, e); return nil;
    }
    int st = 0;
    NSString *o1 = runTool(@GLSLANG_PATH, @[@"-V", [base stringByAppendingString:@".comp"],
                                           @"-o", [base stringByAppendingString:@".spv"]], &st);
    if (st) { NSLog(@"NVMTLCompiler: %@ glslang failed %@", name, o1); return nil; }
    NSString *o2 = runTool(@NAKC_PATH, @[[base stringByAppendingString:@".spv"],
                                        [base stringByAppendingString:@".nak"]], &st);
    if (st) { NSLog(@"NVMTLCompiler: %@ nakc failed %@", name, o2); return nil; }
    NSData *nak = [NSData dataWithContentsOfFile:[base stringByAppendingString:@".nak"]];
    if (cp && nak.length >= 64) {
        (void)nvCacheDir();
        [nak writeToFile:cp atomically:YES];
    }
    return nak;
}

NVMTLKernel *nvCompileGLSL(NSString *name, NSString *glsl) {
    NSData *nak = compileNak(name, glsl);
    if (!nak) return nil;
    if (nak.length < 64 || *(const uint32_t *)nak.bytes != 0x314b414e /*NAK1*/) {
        NSLog(@"NVMTLCompiler: %@ bad .nak", name); return nil;
    }
    const uint32_t *h = nak.bytes;
    NVMTLKernel *k = [NVMTLKernel new];
    k.name = name; k.regs = h[2]; k.slm = h[3]; k.smem = h[4]; k.barriers = h[8];
    k.code = [nak subdataWithRange:NSMakeRange(64, h[1])];
    NSLog(@"NVMTLCompiler: %@ %u bytes %u gprs", name, h[1], h[2]);
    return k;
}

// GLSL for texture2d access on linear (pitch) textures. Format codes are the
// ones NVMTLDevice.m hands over in pc.tF*: 1 R8Unorm, 2 RG8Unorm, 3 RGBA8Unorm,
// 4 BGRA8Unorm, 5 R16Float, 6 RG16Float, 7 RGBA16Float, 8 R32Float, 9 RG32Float,
// 10 RGBA32Float, 11 R32Uint, 12 RGBA8Uint, 13 RGBA32Uint, 14 R8Uint.
// Texels are addressed through a 32-bit word view; 1- and 2-byte formats are
// written with atomics so neighbouring threads can't lose each other's bytes.
// Sampler mode (pc.sM*): bit0 linear, bits 2:1 address (0 clamp to edge,
// 1 repeat, 2 mirrored repeat, 3 clamp to zero), bit3 pixel coordinates.
static NSString *nvTextureHelpers(void) {
    return @
    "layout(buffer_reference, std430, buffer_reference_align = 4) buffer NvW { uint w[]; };\n"
    "uint nvBpp(uint f) {\n"
    "  if (f == 1u || f == 14u) return 1u;\n"
    "  if (f == 2u || f == 5u) return 2u;\n"
    "  if (f == 7u || f == 9u) return 8u;\n"
    "  if (f == 10u || f == 13u) return 16u;\n"
    "  return 4u;\n"
    "}\n"
    "uint nvLd(uint64_t a) { return NvW(a & ~uint64_t(3)).w[0]; }\n"
    "uint64_t nvAddr(uint64_t base, uint p, uint f, uvec2 c) { return base + uint64_t(c.y) * p + uint64_t(c.x * nvBpp(f)); }\n"
    "uint nvSub(uint64_t a, uint bits) { return (nvLd(a) >> (uint(a & 3) * 8u)) & ((1u << bits) - 1u); }\n"
    "vec4 nvRead(uint64_t base, uint p, uint f, uvec2 c) {\n"
    "  uint64_t a = nvAddr(base, p, f, c);\n"
    "  if (f == 1u) return vec4(float(nvSub(a, 8u)) / 255.0, 0, 0, 1);\n"
    "  if (f == 2u) { vec4 v = unpackUnorm4x8(nvSub(a, 16u)); return vec4(v.xy, 0, 1); }\n"
    "  if (f == 3u) return unpackUnorm4x8(nvLd(a));\n"
    "  if (f == 4u) return unpackUnorm4x8(nvLd(a)).zyxw;\n"
    "  if (f == 5u) return vec4(unpackHalf2x16(nvSub(a, 16u)).x, 0, 0, 1);\n"
    "  if (f == 6u) return vec4(unpackHalf2x16(nvLd(a)), 0, 1);\n"
    "  if (f == 7u) return vec4(unpackHalf2x16(nvLd(a)), unpackHalf2x16(nvLd(a + 4)));\n"
    "  if (f == 8u) return vec4(uintBitsToFloat(nvLd(a)), 0, 0, 1);\n"
    "  if (f == 9u) return vec4(uintBitsToFloat(nvLd(a)), uintBitsToFloat(nvLd(a + 4)), 0, 1);\n"
    "  if (f == 10u) return vec4(uintBitsToFloat(nvLd(a)), uintBitsToFloat(nvLd(a + 4)),\n"
    "                            uintBitsToFloat(nvLd(a + 8)), uintBitsToFloat(nvLd(a + 12)));\n"
    "  return vec4(0);\n"
    "}\n"
    "uvec4 nvReadU(uint64_t base, uint p, uint f, uvec2 c) {\n"
    "  uint64_t a = nvAddr(base, p, f, c);\n"
    "  if (f == 11u) return uvec4(nvLd(a), 0, 0, 1);\n"
    "  if (f == 12u) { uint v = nvLd(a); return uvec4(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24); }\n"
    "  if (f == 13u) return uvec4(nvLd(a), nvLd(a + 4), nvLd(a + 8), nvLd(a + 12));\n"
    "  if (f == 14u) return uvec4(nvSub(a, 8u), 0, 0, 1);\n"
    "  return uvec4(0);\n"
    "}\n"
    "void nvSt(uint64_t a, uint v) { NvW(a & ~uint64_t(3)).w[0] = v; }\n"
    "void nvStSub(uint64_t a, uint v, uint bits) {\n"
    "  uint sh = uint(a & 3) * 8u, m = ((1u << bits) - 1u) << sh;\n"
    "  NvW w = NvW(a & ~uint64_t(3));\n"
    "  atomicAnd(w.w[0], ~m);\n"
    "  atomicOr(w.w[0], (v << sh) & m);\n"
    "}\n"
    "void nvWrite(uint64_t base, uint p, uint f, vec4 v, uvec2 c) {\n"
    "  uint64_t a = nvAddr(base, p, f, c);\n"
    "  if (f == 1u) nvStSub(a, uint(clamp(v.x, 0.0, 1.0) * 255.0 + 0.5), 8u);\n"
    "  else if (f == 2u) nvStSub(a, packUnorm4x8(vec4(v.xy, 0, 0)) & 0xffffu, 16u);\n"
    "  else if (f == 3u) nvSt(a, packUnorm4x8(v));\n"
    "  else if (f == 4u) nvSt(a, packUnorm4x8(v.zyxw));\n"
    "  else if (f == 5u) nvStSub(a, packHalf2x16(vec2(v.x, 0)) & 0xffffu, 16u);\n"
    "  else if (f == 6u) nvSt(a, packHalf2x16(v.xy));\n"
    "  else if (f == 7u) { nvSt(a, packHalf2x16(v.xy)); nvSt(a + 4, packHalf2x16(v.zw)); }\n"
    "  else if (f == 8u) nvSt(a, floatBitsToUint(v.x));\n"
    "  else if (f == 9u) { nvSt(a, floatBitsToUint(v.x)); nvSt(a + 4, floatBitsToUint(v.y)); }\n"
    "  else if (f == 10u) { nvSt(a, floatBitsToUint(v.x)); nvSt(a + 4, floatBitsToUint(v.y));\n"
    "                       nvSt(a + 8, floatBitsToUint(v.z)); nvSt(a + 12, floatBitsToUint(v.w)); }\n"
    "}\n"
    "void nvWriteU(uint64_t base, uint p, uint f, uvec4 v, uvec2 c) {\n"
    "  uint64_t a = nvAddr(base, p, f, c);\n"
    "  if (f == 11u) nvSt(a, v.x);\n"
    "  else if (f == 12u) nvSt(a, (v.x & 255u) | (v.y & 255u) << 8 | (v.z & 255u) << 16 | (v.w & 255u) << 24);\n"
    "  else if (f == 13u) { nvSt(a, v.x); nvSt(a + 4, v.y); nvSt(a + 8, v.z); nvSt(a + 12, v.w); }\n"
    "  else if (f == 14u) nvStSub(a, v.x & 255u, 8u);\n"
    "}\n"
    "int nvWrap(int i, int n, uint am) {\n"
    "  if (am == 1u) return ((i % n) + n) % n;\n"
    "  if (am == 2u) { int m = ((i % (2 * n)) + 2 * n) % (2 * n); return m < n ? m : 2 * n - 1 - m; }\n"
    "  return clamp(i, 0, n - 1);\n"
    "}\n"
    "vec4 nvTexel(uint64_t base, uint p, uint f, uint w, uint h, ivec2 c, uint mode) {\n"
    "  uint am = (mode >> 1) & 3u;\n"
    "  if (am == 3u && (c.x < 0 || c.y < 0 || c.x >= int(w) || c.y >= int(h))) return vec4(0);\n"
    "  return nvRead(base, p, f, uvec2(nvWrap(c.x, int(w), am), nvWrap(c.y, int(h), am)));\n"
    "}\n"
    "vec4 nvSample(uint64_t base, uint p, uint f, uint w, uint h, uint mode, vec2 uv) {\n"
    "  vec2 px = (mode & 8u) != 0u ? uv : uv * vec2(float(w), float(h));\n"
    "  if ((mode & 1u) == 0u) return nvTexel(base, p, f, w, h, ivec2(floor(px)), mode);\n"
    "  px -= 0.5;\n"
    "  ivec2 i0 = ivec2(floor(px));\n"
    "  vec2 fr = px - vec2(i0);\n"
    "  vec4 t00 = nvTexel(base, p, f, w, h, i0, mode), t10 = nvTexel(base, p, f, w, h, i0 + ivec2(1, 0), mode);\n"
    "  vec4 t01 = nvTexel(base, p, f, w, h, i0 + ivec2(0, 1), mode), t11 = nvTexel(base, p, f, w, h, i0 + ivec2(1, 1), mode);\n"
    "  return mix(mix(t00, t10, fr.x), mix(t01, t11, fr.x), fr.y);\n"
    "}\n";
}

// struct Name { T field [[attr]]; ... };  ->  name -> @[{type, name, pos}]
// Only float/float2/float3/float4 fields (what vertex outputs carry).
static NSDictionary *parseStructs(NSString *src) {
    NSMutableDictionary *out = [NSMutableDictionary new];
    NSRegularExpression *srx = [NSRegularExpression regularExpressionWithPattern:
        @"struct\\s+(\\w+)\\s*\\{([^}]*)\\}\\s*;" options:0 error:nil];
    NSRegularExpression *frx = [NSRegularExpression regularExpressionWithPattern:
        @"^\\s*(float4|float3|float2|float|half4|half3|half2|half)\\s+(\\w+)\\s*(\\[\\[[^\\]]*\\]\\])?\\s*$"
        options:0 error:nil];
    for (NSTextCheckingResult *m in [srx matchesInString:src options:0 range:NSMakeRange(0, src.length)]) {
        NSString *name = [src substringWithRange:[m rangeAtIndex:1]];
        NSString *body = [src substringWithRange:[m rangeAtIndex:2]];
        NSMutableArray *fields = [NSMutableArray new];
        BOOL ok = YES;
        for (NSString *decl in [body componentsSeparatedByString:@";"]) {
            NSString *d = [decl stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
            if (!d.length) continue;
            NSTextCheckingResult *fm = [frx firstMatchInString:d options:0 range:NSMakeRange(0, d.length)];
            if (!fm) { ok = NO; break; }
            NSString *attr = [fm rangeAtIndex:3].location != NSNotFound ? [d substringWithRange:[fm rangeAtIndex:3]] : @"";
            [fields addObject:@{@"type": [d substringWithRange:[fm rangeAtIndex:1]],
                                @"name": [d substringWithRange:[fm rangeAtIndex:2]],
                                @"pos": @([attr containsString:@"position"])}];
        }
        if (ok && fields.count) out[name] = fields;
    }
    return out;
}

static NSString *structDecl(NSString *name, NSArray *fields) {
    NSMutableString *g = [NSMutableString stringWithFormat:@"struct %@ {", name];
    for (NSDictionary *f in fields) [g appendFormat:@" %@ %@;", f[@"type"], f[@"name"]];
    [g appendString:@" };\n"];
    return g;
}

static NSInteger posSlotOf(NSArray *fields) {
    for (NSUInteger i = 0; i < fields.count; i++) if ([fields[i][@"pos"] boolValue]) return (NSInteger)i;
    return -1;
}

// field value padded to a vec4 slot, and back
static NSString *padVec4(NSString *type, NSString *expr) {
    if ([type hasSuffix:@"4"]) return expr;
    if ([type hasSuffix:@"3"]) return [NSString stringWithFormat:@"vec4(%@, 0.0)", expr];
    if ([type hasSuffix:@"2"]) return [NSString stringWithFormat:@"vec4(%@, 0.0, 0.0)", expr];
    return [NSString stringWithFormat:@"vec4(%@, 0.0, 0.0, 0.0)", expr];
}
static NSString *unpadVec4(NSString *type, NSString *expr) {
    if ([type hasSuffix:@"4"]) return expr;
    if ([type hasSuffix:@"3"]) return [NSString stringWithFormat:@"(%@).xyz", expr];
    if ([type hasSuffix:@"2"]) return [NSString stringWithFormat:@"(%@).xy", expr];
    return [NSString stringWithFormat:@"(%@).x", expr];
}

// Metal vector constructors -> GLSL.
static NSString *glslCtors(NSString *tb) {
    for (NSArray *pair in (@[@[@"float4", @"vec4"], @[@"float3", @"vec3"], @[@"float2", @"vec2"],
                             @[@"half4", @"vec4"], @[@"half3", @"vec3"], @[@"half2", @"vec2"],
                             @[@"int4", @"ivec4"], @[@"int3", @"ivec3"], @[@"int2", @"ivec2"],
                             @[@"uint4", @"uvec4"], @[@"uint3", @"uvec3"], @[@"uint2", @"uvec2"]])) {
        NSRegularExpression *crx = [NSRegularExpression regularExpressionWithPattern:
            [NSString stringWithFormat:@"\\b%@\\s*\\(", pair[0]] options:0 error:nil];
        tb = [crx stringByReplacingMatchesInString:tb options:0
                range:NSMakeRange(0, tb.length) withTemplate:[pair[1] stringByAppendingString:@"("]];
    }
    return tb;
}

// texture methods -> helpers; sampler names -> their push mode
static NSString *translateTextures(NSString *tb, NSDictionary *texByIdx, NSDictionary *sampByIdx,
                                   NSUInteger nTex, NSUInteger nSamp) {
    for (NSUInteger i = 0; i < nTex; i++) {
        NSDictionary *x = texByIdx[@(i)];
        if (!x) continue;
        const BOOL isU = [x[@"elem"] isEqual:@"uint"] || [x[@"elem"] isEqual:@"int"];
        NSString *nm = [NSRegularExpression escapedPatternForString:x[@"name"]];
        NSString *args = [NSString stringWithFormat:@"pc.tA%lu, pc.tP%lu, pc.tF%lu, ",
                          (unsigned long)i, (unsigned long)i, (unsigned long)i];
        NSString *dims = [NSString stringWithFormat:@"pc.tW%lu, pc.tH%lu, ",
                          (unsigned long)i, (unsigned long)i];
        NSArray *rw = @[
            @[@"read", [NSString stringWithFormat:@"%@(%@", isU ? @"nvReadU" : @"nvRead", args]],
            @[@"write", [NSString stringWithFormat:@"%@(%@", isU ? @"nvWriteU" : @"nvWrite", args]],
            @[@"sample", [NSString stringWithFormat:@"nvSample(%@%@", args, dims]],
            @[@"get_width", [NSString stringWithFormat:@"(pc.tW%lu + 0u*(", (unsigned long)i]],
            @[@"get_height", [NSString stringWithFormat:@"(pc.tH%lu + 0u*(", (unsigned long)i]],
        ];
        for (NSArray *pair in rw) {
            NSRegularExpression *mrx = [NSRegularExpression regularExpressionWithPattern:
                [NSString stringWithFormat:@"\\b%@\\s*\\.\\s*%@\\s*\\(", nm, pair[0]] options:0 error:nil];
            NSString *tmpl = [NSRegularExpression escapedTemplateForString:pair[1]];
            tb = [mrx stringByReplacingMatchesInString:tb options:0 range:NSMakeRange(0, tb.length) withTemplate:tmpl];
        }
    }
    // get_width()/get_height() became "(pc.tWi + 0u*()" - close the extra paren.
    tb = [tb stringByReplacingOccurrencesOfString:@"0u*()" withString:@"0u)"];
    for (NSUInteger i = 0; i < nSamp; i++) {
        NSDictionary *x = sampByIdx[@(i)];
        if (x) tb = subIdent(tb, x[@"name"], [NSString stringWithFormat:@"pc.sM%lu", (unsigned long)i]);
    }
    return tb;
}

NSDictionary<NSString *, NVMTLKernel *> *nvCompileKernels(NSString *source) {
    NSMutableDictionary *kernels = [NSMutableDictionary new];
    NSMutableString *src = stripComments(source);
    NSRegularExpression *krx = [NSRegularExpression regularExpressionWithPattern:
        @"(kernel|vertex)\\s+(\\w+)\\s+(\\w+)\\s*\\(" options:0 error:nil];
    NSArray *km = [krx matchesInString:src options:0 range:NSMakeRange(0, src.length)];
    if (!km.count) return kernels;
    NSDictionary *structs = parseStructs(src);
    for (NSTextCheckingResult *m in km) {
        NSString *kind = [src substringWithRange:[m rangeAtIndex:1]];
        NSString *ret = [src substringWithRange:[m rangeAtIndex:2]];
        NSString *name = [src substringWithRange:[m rangeAtIndex:3]];
        const BOOL isVertex = [kind isEqual:@"vertex"];
        NSArray *vfields = isVertex ? structs[ret] : nil;
        if (vfields && posSlotOf(vfields) < 0) vfields = nil;
        if ((!isVertex && ![ret isEqual:@"void"]) || (isVertex && ![ret isEqual:@"float4"] && !vfields)) {
            NSLog(@"NVMTLCompiler: %@ bad return %@", name, ret); continue;
        }
        NSUInteger pp = NSMaxRange(m.range) - 1, depth = 0, pe = NSNotFound;
        for (NSUInteger i = pp; i < src.length; i++) {
            unichar c = [src characterAtIndex:i];
            if (c == '(') depth++;
            else if (c == ')') { if (--depth == 0) { pe = i; break; } }
        }
        if (pe == NSNotFound) { NSLog(@"NVMTLCompiler: %@ bad params", name); continue; }
        NSString *params = [src substringWithRange:NSMakeRange(pp + 1, pe - pp - 1)];
        NSUInteger bo = [src rangeOfString:@"{" options:0 range:NSMakeRange(pe, src.length - pe)].location;
        if (bo == NSNotFound) { NSLog(@"NVMTLCompiler: %@ no body", name); continue; }
        NSUInteger be = matchBrace(src, bo);
        if (be == NSNotFound) { NSLog(@"NVMTLCompiler: %@ unbalanced body", name); continue; }
        NSString *body = [src substringWithRange:NSMakeRange(bo + 1, be - bo - 1)];
        NSMutableArray *bufs = [NSMutableArray new];
        NSMutableArray *texs = [NSMutableArray new], *samps = [NSMutableArray new];
        NSString *tid = nil, *tidType = @"uint", *iidName = nil;
        BOOL bad = NO;
        for (NSString *p in splitTop(params)) {
            NSString *t = [p stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
            if (!t.length) continue;
            NSRegularExpression *brx = [NSRegularExpression regularExpressionWithPattern:
                @"^(?:device|constant)\\s+(?:const\\s+)?(float4|float3|float2|float|int4|int3|int2|int|uint4|uint3|uint2|uint)\\s*(\\*|&)\\s*(\\w+)\\s*\\[\\[buffer\\((\\d+)\\)\\]\\]$"
                options:0 error:nil];
            NSTextCheckingResult *bm = [brx firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
            if (bm) {
                [bufs addObject:@{@"type": [t substringWithRange:[bm rangeAtIndex:1]],
                                  @"name": [t substringWithRange:[bm rangeAtIndex:3]],
                                  @"idx": @([[t substringWithRange:[bm rangeAtIndex:4]] intValue])}];
                continue;
            }
            // texture2d<float|half|int|uint[, access::X]> name [[texture(N)]]
            NSRegularExpression *xrx = [NSRegularExpression regularExpressionWithPattern:
                @"^texture2d\\s*<\\s*(float|half|int|uint)\\s*(?:,\\s*access::(read|write|read_write|sample))?\\s*>\\s*(\\w+)\\s*\\[\\[texture\\((\\d+)\\)\\]\\]$"
                options:0 error:nil];
            NSTextCheckingResult *xm = isVertex ? nil : [xrx firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
            if (xm) {
                [texs addObject:@{@"elem": [t substringWithRange:[xm rangeAtIndex:1]],
                                  @"name": [t substringWithRange:[xm rangeAtIndex:3]],
                                  @"idx": @([[t substringWithRange:[xm rangeAtIndex:4]] intValue])}];
                continue;
            }
            NSRegularExpression *srx = [NSRegularExpression regularExpressionWithPattern:
                @"^sampler\\s+(\\w+)\\s*\\[\\[sampler\\((\\d+)\\)\\]\\]$" options:0 error:nil];
            NSTextCheckingResult *sm = isVertex ? nil : [srx firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
            if (sm) {
                [samps addObject:@{@"name": [t substringWithRange:[sm rangeAtIndex:1]],
                                   @"idx": @([[t substringWithRange:[sm rangeAtIndex:2]] intValue])}];
                continue;
            }
            NSRegularExpression *irx = [NSRegularExpression regularExpressionWithPattern:
                @"^uint\\s+(\\w+)\\s*\\[\\[instance_id\\]\\]$" options:0 error:nil];
            NSTextCheckingResult *im = isVertex ? [irx firstMatchInString:t options:0 range:NSMakeRange(0, t.length)] : nil;
            if (im) { iidName = [t substringWithRange:[im rangeAtIndex:1]]; continue; }
            NSRegularExpression *trx = [NSRegularExpression regularExpressionWithPattern:
                (isVertex ? @"^uint\\s+(\\w+)\\s*\\[\\[vertex_id\\]\\]$"
                          : @"^(uint|uint2|uint3)\\s+(\\w+)\\s*\\[\\[thread_position_in_grid\\]\\]$")
                options:0 error:nil];
            NSTextCheckingResult *tm = [trx firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
            if (tm && !tid) {
                tid = [t substringWithRange:[tm rangeAtIndex:isVertex ? 1 : 2]];
                if (!isVertex) tidType = [t substringWithRange:[tm rangeAtIndex:1]];
                continue;
            }
            NSLog(@"NVMTLCompiler: %@ unsupported param '%@'", name, t); bad = YES; break;
        }
        if (bad || !tid) { if (!tid) NSLog(@"NVMTLCompiler: %@ no thread id", name); continue; }
        NSMutableDictionary *byIdx = [NSMutableDictionary new];
        NSUInteger maxIdx = 0;
        for (NSDictionary *b in bufs) {
            NSUInteger ix = [b[@"idx"] unsignedIntegerValue];
            if (ix >= 16 || !typeInfo(b[@"type"])) {
                NSLog(@"NVMTLCompiler: %@ bad buffer %@", name, b); bad = YES; break;
            }
            byIdx[@(ix)] = b; if (ix > maxIdx) maxIdx = ix;
        }
        if (bad) continue;
        const NSUInteger nPush = bufs.count ? maxIdx + 1 : 0;
        // Textures / samplers are dense by index like the buffers.
        NSMutableDictionary *texByIdx = [NSMutableDictionary new], *sampByIdx = [NSMutableDictionary new];
        NSUInteger nTex = 0, nSamp = 0;
        for (NSDictionary *x in texs) {
            const NSUInteger ix = [x[@"idx"] unsignedIntegerValue];
            if (ix >= 16) { bad = YES; break; }
            texByIdx[@(ix)] = x; if (ix + 1 > nTex) nTex = ix + 1;
        }
        for (NSDictionary *x in samps) {
            const NSUInteger ix = [x[@"idx"] unsignedIntegerValue];
            if (ix >= 16) { bad = YES; break; }
            sampByIdx[@(ix)] = x; if (ix + 1 > nSamp) nSamp = ix + 1;
        }
        if (bad) { NSLog(@"NVMTLCompiler: %@ texture/sampler index out of range", name); continue; }
        NSMutableString *g = [NSMutableString new];
        [g appendString:@"#version 460\n"
         @"#extension GL_EXT_buffer_reference : require\n"
         @"#extension GL_EXT_scalar_block_layout : require\n"
         @"#extension GL_EXT_shader_explicit_arithmetic_types : require\n"
         @"#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n"
         @"#define half float\n#define half2 vec2\n#define half3 vec3\n#define half4 vec4\n"
         @"#define uint2 uvec2\n#define uint3 uvec3\n#define uint4 uvec4\n"
         @"#define int2 ivec2\n#define int3 ivec3\n#define int4 ivec4\n"
         @"layout(push_constant, scalar) uniform PC {\n"];
        // Layout (dwords): buffers 2 each, texture addresses 2 each, then per
        // texture {pitch, width, height, format}, samplers 1 each, and for
        // compute kernels grid[3] + block[3]. The runtime fills the same order.
        for (NSUInteger i = 0; i < nPush; i++)
            [g appendFormat:@"uint64_t b%lu;\n", (unsigned long)i];
        for (NSUInteger i = 0; i < nTex; i++)
            [g appendFormat:@"uint64_t tA%lu;\n", (unsigned long)i];
        for (NSUInteger i = 0; i < nTex; i++)
            [g appendFormat:@"uint tP%lu; uint tW%lu; uint tH%lu; uint tF%lu;\n",
                (unsigned long)i, (unsigned long)i, (unsigned long)i, (unsigned long)i];
        for (NSUInteger i = 0; i < nSamp; i++)
            [g appendFormat:@"uint sM%lu;\n", (unsigned long)i];
        if (isVertex) [g appendString:@"uint64_t out_;\nuint vstart;\nuint vcount;\nuint iid;\n"];
        else [g appendString:@"uint gX; uint gY; uint gZ; uint kX; uint kY; uint kZ;\n"];
        [g appendString:@"} pc;\n"];
        if (nTex) [g appendString:nvTextureHelpers()];
        NSMutableSet *doneTy = [NSMutableSet new];
        for (NSUInteger i = 0; i < nPush; i++) {
            NSDictionary *b = byIdx[@(i)];
            if (!b || [doneTy containsObject:b[@"type"]]) continue;
            [doneTy addObject:b[@"type"]];
            NSArray *ti = typeInfo(b[@"type"]);
            [g appendFormat:@"layout(buffer_reference, scalar) buffer %@ { %@ d[]; };\n", ti[1], ti[0]];
        }
        if (isVertex && ![doneTy containsObject:@"float4"])
            [g appendString:@"layout(buffer_reference, scalar) buffer V4Buf { vec4 d[]; };\n"];
        if (vfields) [g appendString:structDecl(ret, vfields)];
        [g appendString:@"layout(local_size_x=256) in;\nvoid main() {\n"];
        for (NSUInteger i = 0; i < nPush; i++) {
            NSDictionary *b = byIdx[@(i)];
            if (!b) continue;
            NSArray *ti = typeInfo(b[@"type"]);
            [g appendFormat:@"%@ B%lu = %@(pc.b%lu);\n", ti[1], (unsigned long)i, ti[1], (unsigned long)i];
        }
        NSString *tb = body;
        for (NSUInteger i = 0; i < nPush; i++) {
            NSDictionary *b = byIdx[@(i)];
            if (!b) continue;
            tb = subIdent(tb, b[@"name"], [NSString stringWithFormat:@"B%lu.d", (unsigned long)i]);
        }
        tb = glslCtors(tb);
        if (isVertex) {
            // 0.3.3: the launch rounds up to 256; threads past the draw must not read
            // vertex buffers (they end in their own pages now, no staging copy)
            [g appendString:@"if (gl_GlobalInvocationID.x >= pc.vcount) return;\nV4Buf OUT = V4Buf(pc.out_);\n"];
            tb = subIdent(tb, tid, @"(gl_GlobalInvocationID.x + pc.vstart)");
            if (iidName) tb = subIdent(tb, iidName, @"pc.iid");
            NSRegularExpression *rrx = [NSRegularExpression regularExpressionWithPattern:
                @"return\\s+([^;]+);" options:0 error:nil];
            if (vfields) {
                NSMutableString *st = [NSMutableString stringWithFormat:@"{ %@ nv_r = $1;", ret];
                for (NSUInteger j = 0; j < vfields.count; j++)
                    [st appendFormat:@" OUT.d[gl_GlobalInvocationID.x * %luu + %luu] = %@;",
                        (unsigned long)vfields.count, (unsigned long)j,
                        padVec4(vfields[j][@"type"], [@"nv_r." stringByAppendingString:vfields[j][@"name"]])];
                [st appendString:@" return; }"];
                tb = [rrx stringByReplacingMatchesInString:tb options:0
                            range:NSMakeRange(0, tb.length) withTemplate:st];
            } else {
                tb = [rrx stringByReplacingMatchesInString:tb options:0
                            range:NSMakeRange(0, tb.length)
                            withTemplate:@"OUT.d[gl_GlobalInvocationID.x] = $1;"];
            }
        } else {
            // Thread position from the real block shape (the shader is always
            // built with local_size 256, NAK would bake that into
            // gl_GlobalInvocationID) and Metal's non-uniform grid edge.
            [g appendString:@"uvec3 nvGid = gl_WorkGroupID * uvec3(pc.kX, pc.kY, pc.kZ) + gl_LocalInvocationID;\n"
                            @"if (nvGid.x >= pc.gX || nvGid.y >= pc.gY || nvGid.z >= pc.gZ) return;\n"];
            NSString *gidExpr = [tidType isEqual:@"uint3"] ? @"nvGid"
                              : [tidType isEqual:@"uint2"] ? @"nvGid.xy" : @"nvGid.x";
            tb = subIdent(tb, tid, gidExpr);
            tb = translateTextures(tb, texByIdx, sampByIdx, nTex, nSamp);
        }
        [g appendString:tb];
        [g appendString:@"\n}\n"];
        NVMTLKernel *k = nvCompileGLSL(name, g);
        if (!k) continue;
        k.nbuf = (uint32_t)nPush; k.isVertex = isVertex;
        if (isVertex) {
            k.voutStride = vfields ? (uint32_t)vfields.count : 1;
            k.posSlot = vfields ? (uint32_t)posSlotOf(vfields) : 0;
        }
        k.ntex = (uint32_t)nTex; k.nsamp = (uint32_t)nSamp;
        if (!isVertex) { k.glsl = g; k.variants = [NSMutableDictionary new]; }
        kernels[name] = k;
    }
    return kernels;
}

// 0.3.0: the fragment body fused into a compute rasterizer, one launch per
// triangle over its screen bbox. Push layout: the kernel resource block
// (buffers, textures, samplers - same order as compute kernels) then
//   uint64 vout, rt; uint rtP, rtW, rtH, rtF; uint i0, i1, i2, S, pos;
//   uint bx, by, bw, bh; vec4 vp; uint blend;
// vout = vertex outputs (S vec4 slots per vertex, [[position]] in slot pos).
// blend: 0 = off, else 1 | srcRGB<<4 | dstRGB<<8 | srcA<<12 | dstA<<16 |
// opRGB<<20 | opA<<24 (MTLBlendFactor / MTLBlendOperation values).
static NSString *kRasterMain =
    @"float nvEdge(vec2 a, vec2 b, vec2 c) { return (c.x-a.x)*(b.y-a.y)-(c.y-a.y)*(b.x-a.x); }\n"
    @"bool nvTopLeft(vec2 a, vec2 b, float area) {\n"
    @"  vec2 e = area > 0.0 ? b - a : a - b;\n"
    @"  return e.y < 0.0 || (e.y == 0.0 && e.x > 0.0);\n"
    @"}\n"
    @"vec2 nvScreen(vec4 c) {\n"
    @"  vec2 n = c.xy / c.w;\n"
    @"  return vec2(pc.vp.x + (n.x * 0.5 + 0.5) * pc.vp.z, pc.vp.y + (0.5 - n.y * 0.5) * pc.vp.w);\n"
    @"}\n"
    @"vec4 nvFactor(uint f, vec4 s, vec4 d, bool alpha) {\n"
    @"  vec4 r = vec4(0.0);\n"
    @"  if (f == 1u) r = vec4(1.0);\n"
    @"  else if (f == 2u) r = s; else if (f == 3u) r = 1.0 - s;\n"
    @"  else if (f == 4u) r = vec4(s.a); else if (f == 5u) r = vec4(1.0 - s.a);\n"
    @"  else if (f == 6u) r = d; else if (f == 7u) r = 1.0 - d;\n"
    @"  else if (f == 8u) r = vec4(d.a); else if (f == 9u) r = vec4(1.0 - d.a);\n"
    @"  return r;\n"
    @"}\n"
    @"vec4 nvOp(uint op, vec4 a, vec4 b, vec4 s, vec4 d) {\n"
    @"  if (op == 1u) return a - b; if (op == 2u) return b - a;\n"
    @"  if (op == 3u) return min(s, d); if (op == 4u) return max(s, d);\n"
    @"  return a + b;\n"
    @"}\n"
    @"void nvOut(vec4 c, uvec2 xy) {\n"
    @"  if (pc.blend != 0u) {\n"
    @"    vec4 d = nvRead(pc.rt, pc.rtP, pc.rtF, xy);\n"
    @"    uint b = pc.blend;\n"
    @"    vec4 rgb = nvOp((b >> 20) & 15u, c * nvFactor((b >> 4) & 15u, c, d, false), d * nvFactor((b >> 8) & 15u, c, d, false), c, d);\n"
    @"    vec4 al = nvOp((b >> 24) & 15u, c * nvFactor((b >> 12) & 15u, c, d, true), d * nvFactor((b >> 16) & 15u, c, d, true), c, d);\n"
    @"    c = clamp(vec4(rgb.rgb, al.a), 0.0, 1.0);\n"
    @"  }\n"
    @"  nvWrite(pc.rt, pc.rtP, pc.rtF, c, xy);\n"
    @"}\n"
    @"layout(local_size_x=256) in;\n"
    @"void main() {\n"
    @"  uint li = gl_GlobalInvocationID.x;\n"
    @"  if (li >= pc.bw * pc.bh) return;\n"
    @"  uvec2 nvXY = uvec2(pc.bx + li % pc.bw, pc.by + li / pc.bw);\n"
    @"  NvVout VO = NvVout(pc.vout);\n"
    @"  vec4 c0 = pc.tv[pc.pos], c1 = pc.tv[pc.S + pc.pos], c2 = pc.tv[2u * pc.S + pc.pos];\n"
    @"  vec2 s0 = nvScreen(c0), s1 = nvScreen(c1), s2 = nvScreen(c2);\n"
    @"  vec2 p = vec2(nvXY) + 0.5;\n"
    @"  float area = nvEdge(s0, s1, s2);\n"
    @"  if (area == 0.0) return;\n"
    @"  float w0 = nvEdge(s1, s2, p) / area, w1 = nvEdge(s2, s0, p) / area, w2 = nvEdge(s0, s1, p) / area;\n"
    // top-left rule: a pixel exactly on an edge shared by two triangles
    // belongs to one of them only (else blending runs twice there)
    @"  if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) return;\n"
    @"  if ((w0 == 0.0 && !nvTopLeft(s1, s2, area)) || (w1 == 0.0 && !nvTopLeft(s2, s0, area)) ||\n"
    @"      (w2 == 0.0 && !nvTopLeft(s0, s1, area))) return;\n"
    @"  float q0 = w0 / c0.w, q1 = w1 / c1.w, q2 = w2 / c2.w, qs = q0 + q1 + q2;\n"
    @"  q0 /= qs; q1 /= qs; q2 /= qs;\n"
    // depth: dzMode bits 2:0 MTLCompareFunction, bit 3 write (early test)
    @"  if (pc.dzMode != 0u) {\n"
    @"    float z = w0 * c0.z / c0.w + w1 * c1.z / c1.w + w2 * c2.z / c2.w;\n"
    @"    NvDepth D = NvDepth(pc.dz + uint64_t(nvXY.y) * uint64_t(pc.dzP));\n"
    @"    float old = D.d[nvXY.x];\n"
    @"    uint f = pc.dzMode & 7u;\n"
    @"    bool ok = f == 7u || (f == 1u && z < old) || (f == 2u && z == old) || (f == 3u && z <= old) ||\n"
    @"              (f == 4u && z > old) || (f == 5u && z != old) || (f == 6u && z >= old);\n"
    @"    if (!ok) return;\n"
    @"    if ((pc.dzMode & 8u) != 0u) D.d[nvXY.x] = z;\n"
    @"  }\n";

static NVMTLKernel *compileFragmentRaster(NSString *name, NSString *params, NSString *body,
                                          NSDictionary *structs) {
    NSMutableDictionary *byIdx = [NSMutableDictionary new], *texByIdx = [NSMutableDictionary new],
                        *sampByIdx = [NSMutableDictionary new];
    NSUInteger nPush = 0, nTex = 0, nSamp = 0;
    NSString *inName = nil, *inType = nil;
    for (NSString *p in splitTop(params)) {
        NSString *t = [p stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (!t.length) continue;
        NSRegularExpression *irx = [NSRegularExpression regularExpressionWithPattern:
            @"^(\\w+)\\s+(\\w+)\\s*\\[\\[stage_in\\]\\]$" options:0 error:nil];
        NSTextCheckingResult *im = [irx firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
        if (im) {
            inType = [t substringWithRange:[im rangeAtIndex:1]];
            inName = [t substringWithRange:[im rangeAtIndex:2]];
            if (!structs[inType]) { NSLog(@"NVMTLCompiler: fragment %@ stage_in type %@ unknown", name, inType); return nil; }
            continue;
        }
        NSRegularExpression *brx = [NSRegularExpression regularExpressionWithPattern:
            @"^(?:device|constant)\\s+(?:const\\s+)?(float4|float3|float2|float|int4|int3|int2|int|uint4|uint3|uint2|uint)\\s*(\\*|&)\\s*(\\w+)\\s*\\[\\[buffer\\((\\d+)\\)\\]\\]$"
            options:0 error:nil];
        NSTextCheckingResult *bm = [brx firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
        if (bm) {
            const NSUInteger ix = [[t substringWithRange:[bm rangeAtIndex:4]] integerValue];
            if (ix >= 16) return nil;
            byIdx[@(ix)] = @{@"type": [t substringWithRange:[bm rangeAtIndex:1]],
                             @"name": [t substringWithRange:[bm rangeAtIndex:3]]};
            if (ix + 1 > nPush) nPush = ix + 1;
            continue;
        }
        NSRegularExpression *xrx = [NSRegularExpression regularExpressionWithPattern:
            @"^texture2d\\s*<\\s*(float|half|int|uint)\\s*(?:,\\s*access::(read|write|read_write|sample))?\\s*>\\s*(\\w+)\\s*\\[\\[texture\\((\\d+)\\)\\]\\]$"
            options:0 error:nil];
        NSTextCheckingResult *xm = [xrx firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
        if (xm) {
            const NSUInteger ix = [[t substringWithRange:[xm rangeAtIndex:4]] integerValue];
            if (ix >= 16) return nil;
            texByIdx[@(ix)] = @{@"elem": [t substringWithRange:[xm rangeAtIndex:1]],
                                @"name": [t substringWithRange:[xm rangeAtIndex:3]]};
            if (ix + 1 > nTex) nTex = ix + 1;
            continue;
        }
        NSRegularExpression *srx = [NSRegularExpression regularExpressionWithPattern:
            @"^sampler\\s+(\\w+)\\s*\\[\\[sampler\\((\\d+)\\)\\]\\]$" options:0 error:nil];
        NSTextCheckingResult *sm = [srx firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
        if (sm) {
            const NSUInteger ix = [[t substringWithRange:[sm rangeAtIndex:2]] integerValue];
            if (ix >= 16) return nil;
            sampByIdx[@(ix)] = @{@"name": [t substringWithRange:[sm rangeAtIndex:1]]};
            if (ix + 1 > nSamp) nSamp = ix + 1;
            continue;
        }
        NSLog(@"NVMTLCompiler: fragment %@ unsupported param '%@'", name, t);
        return nil;
    }
    NSMutableString *g = [NSMutableString new];
    [g appendString:@"#version 460\n"
     @"#extension GL_EXT_buffer_reference : require\n"
     @"#extension GL_EXT_scalar_block_layout : require\n"
     @"#extension GL_EXT_shader_explicit_arithmetic_types : require\n"
     @"#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n"
     @"#define half float\n#define half2 vec2\n#define half3 vec3\n#define half4 vec4\n"
     @"#define uint2 uvec2\n#define uint3 uvec3\n#define uint4 uvec4\n"
     @"#define int2 ivec2\n#define int3 ivec3\n#define int4 ivec4\n"
     @"layout(push_constant, scalar) uniform PC {\n"];
    for (NSUInteger i = 0; i < nPush; i++) [g appendFormat:@"uint64_t b%lu;\n", (unsigned long)i];
    for (NSUInteger i = 0; i < nTex; i++) [g appendFormat:@"uint64_t tA%lu;\n", (unsigned long)i];
    for (NSUInteger i = 0; i < nTex; i++)
        [g appendFormat:@"uint tP%lu; uint tW%lu; uint tH%lu; uint tF%lu;\n",
            (unsigned long)i, (unsigned long)i, (unsigned long)i, (unsigned long)i];
    for (NSUInteger i = 0; i < nSamp; i++) [g appendFormat:@"uint sM%lu;\n", (unsigned long)i];
    [g appendString:@"uint64_t vout; uint64_t rt; uint rtP; uint rtW; uint rtH; uint rtF;\n"
                    @"uint i0; uint i1; uint i2; uint S; uint pos;\n"
                    @"uint bx; uint by; uint bw; uint bh; vec4 vp; uint blend;\n"
                    @"uint64_t dz; uint dzP; uint dzMode;\n"
                    // 0.3.6: this triangle's vertex slots (3 x S vec4, S <= 8). The
                    // vout buffer is host memory: reading it per pixel was ~190 MB of
                    // PCIe traffic per full-screen triangle.
                    @"vec4 tv[24];\n} pc;\n"];
    [g appendString:@"layout(buffer_reference, scalar) buffer NvDepth { float d[]; };\n"];
    [g appendString:nvTextureHelpers()];
    [g appendString:@"layout(buffer_reference, scalar) buffer NvVout { vec4 d[]; };\n"];
    NSMutableSet *doneTy = [NSMutableSet new];
    for (NSUInteger i = 0; i < nPush; i++) {
        NSDictionary *b = byIdx[@(i)];
        if (!b || [doneTy containsObject:b[@"type"]]) continue;
        [doneTy addObject:b[@"type"]];
        NSArray *ti = typeInfo(b[@"type"]);
        [g appendFormat:@"layout(buffer_reference, scalar) buffer %@ { %@ d[]; };\n", ti[1], ti[0]];
    }
    NSArray *fields = inType ? structs[inType] : nil;
    if (fields) [g appendString:structDecl(inType, fields)];
    [g appendString:kRasterMain];
    for (NSUInteger i = 0; i < nPush; i++) {
        NSDictionary *b = byIdx[@(i)];
        if (!b) continue;
        NSArray *ti = typeInfo(b[@"type"]);
        [g appendFormat:@"  %@ B%lu = %@(pc.b%lu);\n", ti[1], (unsigned long)i, ti[1], (unsigned long)i];
    }
    if (fields) {
        [g appendFormat:@"  %@ nv_in;\n", inType];
        for (NSUInteger j = 0; j < fields.count; j++) {
            NSDictionary *f = fields[j];
            if ([f[@"pos"] boolValue]) {
                [g appendFormat:@"  nv_in.%@ = vec4(p, w0 * c0.z / c0.w + w1 * c1.z / c1.w + w2 * c2.z / c2.w,"
                                @" w0 / c0.w + w1 / c1.w + w2 / c2.w);\n", f[@"name"]];
                continue;
            }
            NSString *interp = [NSString stringWithFormat:
                @"(q0 * pc.tv[%luu] + q1 * pc.tv[pc.S + %luu] + q2 * pc.tv[2u * pc.S + %luu])",
                (unsigned long)j, (unsigned long)j, (unsigned long)j];
            [g appendFormat:@"  nv_in.%@ = %@;\n", f[@"name"], unpadVec4(f[@"type"], interp)];
        }
    }
    NSString *tb = body;
    for (NSUInteger i = 0; i < nPush; i++) {
        NSDictionary *b = byIdx[@(i)];
        if (b) tb = subIdent(tb, b[@"name"], [NSString stringWithFormat:@"B%lu.d", (unsigned long)i]);
    }
    if (inName) tb = subIdent(tb, inName, @"nv_in");
    tb = glslCtors(tb);
    tb = translateTextures(tb, texByIdx, sampByIdx, nTex, nSamp);
    NSRegularExpression *drx = [NSRegularExpression regularExpressionWithPattern:
        @"discard_fragment\\s*\\(\\s*\\)" options:0 error:nil];
    tb = [drx stringByReplacingMatchesInString:tb options:0 range:NSMakeRange(0, tb.length) withTemplate:@"return"];
    NSRegularExpression *rrx = [NSRegularExpression regularExpressionWithPattern:
        @"return\\s+([^;]+);" options:0 error:nil];
    tb = [rrx stringByReplacingMatchesInString:tb options:0 range:NSMakeRange(0, tb.length)
                                  withTemplate:@"{ nvOut(vec4($1), nvXY); return; }"];
    [g appendString:@"  {\n"];   // own scope: the body may reuse our local names
    [g appendString:tb];
    [g appendString:@"\n  }\n}\n"];
    NVMTLKernel *k = nvCompileGLSL([name stringByAppendingString:@"_raster"], g);
    if (!k) return nil;
    k.nbuf = (uint32_t)nPush; k.ntex = (uint32_t)nTex; k.nsamp = (uint32_t)nSamp;
    return k;
}

NSDictionary<NSString *, NVMTLFragment *> *nvCompileFragments(NSString *source) {
    NSMutableDictionary *out = [NSMutableDictionary new];
    NSMutableString *src = stripComments(source);
    NSDictionary *structs = parseStructs(src);
    NSRegularExpression *frx = [NSRegularExpression regularExpressionWithPattern:
        @"fragment\\s+(?:float4|half4)\\s+(\\w+)\\s*\\(" options:0 error:nil];
    NSArray *fm = [frx matchesInString:src options:0 range:NSMakeRange(0, src.length)];
    for (NSTextCheckingResult *m in fm) {
        NSString *name = [src substringWithRange:[m rangeAtIndex:1]];
        NSUInteger pp = NSMaxRange(m.range) - 1, depth = 0, pe = NSNotFound;
        for (NSUInteger i = pp; i < src.length; i++) {
            unichar c = [src characterAtIndex:i];
            if (c == '(') depth++;
            else if (c == ')') { if (--depth == 0) { pe = i; break; } }
        }
        if (pe == NSNotFound) continue;
        NSString *params = [src substringWithRange:NSMakeRange(pp + 1, pe - pp - 1)];
        NSUInteger bo = [src rangeOfString:@"{" options:0 range:NSMakeRange(pe, src.length - pe)].location;
        if (bo == NSNotFound) continue;
        NSUInteger be = matchBrace(src, bo);
        if (be == NSNotFound) continue;
        NSString *body = [src substringWithRange:NSMakeRange(bo + 1, be - bo - 1)];
        NVMTLFragment *f = [NVMTLFragment new];
        f.name = name;
        f.raster = compileFragmentRaster(name, params, body, structs);
        // v1 fallback: constant color, stage_in params only
        NSRegularExpression *crx = [NSRegularExpression regularExpressionWithPattern:
            @"return\\s+float4\\s*\\(\\s*([^,]+)\\s*,\\s*([^,]+)\\s*,\\s*([^,]+)\\s*,\\s*([^)]+)\\s*\\)"
            options:0 error:nil];
        NSTextCheckingResult *cm = [crx firstMatchInString:body options:0 range:NSMakeRange(0, body.length)];
        if (cm) {
            float col[4] = {0, 0, 0, 0};
            BOOL ok = YES;
            for (int i = 0; i < 4 && ok; i++) {
                NSString *num = [[body substringWithRange:[cm rangeAtIndex:1 + i]]
                                 stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                char *end = NULL;
                col[i] = strtof(num.UTF8String, &end);
                ok = end && !*end;
            }
            if (ok) { [f nvSetColor:col]; f.hasColor = YES; }
        }
        if (!f.raster && !f.hasColor) { NSLog(@"NVMTLCompiler: fragment %@ outside the subset", name); continue; }
        out[name] = f;
        NSLog(@"NVMTLCompiler: fragment %@ %@", name, f.raster ? @"compiled (raster v2)" : @"constant color (v1)");
    }
    return out;
}

static NSString *sha256Hex(NSData *d, NSUInteger bytes) {
    unsigned char h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    NSMutableString *hex = [NSMutableString stringWithCapacity:bytes * 2];
    for (NSUInteger i = 0; i < bytes; i++) [hex appendFormat:@"%02x", h[i]];
    return hex;
}

NSString *nvSaveMetallib(NSData *lib) {
    // plain metallib, or a universal file with one slice per GPU family
    static const uint8_t fat[4] = {0xca, 0xfe, 0xba, 0xbe};
    if (lib.length < 16 || (memcmp(lib.bytes, "MTLB", 4) && memcmp(lib.bytes, fat, 4))) return nil;
        (void)nvCacheDir();
    NSString *p = [NSString stringWithFormat:@"%@/%@.metallib", nvCacheDir(), sha256Hex(lib, 16)];
    if (![[NSFileManager defaultManager] fileExistsAtPath:p]) [lib writeToFile:p atomically:YES];
    return p;
}

// 0.6.5: a single function's AIR bitcode (no metallib around it)
NSString *nvSaveAir(NSData *bc) {
    if (bc.length < 8) return nil;
    (void)nvCacheDir();
    NSString *p = [NSString stringWithFormat:@"%@/%@.air", nvCacheDir(), sha256Hex(bc, 16)];
    if (![[NSFileManager defaultManager] fileExistsAtPath:p]) [bc writeToFile:p atomically:YES];
    return p;
}

// OUT.nak.ts: "domain D spacing S prims P ..." (NAK's tessellation info)
static void nvReadTessParams(NVMTLKernel *k, NSString *nakPath) {
    NSString *ts = [NSString stringWithContentsOfFile:[nakPath stringByAppendingString:@".ts"]
                                             encoding:NSUTF8StringEncoding error:nil];
    unsigned d = 0, sp = 0, pr = 0;
    if (ts && sscanf(ts.UTF8String, "domain %u spacing %u prims %u", &d, &sp, &pr) == 3)
        k.tessParams = d | sp << 4 | pr << 8;
}

// 0.8.23: the vertex shader that draws a mesh stage's output (nakc --mesh-gen); mesh = the mesh kernel's info
NVMTLKernel *nvCompileMeshGen(NSArray<NSNumber *> *mesh) {
    struct stat nst;
    if (!nvCompilerStat(&nst) || mesh.count < 9) return nil;
    NSArray *args = @[mesh[5], mesh[3], mesh[4], mesh[8], mesh[6], mesh[7]];   // nvdata maxV maxP vpp vstride gstride
    NSString *key = [NSString stringWithFormat:@"meshgen|%@|%lld.%ld", [args componentsJoinedByString:@","],
                     (long long)nst.st_size, (long)nst.st_mtimespec.tv_sec];
    NSString *nakPath = [NSString stringWithFormat:@"%@/%@.nak", nvCacheDir(),
                         sha256Hex([key dataUsingEncoding:NSUTF8StringEncoding], 16)];
    NSData *nak = [NSData dataWithContentsOfFile:nakPath];
    if (!nak) {
        (void)nvCacheDir();
        int st = 0;
        NSMutableArray *a = [@[@"--mesh-gen", nakPath] mutableCopy];
        for (NSNumber *n in args) [a addObject:n.stringValue];
        NSString *o = runTool(@NAKC_PATH, a, &st);
        if (st) { NSLog(@"NVMTLCompiler: mesh vs failed: %@", o); return nil; }
        nak = [NSData dataWithContentsOfFile:nakPath];
    }
    const uint32_t *h = nak.bytes;
    if (nak.length < 64 || h[0] != 0x314b414e || nak.length < 64 + h[10] * 4 + h[1]) {
        NSLog(@"NVMTLCompiler: mesh vs bad output"); return nil;
    }
    NVMTLKernel *k = [NVMTLKernel new];
    k.name = @"mesh_vs";
    k.regs = h[2]; k.slm = h[3]; k.smem = h[4]; k.barriers = h[8];
    k.code = [nak subdataWithRange:NSMakeRange(64, h[10] * 4 + h[1])];
    k.stage = 1; k.nbuf = 1; k.hwTex = YES;
    return k;
}

NVMTLKernel *nvCompileTessGen(NSString *kind, NSArray<NSNumber *> *locs, uint32_t cps, uint32_t domain) {
    struct stat nst;
    if (!nvCompilerStat(&nst)) return nil;
    NSString *list = locs.count ? [locs componentsJoinedByString:@","] : @"-";
    NSString *key = [NSString stringWithFormat:@"tessgen|%@|%@|%u|%u|%lld.%ld", kind, list, cps, domain,
                     (long long)nst.st_size, (long)nst.st_mtimespec.tv_sec];
    NSString *nakPath = [NSString stringWithFormat:@"%@/%@.nak", nvCacheDir(),
                         sha256Hex([key dataUsingEncoding:NSUTF8StringEncoding], 16)];
    NSData *nak = [NSData dataWithContentsOfFile:nakPath];
    if (!nak) {
        (void)nvCacheDir();
        int st = 0;
        NSString *o = runTool(@NAKC_PATH, @[@"--tess-gen", kind, nakPath, list, [NSString stringWithFormat:@"%u", cps],
                                            [NSString stringWithFormat:@"%u", domain]], &st);
        if (st) { NSLog(@"NVMTLCompiler: tess %@ failed: %@", kind, o); return nil; }
        nak = [NSData dataWithContentsOfFile:nakPath];
    }
    const uint32_t *h = nak.bytes;
    if (nak.length < 64 || h[0] != 0x314b414e || nak.length < 64 + h[10] * 4 + h[1]) {
        NSLog(@"NVMTLCompiler: tess %@ bad output", kind); return nil;
    }
    NVMTLKernel *k = [NVMTLKernel new];
    k.name = [@"tess_" stringByAppendingString:kind];
    k.regs = h[2]; k.slm = h[3]; k.smem = h[4]; k.barriers = h[8];
    k.code = [nak subdataWithRange:NSMakeRange(64, h[10] * 4 + h[1])];
    k.stage = [kind isEqual:@"vs"] ? 1 : 4;
    k.tessCps = cps; k.tessDomain = domain;
    return k;
}

// 0.8.15: the argument list of a [[visible]] function (nakc --visible), what
// -[MTLFunction arguments] reports: "varg <kind> <dataType> <elem/texType>
// <isConstant> <access> <texDataType> <name> <typeName>" lines, each struct
// followed by its "vmem" field lines, and one "vret" (same fields) for the
// return type; nil when nakc cannot read it
NSArray<NSString *> *nvVisibleArguments(NSString *libPath, NSString *name) {
    if (!libPath || !name) return nil;
    NSString *out = [NSString stringWithFormat:@"%@/%@.varg5", nvCacheDir(),
                     sha256Hex([[NSString stringWithFormat:@"%@|%@", libPath, name] dataUsingEncoding:NSUTF8StringEncoding], 16)];
    NSString *text = [NSString stringWithContentsOfFile:out encoding:NSUTF8StringEncoding error:nil];
    if (!text) {
        int st = 0;
        NSString *o = runTool(@NAKC_PATH, @[@"--visible", libPath, name, out], &st);
        if (st) { NSLog(@"NVMTLCompiler: %@ (visible arguments) failed: %@", name, o); return nil; }
        text = [NSString stringWithContentsOfFile:out encoding:NSUTF8StringEncoding error:nil];
    }
    NSMutableArray *lines = [NSMutableArray new];
    for (NSString *l in [text componentsSeparatedByString:@"\n"])
        if ([l hasPrefix:@"varg "] || [l hasPrefix:@"vmem "] || [l hasPrefix:@"vret "]) [lines addObject:l];
    return lines;
}

// 0.8.24: kernels built in this process, by the same key as the disk cache.
// SecurityAgent re-created the same pipelines ~40 times a minute: each time the
// .nak was read again and uploaded to a new heap slot (never freed), and a
// failed kernel ran nakc again. One object per key now; NSNull = failed.
static NSMutableDictionary<NSString *, id> *gAirKernels;
static os_unfair_lock gAirKernelsLock = OS_UNFAIR_LOCK_INIT;
static id airKernelGet(NSString *key) {
    os_unfair_lock_lock(&gAirKernelsLock);
    id k = gAirKernels[key];
    os_unfair_lock_unlock(&gAirKernelsLock);
    return k;
}
static NVMTLKernel *airKernelPut(NSString *key, NVMTLKernel *k) {
    os_unfair_lock_lock(&gAirKernelsLock);
    if (!gAirKernels) gAirKernels = [NSMutableDictionary new];
    id have = gAirKernels[key];                  // two threads built it: keep the first
    if (!have) gAirKernels[key] = k ? k : (id)[NSNull null];
    os_unfair_lock_unlock(&gAirKernelsLock);
    if (have) return [have isKindOfClass:[NVMTLKernel class]] ? have : nil;
    return k;
}
static NVMTLKernel *nvCompileAirUncached(NSString *key, NSString *libPath, NSString *name, uint32_t x, uint32_t y,
                                         uint32_t z, NSArray<NSString *> *extra);

NVMTLKernel *nvCompileAir(NSString *libPath, NSString *name, uint32_t x, uint32_t y, uint32_t z,
                          NSArray<NSString *> *extra) {
    struct stat nst;
    if (!nvCompilerStat(&nst)) return nil;
    // cache key: library file name (content hash), function, shape, constants, compiler
    NSString *key = [NSString stringWithFormat:@"air|%@|%@|%u,%u,%u|%@|%lld.%ld", libPath.lastPathComponent,
                     name, x, y, z, [extra componentsJoinedByString:@","],
                     (long long)nst.st_size, (long)nst.st_mtimespec.tv_sec];
    id have = airKernelGet(key);
    if (have) return [have isKindOfClass:[NVMTLKernel class]] ? have : nil;
    return airKernelPut(key, nvCompileAirUncached(key, libPath, name, x, y, z, extra));
}

static NVMTLKernel *nvCompileAirUncached(NSString *key, NSString *libPath, NSString *name, uint32_t x, uint32_t y,
                                         uint32_t z, NSArray<NSString *> *extra) {
    NSString *base = [NSString stringWithFormat:@"%@/%@", nvCacheDir(),
                      sha256Hex([key dataUsingEncoding:NSUTF8StringEncoding], 16)];
    NSString *nakPath = [base stringByAppendingString:@".nak"];
    NSData *nak = [NSData dataWithContentsOfFile:nakPath];
    NSString *abi = [NSString stringWithContentsOfFile:[nakPath stringByAppendingString:@".abi"]
                                              encoding:NSUTF8StringEncoding error:nil];
    // 0.8.17: the exact nakc command, so a faulting kernel can be rebuilt
    // and read on another machine (hit or miss, the cache hides it otherwise)
    if (getenv("NVMTL_TRACE"))
        os_log(OS_LOG_DEFAULT, "NVMTL_TRACE air %{public}s: nakc --air %{public}s %{public}s %{public}s %u %u %u %{public}s (%{public}s)",
               name.UTF8String, libPath.UTF8String, name.UTF8String, nakPath.UTF8String, x, y, z,
               [extra componentsJoinedByString:@" "].UTF8String, nak && abi ? "cached" : "new");
    if (!nak || !abi) {
        int st = 0;
        NSArray *args = [@[@"--air", libPath, name, nakPath,
                           [NSString stringWithFormat:@"%u", x],
                           [NSString stringWithFormat:@"%u", y],
                           [NSString stringWithFormat:@"%u", z]] arrayByAddingObjectsFromArray:extra ? extra : @[]];
        NSString *o = runTool(@NAKC_PATH, args, &st);
        if (st) {
            // 0.8.24: with the exact command, so an empty message can be reproduced with nakc
            os_log(OS_LOG_DEFAULT, "NVMTLCompiler: %{public}s (AIR) failed (status %d): %{public}s [nakc %{public}s]",
                   name.UTF8String, st, o.UTF8String, [args componentsJoinedByString:@" "].UTF8String);
            return nil;
        }
        // 0.8.15: nakc's notes (unlinked visible functions ...) are worth seeing
        for (NSString *l in [o componentsSeparatedByString:@"\n"])
            if ([l containsString:@"note:"])
                os_log(OS_LOG_DEFAULT, "NVMTLCompiler: %{public}s: %{public}s (link args: %{public}s)", name.UTF8String, l.UTF8String,
                       [[extra filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH 'link='"]]
                           componentsJoinedByString:@" "].UTF8String);
        nak = [NSData dataWithContentsOfFile:nakPath];
        abi = [NSString stringWithContentsOfFile:[nakPath stringByAppendingString:@".abi"]
                                        encoding:NSUTF8StringEncoding error:nil];
    }
    if (nak.length < 64 || *(const uint32_t *)nak.bytes != 0x314b414e || !abi) {
        NSLog(@"NVMTLCompiler: %@ (AIR) bad output", name); return nil;
    }
    const uint32_t *h = nak.bytes;
    NVMTLKernel *k = [NVMTLKernel new];
    k.name = name; k.regs = h[2]; k.slm = h[3]; k.smem = h[4]; k.barriers = h[8];
    // vertex/fragment: the 0x80-byte SPH stays in front of the code
    const uint32_t sph = h[10] * 4;
    k.sampleShading = h[9] == 4 && h[11] != 0;
    if (nak.length < 64 + sph + h[1]) { NSLog(@"NVMTLCompiler: %@ (AIR) short .nak", name); return nil; }
    k.code = [nak subdataWithRange:NSMakeRange(64, sph + h[1])];
    k.airLib = libPath;
    k.airArgs = extra;
    k.variants = [NSMutableDictionary new];
    uint32_t cdOff = 0, cdBytes = 0;
    NSMutableData *tsc = [NSMutableData new];
    k.hwTex = YES;
    for (NSString *line in [abi componentsSeparatedByString:@"\n"]) {
        NSArray *w = [line componentsSeparatedByString:@" "];
        if (w.count >= 2 && [w[0] isEqual:@"nbuf"]) k.nbuf = (uint32_t)[w[1] intValue];
        if (w.count >= 2 && [w[0] isEqual:@"ntex"]) k.ntex = (uint32_t)[w[1] intValue];
        if (w.count >= 2 && [w[0] isEqual:@"stage"]) k.stage = (uint32_t)[w[1] intValue];
        if (w.count >= 2 && [w[0] isEqual:@"io"]) k.io = [line substringFromIndex:3];
        if ((w.count >= 13 && [w[0] isEqual:@"refl"]) || (w.count >= 6 && [w[0] isEqual:@"reflm"]) ||
            (w.count >= 7 && [w[0] isEqual:@"refli"]))
            k.refl = [(k.refl ?: @[]) arrayByAddingObject:line];
        if (w.count >= 10 && [w[0] isEqual:@"mesh"]) {
            NSMutableArray *a = [NSMutableArray new];
            for (NSUInteger i = 1; i < 10; i++) [a addObject:@([w[i] intValue])];
            k.mesh = a;
        }
        if (w.count >= 3 && [w[0] isEqual:@"tess"]) { k.tessDomain = (uint32_t)[w[1] intValue]; k.tessCps = (uint32_t)[w[2] intValue]; }
        if (w.count >= 2 && [w[0] isEqual:@"nsamp"]) k.nsamp = (uint32_t)[w[1] intValue];
        if (w.count >= 3 && [w[0] isEqual:@"csamp"]) {
            const uint32_t idx = nvTscFromAirBits(strtoull([w[2] UTF8String], NULL, 16));
            if (!idx) { NSLog(@"NVMTLCompiler: %@ (AIR) no room for a sampler", name); return nil; }
            [tsc appendBytes:&idx length:4];
        }
        if (w.count >= 3 && [w[0] isEqual:@"cdata"]) { cdOff = (uint32_t)[w[1] intValue]; cdBytes = (uint32_t)[w[2] intValue]; }
        if (w.count >= 3 && [w[0] isEqual:@"rtread"]) {
            k.rtReadDword = (uint32_t)[w[1] intValue] / 4;
            k.rtReadMask = (uint32_t)strtoul([w[2] UTF8String], NULL, 16);
        }
    }
    k.nconstSamp = (uint32_t)(tsc.length / 4);
    k.constTsc = tsc;
    if (cdBytes) {
        k.cdata = [NSData dataWithContentsOfFile:[nakPath stringByAppendingString:@".cdata"]];
        k.cdataDword = cdOff / 4;
        if (k.cdata.length != cdBytes) { NSLog(@"NVMTLCompiler: %@ (AIR) cdata missing", name); return nil; }
    }
    if (k.stage == 3) nvReadTessParams(k, nakPath);
    NSLog(@"NVMTLCompiler: %@ (AIR) %u bytes %u gprs, %u buffers%@", name, h[1], h[2], k.nbuf,
          cdBytes ? [NSString stringWithFormat:@", %u bytes of constants", cdBytes] : @"");
    return k;
}

NVMTLKernel *nvKernelForBlock(NVMTLKernel *k, uint32_t x, uint32_t y, uint32_t z) {
    if (k.airLib) {                              // precompiled: rebuild from the metallib
        if ((x == 256 && y == 1 && z == 1) || !x || !y || !z || x * y * z > 1024) return k;
        NSString *key = [NSString stringWithFormat:@"%u,%u,%u", x, y, z];
        @synchronized (k.variants) {
            NVMTLKernel *v = k.variants[key];
            if (v) return v;
            v = nvCompileAir(k.airLib, k.name, x, y, z, k.airArgs);
            if (!v) return k;
            k.variants[key] = v;
            return v;
        }
    }
    if (!k.glsl || (x == 256 && y == 1 && z == 1) || !x || !y || !z || x * y * z > 1024) return k;
    NSString *key = [NSString stringWithFormat:@"%u,%u,%u", x, y, z];
    @synchronized (k.variants) {
        NVMTLKernel *v = k.variants[key];
        if (v) return v;
        NSString *src = [k.glsl stringByReplacingOccurrencesOfString:@"layout(local_size_x=256) in;"
            withString:[NSString stringWithFormat:@"layout(local_size_x=%u, local_size_y=%u, local_size_z=%u) in;", x, y, z]];
        v = nvCompileGLSL([NSString stringWithFormat:@"%@_%ux%ux%u", k.name, x, y, z], src);
        if (!v) return k;
        v.nbuf = k.nbuf; v.ntex = k.ntex; v.nsamp = k.nsamp; v.isVertex = NO;
        k.variants[key] = v;
        return v;
    }
}
