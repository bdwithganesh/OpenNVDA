#pragma once
// Pinned615 rpc_gsp_rm_alloc_v03_00, NVOS00_PARAMETERS_v03_00, cl0000.h.
// Root request matches NV_RM_RPC_ALLOC_SHARE_DEVICE_FWCLIENT in rpc.h:
// requested hClient in NV0000_ALLOC_PARAMETERS; hParent/hObject/class are0.
#include "NVGspControlAbi615.hpp"
namespace nvgsp {
struct AllocHeader615 {
    uint32_t client, parent, object, objectClass, status, paramsBytes, flags, reserved;
};
struct RootParams615 {
    uint32_t client, processId;
    char processName[100];
    uint32_t padding;
    uint64_t osPidInfo;
};
struct FreeParams615 { uint32_t client, parent, object, status; };
struct DeviceParams615 {
    uint32_t deviceId, clientShare, targetClient, targetDevice, flags, padding;
    uint64_t vaSize, vaStartInternal, vaLimitInternal;
    uint32_t vaMode, tailPadding;
};
struct VaspaceParams615 {
    uint32_t index, flags;
    uint64_t vaSize, vaStartInternal, vaLimitInternal;
    uint32_t bigPageSize, padding;
    uint64_t vaBase;
    uint32_t pasid, tailPadding;
};
struct MemoryParams615 {
    uint32_t owner, type, flags, width, height;
    int32_t pitch;
    uint32_t attr, attr2, format, comprCovg, zcullCovg, padding;
    uint64_t rangeLo, rangeHi, size, alignment, offset, limit, address;
    uint32_t ctagOffset, vaSpace, internalFlags, tag;
    int32_t numaNode;
    uint32_t tailPadding;
};
static_assert(sizeof(AllocHeader615)==32, "615 alloc header");
static_assert(sizeof(RootParams615)==120 && offsetof(RootParams615,osPidInfo)==112,
              "615 NV0000 parameters");
static_assert(sizeof(FreeParams615)==16, "615 NVOS00 free parameters");
static_assert(sizeof(DeviceParams615)==56 && offsetof(DeviceParams615,vaMode)==48,
              "615 NV0080 allocation");
static_assert(sizeof(VaspaceParams615)==56 && offsetof(VaspaceParams615,pasid)==48,
              "615 VAS allocation includes pasid");
static_assert(sizeof(MemoryParams615)==128 && offsetof(MemoryParams615,size)==64 &&
              offsetof(MemoryParams615,offset)==80 && offsetof(MemoryParams615,numaNode)==120,
              "615 NV_MEMORY_ALLOCATION_PARAMS");
constexpr uint32_t kLifecycleClient615=0xc0d00001;
constexpr uint32_t kLifecycleDevice615=0xc0d00080;
constexpr uint32_t kLifecycleSubdevice615=0xc0d02080;
constexpr uint32_t kLifecycleVaspace615=0xc0d090f1;
constexpr uint32_t kLifecycleMemory615=0xc0d00041;
inline bool buildAlloc615(void *data, size_t bytes, uint32_t parent,
                          uint32_t object, uint32_t objectClass,
                          const void *params, uint32_t paramsBytes) {
    if (!data || bytes<sizeof(AllocHeader615) || paramsBytes>bytes-sizeof(AllocHeader615) ||
        (paramsBytes && !params)) return false;
    const AllocHeader615 h{kLifecycleClient615,parent,object,objectClass,0,paramsBytes,0,0};
    __builtin_memcpy(data,&h,sizeof(h));
    if (paramsBytes) __builtin_memcpy(static_cast<uint8_t *>(data)+sizeof(h),params,paramsBytes);
    return true;
}
inline bool buildDevice615(void *data, size_t bytes, uint32_t deviceMask) {
    if (!deviceMask) return false;
    DeviceParams615 p{};
    while (!(deviceMask & 1)) { ++p.deviceId; deviceMask>>=1; }
    return buildAlloc615(data,bytes,kLifecycleClient615,kLifecycleDevice615,0x80,&p,sizeof(p));
}
inline bool buildMemoryProbe615(void *data, size_t bytes) {
    MemoryParams615 p{};
    p.owner=kLifecycleClient615;p.type=6; //NVOS32_TYPE_DMA
    p.flags=0x100; //NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE
    p.attr=1U<<23; //NVOS32_ATTR_PAGE_SIZE_4KB
    p.size=4096;p.alignment=4096;
    return buildAlloc615(data,bytes,kLifecycleSubdevice615,kLifecycleMemory615,0x40,&p,sizeof(p));
}
inline bool memoryProbeRange615(const MemoryParams615 &p, uint64_t fbBytes,
                                uint64_t heapBase=0, uint64_t heapLimit=UINT64_MAX) {
    // video_mem.c sets limit=sizeOut-1; it is relative to offset, unlike
    // the absolute base/limit of a GMC framebuffer region.
    return p.owner==kLifecycleClient615 && p.type==6 && p.size>=4096 &&
        !(p.offset&4095) && p.offset<fbBytes && p.size<=fbBytes-p.offset &&
        p.limit==p.size-1 && p.offset>=heapBase && p.offset<=heapLimit &&
        p.limit<=heapLimit-p.offset;
}
inline bool memoryProbeRegion615(const MemoryParams615 &p,uint64_t fbBytes,
                                 uint64_t base,uint64_t limit,uint32_t flags,uint32_t tag) {
    // ctrl2080fb.h: GSP_RM_RESERVED_HEAP=5. GMC PROTECTED is bit2.
    // Status/identity-validated GSP-RM allocations live in this heap, distinct
    // from the unreserved CPU-RM/PMA heap. Never permit GSP image/metadata.
    return tag==5 && !(flags&4) && memoryProbeRange615(p,fbBytes,base,limit);
}
inline bool buildRootClient615(void *data, size_t bytes) {
    if (!data || bytes<sizeof(AllocHeader615)+sizeof(RootParams615)) return false;
    const AllocHeader615 h{kLifecycleClient615,0,0,0,0,sizeof(RootParams615),0,0};
    RootParams615 p{};
    p.client=kLifecycleClient615;
    const char name[]="NativeDriverLab";
    __builtin_memcpy(p.processName,name,sizeof(name));
    __builtin_memcpy(data,&h,sizeof(h));
    __builtin_memcpy(static_cast<uint8_t *>(data)+sizeof(h),&p,sizeof(p));
    return true;
}
struct LifecycleReply615 {
    uint32_t rpcResult=~0U, privateResult=~0U, status=~0U, paramsBytes=0, flags=0;
    bool valid=false;
    bool successful() const { return valid && !rpcResult && !privateResult && !status && !flags; }
};
inline bool lifecyclePayload615(const void *record, size_t bytes, uint32_t function,
                                size_t minimum, const uint8_t *&payload,
                                uint32_t &payloadBytes, LifecycleReply615 &out) {
    out={};payload=nullptr;payloadBytes=0;
    if (!record || bytes<56 || minimum>bytes-56) return false;
    const auto *p=static_cast<const uint8_t *>(record);
    const uint32_t length=control615U32(p+32);
    if (control615U32(p+24)!=0x03000000 || control615U32(p+28)!=0x43505256 ||
        control615U32(p+36)!=function || length<32 || length>bytes-24 || minimum>length-32)
        return false;
    out.rpcResult=control615U32(p+40);out.privateResult=control615U32(p+44);
    payload=p+56;payloadBytes=length-32;
    return true;
}
inline LifecycleReply615 decodeRootClient615(const void *record, size_t bytes) {
    LifecycleReply615 r;
    const uint8_t *p=nullptr;uint32_t n=0;
    if (!lifecyclePayload615(record,bytes,103,sizeof(AllocHeader615),p,n,r)) return r;
    AllocHeader615 h{};__builtin_memcpy(&h,p,sizeof(h));
    r.status=h.status;r.paramsBytes=h.paramsBytes;r.flags=h.flags;
    if (h.client!=kLifecycleClient615 || h.parent || h.object || h.objectClass || h.reserved ||
        h.paramsBytes!=sizeof(RootParams615) || h.paramsBytes>n-sizeof(h)) return r;
    if (!r.status && control615U32(p+sizeof(h))!=kLifecycleClient615) return r;
    r.valid=true;
    return r;
}
inline LifecycleReply615 decodeRootFree615(const void *record, size_t bytes) {
    LifecycleReply615 r;
    const uint8_t *p=nullptr;uint32_t n=0;
    if (!lifecyclePayload615(record,bytes,10,sizeof(FreeParams615),p,n,r)) return r;
    FreeParams615 h{};__builtin_memcpy(&h,p,sizeof(h));
    r.status=h.status;
    r.valid=h.client==kLifecycleClient615 && !h.parent && h.object==kLifecycleClient615;
    return r;
}
inline LifecycleReply615 decodeAlloc615(const void *record, size_t bytes,
                                       uint32_t parent, uint32_t object,
                                       uint32_t objectClass, uint32_t paramsBytes,
                                       const uint8_t **params=nullptr) {
    if (params) *params=nullptr;
    LifecycleReply615 r;
    const uint8_t *p=nullptr;uint32_t n=0;
    if (!lifecyclePayload615(record,bytes,103,sizeof(AllocHeader615),p,n,r)) return r;
    AllocHeader615 h{};__builtin_memcpy(&h,p,sizeof(h));
    r.status=h.status;r.paramsBytes=h.paramsBytes;r.flags=h.flags;
    r.valid=h.client==kLifecycleClient615 && h.parent==parent && h.object==object &&
        h.objectClass==objectClass && !h.reserved && h.paramsBytes==paramsBytes &&
        h.paramsBytes<=n-sizeof(h);
    if (r.valid && params) *params=p+sizeof(h);
    return r;
}
} // namespace nvgsp
