// Host check for drivers/NVDisplay/NVDisplayEdid.hpp against the real BenQ
// PD2705U EDID (256 bytes, read by NVGspControl over DP AUX).
//
//   clang++ -std=c++17 -Wall -Wextra -Werror tests/nvdisplay_edid_check.cpp -o /tmp/x && /tmp/x [edid.bin]
//
// With no argument the embedded copy below is used; with a path, that file
// is checked too (it must match the embedded copy byte for byte).
#include "../NVDisplayEdid.hpp"
#include <cstdio>
#include <cstring>

static const uint8_t kEdid[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x09, 0xd1, 0x39, 0x80, 0x45, 0x54, 0x00, 0x00,
    0x0a, 0x20, 0x01, 0x04, 0xb5, 0x3c, 0x22, 0x78, 0x3f, 0x28, 0x95, 0xa7, 0x55, 0x4e, 0xa3, 0x26,
    0x0f, 0x50, 0x54, 0xa5, 0x6b, 0x80, 0xd1, 0xc0, 0xb3, 0x00, 0xa9, 0xc0, 0x81, 0x80, 0x81, 0x00,
    0x81, 0xc0, 0x01, 0x01, 0x01, 0x01, 0x4d, 0xd0, 0x00, 0xa0, 0xf0, 0x70, 0x3e, 0x80, 0x30, 0x20,
    0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0xff, 0x00, 0x38, 0x33, 0x4e,
    0x30, 0x33, 0x39, 0x32, 0x30, 0x30, 0x31, 0x39, 0x0a, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32,
    0x4c, 0x87, 0x87, 0x3c, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
    0x00, 0x42, 0x65, 0x6e, 0x51, 0x20, 0x50, 0x44, 0x32, 0x37, 0x30, 0x35, 0x55, 0x0a, 0x01, 0xcc,
    0x02, 0x03, 0x45, 0xf1, 0x4f, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x10, 0x1f, 0x22, 0x21, 0x20, 0x04,
    0x13, 0x12, 0x03, 0x01, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0xe2, 0x00, 0xcf, 0x6d,
    0x03, 0x0c, 0x00, 0x20, 0x00, 0x38, 0x78, 0x20, 0x00, 0x60, 0x01, 0x02, 0x03, 0x68, 0x1a, 0x00,
    0x00, 0x01, 0x01, 0x28, 0x3c, 0x00, 0xe3, 0x05, 0xc3, 0x01, 0xe3, 0x0f, 0x18, 0x00, 0xe6, 0x06,
    0x05, 0x01, 0x62, 0x62, 0x00, 0x56, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50, 0x30, 0x20, 0x35,
    0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1e, 0x4d, 0x6c, 0x80, 0xa0, 0x70, 0x70, 0x3e, 0x80, 0x30,
    0x20, 0x3a, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0,
};

static int gFails = 0;
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++gFails;                                                           \
        }                                                                       \
    } while (0)

static const char *srcName(uint8_t s) {
    return s == nvedid::kSourceBaseDtd ? "base-dtd" : s == nvedid::kSourceCtaDtd ? "cta-dtd"
         : s == nvedid::kSourceCtaVic ? "cta-vic" : "standard";
}

static void dump(const nvedid::Info &info) {
    std::printf("EDID %u.%u name \"%s\" ext %u parsed %u bad %u range %u-%u Hz %u-%u kHz %u MHz\n",
                info.version, info.revision, info.name, info.extensionCount,
                info.extensionsParsed, info.badChecksums, info.minVHz, info.maxVHz,
                info.minHKHz, info.maxHKHz, info.maxPixelClockMHz);
    for (uint32_t i = 0; i < info.count; ++i) {
        const nvedid::Timing &t = info.timings[i];
        std::printf("  [%2u] %-8s %4ux%-4u %3u.%02u Hz pclk %7.3f MHz total %ux%u flags 0x%02x\n", i,
                    srcName(t.source), t.hActive, t.vActive, (t.refreshMilliHz + 5) / 1000,
                    ((t.refreshMilliHz + 5) / 10) % 100, t.pixelClockKHz / 1000.0,
                    t.hActive + t.hBlank, t.vActive + t.vBlank, t.flags);
    }
}

static const nvedid::Timing *findCta(const nvedid::Info &info, uint32_t w, uint32_t h) {
    for (uint32_t i = 0; i < info.count; ++i)
        if (info.timings[i].source == nvedid::kSourceCtaDtd && info.timings[i].hActive == w &&
            info.timings[i].vActive == h)
            return &info.timings[i];
    return nullptr;
}

// Refresh rounded to centi-Hz.
static uint32_t centiHz(const nvedid::Timing &t) { return (t.refreshMilliHz + 5) / 10; }

static void checkEdid(const uint8_t *edid, uint32_t len) {
    static nvedid::Info info;
    CHECK(nvedid::parse(edid, len, info));
    dump(info);
    CHECK(info.valid);
    CHECK(info.version == 1 && info.revision == 4);
    CHECK(info.extensionCount == 1 && info.extensionsParsed == 1 && info.badChecksums == 0);
    CHECK(!std::strcmp(info.name, "BenQ PD2705U"));
    CHECK(info.hasRange && info.minVHz == 50 && info.maxVHz == 76);

    CHECK(info.preferred >= 0);
    if (info.preferred >= 0) {
        const nvedid::Timing &p = info.timings[info.preferred];
        CHECK(p.source == nvedid::kSourceBaseDtd);
        CHECK(p.hActive == 3840 && p.vActive == 2160);
        CHECK(p.pixelClockKHz == 533250);
        CHECK(p.hActive + p.hBlank == 4000);
        CHECK(p.vActive + p.vBlank == 2222);
        CHECK(p.hSyncOffset == 48 && p.hSyncWidth == 32);
        CHECK(p.vSyncOffset == 3 && p.vSyncWidth == 5);
        CHECK(centiHz(p) == 6000);
        CHECK((p.flags & nvedid::kFlagHSyncPositive) && !(p.flags & nvedid::kFlagVSyncPositive));
        // 16.16 refresh: 533.25e6 / (4000 * 2222) = 59.9966 Hz.
        const uint32_t fx = nvedid::refreshFixed16(p);
        CHECK(fx > (59u << 16) + 65000u && fx < (60u << 16));
        CHECK(nvedid::findDetailed(info, 3840, 2160) == info.preferred);
    }
    const nvedid::Timing *a = findCta(info, 2560, 1440);
    CHECK(a && centiHz(*a) == 5995 && a->pixelClockKHz == 241500);
    const nvedid::Timing *b = findCta(info, 1920, 2160);
    CHECK(b && centiHz(*b) == 5999 && b->pixelClockKHz == 277250);

    uint32_t standard = 0;
    for (uint32_t i = 0; i < info.count; ++i)
        standard += info.timings[i].source == nvedid::kSourceStandard;
    CHECK(standard == 6);   // 1920x1080 1680x1050 1600x900 1280x1024 1280x800 1280x720
    CHECK(nvedid::findDetailed(info, 1234, 567) == -1);
    // CTA VICs: 1080p60 (VIC 16) as a full CEA timing, 4K60 VIC 97 folded
    // into the base DTD, 4K30 (VIC 95) kept.
    uint32_t vics = 0, v1080 = 0, v4k60 = 0, v4k30 = 0;
    for (uint32_t i = 0; i < info.count; ++i) {
        const nvedid::Timing &t = info.timings[i];
        if (t.source != nvedid::kSourceCtaVic) continue;
        ++vics;
        v1080 += t.hActive == 1920 && t.vActive == 1080 && t.pixelClockKHz == 148500 &&
                 t.hBlank == 280 && centiHz(t) == 6000;
        v4k60 += t.hActive == 3840 && centiHz(t) == 6000;
        v4k30 += t.hActive == 3840 && centiHz(t) == 3000;
    }
    CHECK(vics == 12 && v1080 == 1 && v4k60 == 0 && v4k30 == 1);
}

int main(int argc, char **argv) {
    checkEdid(kEdid, sizeof(kEdid));

    // Base block only: extension advertised but absent, still parses.
    {
        static nvedid::Info info;
        CHECK(nvedid::parse(kEdid, 128, info) && info.extensionsParsed == 0);
    }
    // Corrupt extension checksum: extension skipped, base still fine.
    {
        uint8_t bad[256];
        std::memcpy(bad, kEdid, sizeof(bad));
        bad[0xC6] ^= 0x01;
        static nvedid::Info info;
        CHECK(nvedid::parse(bad, sizeof(bad), info) && info.badChecksums == 1 &&
              info.extensionsParsed == 0 && !findCta(info, 2560, 1440));
        bad[0x40] ^= 0x01;   // corrupt base block: rejected
        CHECK(!nvedid::parse(bad, sizeof(bad), info) && !info.valid && info.count == 0);
    }

    if (argc > 1) {
        FILE *f = std::fopen(argv[1], "rb");
        CHECK(f != nullptr);
        if (f) {
            uint8_t buf[512];
            const size_t n = std::fread(buf, 1, sizeof(buf), f);
            std::fclose(f);
            std::printf("file %s: %zu bytes\n", argv[1], n);
            CHECK(n == sizeof(kEdid) && !std::memcmp(buf, kEdid, sizeof(kEdid)));
            checkEdid(buf, static_cast<uint32_t>(n));
        }
    }

    if (gFails) {
        std::printf("nvdisplay_edid_check: %d FAILURE(S)\n", gFails);
        return 1;
    }
    std::printf("nvdisplay_edid_check: OK\n");
    return 0;
}
