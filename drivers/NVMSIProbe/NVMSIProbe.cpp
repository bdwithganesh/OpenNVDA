// Read-only MSI capability probe (diagnostic; no hardware writes).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

#define super IOService

class NVMSIProbe : public IOService {
    OSDeclareDefaultStructors(NVMSIProbe)
public:
    bool start(IOService *provider) override {
        if (!super::start(provider)) return false;
        IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
        if (!pci) return false;
        UInt8 cap = 0;
        pci->findPCICapability(0x05, &cap);
        UInt16 ctrl = cap ? pci->configRead16(cap + 2) : 0;
        UInt32 lo = cap ? pci->configRead32(cap + 4) : 0;
        UInt32 hi = cap ? pci->configRead32(cap + 8) : 0;
        UInt16 data = cap ? pci->configRead16(cap + 12) : 0;
        UInt16 cmd = pci->configRead16(0x04);
        UInt32 id = (pci->configRead32(0) >> 16);
        setProperty("NVMSIProbe-vendor-device", id, 32);
        setProperty("NVMSIProbe-cap", cap, 8);
        setProperty("NVMSIProbe-ctrl", ctrl, 16);
        setProperty("NVMSIProbe-addr-lo", lo, 32);
        setProperty("NVMSIProbe-addr-hi", hi, 32);
        setProperty("NVMSIProbe-data", data, 16);
        setProperty("NVMSIProbe-command", cmd, 16);
        // Also publish the specifier array as scalar entries (vector/flags).
        if (OSArray *specs = OSDynamicCast(OSArray, pci->getProperty("IOInterruptSpecifiers"))) {
            setProperty("NVMSIProbe-spec-count", specs->getCount(), 32);
            for (unsigned i = 0; i < specs->getCount() && i < 4; ++i) {
                if (OSData *d = OSDynamicCast(OSData, specs->getObject(i))) {
                    const UInt32 *p = static_cast<const UInt32 *>(d->getBytesNoCopy());
                    char key[48];
                    snprintf(key, sizeof key, "NVMSIProbe-spec%u", i);
                    UInt64 v = (d->getLength() >= 8 && p) ? (UInt64(p[1]) << 32) | p[0] : 0;
                    setProperty(key, v, 64);
                }
            }
        }
        IOLog("NVMSIProbe %s: cap=0x%x ctrl=0x%x addr=%08x%08x data=0x%x cmd=0x%x\n",
              pci->getName(), cap, ctrl, hi, lo, data, cmd);
        return true;
    }
};

OSDefineMetaClassAndStructors(NVMSIProbe, IOService)
