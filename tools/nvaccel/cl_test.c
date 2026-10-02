// OpenCL on the GPU device: vector add, a local-memory reduction and a
// float4 image-free kernel with math builtins; results checked on the CPU.
// Prints the device, each check, and the add bandwidth.
#include <OpenCL/opencl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <mach/mach_time.h>

static const char *kSrc =
    "__kernel void add(__global const float *a, __global const float *b, __global float *c) {\n"
    "  size_t i = get_global_id(0); c[i] = a[i] + b[i]; }\n"
    "__kernel void red(__global const float *a, __global float *o, __local float *s) {\n"
    "  size_t l = get_local_id(0), g = get_global_id(0), n = get_local_size(0);\n"
    "  s[l] = a[g]; barrier(CLK_LOCAL_MEM_FENCE);\n"
    "  for (size_t k = n / 2; k > 0; k >>= 1) { if (l < k) s[l] += s[l + k]; barrier(CLK_LOCAL_MEM_FENCE); }\n"
    "  if (l == 0) o[get_group_id(0)] = s[0]; }\n"
    "__kernel void mathk(__global const float4 *a, __global float4 *o) {\n"
    "  size_t i = get_global_id(0); float4 x = a[i];\n"
    "  o[i] = sqrt(x) + sin(x) * cos(x) + exp(-x) + fmax(x, 0.5f); }\n";

int main(void) {
    cl_platform_id p; cl_uint np = 0;
    clGetPlatformIDs(1, &p, &np);
    cl_device_id dev; cl_uint nd = 0;
    if (clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 1, &dev, &nd) || !nd) { printf("no GPU device\ncl_test: FAIL\n"); return 1; }
    char name[256] = {0}; clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof name, name, NULL);
    cl_ulong gmem = 0; clGetDeviceInfo(dev, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof gmem, &gmem, NULL);
    cl_uint cus = 0; clGetDeviceInfo(dev, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof cus, &cus, NULL);
    printf("  device %s, %u compute units, %llu MiB\n", name, cus, (unsigned long long)(gmem >> 20));
    cl_int err = 0;
    cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
    cl_command_queue q = clCreateCommandQueue(ctx, dev, 0, &err);
    cl_program prog = clCreateProgramWithSource(ctx, 1, &kSrc, NULL, &err);
    if (clBuildProgram(prog, 1, &dev, "", NULL, NULL)) {
        char log[4096] = {0}; clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof log, log, NULL);
        printf("build failed: %s\ncl_test: FAIL\n", log); return 1;
    }
    int bad = 0;
    const size_t N = 1 << 22;
    float *a = malloc(N * 4), *b = malloc(N * 4), *c = malloc(N * 4);
    for (size_t i = 0; i < N; i++) { a[i] = (float)(i % 1000) * 0.5f; b[i] = (float)(i % 777); }
    cl_mem ba = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N * 4, a, &err);
    cl_mem bb = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N * 4, b, &err);
    cl_mem bc = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, N * 4, NULL, &err);
    cl_kernel k = clCreateKernel(prog, "add", &err);
    clSetKernelArg(k, 0, sizeof ba, &ba); clSetKernelArg(k, 1, sizeof bb, &bb); clSetKernelArg(k, 2, sizeof bc, &bc);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double best = 1e30;
    for (int r = 0; r < 5; r++) {
        uint64_t t0 = mach_absolute_time();
        clEnqueueNDRangeKernel(q, k, 1, NULL, &N, NULL, 0, NULL, NULL);
        clFinish(q);
        double ms = (mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
        if (ms < best) best = ms;
    }
    clEnqueueReadBuffer(q, bc, CL_TRUE, 0, N * 4, c, 0, NULL, NULL);
    int w = 0;
    for (size_t i = 0; i < N; i++) if (c[i] != a[i] + b[i]) w++;
    printf("  add %zu floats: %d wrong, %.2f ms (%.1f GB/s)\n", N, w, best, 3.0 * N * 4 / 1e9 / (best / 1e3));
    bad += w != 0;
    // reduction with local memory
    const size_t L = 256, G = N / L;
    cl_mem bo = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, G * 4, NULL, &err);
    cl_kernel kr = clCreateKernel(prog, "red", &err);
    clSetKernelArg(kr, 0, sizeof ba, &ba); clSetKernelArg(kr, 1, sizeof bo, &bo); clSetKernelArg(kr, 2, L * 4, NULL);
    clEnqueueNDRangeKernel(q, kr, 1, NULL, &N, &L, 0, NULL, NULL);
    float *o = malloc(G * 4);
    clEnqueueReadBuffer(q, bo, CL_TRUE, 0, G * 4, o, 0, NULL, NULL);
    w = 0;
    for (size_t g = 0; g < G; g++) { double s = 0; for (size_t i = 0; i < L; i++) s += a[g * L + i]; if (fabs(s - o[g]) > 1e-3 * fabs(s) + 1e-3) w++; }
    printf("  local-memory reduction: %d of %zu groups wrong\n", w, G);
    bad += w != 0;
    // math builtins
    const size_t M = 1 << 16;
    float *x = malloc(M * 16), *y = malloc(M * 16);
    for (size_t i = 0; i < M * 4; i++) x[i] = (float)(i % 1000) * 0.01f;
    cl_mem bx = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, M * 16, x, &err);
    cl_mem by = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, M * 16, NULL, &err);
    cl_kernel km = clCreateKernel(prog, "mathk", &err);
    clSetKernelArg(km, 0, sizeof bx, &bx); clSetKernelArg(km, 1, sizeof by, &by);
    clEnqueueNDRangeKernel(q, km, 1, NULL, &M, NULL, 0, NULL, NULL);
    clEnqueueReadBuffer(q, by, CL_TRUE, 0, M * 16, y, 0, NULL, NULL);
    w = 0;
    for (size_t i = 0; i < M * 4; i++) {
        const double v = sqrt(x[i]) + sin(x[i]) * cos(x[i]) + exp(-x[i]) + fmax(x[i], 0.5);
        if (fabs(v - y[i]) > 1e-3 * fabs(v) + 1e-3) w++;
    }
    printf("  math builtins: %d of %zu wrong\n", w, M * 4);
    bad += w != 0;
    printf("cl_test: %s\n", bad ? "FAIL" : "PASS");
    return bad != 0;
}
