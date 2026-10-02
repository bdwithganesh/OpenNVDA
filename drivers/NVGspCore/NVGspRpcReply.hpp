#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace nvgsp {
enum class AdminRpcResult { Success, Refused, Uncertain };
struct AdminRpcReply {
    AdminRpcResult result;
    uint32_t rpcStatus;
    uint32_t rmStatus;
};
// User-client selector 10: u32 RPC result followed by the raw RM queue entry.
// NVOS00 FREE and CONTROL status: raw +92. RM_ALLOC status: raw +96.
inline AdminRpcReply parseAdminRpcReply(uint32_t function, uint32_t transport,
                                       const void *data, size_t bytes) {
    AdminRpcReply out{AdminRpcResult::Uncertain, ~0u, ~0u};
    if (!data) return out;
    const auto *p = static_cast<const uint8_t *>(data);
    if (bytes >= 4) memcpy(&out.rpcStatus, p, 4);
    const size_t statusAt = function == 103 ? 100 :
                            function == 10 || function == 76 ? 96 : 0;
    // Other RPCs (54 SET_PAGE_DIRECTORY, 79 UNSET_PAGE_DIRECTORY) carry their
    // status in the RPC header result only.
    if (function == 54 || function == 79) {
        if (!transport && bytes >= 4) {
            out.rmStatus = out.rpcStatus;
            out.result = out.rpcStatus ? AdminRpcResult::Refused : AdminRpcResult::Success;
        }
        return out;
    }
    if (!statusAt || bytes < statusAt + 4) return out;
    memcpy(&out.rmStatus, p + statusAt, 4);
    if (!transport && !out.rpcStatus)
        out.result = out.rmStatus ? AdminRpcResult::Refused : AdminRpcResult::Success;
    return out;
}
}
