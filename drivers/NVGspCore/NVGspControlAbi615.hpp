#pragma once
// SPDX-FileCopyrightText: Copyright (c) 2008-2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: MIT
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
// Pinned NVIDIA615.78.08 g_rpc-structures.h rpc_gsp_rm_control_v03_00.
// The existing legacy24-byte control envelope is incompatible with this ABI.
#include <stdint.h>
#include <stddef.h>

namespace nvgsp {
struct ControlHeader615 {
    uint32_t client, object, command, status, paramsBytes, rpcFlags;
    uint32_t controlFlags, accessRight;
    uint64_t reserved;
};
static_assert(sizeof(ControlHeader615) == 40, "615 control header ABI");
static_assert(offsetof(ControlHeader615, reserved) == 32, "615 reserved offset");
constexpr uint32_t kControl615HeaderBytes = 40;
constexpr uint32_t kControl615RecordOffset = 24 + 32;
constexpr uint32_t kDeviceInfo615Bytes = 4 + 512 * 48;
constexpr uint32_t kClassList615Capacity = 200;
constexpr uint32_t kClassList615Bytes = 4 + kClassList615Capacity * 4;

inline uint32_t control615U32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline bool buildControl615(void *buffer, size_t bytes, uint32_t client,
                            uint32_t object, uint32_t command,
                            uint32_t paramsBytes) {
    if (!buffer || bytes < kControl615HeaderBytes ||
        paramsBytes > bytes - kControl615HeaderBytes) return false;
    const ControlHeader615 h{client, object, command, 0, paramsBytes, 0, 0, 0, 0};
    __builtin_memcpy(buffer, &h, sizeof(h));
    return true;
}
struct ControlReply615 {
    ControlHeader615 header{};
    uint32_t rpcResult=~0U, privateResult=~0U;
    const uint8_t *params=nullptr;
    bool successful() const {
        return params && !rpcResult && !privateResult && !header.status &&
            !header.rpcFlags;
    }
};
inline bool decodeControlReply615(const void *record, size_t bytes,
                                 uint32_t client, uint32_t object,
                                 uint32_t command, uint32_t paramsBytes,
                                 ControlReply615 &reply) {
    reply = {};
    constexpr size_t offset = kControl615RecordOffset;
    if (!record || bytes < offset + kControl615HeaderBytes) return false;
    const uint8_t *p = static_cast<const uint8_t *>(record);
    const uint32_t length = control615U32(p+32);
    if (control615U32(p+24) != 0x03000000 ||
        control615U32(p+28) != 0x43505256 || control615U32(p+36) != 76 ||
        length < 32 + kControl615HeaderBytes || length > bytes - 24)
        return false;
    ControlHeader615 h{};
    __builtin_memcpy(&h, p+offset, sizeof(h));
    if (h.client != client || h.object != object || h.command != command ||
        h.paramsBytes != paramsBytes ||
        h.paramsBytes > length - 32 - kControl615HeaderBytes || h.reserved)
        return false;
    reply.header=h;
    reply.rpcResult=control615U32(p+40);
    reply.privateResult=control615U32(p+44);
    reply.params=p+offset+kControl615HeaderBytes;
    return true;
}
inline bool deviceInfoCount615(const void *params, size_t bytes, uint32_t &count) {
    count=0;
    if (!params || bytes != kDeviceInfo615Bytes) return false;
    const uint8_t *p=static_cast<const uint8_t *>(params);
    const uint32_t n=control615U32(p);
    if (!n || n>512) return false;
    for (uint32_t i=0; i<n; ++i)
        if (control615U32(p+4+i*48+20)>1) return false; // NvU32 isEngine
    count=n;
    return true;
}
inline bool classListCount615(const void *params, size_t bytes, uint32_t &count) {
    count=0;
    if (!params || bytes != kClassList615Bytes) return false;
    const uint32_t n=control615U32(static_cast<const uint8_t *>(params));
    if (!n || n>kClassList615Capacity) return false;
    count=n;
    return true;
}
} // namespace nvgsp
