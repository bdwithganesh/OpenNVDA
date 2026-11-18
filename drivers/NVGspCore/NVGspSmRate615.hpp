#pragma once

#include <stddef.h>
#include <stdint.h>

#include "NVGspControlAbi615.hpp"

namespace nvgsp {

// 0.180.145: read-only NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER on the
// lifecycle subdevice (selector 50 op 1). The values are observations of the
// current SM speed selects; nothing here writes them.
//
// NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_PARAMS (615.78.08 ctrl2080gr.h):
//   NV2080_CTRL_GR_ROUTE_INFO grRouteInfo   16 bytes (NvU32 flags, NvU64 route
//                                           8-byte aligned); 0/0 = default GR
//   NvU8 imla0, fmla16, dp, fmla32, ffma, imla1, imla2, imla3, imla4
//   -> 25 bytes, 32 with the struct's 8-byte alignment.
constexpr uint32_t kSmIssueRateCommand615 = 0x20801230;
constexpr uint32_t kSmIssueRateParams615 = 32;
constexpr uint32_t kSmIssueRateFieldOffset615 = 16;
constexpr uint32_t kSmIssueRateFields615 = 9;
// rpc header (24) + record (32 to the control header) + control header (40)
constexpr uint32_t kSmIssueRateReply615 = 96 + kSmIssueRateParams615;

inline const char *smIssueRateField615(uint32_t i) {
    static const char *const kNames[kSmIssueRateFields615] = {
        "imla0", "fmla16", "dp", "fmla32", "ffma", "imla1", "imla2", "imla3", "imla4"};
    return i < kSmIssueRateFields615 ? kNames[i] : "?";
}

// 0 = full rate, n = 1/2^n (up to 1/64); DP only defines 0 (full) and 1
// (reduced). Anything else is reported as "undefined", never guessed.
inline const char *smIssueRateName615(uint32_t field, uint8_t value) {
    static const char *const kRates[7] = {"full", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"};
    if (field == 2) return value == 0 ? "full" : value == 1 ? "reduced" : "undefined";
    return value < 7 ? kRates[value] : "undefined";
}

// The params block of a reply: exactly kSmIssueRateParams615 bytes.
inline bool parseSmIssueRate615(const uint8_t *params, size_t bytes, uint8_t out[kSmIssueRateFields615]) {
    if (!params || !out || bytes != kSmIssueRateParams615) return false;
    for (uint32_t i = 0; i < kSmIssueRateFields615; ++i) out[i] = params[kSmIssueRateFieldOffset615 + i];
    return true;
}

// A whole control reply record: exactly kSmIssueRateReply615 bytes, the
// control header echoed for (client, object, command, params size), then
// either the RM status (*status != 0, false) or the nine fields (true).
// *status is ~0 when the record itself is malformed.
inline bool smIssueRateReply615(const void *record, size_t bytes, uint32_t client, uint32_t object,
                                uint8_t out[kSmIssueRateFields615], uint32_t *status) {
    if (status) *status = ~0U;
    ControlReply615 reply{};
    if (!status || bytes != kSmIssueRateReply615 ||
        !decodeControlReply615(record, bytes, client, object, kSmIssueRateCommand615, kSmIssueRateParams615, reply))
        return false;
    *status = reply.rpcResult ? reply.rpcResult : reply.privateResult ? reply.privateResult : reply.header.status;
    if (*status || reply.header.rpcFlags) return false;
    return parseSmIssueRate615(reply.params, kSmIssueRateParams615, out);
}

// "imla0=0,fmla16=0,...,imla4=0" into s (capacity >= 128); returns the length
inline size_t smIssueRateString615(const uint8_t v[kSmIssueRateFields615], char *s, size_t capacity) {
    size_t n = 0;
    if (!s || capacity < 128) return 0;
    for (uint32_t i = 0; i < kSmIssueRateFields615; ++i) {
        const char *name = smIssueRateField615(i);
        if (i) s[n++] = ',';
        while (*name) s[n++] = *name++;
        s[n++] = '=';
        const uint8_t x = v[i];
        if (x >= 100) s[n++] = char('0' + x / 100);
        if (x >= 10) s[n++] = char('0' + x / 10 % 10);
        s[n++] = char('0' + x % 10);
    }
    s[n] = 0;
    return n;
}

}  // namespace nvgsp
