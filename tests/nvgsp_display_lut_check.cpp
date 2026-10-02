#include "../drivers/NVGspCore/NVGspDisplayLut.hpp"
#include <assert.h>
#include <math.h>
#include <stdio.h>

// Decode table entries independently of the production encoder. OLUT entries
// are fixed-point; interpreting their bits as half must not give identity.
static double half_value(uint16_t bits) {
    const unsigned exponent = (bits >> 10) & 31;
    const unsigned mantissa = bits & 1023;
    return exponent ? ldexp(1.0 + mantissa/1024.0, int(exponent)-15)
                    : ldexp(double(mantissa), -24);
}

int main() {
    uint32_t output[nvgsp::kIdentityLutWords+1];
    uint32_t input[nvgsp::kIdentityLutWords+1];
    for (auto &word : output) word = 0xdeadbeef;
    for (auto &word : input) word = 0xdeadbeef;
    assert(!nvgsp::buildIdentityDisplayLut(nullptr, nvgsp::kIdentityLutWords, false));
    assert(!nvgsp::buildIdentityDisplayLut(output, nvgsp::kIdentityLutWords-1, false));
    for (auto word : output) assert(word == 0xdeadbeef);
    assert(nvgsp::buildIdentityDisplayLut(output, nvgsp::kIdentityLutWords, false));
    assert(nvgsp::buildIdentityDisplayLut(input, nvgsp::kIdentityLutWords, true));
    for (unsigned i = 0; i < 8; ++i) assert(output[i] == 0 && input[i] == 0);
    for (unsigned i = 0; i < 1025; ++i) {
        const unsigned at = (4+i)*2;
        const double identity = double(i < 1024 ? i : 1023)/1024.0;
        const uint16_t out = output[at] & 0xffff, in = input[at] & 0xffff;
        assert(double(out)/65536.0 == identity);
        assert(half_value(in) == identity);
        assert(output[at] >> 16 == out && output[at+1] == out);
        assert(input[at] >> 16 == in && input[at+1] == in);
    }
    assert(output[1032] == 0x80008000 && input[1032] == 0x38003800); // 0.5
    assert(output[nvgsp::kIdentityLutWords] == 0xdeadbeef);
    assert(input[nvgsp::kIdentityLutWords] == 0xdeadbeef);
    puts("display LUT: 1025 decoded identities, distinct encodings, header, tail and bounds PASS");
}
