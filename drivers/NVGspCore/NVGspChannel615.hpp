#pragma once
/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// NVIDIA 615.78.08 commit 1a82fbfda8f321b056eb3568850f9316c5e465c5:
// alloc/alloc_channel.h and nvos.h. Keep the retired 570 ABI separate.
// tools/nvgsp_channel_layout615.cpp compares every field against official SDK.
#include <stddef.h>
#include <stdint.h>
namespace nvgsp {
struct alignas(8) MemoryDesc615 {
    uint64_t base, size;
    uint32_t addressSpace, cacheAttrib;
};
struct alignas(8) ChannelAlloc615 {
    uint32_t hObjectError, hObjectBuffer;
    uint64_t gpFifoOffset;
    uint32_t gpFifoEntries, flags, hContextShare, hVASpace, hHandleVASpace;
    uint32_t hUserdMemory[8];
    uint64_t userdOffset[8];
    uint32_t engineType, cid, subDeviceId, hObjectEccError;
    MemoryDesc615 instanceMem, userdMem, ramfcMem, mthdbufMem;
    uint32_t hPhysChannelGroup, internalFlags;
    MemoryDesc615 errorNotifierMem, eccErrorNotifierMem;
    uint32_t ProcessID, SubProcessID, encryptIv[3], decryptIv[3], hmacNonce[8];
    uint32_t tpcConfigID;
};
struct ChannelGroup615 {
    uint32_t hObjectError, hObjectEccError, hVASpace, engineType;
    uint8_t bIsCallingContextVgpuPlugin;
    uint8_t reservedPadding[3];
    uint32_t reserved, internalFlags;
};
struct ContextShare615 { uint32_t hVASpace, flags, subctxId; };
// g_sdk-structures.h NVOS46/47_PARAMETERS_v2C_05. The wire map omits
// NVOS46_PARAMETERS.flags2; using the public OS ioctl struct would shift fields.
struct alignas(8) DmaMap615 {
    uint32_t client, device, dma, memory;
    uint64_t offset, length;
    uint32_t flags, kindOverride;
    uint64_t dmaOffset;
    uint32_t status, padding;
};
struct alignas(8) DmaUnmap615 {
    uint32_t client, device, dma, memory, flags, padding;
    uint64_t dmaOffset, size;
    uint32_t status, tailPadding;
};
static_assert(sizeof(DmaMap615)==56 && offsetof(DmaMap615,status)==48,"615 DMA map wire ABI");
static_assert(sizeof(DmaUnmap615)==48 && offsetof(DmaUnmap615,status)==40,"615 DMA unmap wire ABI");
static_assert(sizeof(MemoryDesc615)==24,"615 memory descriptor ABI");
static_assert(sizeof(ChannelAlloc615)==376,"615 channel ABI (570 was 368)");
static_assert(offsetof(ChannelAlloc615,hHandleVASpace)==32,"615 handle VA field");
static_assert(offsetof(ChannelAlloc615,hUserdMemory)==36,"615 USERD handles");
static_assert(offsetof(ChannelAlloc615,userdOffset)==72,"615 USERD offsets");
static_assert(offsetof(ChannelAlloc615,engineType)==136,"615 engine type");
static_assert(offsetof(ChannelAlloc615,instanceMem)==152,"615 instance descriptor");
static_assert(offsetof(ChannelAlloc615,mthdbufMem)==224,"615 method descriptor");
static_assert(offsetof(ChannelAlloc615,internalFlags)==252,"615 internal flags");
static_assert(offsetof(ChannelAlloc615,tpcConfigID)==368,"615 trailing field");
static_assert(sizeof(ChannelGroup615)==28,"615 channel group ABI");
static_assert(sizeof(ContextShare615)==12,"615 context share ABI");
}
