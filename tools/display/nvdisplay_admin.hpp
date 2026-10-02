// Fixed internal-client display diagnostics. Not a general RM interface.
#pragma once
#include "nvcursor_common.h"
#include "../../drivers/NVGspCore/NVGspRpcReply.hpp"
#include "../../drivers/NVGspCore/NVGspChannel.hpp"
static constexpr uint32_t client = 0xc0d00001, parent = 0xc0d02080;
static constexpr uint64_t vramBytes = 0x400000000ULL;

static inline bool vread(io_connect_t c, uint64_t address, uint32_t *out, size_t count) {
    if ((address&3) || !out || !count) return false;
    // Older selector11 accepts DWORD addresses but its paired-PTE reader
    // requires QWORD alignment. Read the leading high DWORD separately.
    if (address&4) {
        uint64_t in[] = {address-4, 2, 0}; uint32_t pair[2]; size_t bytes = sizeof(pair);
        if (IOConnectCallMethod(c, 11, in, 3, nullptr, 0, nullptr, nullptr,
                                pair, &bytes) || bytes != sizeof(pair)) return false;
        *out++ = pair[1]; address += 4; --count;
    }
    while (count) {
        const size_t n = count > 1024 ? 1024 : count;
        uint64_t in[] = {address, n, 0};
        size_t bytes = n * 4;
        if (IOConnectCallMethod(c, 11, in, 3, nullptr, 0, nullptr, nullptr,
                                out, &bytes) || bytes != n * 4) return false;
        address += n * 4; out += n; count -= n;
    }
    return true;
}

static inline bool vwrite(io_connect_t c, uint64_t address, const uint32_t *in, size_t count) {
    uint64_t scalar[] = {address, count, 1};
    return count <= 1024 && !IOConnectCallMethod(c, 11, scalar, 3, in, count*4,
        nullptr, nullptr, nullptr, nullptr);
}

static inline nvgsp::AdminRpcResult rpc(io_connect_t c, uint32_t function,
                               const void *params, size_t bytes,
                               uint8_t reply[4096], size_t *replyBytes) {
    uint8_t input[164] = {};
    if (bytes > sizeof(input)-4) return nvgsp::AdminRpcResult::Uncertain;
    memcpy(input, &function, 4); memcpy(input+4, params, bytes);
    memset(reply, 0xff, 4096); *replyBytes = 4096;
    const auto kr = IOConnectCallStructMethod(c, 10, input, bytes+4, reply, replyBytes);
    const auto status = nvgsp::parseAdminRpcReply(function, kr, reply, *replyBytes);
    printf("rpc function=%u transport=%08x bytes=%zu rpc=%08x rm=%08x result=%s\n",
           function, kr, *replyBytes, status.rpcStatus, status.rmStatus,
           status.result == nvgsp::AdminRpcResult::Success ? "success" :
           status.result == nvgsp::AdminRpcResult::Refused ? "refused" : "uncertain");
    fflush(stdout);
    return status.result;
}

static inline nvgsp::AdminRpcResult alloc(io_connect_t c, uint32_t handle, uint32_t cls,
                                  const void *params, size_t bytes,
                                  uint8_t reply[4096], size_t *replyBytes) {
    if (bytes > 128) return nvgsp::AdminRpcResult::Uncertain;
    uint8_t p[160] = {};
    const uint32_t n = (uint32_t)bytes;
    memcpy(p, &client, 4); memcpy(p+4, &parent, 4);
    memcpy(p+8, &handle, 4); memcpy(p+12, &cls, 4); memcpy(p+20, &n, 4);
    memcpy(p+32, params, bytes);
    return rpc(c, 103, p, bytes+32, reply, replyBytes);
}

static inline nvgsp::AdminRpcResult release(io_connect_t c, uint32_t handle) {
    const uint32_t p[] = {client, parent, handle, 0};
    uint8_t reply[4096]; size_t bytes;
    return rpc(c, 10, p, sizeof(p), reply, &bytes);
}
