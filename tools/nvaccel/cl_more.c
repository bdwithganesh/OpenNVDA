// cl_more: OpenCL coverage beyond cl_test: by-value scalars, struct arg, atomics, int vectors,
// image2d read/write, sub-buffer, copy, mixed arg order. Prints PASS/FAIL per case.
#include <OpenCL/opencl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static const char *src =
"typedef struct { float a; int b; float c; } P;\n"
"__kernel void scal(__global float *o, float s, int add, __global const float *in) { size_t i = get_global_id(0); o[i] = in[i] * s + add; }\n"
"__kernel void st(__global float *o, P p) { size_t i = get_global_id(0); o[i] = p.a * i + p.b + p.c; }\n"
"__kernel void at(__global int *c) { atomic_inc(&c[0]); atomic_add(&c[1], 2); atomic_max(&c[2], (int)get_global_id(0)); }\n"
"__kernel void iv(__global int4 *o) { int i = get_global_id(0); int4 v = (int4)(i, i*2, i^3, -i); o[i] = (v << 1) + (v >> 1) - (v & 5); }\n"
"__kernel void img(__read_only image2d_t in, __write_only image2d_t out) { int2 p = (int2)(get_global_id(0), get_global_id(1));\n"
"  float4 c = read_imagef(in, CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST, p); write_imagef(out, p, c * 0.5f + (float4)(0.25f)); }\n"
"__kernel void mix(__local float *l, __global float *o, int k, __local int *li, __global const float *a) { size_t i = get_global_id(0), j = get_local_id(0);\n"
"  l[j] = a[i]; li[j] = k; barrier(CLK_LOCAL_MEM_FENCE); o[i] = l[(j + 1) % get_local_size(0)] + li[j]; }\n";
static cl_context ctx; static cl_command_queue q; static cl_program prog; static int fails;
#define N 1024
static void rep(const char *n, int bad) { printf("  %-10s %s (%d bad)\n", n, bad ? "FAIL" : "PASS", bad); fails += bad != 0; }
int main(void) {
    cl_platform_id p; cl_device_id dev; cl_int e;
    clGetPlatformIDs(1, &p, NULL); clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 1, &dev, NULL);
    char name[128]; clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof name, name, NULL); printf("device %s\n", name);
    ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &e); q = clCreateCommandQueue(ctx, dev, 0, &e);
    prog = clCreateProgramWithSource(ctx, 1, &src, NULL, &e);
    if (clBuildProgram(prog, 1, &dev, "", NULL, NULL)) { char log[8192]; clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof log, log, NULL); printf("build: %s\n", log); return 1; }
    size_t n = N, l = 64; float in[N], out[N]; for (int i = 0; i < N; i++) in[i] = i * 0.5f;
    cl_mem bi = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof in, in, &e), bo = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof out, NULL, &e);
    { cl_kernel k = clCreateKernel(prog, "scal", &e); float s = 3; int a = 7; cl_uint na = 0; clGetKernelInfo(k, CL_KERNEL_NUM_ARGS, 4, &na, NULL);
      int r = clSetKernelArg(k, 0, 8, &bo) | clSetKernelArg(k, 1, 4, &s) | clSetKernelArg(k, 2, 4, &a) | clSetKernelArg(k, 3, 8, &bi);
      clEnqueueNDRangeKernel(q, k, 1, NULL, &n, NULL, 0, NULL, NULL); clEnqueueReadBuffer(q, bo, 1, 0, sizeof out, out, 0, NULL, NULL);
      int bad = r ? N : 0; for (int i = 0; i < N && !r; i++) bad += out[i] != in[i] * 3 + 7; printf("  scal nargs %u setarg %d\n", na, r); rep("scalars", bad); }
    { cl_kernel k = clCreateKernel(prog, "st", &e); struct { float a; int b; float c; } pv = {2, 3, 0.5f};
      int r = clSetKernelArg(k, 0, 8, &bo) | clSetKernelArg(k, 1, sizeof pv, &pv); clEnqueueNDRangeKernel(q, k, 1, NULL, &n, NULL, 0, NULL, NULL);
      clEnqueueReadBuffer(q, bo, 1, 0, sizeof out, out, 0, NULL, NULL); int bad = r ? N : 0; for (int i = 0; i < N && !r; i++) bad += out[i] != 2.0f * i + 3 + 0.5f; rep("struct", bad); }
    { cl_kernel k = clCreateKernel(prog, "at", &e); int c[3] = {0, 0, -1}; cl_mem bc = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof c, c, &e);
      clSetKernelArg(k, 0, 8, &bc); clEnqueueNDRangeKernel(q, k, 1, NULL, &n, NULL, 0, NULL, NULL); clEnqueueReadBuffer(q, bc, 1, 0, sizeof c, c, 0, NULL, NULL);
      rep("atomics", (c[0] != N) + (c[1] != 2 * N) + (c[2] != N - 1)); }
    { cl_kernel k = clCreateKernel(prog, "iv", &e); cl_mem b4 = clCreateBuffer(ctx, CL_MEM_READ_WRITE, N * 16, NULL, &e); int o4[N * 4];
      clSetKernelArg(k, 0, 8, &b4); clEnqueueNDRangeKernel(q, k, 1, NULL, &n, NULL, 0, NULL, NULL); clEnqueueReadBuffer(q, b4, 1, 0, sizeof o4, o4, 0, NULL, NULL);
      int bad = 0; for (int i = 0; i < N; i++) { int v[4] = {i, i * 2, i ^ 3, -i}; for (int c = 0; c < 4; c++) bad += o4[i * 4 + c] != (v[c] << 1) + (v[c] >> 1) - (v[c] & 5); } rep("int4", bad); }
    { cl_image_format f = {CL_RGBA, CL_UNORM_INT8}; size_t W = 64, H = 32; unsigned char px[64 * 32 * 4], ro[64 * 32 * 4]; for (size_t i = 0; i < sizeof px; i++) px[i] = (unsigned char)(i * 7);
      cl_mem ii = clCreateImage2D(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, &f, W, H, 0, px, &e); cl_mem io = clCreateImage2D(ctx, CL_MEM_WRITE_ONLY, &f, W, H, 0, NULL, &e);
      cl_kernel k = clCreateKernel(prog, "img", &e); int r = clSetKernelArg(k, 0, 8, &ii) | clSetKernelArg(k, 1, 8, &io); size_t g[2] = {W, H};
      clEnqueueNDRangeKernel(q, k, 2, NULL, g, NULL, 0, NULL, NULL); size_t o[3] = {0}, rg[3] = {W, H, 1}; clEnqueueReadImage(q, io, 1, o, rg, 0, 0, ro, 0, NULL, NULL);
      int bad = r ? 1 : 0; for (size_t i = 0; i < sizeof px && !r; i++) { int want = (int)(px[i] * 0.5f + 63.75f + 0.5f); bad += abs(ro[i] - want) > 1; } printf("  img setarg %d\n", r); rep("image2d", bad); }
    { cl_buffer_region rg = {256 * 4, 256 * 4}; cl_mem sb = clCreateSubBuffer(bi, CL_MEM_READ_ONLY, CL_BUFFER_CREATE_TYPE_REGION, &rg, &e); float t[256];
      clEnqueueReadBuffer(q, sb, 1, 0, sizeof t, t, 0, NULL, NULL); int bad = e ? 256 : 0; for (int i = 0; i < 256 && !e; i++) bad += t[i] != in[256 + i]; rep("subbuffer", bad);
      cl_mem cp = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof in, NULL, &e); clEnqueueCopyBuffer(q, bi, cp, 0, 0, sizeof in, 0, NULL, NULL);
      clEnqueueReadBuffer(q, cp, 1, 0, sizeof out, out, 0, NULL, NULL); bad = memcmp(out, in, sizeof in) != 0; rep("copy", bad); }
    { cl_kernel k = clCreateKernel(prog, "mix", &e); int kk = 5; cl_uint na = 0; clGetKernelInfo(k, CL_KERNEL_NUM_ARGS, 4, &na, NULL);
      int r = clSetKernelArg(k, 0, l * 4, NULL) | clSetKernelArg(k, 1, 8, &bo) | clSetKernelArg(k, 2, 4, &kk) | clSetKernelArg(k, 3, l * 4, NULL) | clSetKernelArg(k, 4, 8, &bi);
      clEnqueueNDRangeKernel(q, k, 1, NULL, &n, &l, 0, NULL, NULL); clEnqueueReadBuffer(q, bo, 1, 0, sizeof out, out, 0, NULL, NULL);
      int bad = r ? N : 0; for (int i = 0; i < N && !r; i++) { int j = i % 64; bad += out[i] != in[i - j + (j + 1) % 64] + 5; } printf("  mix nargs %u setarg %d\n", na, r); rep("mixed", bad); }
    printf("cl_more: %s\n", fails ? "FAIL" : "PASS"); return fails != 0;
}
