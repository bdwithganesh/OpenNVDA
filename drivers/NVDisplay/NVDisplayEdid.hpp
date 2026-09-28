// NVDisplayEdid.hpp, tiny freestanding EDID parser shared by NVDisplay (kernel)
// and tests/nvdisplay_edid_check.cpp (host).
//
// Plain C++14, <stdint.h> only: no library calls, no allocation, no exceptions.
// Parses the base block (detailed timing descriptors, standard timings, monitor
// name, range limits) and CTA-861 extension blocks (their DTDs). Blocks whose
// checksum fails are skipped; a bad base block rejects the whole EDID.

#ifndef NVDISPLAY_EDID_HPP
#define NVDISPLAY_EDID_HPP

#include <stdint.h>

namespace nvedid {

enum : uint32_t {
    kMaxTimings = 32,
    kBlockSize = 128,
};

enum Source : uint8_t {
    kSourceBaseDtd = 0,
    kSourceStandard = 1,
    kSourceCtaDtd = 2,
    kSourceCtaVic = 3,       // CTA video data block entry, timing from the CEA-861 table
};

enum TimingFlags : uint8_t {
    kFlagPreferred = 0x01,   // first DTD of the base block
    kFlagInterlaced = 0x02,
    kFlagHSyncPositive = 0x04,
    kFlagVSyncPositive = 0x08,
    kFlagDetailed = 0x10,    // Full timing known (DTD); standard timings only have size+rate
};

struct Timing {
    uint32_t pixelClockKHz;  // 0 for standard timings
    uint16_t hActive, hBlank, hSyncOffset, hSyncWidth;
    uint16_t vActive, vBlank, vSyncOffset, vSyncWidth;
    uint16_t hImageMm, vImageMm;
    uint32_t refreshMilliHz; // DTD: exact from clock/totals; standard: nominal * 1000
    uint8_t source;          // Source
    uint8_t flags;           // TimingFlags
    uint16_t reserved;
};

struct Info {
    bool valid;              // base header + checksum OK
    uint8_t version, revision;
    uint8_t extensionCount;  // as advertised in the base block
    uint8_t extensionsParsed;// CTA extensions present in the buffer with good checksum
    uint8_t badChecksums;    // extension blocks rejected
    char name[14];           // monitor name descriptor (0xFC), NUL terminated, trimmed
    bool hasRange;
    uint8_t minVHz, maxVHz, minHKHz, maxHKHz;
    uint16_t maxPixelClockMHz;
    int32_t preferred;       // index into timings, -1 if none
    uint32_t count;
    Timing timings[kMaxTimings];
};

inline bool blockChecksumOk(const uint8_t *b) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < kBlockSize; ++i) sum = static_cast<uint8_t>(sum + b[i]);
    return sum == 0;
}

// Refresh in milli-Hz (rounded) from a pixel clock in kHz and totals.
inline uint32_t refreshMilliHz(uint32_t pclkKHz, uint32_t hTotal, uint32_t vTotal) {
    const uint64_t pixels = static_cast<uint64_t>(hTotal) * vTotal;
    if (!pixels) return 0;
    const uint64_t num = static_cast<uint64_t>(pclkKHz) * 1000ULL * 1000ULL;
    return static_cast<uint32_t>((num + pixels / 2) / pixels);
}

// Refresh as 16.16 fixed point (rounded), as IODisplayModeInformation wants.
inline uint32_t refreshFixed16(const Timing &t) {
    const uint64_t pixels = static_cast<uint64_t>(t.hActive + t.hBlank) * (t.vActive + t.vBlank);
    if (!(t.flags & kFlagDetailed) || !pixels)
        return static_cast<uint32_t>((static_cast<uint64_t>(t.refreshMilliHz) << 16) / 1000);
    uint64_t num = (static_cast<uint64_t>(t.pixelClockKHz) * 1000ULL) << 16;
    if (t.flags & kFlagInterlaced) num *= 2;
    return static_cast<uint32_t>((num + pixels / 2) / pixels);
}

inline bool addTiming(Info &info, const Timing &t) {
    if (info.count >= kMaxTimings) return false;
    info.timings[info.count++] = t;
    return true;
}

// Parses an 18-byte detailed timing descriptor. Returns false for display
// descriptors (pixel clock 0) or nonsense.
inline bool parseDtd(const uint8_t *d, uint8_t source, Timing &t) {
    const uint32_t pclk10k = static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8);
    if (!pclk10k) return false;
    Timing z = {};
    t = z;
    t.pixelClockKHz = pclk10k * 10;
    t.hActive = static_cast<uint16_t>(d[2] | ((d[4] & 0xF0) << 4));
    t.hBlank = static_cast<uint16_t>(d[3] | ((d[4] & 0x0F) << 8));
    t.vActive = static_cast<uint16_t>(d[5] | ((d[7] & 0xF0) << 4));
    t.vBlank = static_cast<uint16_t>(d[6] | ((d[7] & 0x0F) << 8));
    t.hSyncOffset = static_cast<uint16_t>(d[8] | ((d[11] & 0xC0) << 2));
    t.hSyncWidth = static_cast<uint16_t>(d[9] | ((d[11] & 0x30) << 4));
    t.vSyncOffset = static_cast<uint16_t>((d[10] >> 4) | ((d[11] & 0x0C) << 2));
    t.vSyncWidth = static_cast<uint16_t>((d[10] & 0x0F) | ((d[11] & 0x03) << 4));
    t.hImageMm = static_cast<uint16_t>(d[12] | ((d[14] & 0xF0) << 4));
    t.vImageMm = static_cast<uint16_t>(d[13] | ((d[14] & 0x0F) << 8));
    const uint8_t f = d[17];
    t.flags = kFlagDetailed;
    if (f & 0x80) t.flags |= kFlagInterlaced;
    if ((f & 0x18) == 0x18) {          // digital separate sync: bit2 = V, bit1 = H polarity
        if (f & 0x04) t.flags |= kFlagVSyncPositive;
        if (f & 0x02) t.flags |= kFlagHSyncPositive;
    } else if ((f & 0x18) == 0x10) {   // digital composite: bit1 = H polarity
        if (f & 0x02) t.flags |= kFlagHSyncPositive;
    }
    t.source = source;
    if (!t.hActive || !t.vActive) return false;
    uint32_t mhz = refreshMilliHz(t.pixelClockKHz, t.hActive + t.hBlank, t.vActive + t.vBlank);
    if (t.flags & kFlagInterlaced) mhz *= 2;
    t.refreshMilliHz = mhz;
    return true;
}

inline void parseDisplayDescriptor(const uint8_t *d, Info &info) {
    const uint8_t tag = d[3];
    if (tag == 0xFC) {                 // monitor name
        uint32_t n = 0;
        for (uint32_t i = 5; i < 18 && n < sizeof(info.name) - 1; ++i) {
            const uint8_t c = d[i];
            if (c == 0x0A || c == 0x00) break;
            info.name[n++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
        }
        while (n && info.name[n - 1] == ' ') --n;
        info.name[n] = '\0';
    } else if (tag == 0xFD) {          // range limits (EDID 1.4 offset flags in d[4])
        const uint8_t off = d[4];
        info.hasRange = true;
        info.minVHz = d[5];
        info.maxVHz = d[6];
        info.minHKHz = d[7];
        info.maxHKHz = d[8];
        // +255 offsets (EDID 1.4) cannot be represented in uint8_t; clamp.
        if (off & 0x02) info.maxVHz = 0xFF;
        if (off & 0x08) info.maxHKHz = 0xFF;
        info.maxPixelClockMHz = static_cast<uint16_t>(d[9] * 10);
    }
}

inline bool parseStandardTiming(uint8_t b0, uint8_t b1, uint8_t revision, Timing &t) {
    if ((b0 == 0x01 && b1 == 0x01) || b0 == 0x00) return false;
    Timing z = {};
    t = z;
    const uint32_t w = (static_cast<uint32_t>(b0) + 31) * 8;
    uint32_t h = 0;
    switch (b1 >> 6) {
    case 0: h = revision >= 3 ? w * 10 / 16 : w; break;   // 16:10 (1:1 before 1.3)
    case 1: h = w * 3 / 4; break;
    case 2: h = w * 4 / 5; break;
    default: h = w * 9 / 16; break;
    }
    t.hActive = static_cast<uint16_t>(w);
    t.vActive = static_cast<uint16_t>(h);
    t.refreshMilliHz = ((b1 & 0x3F) + 60u) * 1000u;
    t.source = kSourceStandard;
    return true;
}

// CEA-861 progressive formats we can drive (VIC, pclk kHz, hActive, hBlank,
// hFront, hSync, vActive, vBlank, vFront, vSync); all +h +v except VIC 1.
struct VicTiming { uint8_t vic; uint32_t pclkKHz; uint16_t v[8]; };
static const VicTiming kVics[] = {
    {1, 25175, {640, 160, 16, 96, 480, 45, 10, 2}},
    {4, 74250, {1280, 370, 110, 40, 720, 30, 5, 5}},
    {16, 148500, {1920, 280, 88, 44, 1080, 45, 4, 5}},
    {19, 74250, {1280, 700, 440, 40, 720, 30, 5, 5}},
    {31, 148500, {1920, 720, 528, 44, 1080, 45, 4, 5}},
    {32, 74250, {1920, 830, 638, 44, 1080, 45, 4, 5}},
    {33, 74250, {1920, 720, 528, 44, 1080, 45, 4, 5}},
    {34, 74250, {1920, 280, 88, 44, 1080, 45, 4, 5}},
    {93, 297000, {3840, 1660, 1276, 88, 2160, 90, 8, 10}},
    {94, 297000, {3840, 1440, 1056, 88, 2160, 90, 8, 10}},
    {95, 297000, {3840, 560, 176, 88, 2160, 90, 8, 10}},
    {96, 594000, {3840, 1440, 1056, 88, 2160, 90, 8, 10}},
    {97, 594000, {3840, 560, 176, 88, 2160, 90, 8, 10}},
};

inline bool vicTiming(uint8_t vic, Timing &t) {
    for (const VicTiming &k : kVics) {
        if (k.vic != vic) continue;
        Timing z = {};
        t = z;
        t.pixelClockKHz = k.pclkKHz;
        t.hActive = k.v[0]; t.hBlank = k.v[1]; t.hSyncOffset = k.v[2]; t.hSyncWidth = k.v[3];
        t.vActive = k.v[4]; t.vBlank = k.v[5]; t.vSyncOffset = k.v[6]; t.vSyncWidth = k.v[7];
        t.flags = kFlagDetailed;
        if (vic != 1) t.flags |= kFlagHSyncPositive | kFlagVSyncPositive;
        t.source = kSourceCtaVic;
        t.refreshMilliHz = refreshMilliHz(t.pixelClockKHz, t.hActive + t.hBlank, t.vActive + t.vBlank);
        return true;
    }
    return false;
}

// Same active size and refresh within 0.5 Hz: a VIC repeating a DTD.
inline bool haveTiming(const Info &info, const Timing &t) {
    for (uint32_t i = 0; i < info.count; ++i) {
        const Timing &o = info.timings[i];
        const uint32_t d = o.refreshMilliHz > t.refreshMilliHz ? o.refreshMilliHz - t.refreshMilliHz
                                                               : t.refreshMilliHz - o.refreshMilliHz;
        if ((o.flags & kFlagDetailed) && o.hActive == t.hActive && o.vActive == t.vActive && d < 500)
            return true;
    }
    return false;
}

inline void parseCta(const uint8_t *b, Info &info) {
    // b[0] = 0x02 tag, b[1] = revision, b[2] = DTD offset (0 or 4: no DTDs).
    const uint32_t dtdOff = b[2];
    if (dtdOff < 4 || dtdOff > kBlockSize - 1) return;
    // Data blocks at 4..dtdOff: video data block (tag 2) lists VICs.
    for (uint32_t i = 4; i < dtdOff;) {
        const uint32_t tag = b[i] >> 5, n = b[i] & 31;
        if (i + 1 + n > dtdOff) break;
        if (tag == 2) {
            for (uint32_t j = 0; j < n; ++j) {
                Timing t;
                if (vicTiming(b[i + 1 + j] & 0x7F, t) && !haveTiming(info, t)) addTiming(info, t);
            }
        }
        i += 1 + n;
    }
    for (uint32_t off = dtdOff; off + 18 <= kBlockSize - 1; off += 18) {
        Timing t;
        if (!parseDtd(b + off, kSourceCtaDtd, t)) break;   // zero pixel clock ends the list
        addTiming(info, t);
    }
}

// Parses len bytes of EDID (base + extensions). Always initialises *info.
inline bool parse(const uint8_t *edid, uint32_t len, Info &info) {
    // Zero in place: Info is ~1.3 KB, no stack temporary.
    uint8_t *raw = reinterpret_cast<uint8_t *>(&info);
    for (uint32_t i = 0; i < sizeof(Info); ++i) raw[i] = 0;
    info.preferred = -1;
    const uint8_t hdr[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (!edid || len < kBlockSize) return false;
    for (uint32_t i = 0; i < 8; ++i)
        if (edid[i] != hdr[i]) return false;
    if (!blockChecksumOk(edid)) return false;
    info.valid = true;
    info.version = edid[0x12];
    info.revision = edid[0x13];
    info.extensionCount = edid[0x7E];

    // Detailed timing / display descriptors at 0x36, 0x48, 0x5A, 0x6C.
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t *d = edid + 0x36 + i * 18;
        Timing t;
        if (parseDtd(d, kSourceBaseDtd, t)) {
            if (i == 0) t.flags |= kFlagPreferred;   // mandatory since EDID 1.4
            if (addTiming(info, t) && i == 0)
                info.preferred = static_cast<int32_t>(info.count - 1);
        } else if (d[0] == 0 && d[1] == 0) {
            parseDisplayDescriptor(d, info);
        }
    }
    // Standard timings at 0x26..0x35.
    for (uint32_t i = 0; i < 8; ++i) {
        Timing t;
        if (parseStandardTiming(edid[0x26 + i * 2], edid[0x27 + i * 2], info.revision, t))
            addTiming(info, t);
    }
    // Extension blocks actually present in the buffer.
    for (uint32_t blk = 1; blk <= info.extensionCount && (blk + 1) * kBlockSize <= len; ++blk) {
        const uint8_t *b = edid + blk * kBlockSize;
        if (!blockChecksumOk(b)) {
            ++info.badChecksums;
            continue;
        }
        if (b[0] == 0x02) {
            ++info.extensionsParsed;
            parseCta(b, info);
        }
    }
    return true;
}

// Index of the best DTD for a given active size: the preferred one if it
// matches, else the first detailed timing with that size; -1 if none.
inline int32_t findDetailed(const Info &info, uint32_t w, uint32_t h) {
    if (info.preferred >= 0) {
        const Timing &p = info.timings[info.preferred];
        if (p.hActive == w && p.vActive == h && !(p.flags & kFlagInterlaced)) return info.preferred;
    }
    for (uint32_t i = 0; i < info.count; ++i) {
        const Timing &t = info.timings[i];
        if ((t.flags & kFlagDetailed) && !(t.flags & kFlagInterlaced) &&
            t.hActive == w && t.vActive == h)
            return static_cast<int32_t>(i);
    }
    return -1;
}

}  // namespace nvedid

#endif  // NVDISPLAY_EDID_HPP
