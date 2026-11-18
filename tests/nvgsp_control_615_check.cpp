#include "../drivers/NVGspCore/NVGspControlAbi615.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
using namespace nvgsp;
static void put(std::vector<uint8_t>& b, size_t off, uint32_t n) {
    for (unsigned i=0;i<4;++i) b[off+i]=uint8_t(n>>(8*i));
}
int main(int argc, char **argv) {
    constexpr uint32_t client=0xc2000007, object=0xabcd2080, cmd=0x20800a40;
    std::vector<uint8_t> record(96+kDeviceInfo615Bytes,0);
    assert(!buildControl615(nullptr,40,client,object,cmd,0));
    assert(!buildControl615(record.data(),24+kDeviceInfo615Bytes,client,object,cmd,kDeviceInfo615Bytes));
    assert(buildControl615(record.data()+56,record.size()-56,client,object,cmd,kDeviceInfo615Bytes));
    put(record,24,0x03000000);put(record,28,0x43505256);
    put(record,32,uint32_t(record.size()-24));put(record,36,76);
    put(record,96,14);put(record,100+20,1);
    ControlReply615 r;
    assert(decodeControlReply615(record.data(),record.size(),client,object,cmd,kDeviceInfo615Bytes,r));
    uint32_t count=0;
    assert(r.successful() && deviceInfoCount615(r.params,r.header.paramsBytes,count) && count==14);
    for (size_t i=0;i<record.size();++i)
        assert(!decodeControlReply615(record.data(),i,client,object,cmd,kDeviceInfo615Bytes,r));
    assert(!decodeControlReply615(record.data(),record.size(),client+1,object,cmd,kDeviceInfo615Bytes,r));
    assert(!decodeControlReply615(record.data(),record.size(),client,object+1,cmd,kDeviceInfo615Bytes,r));
    assert(!decodeControlReply615(record.data(),record.size(),client,object,cmd+1,kDeviceInfo615Bytes,r));
    put(record,68,31);
    assert(decodeControlReply615(record.data(),record.size(),client,object,cmd,kDeviceInfo615Bytes,r) && !r.successful());
    put(record,68,0);put(record,44,1);
    assert(decodeControlReply615(record.data(),record.size(),client,object,cmd,kDeviceInfo615Bytes,r) && !r.successful());
    put(record,44,0);put(record,76,1);
    assert(decodeControlReply615(record.data(),record.size(),client,object,cmd,kDeviceInfo615Bytes,r) && !r.successful());
    put(record,76,0);put(record,88,1);
    assert(!decodeControlReply615(record.data(),record.size(),client,object,cmd,kDeviceInfo615Bytes,r));
    put(record,88,0);put(record,96,513);
    assert(!deviceInfoCount615(record.data()+96,kDeviceInfo615Bytes,count));
    put(record,96,14);put(record,120,2);
    assert(!deviceInfoCount615(record.data()+96,kDeviceInfo615Bytes,count));
    std::vector<uint8_t> classes(kClassList615Bytes,0);
    put(classes,0,200);assert(classListCount615(classes.data(),classes.size(),count) && count==200);
    put(classes,0,201);assert(!classListCount615(classes.data(),classes.size(),count));
    assert(!classListCount615(classes.data(),404,count));
    if (argc>=2) {
        // Real GPU capture from27 sent an undersized24B header. It must fail
        // bounds validation, even though RPC result and control status are0.
        std::ifstream f(argv[1],std::ios::binary);
        std::vector<uint8_t> bad((std::istreambuf_iterator<char>(f)),{});
        assert(f.good() || f.eof());assert(bad.size()>=7*4096);
        assert(!decodeControlReply615(bad.data(),bad.size(),client,object,cmd,kDeviceInfo615Bytes,r));
    }
    if (argc==4) {
        std::ifstream df(argv[2],std::ios::binary), cf(argv[3],std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(df)),{});
        std::vector<uint8_t> c((std::istreambuf_iterator<char>(cf)),{});
        assert(decodeControlReply615(d.data(),d.size(),client,object,cmd,kDeviceInfo615Bytes,r));
        assert(r.successful() && deviceInfoCount615(r.params,r.header.paramsBytes,count) && count==14);
        assert(decodeControlReply615(c.data(),c.size(),client,0xabcd0080,0x800292,kClassList615Bytes,r));
        assert(r.successful() && classListCount615(r.params,r.header.paramsBytes,count) && count==114);
    }
    std::puts("615 control ABI: bounds, exact handles/command, status, 512 devices, 200 classes PASS");
}
