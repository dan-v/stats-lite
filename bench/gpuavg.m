// Is IOAccelerator "Device Utilization %" averaged since the previous read?
// A Metal kernel keeps the GPU busy ~50% of the time (100 ms on / 100 ms off) on a
// background thread while the main thread reads utilization at two cadences.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <unistd.h>
static io_service_t gpu;
static int util(void) {
    CFDictionaryRef d = IORegistryEntryCreateCFProperty(gpu, CFSTR("PerformanceStatistics"), NULL, 0);
    int v = -1;
    CFNumberRef n = d ? CFDictionaryGetValue(d, CFSTR("Device Utilization %")) : NULL;
    if (n) CFNumberGetValue(n, kCFNumberIntType, &v);
    if (d) CFRelease(d);
    return v;
}
int main(void) { @autoreleasepool {
    gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSString *src = @"kernel void spin(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) {"
                     " float x = i; for (int k = 0; k < 20000; k++) x = x * 1.0001f + 0.5f; o[i] = x; }";
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"spin"] error:&e];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> buf = [dev newBufferWithLength:4 * 1024 * 1024 options:MTLResourceStorageModeShared];
    __block volatile int run = 1;
    [NSThread detachNewThreadWithBlock:^{
        while (run) {
            NSDate *end = [NSDate dateWithTimeIntervalSinceNow:0.1];
            while ([end timeIntervalSinceNow] > 0) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:ps];
                [enc setBuffer:buf offset:0 atIndex:0];
                [enc dispatchThreads:MTLSizeMake(1024 * 1024, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
            }
            usleep(100000);
        }
    }];
    sleep(2);
    util();
    printf("every 0.25 s:");
    for (int i = 0; i < 16; i++) { usleep(250000); printf(" %d", util()); }
    printf("\nevery 2 s:   ");
    for (int i = 0; i < 6; i++) { sleep(2); printf(" %d", util()); }
    printf("\nevery 1 s:   ");
    for (int i = 0; i < 8; i++) { sleep(1); printf(" %d", util()); }
    printf("\n");
    run = 0;
    usleep(300000);
    printf("idle, 1 s:   ");
    for (int i = 0; i < 4; i++) { sleep(1); printf(" %d", util()); }
    printf("\n");
}}
