#pragma once

// Kernel-side staging for GSP sysmem allocations. This owns the I/O mapping
// for its full lifetime; callers must not publish bus addresses after release.
#include "NVGspDma.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

namespace nvgsp {

class DmaBuffer {
public:
    DmaBuffer() = default;
    ~DmaBuffer() { release(); }
    DmaBuffer(const DmaBuffer &) = delete;
    DmaBuffer &operator=(const DmaBuffer &) = delete;

    // Allocate outside an interrupt or gated workloop. A contiguous buffer is
    // required for WPR metadata, signature, bootloader and radix table root.
    // The large firmware image should use scattered pages instead.
    bool allocate(size_t size, bool contiguous, IOMapper *mapper = nullptr,
                  uint32_t commandAlignment = 4096) {
        lastError_ = 0;
        if (memory_ || !size || size > UINT32_MAX || (size & 4095) ||
            (commandAlignment!=1 && commandAlignment!=4096)) {
            lastError_ = 1;
            return false;
        }
        size_ = size;
        contiguous_ = contiguous;
        memory_ = contiguous
            ? IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                  kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                  size, 0xffffffffffffULL)
            : IOBufferMemoryDescriptor::inTaskWithOptions(
                  kernel_task, kIODirectionInOut, size, 4096);
        if (!memory_) { lastError_ = 2; release(); return false; }
        if (!memory_->getBytesNoCopy()) { lastError_ = 3; release(); return false; }
        bzero(memory_->getBytesNoCopy(), size);

        command_ = IODMACommand::withSpecification(
            kIODMACommandOutputHost64, 48, 0, IODMACommand::kMapped,
            0, commandAlignment, mapper);
        if (!command_) { lastError_ = 4; release(); return false; }
        if (command_->setMemoryDescriptor(memory_) != kIOReturnSuccess) {
            lastError_ = 5; release(); return false;
        }
        if (contiguous) {
            if (!singleDmaAddress(command_, size, &baseBusAddress_)) {
                lastError_ = 6; release(); return false;
            }
        } else {
            pageCount_ = size / 4096;
            pageAddresses_ = static_cast<uint64_t *>(IOMalloc(pageCount_ * sizeof(uint64_t)));
            if (!pageAddresses_ || !walkDmaPages(command_, size, recordPage, this)) {
                lastError_ = pageAddresses_ ? 8 : 7; release(); return false;
            }
        }
        return true;
    }

    bool write(size_t offset, const void *data, size_t length) {
        if (!command_ || !data || offset > size_ || length > size_ - offset)
            return false;
        return command_->writeBytes(offset, data, length) == length &&
               command_->synchronize(kIODirectionOut) == kIOReturnSuccess;
    }

    // Access the actual prepared DMA command buffer, including bounce pages.
    // Never synchronize the whole descriptor while the device owns adjacent
    // queue fields: an Out copy could overwrite a concurrently published PUT.
    bool readDeviceRange(size_t offset, void *data, size_t length) const {
        if (!command_ || !data || !length || offset > size_ || length > size_-offset)
            return false;
        __sync_synchronize();
        const bool ok=command_->readBytes(offset,data,length)==length;
        __sync_synchronize();
        return ok;
    }
    bool writeDeviceRange(size_t offset, const void *data, size_t length) {
        if (!command_ || !data || !length || offset > size_ || length > size_-offset)
            return false;
        __sync_synchronize();
        const bool ok=command_->writeBytes(offset,data,length)==length;
        __sync_synchronize();
        return ok;
    }

    uint64_t busAddress() const { return contiguous_ ? baseBusAddress_ : 0; }
    bool busPage(size_t index, uint64_t *address) const {
        if (!address || index >= size_ / 4096) return false;
        *address = contiguous_ ? baseBusAddress_ + index * 4096
                               : pageAddresses_[index];
        return *address != 0;
    }
    size_t size() const { return size_; }
    IOBufferMemoryDescriptor *descriptor() const { return memory_; }
    uint32_t lastError() const { return lastError_; }
    void *bytes() const { return memory_ ? memory_->getBytesNoCopy() : nullptr; }
    bool syncToDevice() const {
        return command_ &&
               command_->synchronize(kIODirectionOut) == kIOReturnSuccess;
    }
    bool syncFromDevice() const {
        return command_ &&
               command_->synchronize(kIODirectionIn) == kIOReturnSuccess;
    }

    void release() {
        if (pageAddresses_) {
            IOFree(pageAddresses_, pageCount_ * sizeof(uint64_t));
            pageAddresses_ = nullptr;
        }
        if (command_) {
            command_->clearMemoryDescriptor();
            command_->release();
            command_ = nullptr;
        }
        if (memory_) { memory_->release(); memory_ = nullptr; }
        size_ = pageCount_ = 0;
        baseBusAddress_ = 0;
        contiguous_ = false;
    }

private:
    static bool recordPage(void *context, uint64_t index, uint64_t address) {
        DmaBuffer *self = static_cast<DmaBuffer *>(context);
        if (index >= self->pageCount_) return false;
        self->pageAddresses_[index] = address;
        return true;
    }

    IOBufferMemoryDescriptor *memory_ = nullptr;
    IODMACommand *command_ = nullptr;
    uint64_t *pageAddresses_ = nullptr;
    size_t size_ = 0;
    size_t pageCount_ = 0;
    uint64_t baseBusAddress_ = 0;
    bool contiguous_ = false;
    uint32_t lastError_ = 0;
};

} // namespace nvgsp
