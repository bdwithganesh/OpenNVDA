// MTLSharedEvent handles, the way WebKit's GPU process and CoreAnimation use
// them: newSharedEventHandle, newSharedEventWithHandle, CPU and GPU signals
// seen through both objects, and the handle sent through NSXPCCoder (an
// anonymous XPC listener looped back to this process: the same encode /
// decode of the mach port a second process would do), with the receiving
// side signalling on the GPU. Run on the M1 for the reference.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <xpc/xpc.h>
#import <objc/message.h>
#import <objc/runtime.h>

@protocol NVEvProto
- (void)takeHandle:(MTLSharedEventHandle *)h reply:(void (^)(BOOL))reply;
@end
@interface NVEvSide : NSObject <NSXPCListenerDelegate, NVEvProto>
@property (strong) id<MTLDevice> dev;
@end
@implementation NVEvSide
- (BOOL)listener:(NSXPCListener *)l shouldAcceptNewConnection:(NSXPCConnection *)c {
    (void)l;
    c.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(NVEvProto)];
    c.exportedObject = self;
    [c resume];
    return YES;
}
- (void)takeHandle:(MTLSharedEventHandle *)h reply:(void (^)(BOOL))reply {
    id<MTLSharedEvent> e = h ? [self.dev newSharedEventWithHandle:h] : nil;
    if (e) {
        id<MTLCommandBuffer> cb = [[self.dev newCommandQueue] commandBuffer];
        [cb encodeSignalEvent:e value:12];
        [cb commit];
        [cb waitUntilCompleted];
    }
    reply(e != nil);
}
@end

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLSharedEvent> ev = [dev newSharedEvent];
        MTLSharedEventHandle *h = [ev newSharedEventHandle];
        printf("  newSharedEvent %s, handle %s (%s)\n", ev ? class_getName([(id)ev class]) : "nil", h ? "yes" : "nil",
               h ? class_getName([h class]) : "-");
        if (!h) { printf("metal_shared_event_xproc_test: FAIL\n"); return 1; }
        // same-process round trip through the handle first
        id<MTLSharedEvent> ev2 = [dev newSharedEventWithHandle:h];
        printf("  newSharedEventWithHandle (same process) %s\n", ev2 ? class_getName([(id)ev2 class]) : "nil");
        int bad = !ev2;
        if (ev2) {
            ev.signaledValue = 5;
            printf("  signal 5 on the original -> the handle's event sees %llu\n", ev2.signaledValue);
            bad += ev2.signaledValue != 5;
            // GPU signal through the second object, seen on the first
            id<MTLCommandQueue> q = [dev newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [cb encodeSignalEvent:ev2 value:9];
            [cb commit];
            [cb waitUntilCompleted];
            printf("  GPU signal 9 on the handle's event -> original sees %llu\n", ev.signaledValue);
            bad += ev.signaledValue != 9;
        }
        // the XPC transport WebKit uses: the handle goes through NSXPCCoder
        // (an anonymous listener looped back to this process), the other
        // side makes the event from it and signals 12 on the GPU
        NVEvSide *side = [NVEvSide new];
        side.dev = dev;
        NSXPCListener *l = [NSXPCListener anonymousListener];
        l.delegate = side;
        [l resume];
        NSXPCConnection *c = [[NSXPCConnection alloc] initWithListenerEndpoint:l.endpoint];
        c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(NVEvProto)];
        [c resume];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block BOOL made = NO;
        [[c remoteObjectProxyWithErrorHandler:^(NSError *e) { printf("  xpc error %s\n", e.description.UTF8String); dispatch_semaphore_signal(done); }]
            takeHandle:h reply:^(BOOL ok) { made = ok; dispatch_semaphore_signal(done); }];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        printf("  handle through NSXPCCoder: event made %s, original now %llu\n", made ? "yes" : "no", ev.signaledValue);
        bad += !made || ev.signaledValue != 12;
        [c invalidate];
        printf("metal_shared_event_xproc_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
