#include "../drivers/NVGspCore/NVGspExternalVas615.hpp"
#include "../drivers/NVGspCore/NVGspClientGr615.hpp"
#include "../drivers/NVGspCore/NVGspDiagnosticVram615.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#ifdef NVGSP_PRIMARY_FAULT_HEADERS
#include <nvos.h>
#include <ctrl/ctrl0080/ctrl0080dma.h>
#include <dev_mmu.h>
static_assert(nvgsp::kExternalFaultVasFlags615==(NV_VASPACE_ALLOCATION_FLAGS_IS_EXTERNALLY_OWNED|NV_VASPACE_ALLOCATION_FLAGS_ENABLE_PAGE_FAULTING),"primary external fault flags");
static_assert(sizeof(nvgsp::SetPageDirectory615)==sizeof(NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS),"primary PDB control size");
static_assert(offsetof(nvgsp::SetPageDirectory615,vas)==offsetof(NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS,hVASpace),"primary VAS offset");
static_assert(offsetof(nvgsp::SetPageDirectory615,subdevice)==offsetof(NV0080_CTRL_DMA_SET_PAGE_DIRECTORY_PARAMS,subDeviceId),"primary subdevice offset");
static_assert((1?NV_MMU_VER2_PDE_APERTURE)==2 && (0?NV_MMU_VER2_PDE_APERTURE)==1,"primary PDE aperture");
static_assert((1?NV_MMU_VER2_PDE_ADDRESS_VID)==32 && (0?NV_MMU_VER2_PDE_ADDRESS_VID)==8,"primary PDE VID address");
static_assert(NV_MMU_VER2_PDE__SIZE==8 && NV_MMU_VER2_DUAL_PDE__SIZE==16,"primary PDE sizes");
#endif
int main() {
 nvgsp::ExternalSmallTree615 p{};constexpr uint64_t root=0x120000000ULL;
 assert(nvgsp::externalSmallTree615(root,1ULL<<32,&p));
 assert(p.address[0]==root && p.address[1]==root+4096 && p.address[2]==root+8192+8*8 && p.address[3]==root+12288+8);
 assert(p.leaf==root+16384);
 for(unsigned i=0;i<4;++i)assert(p.value[i]==((root+(i+1)*4096)>>4|2));
 nvgsp::ExternalSmallTree615 other{};
 assert(nvgsp::externalSmallTree615(root,2ULL<<32,&other));
 // The fixed cross-VAS negative target reaches an unpopulated PD1 slot in
 // A's zeroed tree, while B has its own private root and populated slot.
 assert(other.address[0]==p.address[0] && other.address[1]==p.address[1]);
 assert(other.address[2]!=p.address[2] && other.leaf==p.leaf);
 for(uint64_t va: {4096ULL,0x1fd000ULL,(1ULL<<49)-0x3000}) {
  assert(nvgsp::externalSmallTree615(root,va,&p));
  for(unsigned i=0;i<4;++i)assert(p.address[i]>=root+i*4096 && p.address[i]+8<=root+(i+1)*4096);
  assert(p.leaf>=root+16384 && p.leaf+24<=root+20480);
 }
 assert(!nvgsp::externalSmallTree615(root,0x1fe000,&p));
 assert(!nvgsp::externalSmallTree615(root,1ULL<<49,&p));
 assert(!nvgsp::externalSmallTree615(root,0,&p));
 assert(!nvgsp::externalSmallTree615(root+1,1ULL<<32,&p));
 assert(!nvgsp::externalSmallTree615((1ULL<<37)-4096,1ULL<<32,&p));
 assert(!nvgsp::externalSmallTree615(root,1ULL<<32,nullptr));

 // Generic owned tree: ring page + 24MiB GR window, one PD1 slot.
 {
  nvgsp::ExternalTree615 big{};
  const uint64_t lo=0x104000000ULL-4096,hi=0x104000000ULL+0x1784000ULL;
  assert(nvgsp::externalTreeTables615(lo,hi)==13);
  assert(nvgsp::externalTreePlan615(root,lo,hi,&big) && big.treeBytes==17*4096 && big.firstChunk==(lo>>21));
  assert(big.slots==1 && big.upperCount==3 && big.tableBase==root+16384 && big.treeBytes==nvgsp::externalTreeBytes615(lo,hi));
  // The single-chunk case reproduces the proven small tree exactly.
  nvgsp::ExternalTree615 one{};nvgsp::ExternalSmallTree615 small{};
  assert(nvgsp::externalTreePlan615(root,1ULL<<32,(1ULL<<32)+0x3000,&one) && one.tables==1);
  assert(nvgsp::externalSmallTree615(root,1ULL<<32,&small));
  for(int k=0;k<3;++k)assert(one.upperAddress[k]==small.address[k] && one.upperValue[k]==small.value[k]);
  assert(one.pd0Address[0]==small.address[3] && one.pd0Value[0]==small.value[3]);
  uint64_t leaf=0;assert(nvgsp::externalTreeLeaf615(one,1ULL<<32,&leaf) && leaf==small.leaf);
  assert(nvgsp::externalTreeLeaf615(big,lo,&leaf) && leaf==root+16384+((lo>>12)&511)*8);
  assert(nvgsp::externalTreeLeaf615(big,0x104000000ULL,&leaf) && leaf==root+16384+4096);
  assert(nvgsp::externalTreeLeaf615(big,hi-4096,&leaf) && leaf==root+16384+12*4096+(((hi-4096)>>12)&511)*8);
  assert(!nvgsp::externalTreeLeaf615(big,hi,&leaf) && !nvgsp::externalTreeLeaf615(big,lo-4096,&leaf));
  assert(nvgsp::externalTreeTables615(lo,lo+33ULL*0x200000)==34);   // 0.180.107: up to 256 tables
 }
 // 0.180.118: a span over several PD1 slots: one PD0 page per slot, tables after them.
 {
  nvgsp::ExternalTree615 two{};
  const uint64_t lo=0x13fe00000ULL,hi=0x140200000ULL;   // 2 chunks, crosses the slot at 0x140000000
  assert(nvgsp::externalTreeTables615(lo,hi)==2 && nvgsp::externalTreePlan615(root,lo,hi,&two));
  assert(two.slots==2 && two.upperCount==4 && two.treeBytes==(3+2+2)*4096 && two.tableBase==root+5*4096);
  assert(two.upperAddress[2]==root+8192+((lo>>29)&511)*8 && two.upperValue[2]==(((root+12288)>>12)<<8|2));
  assert(two.upperAddress[3]==root+8192+(((lo>>29)+1)&511)*8 && two.upperValue[3]==(((root+16384)>>12)<<8|2));
  assert(two.pd0Address[0]==root+12288+((lo>>21)&255)*16+8);   // last entry of the first PD0
  assert(two.pd0Address[1]==root+16384+0*16+8);                 // first entry of the second PD0
  assert(two.pd0Value[1]==(((root+6*4096)>>12)<<8|2));
  uint64_t leaf=0;
  assert(nvgsp::externalTreeLeaf615(two,0x140000000ULL,&leaf) && leaf==root+6*4096);
  assert(nvgsp::externalTreeLeaf615(two,lo+4096,&leaf) && leaf==root+5*4096+((lo>>12)&511)*8+8);
  // the client layout: GR window + 1 GiB user + 512 MiB wrap windows, five slots
  const uint64_t clo=0x104000000ULL-8192,chi=0x106000000ULL+(1ULL<<30)+(512ULL<<20);
  nvgsp::ExternalTree615 c{};
  assert(nvgsp::externalTreePlan615(root,clo,chi,&c) && c.slots==4 && c.tables==((chi-1)>>21)-(clo>>21)+1);
  assert(nvgsp::externalTreeLeaf615(c,chi-4096,&leaf) && leaf==c.tableBase+(c.tables-1)*4096+511*8);
  // 0.180.157: 16 PD1 slots; nine are accepted now, seventeen are not
  // 0.180.168: 32 PD1 slots; seventeen are accepted now, thirty-three are not
  assert(nvgsp::externalTreeTables615(0x100000000ULL,0x100000000ULL+(4ULL<<30)+4096)==2049);
  assert(nvgsp::externalTreeTables615(0x100000000ULL,0x100000000ULL+(8ULL<<30)+4096)==4097);
  assert(!nvgsp::externalTreeTables615(0x100000000ULL,0x100000000ULL+(32ULL<<30)+4096));   // 64 PD1 slots (0.180.193)
  // the live 0.180.163 client: ring below GR at 0x104000000, 2.5 GiB user +
  // 4 GiB wrap windows (0.180.156 measured lo 0x103ffe000, hi 0x1c5800000
  // with the 512 MiB wrap window)
  {
   const uint64_t plo=0x103ffe000ULL,phi=0x1c5800000ULL+(3584ULL<<20);
   nvgsp::ExternalTree615 *p=new nvgsp::ExternalTree615();
   assert(nvgsp::externalTreePlan615(root,plo,phi,p) && p->slots==14 && p->tables==3341);
   assert(p->treeBytes==(3+14+3341)*4096ULL && p->treeBytes<=(16ULL<<20));   // kClientTreeReserve615
   assert(nvgsp::externalTreeLeaf615(*p,phi-4096,&leaf) && leaf==p->tableBase+(p->tables-1)*4096+511*8);
   delete p;
  }
  // 0.180.168: the same client with the 12 GiB wrap window
  {
   const uint64_t plo=0x103ffe000ULL,phi=0x1a5800000ULL+nvgsp::kClientWrapWindow615;
   assert(nvgsp::kClientWrapWindow615==(12ULL<<30) && phi==0x4a5800000ULL);
   nvgsp::ExternalTree615 *p=new nvgsp::ExternalTree615();
   assert(nvgsp::externalTreePlan615(root,plo,phi,p) && p->slots==30 && p->tables==7437);
   assert(p->treeBytes==(3+30+7437)*4096ULL && p->treeBytes<=nvgsp::kClientTreeReserve615);
   assert(nvgsp::externalTreeLeaf615(*p,phi-4096,&leaf) && leaf==p->tableBase+(p->tables-1)*4096+511*8);
   assert(p->upperAddress[2+29]==root+8192+(((plo>>29)+29)&511)*8);
   assert(!nvgsp::externalTreeLeaf615(*p,phi,&leaf));
   delete p;
  }
  // 0.180.193: 16 GiB allocation window + 12 GiB wrap window
  {
   const uint64_t plo=0x103ffe000ULL,phi=0x105800000ULL+nvgsp::kClientUserWindow615+nvgsp::kClientWrapWindow615;
   nvgsp::ExternalTree615 *p=new nvgsp::ExternalTree615();
   assert(nvgsp::externalTreePlan615(root,plo,phi,p) && p->slots==57 && p->tables==14349);
   assert(p->treeBytes<=nvgsp::kClientTreeReserve615 && p->treeBytes+(2ULL<<20)<=nvgsp::kDiagnosticVramMaxRequest615);
   assert(nvgsp::externalTreeLeaf615(*p,phi-4096,&leaf) && leaf==p->tableBase+(p->tables-1)*4096+511*8);
   delete p;
  }
  assert(!nvgsp::externalTreeTables615(0x7fffe00000ULL,0x8000200000ULL));   // crosses a PD2 entry
 }
 puts("ExternalVAS615: primary flags/PDB ABI/PDE encoding, owned five-level49-bit tree, distinct levels and boundary rejection PASS");
}
