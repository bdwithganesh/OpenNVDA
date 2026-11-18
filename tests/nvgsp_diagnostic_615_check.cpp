#include "../drivers/NVGspCore/NVGspDiagnostic615.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
using namespace nvgsp;
int main(int argc,char **argv) {
    assert(rpcReplyCompleted615(false,56,7,7));
    assert(!rpcReplyCompleted615(false,0,7,7)); // cleared by reset/quiesce is not a reply
    assert(!rpcReplyCompleted615(false,55,7,7));
    assert(!rpcReplyCompleted615(false,100,7,8));
    assert(!rpcReplyCompleted615(true,100,7,7));
    uint32_t classes[]={0x40,0x50a0};
    alignas(8) uint8_t req[32+128]{};
    MemoryParams615 p{};p.owner=kLifecycleClient615;p.size=4096;
    assert(buildAlloc615(req,sizeof(req),kLifecycleSubdevice615,0xc0d00042,0x40,&p,sizeof(p)));
    assert(diagnosticRequest615(103,req,sizeof(req),classes,sizeof(classes)));
    assert(!diagnosticRequest615(103,req,sizeof(req)-1,classes,sizeof(classes)));
    auto *h=reinterpret_cast<AllocHeader615 *>(req);
    h->object=kLifecycleMemory615;assert(!diagnosticRequest615(103,req,sizeof(req),classes,sizeof(classes)));
    h->object=0xc0d00042;h->client=0xc2000007;assert(!diagnosticRequest615(103,req,sizeof(req),classes,sizeof(classes)));
    h->client=kLifecycleClient615;h->objectClass=0xdeadbeef;assert(!diagnosticRequest615(103,req,sizeof(req),classes,sizeof(classes)));
    h->objectClass=0x40;h->reserved=1;assert(!diagnosticRequest615(103,req,sizeof(req),classes,sizeof(classes)));h->reserved=0;
    alignas(8) uint8_t control[44]{};assert(buildControl615(control,sizeof(control),kLifecycleClient615,kLifecycleSubdevice615,0x20802a08,4));
    assert(diagnosticRequest615(76,control,sizeof(control),nullptr,0));
    auto *ch=reinterpret_cast<ControlHeader615 *>(control);
    ch->client=0xc2000007;assert(!diagnosticRequest615(76,control,sizeof(control),nullptr,0));ch->client=kLifecycleClient615;
    ch->paramsBytes=8;assert(!diagnosticRequest615(76,control,sizeof(control),nullptr,0));
    FreeParams615 f{kLifecycleClient615,0,0xc0d00042,0};
    assert(diagnosticRequest615(10,&f,sizeof(f),nullptr,0));
    f.object=kLifecycleClient615;assert(!diagnosticRequest615(10,&f,sizeof(f),nullptr,0));
    f.object=kLifecycleDevice615;assert(!diagnosticRequest615(10,&f,sizeof(f),nullptr,0));
    f.object=kLifecycleSubdevice615;assert(!diagnosticRequest615(10,&f,sizeof(f),nullptr,0));
    f.object=kLifecycleVaspace615;assert(!diagnosticRequest615(10,&f,sizeof(f),nullptr,0));
    f.object=kLifecycleMemory615;assert(!diagnosticRequest615(10,&f,sizeof(f),nullptr,0));
    assert(!diagnosticRequest615(14,req,sizeof(req),classes,sizeof(classes)));
    DmaMap615 m{kLifecycleClient615,kLifecycleDevice615,0xc0d00057,0xc0d00055,
                0,4096,0x100,0,0,0,0};
    assert(!diagnosticRequest615(14,&m,sizeof(m),nullptr,0));
    DmaUnmap615 u{kLifecycleClient615,kLifecycleDevice615,0xc0d00057,0xc0d00055,
                 0,0,0x100000000,4096,0,0};
    assert(!diagnosticRequest615(15,&u,sizeof(u),nullptr,0));
    assert(diagnosticKickHandle615(0xc0d00056));
    assert(!diagnosticKickHandle615(kLifecycleClient615));
    assert(!diagnosticKickHandle615(0xc0e9006f));
    assert(!diagnosticKickHandle615(0xabcd2080));
    if(argc==2) {
        std::ifstream input(argv[1],std::ios::binary);
        std::vector<uint8_t> actual((std::istreambuf_iterator<char>(input)),{});
        assert(actual.size()>=100);const size_t n=control615U32(actual.data()+96);
        assert(n<=200 && actual.size()>=100+n*4);
        assert(diagnosticRequest615(103,req,sizeof(req),actual.data()+100,n*4));
    }
    std::puts("615diagnostics: owned/supported resources, baseline protection, exact payloads PASS");
}
