#include "../drivers/NVGspCore/NVGspRpcReply.hpp"
#include <array>
#include <cassert>
#include <cstdio>
int main() {
    using namespace nvgsp;
    std::array<uint8_t, 484> reply{};
    // Actual live FREE replies are 100 bytes, including selector-10 prefix.
    assert(parseAdminRpcReply(10, 0, reply.data(), 100).result == AdminRpcResult::Success);
    assert(parseAdminRpcReply(103, 0, reply.data(), 100).result == AdminRpcResult::Uncertain);
    assert(parseAdminRpcReply(103, 0, reply.data(), 484).result == AdminRpcResult::Success);
    assert(parseAdminRpcReply(76, 0, reply.data(), 110).result == AdminRpcResult::Success);
    uint32_t refusal = 0x1f;
    memcpy(reply.data() + 96, &refusal, 4);
    assert(parseAdminRpcReply(10, 0, reply.data(), 100).result == AdminRpcResult::Refused);
    assert(parseAdminRpcReply(103, 0, reply.data(), 484).result == AdminRpcResult::Success);
    memcpy(reply.data() + 100, &refusal, 4);
    assert(parseAdminRpcReply(103, 0, reply.data(), 484).rmStatus == refusal);
    assert(parseAdminRpcReply(103, 0, reply.data(), 484).result == AdminRpcResult::Refused);
    assert(parseAdminRpcReply(10, 1, reply.data(), 100).result == AdminRpcResult::Uncertain);
    memcpy(reply.data(), &refusal, 4);
    assert(parseAdminRpcReply(10, 0, reply.data(), 100).result == AdminRpcResult::Uncertain);
    assert(parseAdminRpcReply(10, 0, reply.data(), 99).result == AdminRpcResult::Uncertain);
    assert(parseAdminRpcReply(10, 0, nullptr, 100).result == AdminRpcResult::Uncertain);
    assert(parseAdminRpcReply(999, 0, reply.data(), 484).result == AdminRpcResult::Uncertain);
    {   // header-only status: 54 SET / 79 UNSET_PAGE_DIRECTORY
        std::array<uint8_t, 64> h{};
        assert(parseAdminRpcReply(54, 0, h.data(), 64).result == AdminRpcResult::Success);
        h[0] = 0x1f;
        assert(parseAdminRpcReply(79, 0, h.data(), 64).result == AdminRpcResult::Refused);
        assert(parseAdminRpcReply(54, 1, h.data(), 64).result == AdminRpcResult::Uncertain);
        assert(parseAdminRpcReply(54, 0, h.data(), 2).result == AdminRpcResult::Uncertain);
    }
    puts("admin RPC reply: live FREE/ALLOC/CONTROL sizes, refusals and ambiguity PASS");
}
