// gpuload: busy-loops a Metal compute kernel for argv[1] seconds at roughly argv[2] duty (0..1).
// clang -O2 -fobjc-arc -framework Metal -framework Foundation bench/gpuload.m -o gpuload
#import <Metal/Metal.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 20, duty = argc > 2 ? atof(argv[2]) : 1;
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nkernel void k(device float *b [[buffer(0)]], uint i [[thread_position_in_grid]]) {"
                     " float x = b[i]; for (int j = 0; j < 20000; j++) x = fma(x, 1.0001f, 0.0001f); b[i] = x; }";
    id<MTLLibrary> lib = [d newLibraryWithSource:src options:nil error:nil];
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:nil];
    id<MTLBuffer> buf = [d newBufferWithLength:4 * 65536 options:0];
    id<MTLCommandQueue> q = [d newCommandQueue];
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:secs];
    while ([end timeIntervalSinceNow] > 0) {
        NSDate *t0 = [NSDate date];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:ps];
        [e setBuffer:buf offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(65536, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        double busy = -[t0 timeIntervalSinceNow];
        if (duty < 1) usleep((useconds_t)(busy * (1 - duty) / duty * 1e6));
    }
}
