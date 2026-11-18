#pragma once
#include <stdint.h>
#include <stddef.h>
#pragma push_macro("MW")
#undef MW
#define MW(bits) bits
#include "NVGspComputeDefinitions615.hpp"

namespace nvgsp {
struct ComputePacket615 {
    uint32_t qmd[64]{},commands[32]{};
    uint32_t commandCount=0,releasePayload=0;
};
// The currently proven ABI is one32-byte constant buffer, one64-thread CTA,
// no shared/local memory. No caller method stream or physical address is used.
inline bool computePacket615(uint64_t program,uint64_t descriptor,uint64_t constant,
                             uint64_t release,uint32_t registers,uint32_t payload,
                             ComputePacket615 *out,uint32_t gridX=1,uint32_t constantBytes=32) {
    // 0.180.99: gridX CTAs of the fixed 64x1x1 block; CB0 of 16..256 bytes.
    if(!gridX || gridX>65535 || constantBytes<16 || constantBytes>256 || (constantBytes&15))return false;
    if(!out || !program || !descriptor || !constant || !release || !registers || registers>255 || !payload ||
       program>=(1ULL<<49) || constant>=(1ULL<<49) || descriptor>=(1ULL<<40) || release>=(1ULL<<40) ||
       (program&127) || (descriptor&255) || (constant&15) || (release&3))return false;
    ComputePacket615 p{};bool valid=true;
    auto put=[&](unsigned hi,unsigned lo,uint64_t value) {
        if(hi>=2048 || lo>hi || hi-lo>=64 || (hi-lo<63 && value>=(1ULL<<(hi-lo+1)))) {valid=false;return;}
        for(unsigned bit=lo;bit<=hi;++bit) {
            const uint32_t mask=1U<<(bit%32);
            p.qmd[bit/32]=(p.qmd[bit/32]&~mask)|(((value>>(bit-lo))&1)?mask:0);
        }
    };
#define NVGSP_QMD615(field,value) put((1 ? NVC6C0_QMDV03_00_##field),(0 ? NVC6C0_QMDV03_00_##field),(value))
#define NVGSP_CB615(field,value) put((1 ? NVC6C0_QMDV03_00_##field(0)),(0 ? NVC6C0_QMDV03_00_##field(0)),(value))
    NVGSP_QMD615(QMD_MAJOR_VERSION,3);NVGSP_QMD615(QMD_VERSION,0);
    NVGSP_QMD615(API_VISIBLE_CALL_LIMIT,NVC6C0_QMDV03_00_API_VISIBLE_CALL_LIMIT_NO_CHECK);
    NVGSP_QMD615(SAMPLER_INDEX,NVC6C0_QMDV03_00_SAMPLER_INDEX_INDEPENDENTLY);
    NVGSP_QMD615(SM_GLOBAL_CACHING_ENABLE,1);
    NVGSP_QMD615(CTA_RASTER_WIDTH,gridX);NVGSP_QMD615(CTA_RASTER_HEIGHT,1);NVGSP_QMD615(CTA_RASTER_DEPTH,1);
    NVGSP_QMD615(CTA_THREAD_DIMENSION0,64);NVGSP_QMD615(CTA_THREAD_DIMENSION1,1);NVGSP_QMD615(CTA_THREAD_DIMENSION2,1);
    NVGSP_QMD615(REGISTER_COUNT_V,registers);NVGSP_QMD615(BARRIER_COUNT,0);
    NVGSP_QMD615(PROGRAM_ADDRESS_LOWER,program&0xffffffff);NVGSP_QMD615(PROGRAM_ADDRESS_UPPER,program>>32);
    NVGSP_QMD615(SHARED_MEMORY_SIZE,0);NVGSP_QMD615(SHADER_LOCAL_MEMORY_LOW_SIZE,0);NVGSP_QMD615(SHADER_LOCAL_MEMORY_HIGH_SIZE,0);
    // Mesa26.0.8 nak/qmd.rs AdaQMD3 initialization, already verified on AD103.
    NVGSP_QMD615(MIN_SM_CONFIG_SHARED_MEM_SIZE,1);NVGSP_QMD615(TARGET_SM_CONFIG_SHARED_MEM_SIZE,1);
    NVGSP_QMD615(MAX_SM_CONFIG_SHARED_MEM_SIZE,26);
    NVGSP_CB615(CONSTANT_BUFFER_VALID,1);NVGSP_CB615(CONSTANT_BUFFER_ADDR_LOWER,constant&0xffffffff);
    NVGSP_CB615(CONSTANT_BUFFER_ADDR_UPPER,constant>>32);NVGSP_CB615(CONSTANT_BUFFER_SIZE_SHIFTED4,constantBytes>>4);
    NVGSP_QMD615(RELEASE0_ADDRESS_LOWER,release&0xffffffff);NVGSP_QMD615(RELEASE0_ADDRESS_UPPER,release>>32);
    NVGSP_QMD615(RELEASE0_ENABLE,NVC6C0_QMDV03_00_RELEASE0_ENABLE_TRUE);
    NVGSP_QMD615(RELEASE0_MEMBAR_TYPE,NVC6C0_QMDV03_00_RELEASE0_MEMBAR_TYPE_FE_SYSMEMBAR);
    NVGSP_QMD615(RELEASE0_STRUCTURE_SIZE,NVC6C0_QMDV03_00_RELEASE0_STRUCTURE_SIZE_SEMAPHORE_ONE_WORD);
    NVGSP_QMD615(RELEASE0_PAYLOAD_LOWER,payload);
#undef NVGSP_QMD615
#undef NVGSP_CB615
    auto header=[](uint32_t method,uint32_t count) {return 0x20000000U|(count<<16)|(1U<<13)|(method>>2);};
    auto immediate=[](uint32_t method,uint32_t value) {return 0x80000000U|(value<<16)|(1U<<13)|(method>>2);};
    const uint32_t words[]={
        header(NVC9C0_SET_OBJECT,1),ADA_COMPUTE_A,
        header(NVC9C0_SET_SHADER_SHARED_MEMORY_WINDOW_A,2),0,0xfe000000,
        header(NVC9C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A,2),0,0xff000000,
        header(NVC9C0_INVALIDATE_SHADER_CACHES,1),0x1011,
        immediate(NVC9C0_INVALIDATE_SKED_CACHES,0),
        header(NVC9C0_SEND_PCAS_A,1),uint32_t(descriptor>>8),
        immediate(NVC9C0_SEND_SIGNALING_PCAS2_B,NVC9C0_SEND_SIGNALING_PCAS2_B_PCAS_ACTION_INVALIDATE_COPY_SCHEDULE)};
    p.commandCount=sizeof(words)/sizeof(words[0]);p.releasePayload=payload;
    __builtin_memcpy(p.commands,words,sizeof(words));
    if(!valid)return false;
    *out=p;return true;
}

inline bool computeNak615(const void *binary,size_t bytes,uint32_t *codeBytes,uint32_t *registers) {
    if(!binary || !codeBytes || !registers || bytes<80 || bytes>32768+64)return false;
    uint32_t h[16]{};__builtin_memcpy(h,binary,sizeof(h));
    if(h[0]!=0x314b414e || !h[1] || h[1]>32768 || (h[1]&15) || bytes!=64+h[1] ||
       !h[2] || h[2]>255 || h[3] || h[4] || h[5]!=64 || h[6]!=1 || h[7]!=1 || h[8] || h[9]!=5)
        return false;
    for(unsigned i=10;i<16;++i)if(h[i])return false;
    *codeBytes=h[1];*registers=h[2];return true;
}
struct ComputeResult615 {
    uint32_t version,words,sequence,computeRelease,hostFence,gpGet,gpPut,reserved;
    uint32_t output[64];
};
static_assert(sizeof(ComputeResult615)==288,"Native compute615 result ABI");
}
#pragma pop_macro("MW")
