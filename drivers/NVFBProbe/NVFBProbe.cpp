// Read-only RTX 4080 survey. Sonoma already has a working GOP desktop through IONDRVFramebuffer.
// This probe declines attachment so it does not take over the display or initialize the GPU.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSKextLib.h>
#include "../NVGspCore/NVGspDmaBuffer.hpp"
#include "../NVGspCore/NVGspFalcon.hpp"
#include "../NVGspCore/NVGspFbLayout.hpp"
#include "../NVGspCore/NVGspFwsecStaging.hpp"
#include "../NVGspCore/NVGspVbios.hpp"
#include "../NVGspCore/NVGspPackage.hpp"
#include "../NVGspCore/NVGspStaging.hpp"
#include "../NVGspCore/NVGspBooterStaging.hpp"
#include "../NVGspCore/NVGspInit.hpp"

#ifdef NVGSP_EMBEDDED_PACKAGE
extern "C" const UInt8 nvgsp_embedded_start[];
extern "C" const UInt8 nvgsp_embedded_end[];
#endif

class NVFBProbe : public IOService {
    OSDeclareDefaultStructors(NVFBProbe)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
};

OSDefineMetaClassAndStructors(NVFBProbe, IOService)

namespace {
constexpr IOByteCount kBoot0 = 0x00000000;
constexpr IOByteCount kBoot42 = 0x00000A00;
constexpr IOByteCount kWpr2Lo = 0x001FA824;
constexpr IOByteCount kWpr2Hi = 0x001FA828;
constexpr IOByteCount kGspRiscvCpuCtl = 0x00118388;
constexpr IOByteCount kUsableFbSizeMb = 0x001183A4;
constexpr IOByteCount kVgaWorkspaceBase = 0x00625F04;
constexpr IOByteCount kHeadStateBase = 0x00612078;
constexpr IOByteCount kHeadStride = 0x800;
constexpr IOByteCount kSec2Ucode3Fuse = 0x00824148;
constexpr IOByteCount kGspUcode9Fuse = 0x008241E0;
constexpr IOByteCount kGspFalconMailbox0 = 0x00110040;
constexpr IOByteCount kGspFalconMailbox1 = 0x00110044;
constexpr IOByteCount kGspFalconRm = 0x00110084;
constexpr IOByteCount kGspFalconHwcfg2 = 0x001100F4;
constexpr IOByteCount kGspFalconCpuCtl = 0x00110100;
constexpr IOByteCount kGspFalconBootVec = 0x00110104;
constexpr IOByteCount kGspFalconDmaCtl = 0x0011010C;
constexpr IOByteCount kGspFalconDmaTrfCmd = 0x00110118;
constexpr IOByteCount kGspFalconEngine = 0x001103C0;
constexpr IOByteCount kGspFbifTranscfg0 = 0x00110600;
constexpr IOByteCount kGspFbifCtl = 0x00110624;
constexpr IOByteCount kGspBromModSel = 0x00111180;
constexpr IOByteCount kGspBromUcodeId = 0x00111198;
constexpr IOByteCount kGspBromEngineMask = 0x0011119C;
constexpr IOByteCount kGspBromParaAddr0 = 0x00111210;
constexpr IOByteCount kGspRiscvBcrCtl = 0x00111668;
constexpr IOByteCount kFwsecFrtsScratch = 0x00001438;
constexpr IOByteCount kPromBase = 0x00300000;
constexpr size_t kPromBytes = 0x100000;

bool readBar0(IOMemoryMap *map, IOByteCount offset, UInt32 *value) {
    if (!map || !value || offset > map->getLength() ||
        map->getLength() - offset < sizeof(UInt32)) return false;
    const volatile UInt32 *reg = reinterpret_cast<const volatile UInt32 *>(
        map->getVirtualAddress() + offset);
    *value = *reg;
    return true;
}
bool writeBar0(IOMemoryMap *map, IOByteCount offset, UInt32 value) {
    if (!map || offset > map->getLength() ||
        map->getLength() - offset < sizeof(UInt32)) return false;
    volatile UInt32 *reg = reinterpret_cast<volatile UInt32 *>(
        map->getVirtualAddress() + offset);
    *reg = value;
    OSSynchronizeIO();
    return true;
}
struct Bar0Io {
    IOMemoryMap *map;
    bool read(uint32_t offset, uint32_t *value) {
        return readBar0(map, offset, value);
    }
    bool write(uint32_t offset, uint32_t value) {
        return writeBar0(map, offset, value);
    }
    void delay(uint32_t microseconds) { IODelay(microseconds); }
};

void stageGspPackage(IOPCIDevice *pci, const void *bytes, uint64_t packageBytes,
                     uint32_t fbSizeMb, uint32_t vgaWorkspace,
                     uint32_t sec2FuseVersion) {
    if (!pci || !bytes) return;
    nvgsp::PackageView package[5]{};
    const bool packageOk = nvgsp::parsePackage(bytes, packageBytes, package);
    nvgsp::BootUcodeDesc bootDesc{};
    nvgsp::BooterView booterView{};
    if (packageOk) __builtin_memcpy(&bootDesc,
        package[nvgsp::kPackageBootDescriptor - 1].data, sizeof(bootDesc));
    const bool booterParseOk = packageOk && nvgsp::parseBooterLoad(
        package[nvgsp::kPackageBooterLoad - 1].data,
        package[nvgsp::kPackageBooterLoad - 1].size, &booterView);
    nvgsp::FbLayout layout{};
    const bool layoutOk = packageOk && fbSizeMb && nvgsp::planAd103FbLayout(
        static_cast<uint64_t>(fbSizeMb) << 20,
        (vgaWorkspace & (1U << 3)) != 0,
        static_cast<uint64_t>(vgaWorkspace & 0xffffff00U) << 8,
        package[nvgsp::kPackageBootImage - 1].size,
        package[nvgsp::kPackageFwImage - 1].size, &layout);
    nvgsp::GspStaging gsp;
    const bool gspOk = layoutOk && gsp.stage(layout, bootDesc,
        package[nvgsp::kPackageFwImage - 1].data,
        package[nvgsp::kPackageFwImage - 1].size,
        package[nvgsp::kPackageSignature - 1].data,
        package[nvgsp::kPackageSignature - 1].size,
        package[nvgsp::kPackageBootImage - 1].data,
        package[nvgsp::kPackageBootImage - 1].size);
    nvgsp::BooterStaging booter;
    const bool booterOk = booterParseOk && sec2FuseVersion != UINT32_MAX &&
        booter.stage(booterView, sec2FuseVersion);
    nvgsp::GspInitStaging init;
    // NVGspCore's stage() now takes the system-info block; this probe
    // only stages buffers, so an empty block is enough.
    const bool initOk = init.stage(nvgsp::GspSystemInfoParameters{});
    pci->setProperty("NVFBProbe-GSP-package-bytes", packageBytes, 64);
    pci->setProperty("NVFBProbe-GSP-package-ok", packageOk);
    pci->setProperty("NVFBProbe-GSP-stage-layout-ok", layoutOk);
    pci->setProperty("NVFBProbe-GSP-firmware-stage-ok", gspOk);
    pci->setProperty("NVFBProbe-GSP-booter-stage-ok", booterOk);
    pci->setProperty("NVFBProbe-GSP-init-stage-ok", initOk);
    pci->setProperty("NVFBProbe-GSP-metadata-bus", gsp.metadataBusAddress(), 64);
    pci->setProperty("NVFBProbe-GSP-booter-bus", booter.busAddress(), 64);
    pci->setProperty("NVFBProbe-GSP-libos-args-bus", init.libosArgsBus(), 64);
}

struct GspResourceContext {
    IOPCIDevice *pci;
    uint32_t fbSizeMb, vgaWorkspace, sec2FuseVersion;
};
void gspResourceCallback(OSKextRequestTag, OSReturn result, const void *data,
                         uint32_t length, void *opaque) {
    GspResourceContext *ctx = static_cast<GspResourceContext *>(opaque);
    if (!ctx || !ctx->pci) return;
    ctx->pci->setProperty("NVFBProbe-GSP-resource-result", result, 32);
    ctx->pci->setProperty("NVFBProbe-GSP-resource-bytes", length, 32);
    if (result == kOSReturnSuccess && data)
        stageGspPackage(ctx->pci, data, length, ctx->fbSizeMb,
                        ctx->vgaWorkspace, ctx->sec2FuseVersion);
    ctx->pci->release();
    IOFree(ctx, sizeof(*ctx));
}
}

IOService *NVFBProbe::probe(IOService *provider, SInt32 *score) {
    if (!IOService::probe(provider, score)) return nullptr;
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;

    const UInt16 vendor = pci->configRead16(kIOPCIConfigVendorID);
    const UInt16 device = pci->configRead16(kIOPCIConfigDeviceID);
    // Defense in depth: refuse anything but the GPU under development, even if plist matching changes.
    if (vendor != 0x10DE || device != 0x2704) return nullptr;

    IOLog("NVFBProbe: ==== begin ==== vendor=%04x device=%04x class=%06x\n",
          vendor, device, static_cast<unsigned int>(pci->configRead32(kIOPCIConfigRevisionID) >> 8));

    // Console/framebuffer the kernel is drawing on right now.
    PE_Video info;
    bzero(&info, sizeof(info));
    IOPlatformExpert *platform = getPlatform();
    if (platform) {
        platform->getConsoleInfo(&info);
        IOLog("NVFBProbe: console base=0x%llx %lux%lu depth=%lu rowBytes=%lu\n",
              static_cast<unsigned long long>(info.v_baseAddr),
              static_cast<unsigned long>(info.v_width),
              static_cast<unsigned long>(info.v_height),
              static_cast<unsigned long>(info.v_depth),
              static_cast<unsigned long>(info.v_rowBytes));
    } else {
        IOLog("NVFBProbe: no platform expert\n");
    }

    // BAR layout, and which BAR (if any) contains the console base.
    for (int i = 0; i < 6; i++) {
        const UInt8 reg = static_cast<UInt8>(kIOPCIConfigBaseAddress0 + i * 4);
        IODeviceMemory *mem = pci->getDeviceMemoryWithRegister(reg);
        if (!mem) continue;
        const IOPhysicalAddress start = mem->getPhysicalAddress();
        const IOByteCount len = mem->getLength();
        const bool holdsConsole = info.v_baseAddr >= start &&
                                  info.v_baseAddr < start + len;
        IOLog("NVFBProbe: BAR%d reg=0x%02x phys=0x%llx len=0x%llx%s\n", i, reg,
              static_cast<unsigned long long>(start),
              static_cast<unsigned long long>(len),
              holdsConsole ? "   <== CONSOLE IS HERE" : "");
    }

    // Read only stable identification/status registers. BAR0 is never written and bus mastering
    // is never enabled here. A failed map or short BAR simply leaves the register survey absent.
    IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IOMemoryMap *map = bar0 ? bar0->map() : nullptr;
    if (map) {
        UInt32 boot0 = 0, boot42 = 0, wprLo = 0, wprHi = 0, gspCtl = 0;
        const bool bootIdOk = readBar0(map, kBoot0, &boot0) &&
                              readBar0(map, kBoot42, &boot42);
        if (bootIdOk) {
            const UInt32 architecture = ((boot0 >> 24) & 0x1f) | (((boot0 >> 8) & 1) << 5);
            const UInt32 implementation = (boot0 >> 20) & 0xf;
            IOLog("NVFBProbe: BOOT0=0x%08x BOOT42=0x%08x arch=0x%x impl=0x%x\n",
                  boot0, boot42, architecture, implementation);
        }
        if (readBar0(map, kWpr2Lo, &wprLo) && readBar0(map, kWpr2Hi, &wprHi)) {
            IOLog("NVFBProbe: WPR2 lo=0x%08x hi=0x%08x enabled=%u\n",
                  wprLo, wprHi, (wprHi >> 31) & 1);
        }
        if (readBar0(map, kGspRiscvCpuCtl, &gspCtl)) {
            IOLog("NVFBProbe: GSP RISC-V CPUCTL=0x%08x\n", gspCtl);
        }
        UInt32 fbSizeMb = 0, vgaWorkspace = 0;
        if (readBar0(map, kUsableFbSizeMb, &fbSizeMb) &&
            readBar0(map, kVgaWorkspaceBase, &vgaWorkspace)) {
            const bool valid = (vgaWorkspace & (1U << 3)) != 0;
            const unsigned long long workspaceOffset =
                static_cast<unsigned long long>(vgaWorkspace & 0xffffff00U) << 8;
            IOLog("NVFBProbe: usable-FB=%u MiB VGA-workspace-reg=0x%08x valid=%u offset=0x%llx\n",
                  fbSizeMb, vgaWorkspace, valid, workspaceOffset);
        }
        for (unsigned head = 0; head < 4; ++head) {
            UInt32 state = 0;
            if (readBar0(map, kHeadStateBase + head * kHeadStride, &state)) {
                IOLog("NVFBProbe: head%u state=0x%08x mode=%u\n",
                      head, state, (state >> 8) & 3);
            }
        }
        UInt32 fuseRaw = 0;
        UInt32 sec2FuseVersion = UINT32_MAX;
        if (readBar0(map, kSec2Ucode3Fuse, &fuseRaw)) {
            sec2FuseVersion = 0;
            UInt32 bits = fuseRaw;
            while (bits) { ++sec2FuseVersion; bits >>= 1; }
            pci->setProperty("NVFBProbe-SEC2-ucode3-fuse-raw", fuseRaw, 32);
            pci->setProperty("NVFBProbe-SEC2-ucode3-fuse-version", sec2FuseVersion, 32);
            IOLog("NVFBProbe: SEC2 ucode3 fuse raw=0x%08x version=%u\n",
                  fuseRaw, sec2FuseVersion);
        }
        UInt32 gspFuseVersion = UINT32_MAX;
        if (readBar0(map, kGspUcode9Fuse, &fuseRaw)) {
            gspFuseVersion = 0;
            UInt32 bits = fuseRaw;
            while (bits) { ++gspFuseVersion; bits >>= 1; }
            pci->setProperty("NVFBProbe-GSP-ucode9-fuse-raw", fuseRaw, 32);
            pci->setProperty("NVFBProbe-GSP-ucode9-fuse-version", gspFuseVersion, 32);
        }
        int resetArg = 0, fwsecArg = 0;
        const bool wantsReset = PE_parse_boot_argn("nvgspreset", &resetArg, sizeof(resetArg)) && resetArg == 1;
        const bool wantsFwsec = PE_parse_boot_argn("nvgspfwsec", &fwsecArg, sizeof(fwsecArg)) && fwsecArg == 1;
#ifdef NVGSP_EMBEDDED_PACKAGE
        int stageArg = 0;
        const bool wantsStage = PE_parse_boot_argn("nvgspstage", &stageArg, sizeof(stageArg)) && stageArg == 1;
        if (wantsStage && !pci->getProperty("NVFBProbe-GSP-stage-attempted")) {
            pci->setProperty("NVFBProbe-GSP-stage-attempted", true);
            const uint64_t packageBytes = static_cast<uint64_t>(
                nvgsp_embedded_end - nvgsp_embedded_start);
            stageGspPackage(pci, nvgsp_embedded_start, packageBytes,
                            fbSizeMb, vgaWorkspace, sec2FuseVersion);
        }
#endif
        int resourceArg = 0;
        const bool wantsResource = PE_parse_boot_argn(
            "nvgspresource", &resourceArg, sizeof(resourceArg)) && resourceArg == 1;
        if (wantsResource &&
            !pci->getProperty("NVFBProbe-GSP-resource-attempted")) {
            pci->setProperty("NVFBProbe-GSP-resource-attempted", true);
            auto *ctx = static_cast<GspResourceContext *>(IOMalloc(sizeof(GspResourceContext)));
            if (ctx) {
                *ctx = GspResourceContext{pci, fbSizeMb, vgaWorkspace, sec2FuseVersion};
                pci->retain();
                OSKextRequestTag tag = kOSKextRequestTagInvalid;
                const OSReturn request = OSKextRequestResource(
                    "org.local.macosdevicelab.NVFBProbe", "ad103-gsp-570.144.pkg",
                    gspResourceCallback, ctx, &tag);
                pci->setProperty("NVFBProbe-GSP-resource-request", request, 32);
                pci->setProperty("NVFBProbe-GSP-resource-tag", tag, 32);
                if (request != kOSReturnSuccess) {
                    pci->release();
                    IOFree(ctx, sizeof(*ctx));
                }
            }
        }
        bool gspResetReady = false;
        if ((wantsReset || wantsFwsec) && !pci->getProperty("NVFBProbe-GSP-reset-attempted")) {
            UInt32 beforeHwcfg2 = 0, beforeCpuCtl = 0, afterHwcfg2 = 0, afterCpuCtl = 0;
            readBar0(map, kGspFalconHwcfg2, &beforeHwcfg2);
            readBar0(map, kGspFalconCpuCtl, &beforeCpuCtl);
            pci->setProperty("NVFBProbe-GSP-reset-attempted", true);
            pci->setProperty("NVFBProbe-GSP-reset-before-hwcfg2", beforeHwcfg2, 32);
            pci->setProperty("NVFBProbe-GSP-reset-before-cpuctl", beforeCpuCtl, 32);

            bool resetOk = writeBar0(map, kGspFalconEngine, 1);
            UInt32 propagation = 0;
            for (unsigned i = 0; i < 10; ++i)
                resetOk = readBar0(map, kGspFalconEngine, &propagation) && resetOk;
            resetOk = writeBar0(map, kGspFalconEngine, 0) && resetOk;
            for (unsigned i = 0; i < 10; ++i)
                resetOk = readBar0(map, kGspFalconEngine, &propagation) && resetOk;

            bool scrubDone = false;
            for (unsigned i = 0; i < 5000; ++i) {
                if (readBar0(map, kGspFalconHwcfg2, &afterHwcfg2) &&
                    !(afterHwcfg2 & (1U << 12))) {
                    scrubDone = true;
                    break;
                }
                IODelay(10);
            }
            UInt32 beforeBcr = 0, afterBcr = 0, falconRm = 0;
            bool switchOk = true;
            if (afterHwcfg2 & (1U << 10)) {
                readBar0(map, kGspRiscvBcrCtl, &beforeBcr);
                switchOk = writeBar0(map, kGspRiscvBcrCtl, 0);
                bool valid = false;
                for (unsigned i = 0; i < 200000; ++i) {
                    if (readBar0(map, kGspRiscvBcrCtl, &afterBcr) && (afterBcr & 1)) {
                        valid = true;
                        break;
                    }
                    IODelay(10);
                }
                switchOk = switchOk && valid;
            }
            const bool identityOk = bootIdOk && writeBar0(map, kGspFalconRm, boot0) &&
                                    readBar0(map, kGspFalconRm, &falconRm) &&
                                    falconRm == boot0;
            readBar0(map, kGspFalconCpuCtl, &afterCpuCtl);
            pci->setProperty("NVFBProbe-GSP-reset-after-hwcfg2", afterHwcfg2, 32);
            pci->setProperty("NVFBProbe-GSP-reset-after-cpuctl", afterCpuCtl, 32);
            pci->setProperty("NVFBProbe-GSP-reset-bcr-before", beforeBcr, 32);
            pci->setProperty("NVFBProbe-GSP-reset-bcr-after", afterBcr, 32);
            pci->setProperty("NVFBProbe-GSP-reset-falcon-rm", falconRm, 32);
            pci->setProperty("NVFBProbe-GSP-reset-switch-ok", switchOk);
            pci->setProperty("NVFBProbe-GSP-reset-identity-ok", identityOk);
            gspResetReady = resetOk && scrubDone && switchOk && identityOk;
            pci->setProperty("NVFBProbe-GSP-reset-ok", gspResetReady);
            IOLog("NVFBProbe: GSP reset-only before hwcfg2=0x%08x cpuctl=0x%08x "
                  "after hwcfg2=0x%08x cpuctl=0x%08x bcr=0x%08x rm=0x%08x ok=%u\n",
                  beforeHwcfg2, beforeCpuCtl, afterHwcfg2, afterCpuCtl,
                  afterBcr, falconRm,
                  gspResetReady);
        }
        struct SurveyRegister { IOByteCount offset; const char *property; };
        const SurveyRegister gspSurvey[] = {
            {kGspFalconMailbox0, "NVFBProbe-GSP-mailbox0"},
            {kGspFalconMailbox1, "NVFBProbe-GSP-mailbox1"},
            {kGspFalconRm, "NVFBProbe-GSP-falcon-rm"},
            {kGspFalconHwcfg2, "NVFBProbe-GSP-hwcfg2"},
            {kGspFalconCpuCtl, "NVFBProbe-GSP-falcon-cpuctl"},
            {kGspFalconBootVec, "NVFBProbe-GSP-bootvec"},
            {kGspFalconDmaCtl, "NVFBProbe-GSP-dmactl"},
            {kGspFalconDmaTrfCmd, "NVFBProbe-GSP-dmatrfcmd"},
            {kGspFalconEngine, "NVFBProbe-GSP-engine"},
            {kGspFbifTranscfg0, "NVFBProbe-GSP-fbif-transcfg0"},
            {kGspFbifCtl, "NVFBProbe-GSP-fbif-ctl"},
            {kGspBromModSel, "NVFBProbe-GSP-brom-modsel"},
            {kGspBromUcodeId, "NVFBProbe-GSP-brom-ucode-id"},
            {kGspBromEngineMask, "NVFBProbe-GSP-brom-engine-mask"},
            {kGspBromParaAddr0, "NVFBProbe-GSP-brom-paraaddr0"},
            {kGspRiscvBcrCtl, "NVFBProbe-GSP-riscv-bcr"},
            {kFwsecFrtsScratch, "NVFBProbe-GSP-frts-scratch"},
        };
        for (const auto &reg : gspSurvey) {
            UInt32 value = 0;
            if (readBar0(map, reg.offset, &value))
                pci->setProperty(reg.property, value, 32);
        }
        // Read NVIDIA's published 1 MiB PROM aperture. Accept it only after
        // walking a valid PCI expansion-ROM image chain to its LAST image.
        UInt8 *rom = static_cast<UInt8 *>(IOMalloc(kPromBytes));
        if (rom && map->getLength() >= kPromBase + kPromBytes) {
            for (size_t i = 0; i < kPromBytes; i += 4) {
                UInt32 word = 0;
                readBar0(map, kPromBase + i, &word);
                __builtin_memcpy(rom + i, &word, sizeof(word));
            }
            size_t offset = 0, actual = 0;
            unsigned images = 0;
            while (offset + 0x1a < kPromBytes && images < 32 &&
                   rom[offset] == 0x55 && rom[offset + 1] == 0xaa) {
                const UInt16 pcirOff = static_cast<UInt16>(rom[offset + 0x18]) |
                    (static_cast<UInt16>(rom[offset + 0x19]) << 8);
                const size_t pcir = offset + pcirOff;
                if (pcir + 22 > kPromBytes || rom[pcir] != 'P' ||
                    rom[pcir + 1] != 'C' || rom[pcir + 2] != 'I' ||
                    rom[pcir + 3] != 'R') break;
                const UInt16 blocks = static_cast<UInt16>(rom[pcir + 16]) |
                    (static_cast<UInt16>(rom[pcir + 17]) << 8);
                const size_t imageBytes = static_cast<size_t>(blocks) * 512;
                if (!imageBytes || imageBytes > kPromBytes - offset) break;
                actual = offset + imageBytes;
                ++images;
                if (rom[pcir + 21] & 0x80) break;
                offset = actual;
            }
            if (images && actual && actual <= kPromBytes) {
                // NVIDIA's Falcon table can reference subimages after the
                // standard LAST image via its PCI Data Extension. Preserve
                // the complete published PROM aperture, as upstream RM does.
                pci->setProperty("NVFBProbe-VBIOS", rom, static_cast<unsigned>(kPromBytes));
                pci->setProperty("NVFBProbe-VBIOS-size", kPromBytes, 32);
                pci->setProperty("NVFBProbe-VBIOS-standard-chain-size", actual, 32);
                pci->setProperty("NVFBProbe-VBIOS-images", images, 32);
                IOLog("NVFBProbe: captured VBIOS bytes=%lu standard-chain=%lu images=%u\n",
                      static_cast<unsigned long>(kPromBytes),
                      static_cast<unsigned long>(actual), images);
                // probe() can be called repeatedly while matching settles.
                // Preserve the first hardware attempt and its telemetry.
                if (wantsFwsec &&
                    !pci->getProperty("NVFBProbe-FWSEC-attempted")) {
                    pci->setProperty("NVFBProbe-FWSEC-attempted", true);
                    nvgsp::VbiosFwsecView vbios{};
                    nvgsp::FbLayout layout{};
                    const bool vbiosOk = nvgsp::parseVbiosFwsec(rom, kPromBytes, &vbios);
                    const bool workspaceValid = (vgaWorkspace & (1U << 3)) != 0;
                    const uint64_t workspaceOffset =
                        static_cast<uint64_t>(vgaWorkspace & 0xffffff00U) << 8;
                    const bool layoutOk = fbSizeMb && nvgsp::planAd103FbLayout(
                        static_cast<uint64_t>(fbSizeMb) << 20, workspaceValid,
                        workspaceOffset, 36864, 63541248, &layout);
                    const UInt16 pciCommandBefore = pci->configRead16(kIOPCIConfigCommand);
                    const bool busMasterBefore = (pciCommandBefore & 4) != 0;
                    // IOPCIDevice::setBusMasterEnable() returns the previous
                    // command-bit state, not whether the write succeeded. A
                    // false return is expected when enabling a device whose
                    // bus-master bit was initially clear.
                    if (!busMasterBefore) pci->setBusMasterEnable(true);
                    const bool busMaster =
                        (pci->configRead16(kIOPCIConfigCommand) & 4) != 0;
                    nvgsp::FwsecStaging staging;
                    const bool stageOk = vbiosOk && layoutOk && gspResetReady && busMaster &&
                        gspFuseVersion != UINT32_MAX &&
                        staging.stage(vbios.fwsec, gspFuseVersion, layout.frtsOffset,
                                      nullptr, vbios.dmaImageSize);
                    nvgsp::FwsecExecutionResult execution{};
                    Bar0Io io{map};
                    const bool executeOk = stageOk && nvgsp::executeFwsecFrts(
                        io, vbios.fwsec, staging.busAddress(), staging.imageSize(),
                        layout.frtsOffset, &execution);
                    if (!busMasterBefore) pci->setBusMasterEnable(false);
                    const bool busMasterRestored =
                        ((pci->configRead16(kIOPCIConfigCommand) & 4) != 0) ==
                        busMasterBefore;
                    pci->setProperty("NVFBProbe-FWSEC-vbios-ok", vbiosOk);
                    pci->setProperty("NVFBProbe-FWSEC-layout-ok", layoutOk);
                    pci->setProperty("NVFBProbe-FWSEC-bus-master-before", busMasterBefore);
                    pci->setProperty("NVFBProbe-FWSEC-bus-master", busMaster);
                    pci->setProperty("NVFBProbe-FWSEC-bus-master-restored", busMasterRestored);
                    pci->setProperty("NVFBProbe-FWSEC-stage-ok", stageOk);
                    pci->setProperty("NVFBProbe-FWSEC-stage-failure", staging.failure(), 32);
                    pci->setProperty("NVFBProbe-FWSEC-dma-failure", staging.dmaFailure(), 32);
                    pci->setProperty("NVFBProbe-FWSEC-execute-ok", executeOk);
                    pci->setProperty("NVFBProbe-FWSEC-stage-bus", staging.busAddress(), 64);
                    pci->setProperty("NVFBProbe-FWSEC-stage-bytes", staging.imageSize(), 32);
                    pci->setProperty("NVFBProbe-FWSEC-frts-offset", layout.frtsOffset, 64);
                    pci->setProperty("NVFBProbe-FWSEC-dma-transfers", execution.dmaTransfers, 32);
                    pci->setProperty("NVFBProbe-FWSEC-cpuctl", execution.cpuCtl, 32);
                    pci->setProperty("NVFBProbe-FWSEC-mailbox0", execution.mailbox0, 32);
                    pci->setProperty("NVFBProbe-FWSEC-mailbox1", execution.mailbox1, 32);
                    pci->setProperty("NVFBProbe-FWSEC-scratch", execution.frtsScratch, 32);
                    pci->setProperty("NVFBProbe-FWSEC-wpr2-lo", execution.wpr2Lo, 32);
                    pci->setProperty("NVFBProbe-FWSEC-wpr2-hi", execution.wpr2Hi, 32);
                    IOLog("NVFBProbe: FWSEC vbios=%u layout=%u reset=%u busmaster=%u/%u/%u "
                          "stage=%u execute=%u dma=%u scratch=0x%08x wpr2=%08x:%08x\n",
                          vbiosOk, layoutOk, gspResetReady, busMasterBefore, busMaster,
                          busMasterRestored, stageOk,
                          executeOk, execution.dmaTransfers, execution.frtsScratch,
                          execution.wpr2Hi, execution.wpr2Lo);
                }
            } else {
                pci->setProperty("NVFBProbe-VBIOS-valid", false);
                IOLog("NVFBProbe: PROM did not contain a valid PCI ROM chain\n");
            }
        }
        if (rom) IOFree(rom, kPromBytes);
        map->release();
    } else {
        IOLog("NVFBProbe: BAR0 map unavailable; register survey skipped\n");
    }

    // Host-memory DMA mapping survey only: no GPU DMA is started and neither
    // allocation is published to the card. Both are released before return.
    nvgsp::DmaBuffer small, scattered;
    const bool smallOk = small.allocate(4096, true);
    const bool scatteredOk = scattered.allocate(2 * 1024 * 1024, false);
    uint64_t first = 0, last = 0;
    const bool pagesOk = scatteredOk &&
        scattered.busPage(0, &first) && scattered.busPage(511, &last);
    pci->setProperty("NVFBProbe-DMA-small-ok", smallOk);
    pci->setProperty("NVFBProbe-DMA-scatter-ok", scatteredOk && pagesOk);
    pci->setProperty("NVFBProbe-DMA-small-bus", small.busAddress(), 64);
    pci->setProperty("NVFBProbe-DMA-first-bus", first, 64);
    pci->setProperty("NVFBProbe-DMA-last-bus", last, 64);
    IOLog("NVFBProbe: DMA-map small=%u addr=0x%llx scatter=%u pages=%u first=0x%llx last=0x%llx\n",
          smallOk, static_cast<unsigned long long>(small.busAddress()),
          scatteredOk, pagesOk,
          static_cast<unsigned long long>(first),
          static_cast<unsigned long long>(last));

    IOLog("NVFBProbe: cmd=0x%04x  ==== end ==== declining attachment\n",
          pci->configRead16(kIOPCIConfigCommand));
    // Diagnostic only. The existing IONDRVFramebuffer remains the display driver.
    return nullptr;
}
