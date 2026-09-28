#pragma once

#include "NVGspBooter.hpp"
#include "NVGspDmaBuffer.hpp"

namespace nvgsp {

class BooterStaging {
public:
    bool stage(const BooterView &view, uint32_t fuseVersion,
               IOMapper *mapper = nullptr) {
        lastError_ = 0;
        if (ready_ || !view.image || !view.imageSize || !view.signatureSize ||
            view.patchLocation > view.imageSize ||
            view.signatureSize > view.imageSize - view.patchLocation) {
            lastError_ = 1;
            return false;
        }
        const uint8_t *signature = signatureForFuse(view, fuseVersion);
        if (!signature) { lastError_ = 2; return false; }
        const size_t allocation = (view.imageSize + 4095) & ~size_t(4095);
        if (!image_.allocate(allocation, true, mapper)) {
            lastError_ = 100 + image_.lastError();
            image_.release();
            return false;
        }
        void *owned = image_.bytes();
        if (!owned) {
            lastError_ = 200;
            image_.release();
            return false;
        }
        __builtin_memcpy(owned, view.image, view.imageSize);
        __builtin_memcpy(static_cast<uint8_t *>(owned) + view.patchLocation,
                         signature, view.signatureSize);
        if (!image_.syncToDevice()) {
            lastError_ = 201;
            image_.release();
            return false;
        }
        imageSize_ = view.imageSize;
        layout_ = view.layout;
        patchLocation_ = view.patchLocation;
        engineId_ = view.engineId;
        ucodeId_ = view.ucodeId;
        selectedSignature_ = view.signatureCount - 1 - fuseVersion;
        ready_ = true;
        return true;
    }
    void reset() { image_.release(); ready_ = false; imageSize_ = 0; }
    bool ready() const { return ready_; }
    uint64_t busAddress() const { return ready_ ? image_.busAddress() : 0; }
    uint32_t imageSize() const { return imageSize_; }
    uint32_t selectedSignature() const { return selectedSignature_; }
    uint32_t patchLocation() const { return patchLocation_; }
    uint32_t engineId() const { return engineId_; }
    uint32_t ucodeId() const { return ucodeId_; }
    uint32_t lastError() const { return lastError_; }
    const FalconLayout &layout() const { return layout_; }

private:
    DmaBuffer image_;
    FalconLayout layout_{};
    uint32_t imageSize_ = 0, patchLocation_ = 0, selectedSignature_ = 0;
    uint32_t engineId_ = 0, ucodeId_ = 0;
    uint32_t lastError_ = 0;
    bool ready_ = false;
};

} // namespace nvgsp
