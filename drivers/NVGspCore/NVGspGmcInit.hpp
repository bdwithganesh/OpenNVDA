#pragma once

// SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

// 615.78.08 GMCAPI/NVKV wire definitions from NVIDIA's MIT-licensed
// gmcapi_base.h, gmcapi_system_info.h, nvkv.h and kernel_gsp.c, pinned at
// 1a82fbfda8f321b056eb3568850f9316c5e465c5. No hardware access here.
#include "NVGspInitAbi.hpp"

namespace nvgsp {
constexpr uint32_t kGmcInitCommand = 0x10001;
constexpr uint64_t kGmcInitSequence = 0x474d430000000001ULL;
constexpr uint32_t kGmcResponseFlag = 0x01000000;
constexpr uint32_t kGmcNvdmHeader = 0x7e | (0x10de << 8) | (0x26U << 24);
constexpr uint32_t kGmcInitMaxResponse = 48 * 1024;
struct GmcHeader {
    uint32_t command, size;
    uint64_t sequence;
    uint32_t statusOrMaxResponse;
    uint32_t reserved[5];
};
static_assert(sizeof(GmcHeader) == 40, "NVIDIA GMCAPI header ABI");
static_assert(offsetof(GmcHeader, sequence) == 8, "GMCAPI sequence ABI");
// gmcapi_table.h MEMORY_MGMT/1: fire-and-forget MMU_FAULT_QUEUED.
// Recognizing/logging the notice does not mean the GPU fault was serviced.
inline bool parseGmcMmuNotice615(const void *packet,size_t bytes,GmcHeader *out) {
    if (!packet || !out || bytes!=sizeof(GspQueueElementHeader)+sizeof(GmcHeader)) return false;
    GspQueueElementHeader q{};GmcHeader h{};
    __builtin_memcpy(&q,packet,sizeof(q));
    __builtin_memcpy(&h,static_cast<const uint8_t *>(packet)+sizeof(q),sizeof(h));
    if(q.mctpMagic!=kMctpMagic || q.mctpHeader!=kMctpHeader ||
       q.mctpPayloadSize!=bytes || q.nvdmPayloadSize!=sizeof(h) || q.nvdmHeader!=kGmcNvdmHeader ||
       h.command!=0x40001 || h.size || !h.sequence || h.statusOrMaxResponse) return false;
    for(auto v:h.reserved)if(v)return false;
    *out=h;return true;
}
inline uint32_t gmcHypervisorType(uint32_t ebx, uint32_t ecx, uint32_t edx) {
    char vendor[12];
    __builtin_memcpy(vendor,&ebx,4);__builtin_memcpy(vendor+4,&ecx,4);
    __builtin_memcpy(vendor+8,&edx,4);
    static const char names[4][13]={"XenVMMXenVMM","VMwareVMware","Microsoft Hv","KVMKVMKVM"};
    for (unsigned i=0;i<4;++i) {
        bool match=true;
        for (unsigned j=0;j<12;++j) if (vendor[j]!=names[i][j]) match=false;
        if (match) return i;
    }
    return 4;
}

// Every instruction and its padded data occupy whole 64-bit words.
class NvkvWriter {
public:
    NvkvWriter(uint8_t *data, size_t capacity) : data_(data), capacity_(capacity) {}
    bool word(uint64_t value) {
        if (!data_ || used_ > capacity_ || capacity_ - used_ < 8) return false;
        __builtin_memcpy(data_ + used_, &value, 8); used_ += 8; return true;
    }
    bool u32(uint16_t key, uint32_t value) {
        return word((uint64_t(value) << 32) | key);
    }
    bool u64(uint16_t key, uint64_t value) {
        return word((1ULL << 32) | (2ULL << 28) | key) && word(value);
    }
    bool bytes(uint16_t key, const void *data, size_t count) {
        if ((!data && count) || count > UINT32_MAX ||
            count > SIZE_MAX - 7) return false;
        const size_t padded = (count + 7) & ~size_t(7);
        if (used_ > capacity_ || capacity_ - used_ < 8 ||
            padded > capacity_ - used_ - 8) return false;
        if (!word((uint64_t(count) << 32) | (3ULL << 28) | key)) return false;
        for (size_t i=0;i<padded;++i) data_[used_+i]=0;
        if (count) __builtin_memcpy(data_+used_,data,count);
        used_+=padded; return true;
    }
    size_t size() const { return used_; }
private:
    uint8_t *data_; size_t capacity_, used_=0;
};

inline uint32_t gmcRead32(const void *p) {
    uint32_t v=0; __builtin_memcpy(&v,p,4); return v;
}
inline bool gmcEncodeRegistry(NvkvWriter &kv, const uint8_t *registry, size_t bytes) {
    if (!registry) return bytes == 0;
    if (bytes < 8) return false;
    const uint32_t size=gmcRead32(registry), count=gmcRead32(registry+4);
    if (size < 8 || size > bytes || count > (size-8)/16 || count > 32) return false;
    const size_t strings=8+size_t(count)*16;
    for (uint32_t i=0;i<count;++i) {
        const uint8_t *entry=registry+8+i*16;
        const uint32_t name=gmcRead32(entry), value=gmcRead32(entry+8), length=gmcRead32(entry+12);
        if (name < strings || name >= size || !length) return false;
        size_t nameBytes=0;
        while (nameBytes < size-name && registry[name+nameBytes] && nameBytes < 256) ++nameBytes;
        if (!nameBytes || nameBytes >= 256 || nameBytes >= size-name) return false;
        if (!kv.bytes(0x3070,registry+name,nameBytes+1)) return false;
        if (entry[4] == 1) {
            if (length != 4 || !kv.u32(0x3071,value)) return false;
        } else if (entry[4] == 2 || entry[4] == 3) {
            if (value < strings || value > size || length > size-value || length > 4096 ||
                (entry[4] == 3 && registry[value+length-1] != 0) ||
                !kv.bytes(entry[4] == 3 ? 0x3072 : 0x3073,registry+value,length)) return false;
        } else return false;
    }
    return true;
}

inline bool buildGmcInit(uint8_t *record, size_t capacity,
                         const GspSystemInfoParameters &system,
                         const uint8_t *registry, size_t registryBytes,
                         uint32_t *messageBytes) {
    constexpr size_t prefix=sizeof(GspQueueElementHeader)+sizeof(GmcHeader);
    if (messageBytes) *messageBytes=0;
    if (!record || !messageBytes || capacity < prefix || capacity > 65536 ||
        !system.bar0 || !system.bar1 || !system.bar2) return false;
    for (size_t i=0;i<capacity;++i) record[i]=0;
    NvkvWriter kv(record+prefix,capacity-prefix);
    const uint64_t pcieFlags=uint64_t(!!system.gpuBehindBridge) |
        (uint64_t(!!system.flrSupported)<<1) | (uint64_t(!!system.bar0Is64Bit)<<2) |
        (uint64_t(!!system.mnocAvailable)<<3);
    const uint64_t platformFlags=(uint64_t(!!system.isPassthru)<<1) |
                                  (uint64_t(!!system.isPrimary)<<2);
    static const char driverName[]="NVGspControl";
    static const char driverVersion[]="0.180.27";
    if (!kv.u32(0x0001,system.pciDeviceId) ||
        !kv.u32(0x0002,system.pciSubDeviceId) || !kv.u32(0x0003,system.pciRevisionId) ||
        !kv.u32(0x0010,0x88000) || !kv.u32(0x0011,0x1000) ||
        !kv.u32(0x0040,system.hypervisorType) || !kv.u32(0x0041,system.virtualConfigBits) ||
        !kv.u32(0x0052,system.pcieLinkCap) || !kv.u32(0x0070,1) ||
        !kv.u32(0x0071,1) || !kv.u32(0x0078,4096) ||
        !kv.u64(0x1000,pcieFlags) || !kv.u64(0x1001,platformFlags) ||
        !kv.u64(0x1002,0) || !kv.u64(0x1003,0) ||
        !kv.u64(0x1010,system.bar0) || !kv.u64(0x1011,system.bar1) ||
        !kv.u64(0x1012,system.bar2) || !kv.u64(0x1013,0) ||
        !kv.u64(0x1020,system.domainBusDevice) ||
        !kv.u64(0x1030,0) || !kv.u64(0x1031,system.consoleMemSize) ||
        !kv.u64(0x1032,system.maxUserVa) ||
        !kv.bytes(0x1100,driverName,sizeof(driverName)) ||
        !kv.bytes(0x1101,driverVersion,sizeof(driverVersion)) ||
        !kv.u32(0x1102,0) || !kv.u32(0x1110,3) ||
        !gmcEncodeRegistry(kv,registry,registryBytes)) return false;
    GmcHeader gmc{};
    gmc.command=kGmcInitCommand; gmc.size=uint32_t(kv.size());
    gmc.sequence=kGmcInitSequence; gmc.statusOrMaxResponse=kGmcInitMaxResponse;
    GspQueueElementHeader envelope{};
    envelope.mctpMagic=kMctpMagic; envelope.mctpHeader=kMctpHeader;
    envelope.mctpPayloadSize=uint32_t(prefix+kv.size());
    envelope.nvdmHeader=kGmcNvdmHeader;
    envelope.nvdmPayloadSize=uint32_t(sizeof(gmc)+kv.size());
    __builtin_memcpy(record,&envelope,sizeof(envelope));
    __builtin_memcpy(record+sizeof(envelope),&gmc,sizeof(gmc));
    *messageBytes=envelope.mctpPayloadSize; return true;
}

inline bool parseGmcInitResponse(const void *record, size_t bytes, GmcHeader *out) {
    if (!record || !out || bytes < sizeof(GspQueueElementHeader)+sizeof(GmcHeader) ||
        bytes > 65536) return false;
    const uint8_t *p=static_cast<const uint8_t *>(record);
    GspQueueElementHeader q{}; GmcHeader g{};
    __builtin_memcpy(&q,p,sizeof(q));__builtin_memcpy(&g,p+sizeof(q),sizeof(g));
    if (q.mctpMagic!=kMctpMagic || q.mctpHeader!=kMctpHeader ||
        q.nvdmHeader!=kGmcNvdmHeader || q.reserved ||
        q.mctpPayloadSize > bytes || q.mctpPayloadSize < sizeof(q)+sizeof(g) ||
        q.nvdmPayloadSize != q.mctpPayloadSize-sizeof(q) ||
        g.size != q.nvdmPayloadSize-sizeof(g) || g.size > kGmcInitMaxResponse ||
        g.command != (kGmcInitCommand|kGmcResponseFlag) || g.sequence != kGmcInitSequence)
        return false;
    for (auto value:g.reserved) if (value) return false;
    *out=g; return true;
}
} // namespace nvgsp
