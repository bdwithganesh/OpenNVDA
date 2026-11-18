// 0.180.145: GET_SM_ISSUE_RATE_MODIFIER reply decoding (selector 50 op 1),
// on synthetic control-reply records: field decode, names, the property
// string, and rejection of every wrong size / header / status. With
// -DNVGSP_PRIMARY_FAULT_HEADERS the ABI is checked against 615.78.08
// ctrl2080gr.h.
#include "../drivers/NVGspCore/NVGspSmRate615.hpp"
#include "../drivers/NVGspCore/NVGspLifecycle615.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef NVGSP_PRIMARY_FAULT_HEADERS
#include "nvtypes.h"
#include "ctrl/ctrl2080/ctrl2080gr.h"
#include <cstddef>
static_assert(NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER == nvgsp::kSmIssueRateCommand615, "command");
static_assert(sizeof(NV2080_CTRL_GR_ROUTE_INFO) == 16, "route info");
static_assert(sizeof(NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_PARAMS) == nvgsp::kSmIssueRateParams615, "params");
static_assert(offsetof(NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_PARAMS, imla0) == nvgsp::kSmIssueRateFieldOffset615, "imla0");
static_assert(offsetof(NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_PARAMS, fmla32) == nvgsp::kSmIssueRateFieldOffset615 + 3, "fmla32");
static_assert(offsetof(NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_PARAMS, imla4) == nvgsp::kSmIssueRateFieldOffset615 + 8, "imla4");
#endif
using namespace nvgsp;

static void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) { memcpy(b.data() + at, &v, 4); }

// a reply record as GSP returns it for a function-76 control
static std::vector<uint8_t> record(const uint8_t fields[9], uint32_t status, uint32_t rpcResult, size_t total,
                                   uint32_t paramsBytes = kSmIssueRateParams615, uint32_t command = kSmIssueRateCommand615,
                                   uint32_t object = kLifecycleSubdevice615) {
    std::vector<uint8_t> b(total, 0xa5);
    memset(b.data(), 0, total < 96 + 16 ? total : 96 + 16);   // rpc header, record, control header, route
    put32(b, 24, 0x03000000); put32(b, 28, 0x43505256); put32(b, 32, uint32_t(total - 24)); put32(b, 36, 76);
    put32(b, 40, rpcResult); put32(b, 44, 0);
    const ControlHeader615 h{kLifecycleClient615, object, command, status, paramsBytes, 0, 0, 0, 0};
    memcpy(b.data() + 56, &h, sizeof(h));
    if (total >= 96 + 16 + 9) memcpy(b.data() + 96 + 16, fields, 9);   // fields after the 16-byte route; padding stays 0xa5
    return b;
}

int main() {
    const uint8_t f[9] = {0, 1, 0, 1, 0, 2, 3, 4, 6};
    uint8_t v[9]; uint32_t st = 0;
    // exact record: decoded fields
    auto ok = record(f, 0, 0, kSmIssueRateReply615);
    assert(kSmIssueRateReply615 == 128);
    assert(smIssueRateReply615(ok.data(), ok.size(), kLifecycleClient615, kLifecycleSubdevice615, v, &st));
    assert(st == 0 && !memcmp(v, f, 9));
    // names
    assert(!strcmp(smIssueRateField615(3), "fmla32") && !strcmp(smIssueRateField615(8), "imla4"));
    assert(!strcmp(smIssueRateName615(3, 1), "1/2") && !strcmp(smIssueRateName615(0, 0), "full"));
    assert(!strcmp(smIssueRateName615(8, 6), "1/64") && !strcmp(smIssueRateName615(1, 7), "undefined"));
    assert(!strcmp(smIssueRateName615(2, 1), "reduced") && !strcmp(smIssueRateName615(2, 2), "undefined"));
    char text[128];
    const size_t n = smIssueRateString615(v, text, sizeof(text));
    assert(n == strlen(text) &&
           !strcmp(text, "imla0=0,fmla16=1,dp=0,fmla32=1,ffma=0,imla1=2,imla2=3,imla3=4,imla4=6"));
    assert(smIssueRateString615(v, text, 64) == 0);
    // wrong total sizes
    for (size_t total : {size_t(127), size_t(129), size_t(96), size_t(120), size_t(256)}) {
        auto bad = record(f, 0, 0, total);
        assert(!smIssueRateReply615(bad.data(), bad.size(), kLifecycleClient615, kLifecycleSubdevice615, v, &st) && st == ~0U);
    }
    // wrong params size, command, object, client
    { auto b = record(f, 0, 0, kSmIssueRateReply615, 31); assert(!smIssueRateReply615(b.data(), b.size(), kLifecycleClient615, kLifecycleSubdevice615, v, &st)); }
    { auto b = record(f, 0, 0, kSmIssueRateReply615, 33); assert(!smIssueRateReply615(b.data(), b.size(), kLifecycleClient615, kLifecycleSubdevice615, v, &st)); }
    { auto b = record(f, 0, 0, kSmIssueRateReply615, 32, 0x20801231); assert(!smIssueRateReply615(b.data(), b.size(), kLifecycleClient615, kLifecycleSubdevice615, v, &st)); }
    { auto b = record(f, 0, 0, kSmIssueRateReply615, 32, kSmIssueRateCommand615, kLifecycleDevice615); assert(!smIssueRateReply615(b.data(), b.size(), kLifecycleClient615, kLifecycleSubdevice615, v, &st)); }
    assert(!smIssueRateReply615(ok.data(), ok.size(), 0xc0d00002, kLifecycleSubdevice615, v, &st));
    // RM refusal: the status comes back, no fields
    { auto b = record(f, 0x56, 0, kSmIssueRateReply615); assert(!smIssueRateReply615(b.data(), b.size(), kLifecycleClient615, kLifecycleSubdevice615, v, &st) && st == 0x56); }
    { auto b = record(f, 0, 0x1f, kSmIssueRateReply615); assert(!smIssueRateReply615(b.data(), b.size(), kLifecycleClient615, kLifecycleSubdevice615, v, &st) && st == 0x1f); }
    // the params block alone
    assert(parseSmIssueRate615(ok.data() + 96, 32, v) && !parseSmIssueRate615(ok.data() + 96, 31, v) &&
           !parseSmIssueRate615(ok.data() + 96, 33, v) && !parseSmIssueRate615(nullptr, 32, v));
    // a request is a valid diagnostic control (lifecycle client, 0xc0d subdevice)
    alignas(8) uint8_t req[40 + kSmIssueRateParams615]{};
    assert(buildControl615(req, sizeof(req), kLifecycleClient615, kLifecycleSubdevice615, kSmIssueRateCommand615, kSmIssueRateParams615));
    printf("SmIssueRate615: 128-byte reply, route 16 + 9 fields + padding, names, property string, "
           "wrong sizes/header/status rejected%s PASS\n",
#ifdef NVGSP_PRIMARY_FAULT_HEADERS
           ", ctrl2080gr.h ABI"
#else
           ""
#endif
    );
    return 0;
}
