#pragma once

#include "NVGspDmaBuffer.hpp"
#include "NVGspFwsec.hpp"

namespace nvgsp {

class FwsecStaging {
public:
    bool stage(const FwsecView &view, uint32_t fuseVersion,
               uint64_t frtsOffset, IOMapper *mapper = nullptr,
               uint32_t dmaImageBytes = 0, uint32_t initCommand = 0x15) {
        failure_ = dmaFailure_ = 0;
        if (ready_ || !view.desc.storedSize) { failure_ = 1; return false; }
        if (!dmaImageBytes) dmaImageBytes = view.desc.storedSize;
        if (dmaImageBytes < view.desc.storedSize || (dmaImageBytes & 255)) {
            failure_ = 2; return false;
        }
        const size_t allocation = (size_t(dmaImageBytes) + 4095) & ~size_t(4095);
        if (!image_.allocate(allocation, true, mapper)) {
            failure_ = 3;
            dmaFailure_ = image_.lastError();
            reset();
            return false;
        }
        if (!patchFwsecFrts(view, fuseVersion, frtsOffset,
                            image_.bytes(), image_.size(), &patch_, initCommand)) {
            failure_ = 4;
            reset();
            return false;
        }
        if (dmaImageBytes > view.desc.storedSize)
            __builtin_memcpy(static_cast<uint8_t *>(image_.bytes()) + view.desc.storedSize,
                             view.image + view.desc.storedSize,
                             dmaImageBytes - view.desc.storedSize);
        if (!image_.syncToDevice()) {
            failure_ = 5;
            reset();
            return false;
        }
        imageSize_ = dmaImageBytes;
        imemSize_ = view.desc.imemLoadSize;
        dmemSize_ = view.desc.dmemLoadSize;
        frtsOffset_ = frtsOffset;
        ready_ = true;
        return true;
    }

    void reset() {
        ready_ = false;
        image_.release();
        patch_ = FwsecPatchInfo{};
        imageSize_ = imemSize_ = dmemSize_ = 0;
        frtsOffset_ = 0;
    }
    bool ready() const { return ready_; }
    uint64_t busAddress() const { return ready_ ? image_.busAddress() : 0; }
    uint32_t imageSize() const { return imageSize_; }
    uint32_t imemSize() const { return imemSize_; }
    uint32_t dmemSize() const { return dmemSize_; }
    uint64_t frtsOffset() const { return frtsOffset_; }
    uint32_t failure() const { return failure_; }
    uint32_t dmaFailure() const { return dmaFailure_; }
    const FwsecPatchInfo &patchInfo() const { return patch_; }

private:
    DmaBuffer image_;
    FwsecPatchInfo patch_{};
    uint32_t imageSize_ = 0, imemSize_ = 0, dmemSize_ = 0;
    uint64_t frtsOffset_ = 0;
    bool ready_ = false;
    uint32_t failure_ = 0, dmaFailure_ = 0;
};

} // namespace nvgsp
