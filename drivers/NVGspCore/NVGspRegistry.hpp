#pragma once

#include <stdint.h>

namespace nvgsp {

// GSP-RM registry (SET_REGISTRY, rpc 73) built from a
// "Key=Value;Key2=Value2" spec, the same syntax as NVIDIA's and nouveau's
// NVreg_RegistryDwords. Numbers (decimal or 0x hex) become DWORD entries,
// anything else a STRING entry. Layout (nouveau r535 build_registry /
// NVIDIA PACKED_REGISTRY_TABLE): u32 size, u32 numEntries, numEntries x
// {u32 nameOffset, u8 type, pad[3], u32 data, u32 length}, then per entry:
// NUL-terminated key, [STRING value bytes] data = the dword, or the table
// offset of the string value.
constexpr uint8_t kRegistryTypeDword = 1;
constexpr uint8_t kRegistryTypeString = 3;
constexpr uint32_t kRegistryMaxEntries = 32;
constexpr uint32_t kRegistryEntryBytes = 16;

inline bool registrySpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline bool registryParseNumber(const char *s, uint32_t len, uint32_t *out) {
    if (!len) return false;
    uint64_t v = 0;
    uint32_t i = 0, base = 10;
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    for (; i < len; ++i) {
        const char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = v * base + d;
        if (v > 0xffffffffULL) return false;
    }
    if (base == 16 && len == 2) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

// Builds the table into out[0..cap). Malformed items ("=x", "k=", no '=')
// are skipped; returns false only if nothing fits or the table would
// overflow `cap` / kRegistryMaxEntries. *entriesOut counts accepted items.
inline bool buildRegistry(const char *spec, uint32_t specLen, uint8_t *out, uint32_t cap,
                          uint32_t *bytesOut, uint32_t *entriesOut) {
    if (!out || !bytesOut || !entriesOut || cap < 8) return false;
    struct Item { const char *k; uint32_t kl; const char *v; uint32_t vl; };
    Item items[kRegistryMaxEntries];
    uint32_t n = 0;
    for (uint32_t i = 0; spec && i < specLen;) {
        uint32_t j = i;
        while (j < specLen && spec[j] != ';' && spec[j]) ++j;
        // trim and split at '='
        uint32_t a = i, b = j;
        while (a < b && registrySpace(spec[a])) ++a;
        while (b > a && registrySpace(spec[b - 1])) --b;
        uint32_t eq = a;
        while (eq < b && spec[eq] != '=') ++eq;
        if (eq > a && eq + 1 < b) {
            uint32_t ke = eq, vs = eq + 1;
            while (ke > a && registrySpace(spec[ke - 1])) --ke;
            while (vs < b && registrySpace(spec[vs])) ++vs;
            if (ke > a && vs < b) {
                if (n == kRegistryMaxEntries) return false;
                items[n++] = {spec + a, ke - a, spec + vs, b - vs};
            }
        }
        if (j < specLen && !spec[j]) break;
        i = j + 1;
    }
    uint64_t size = 8 + uint64_t(n) * kRegistryEntryBytes;
    for (uint32_t e = 0; e < n; ++e) {
        uint32_t d;
        size += items[e].kl + 1;
        if (!registryParseNumber(items[e].v, items[e].vl, &d)) size += items[e].vl + 1;
    }
    if (size > cap) return false;
    for (uint32_t i = 0; i < size; ++i) out[i] = 0;
    uint32_t str = 8 + n * kRegistryEntryBytes;
    for (uint32_t e = 0; e < n; ++e) {
        uint8_t *ent = out + 8 + e * kRegistryEntryBytes;
        uint32_t data = 0, length = 4;
        uint8_t type = kRegistryTypeDword;
        const uint32_t name = str;
        for (uint32_t i = 0; i < items[e].kl; ++i) out[str++] = static_cast<uint8_t>(items[e].k[i]);
        out[str++] = 0;
        if (!registryParseNumber(items[e].v, items[e].vl, &data)) {
            type = kRegistryTypeString;
            data = str;
            length = items[e].vl + 1;
            for (uint32_t i = 0; i < items[e].vl; ++i) out[str++] = static_cast<uint8_t>(items[e].v[i]);
            out[str++] = 0;
        }
        __builtin_memcpy(ent, &name, 4);
        ent[4] = type;
        __builtin_memcpy(ent + 8, &data, 4);
        __builtin_memcpy(ent + 12, &length, 4);
    }
    const uint32_t total = static_cast<uint32_t>(size);
    __builtin_memcpy(out, &total, 4);
    __builtin_memcpy(out + 4, &n, 4);
    *bytesOut = total;
    *entriesOut = n;
    return true;
}

}  // namespace nvgsp
