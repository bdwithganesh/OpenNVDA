#pragma once

// Prepare all first-boot sysmem objects without programming the GPU.
#include "NVGspBoot.hpp"
#include "NVGspDmaBuffer.hpp"

namespace nvgsp {

class GspStaging {
public:
    GspStaging() = default;
    GspStaging(const GspStaging &) = delete;
    GspStaging &operator=(const GspStaging &) = delete;

    bool stage(const FbLayout &fb, const BootUcodeDesc &desc,
               const void *fwImage, size_t fwBytes,
               const void *signature, size_t signatureBytes,
               const void *bootImage, size_t bootBytes,
               IOMapper *mapper = nullptr) {
        if (ready_ || !fwImage || !signature || !bootImage ||
            !fwBytes || (fwBytes & 4095) ||
            signatureBytes != 4096 || !bootBytes || (bootBytes & 4095) ||
            !validBootUcodeDesc(desc, bootBytes))
            return false;
        RadixLayout radixLayoutValue{};
        if (!radixLayout(fwBytes, &radixLayoutValue) ||
            !signature_.allocate(signatureBytes, true, mapper) ||
            !bootloader_.allocate(bootBytes, true, mapper) ||
            !radix_.allocate(radixLayoutValue.tableBytes, true, mapper) ||
            !metadata_.allocate(4096, true, mapper) ||
            !firmware_.allocate(fwBytes, false, mapper) ||
            !firmware_.write(0, fwImage, fwBytes) ||
            !signature_.write(0, signature, signatureBytes) ||
            !bootloader_.write(0, bootImage, bootBytes) ||
            !fillRadixTables(radixLayoutValue, radix_.bytes(), radix_.size(),
                             tablePage, imagePage, this) ||
            !radix_.syncToDevice()) {
            reset();
            return false;
        }

        BootDma dma{radix_.busAddress(), bootloader_.busAddress(),
                    signature_.busAddress(), signatureBytes};
        if (!buildAd103WprMeta(fb, desc, bootBytes, fwBytes, dma, &meta_) ||
            !metadata_.write(0, &meta_, sizeof(meta_))) {
            reset();
            return false;
        }
        appVersion_ = desc.appVersion;
        ready_ = true;
        return true;
    }

    bool ready() const { return ready_; }
    uint64_t metadataBusAddress() const {
        return ready_ ? metadata_.busAddress() : 0;
    }
    const WprMeta *metadata() const { return ready_ ? &meta_ : nullptr; }
    // Booter/GSP write back into the DMA copy (verified, bootCount); NVIDIA
    // resets them before every bootstrap (kernel_gsp_tu102.c:816), so each
    // boot, incl. the S3 re-boot, re-publishes the pristine metadata.
    bool rewriteMetadata() { return ready_ && metadata_.write(0, &meta_, sizeof(meta_)); }
    uint32_t appVersion() const { return ready_ ? appVersion_ : 0; }

    void reset() {
        ready_ = false;
        metadata_.release();
        radix_.release();
        bootloader_.release();
        signature_.release();
        firmware_.release();
        bzero(&meta_, sizeof(meta_));
        appVersion_ = 0;
    }

private:
    static bool tablePage(void *context, uint64_t index, uint64_t *address) {
        GspStaging *self = static_cast<GspStaging *>(context);
        return self->radix_.busPage(index, address);
    }
    static bool imagePage(void *context, uint64_t index, uint64_t *address) {
        GspStaging *self = static_cast<GspStaging *>(context);
        return self->firmware_.busPage(index, address);
    }

    DmaBuffer firmware_, signature_, bootloader_, radix_, metadata_;
    WprMeta meta_{};
    uint32_t appVersion_ = 0;
    bool ready_ = false;
};

} // namespace nvgsp
