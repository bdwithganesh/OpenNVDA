#pragma once

// Walk a prepared IODMACommand's GPU-visible I/O bus segments. Do not use
// IOMemoryDescriptor::getPhysicalSegment() as a substitute for DMA mapping.
#include "NVGspAbi.hpp"
#include <IOKit/IODMACommand.h>

namespace nvgsp {

using DmaPage = bool (*)(void *context, uint64_t index, uint64_t busAddress);

// A firmware radix entry points to a whole 4 KiB DMA page. Reject mappings
// that split a logical page across bus segments or wrap the address range.
inline bool walkDmaPages(IODMACommand *command, uint64_t length,
                         DmaPage consume, void *context) {
    if (!command || !consume || !length || (length & 4095)) return false;
    UInt64 offset = 0;
    uint64_t pageIndex = 0;
    while (offset < length) {
        IODMACommand::Segment64 segment{};
        UInt32 count = 1;
        const UInt64 before = offset;
        if (command->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
            count != 1 || offset <= before || offset > length ||
            segment.fLength != offset - before ||
            (segment.fIOVMAddr & 4095) || (segment.fLength & 4095) ||
            segment.fIOVMAddr > UINT64_MAX - segment.fLength)
            return false;
        for (uint64_t bytes = 0; bytes < segment.fLength; bytes += 4096) {
            if (!consume(context, pageIndex++, segment.fIOVMAddr + bytes))
                return false;
        }
    }
    return pageIndex == length / 4096;
}

// Metadata, signature, bootloader and the radix root each need one bus
// contiguous allocation. The command must already hold a prepared descriptor.
inline bool singleDmaAddress(IODMACommand *command, uint64_t length,
                             uint64_t *busAddress) {
    if (!command || !busAddress || !length) return false;
    UInt64 offset = 0;
    IODMACommand::Segment64 segment{};
    UInt32 count = 1;
    if (command->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
        count != 1 || segment.fLength != length || offset != length ||
        (segment.fIOVMAddr & 4095) ||
        segment.fIOVMAddr > UINT64_MAX - length)
        return false;
    *busAddress = segment.fIOVMAddr;
    return true;
}

} // namespace nvgsp
