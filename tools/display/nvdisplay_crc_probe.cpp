// Fixed bounded CRC capture mechanics, without LUT/cursor output claims.
#include "nvdisplay_crc_resource.hpp"

int main(int argc,char **argv){
    if(argc!=2 || (strcmp(argv[1],"--capture") && strcmp(argv[1],"--fixture"))){
        fprintf(stderr,"usage: sudo nvdisplay_crc_probe --capture|--fixture\n");return 2;}
    const auto service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("NVGspControl"));
    io_connect_t c=0;
    if(!service || IOServiceOpen(service,mach_task_self(),0,&c)){if(service)IOObjectRelease(service);return 1;}
    const int result=crc_resource_trial(c,service,!strcmp(argv[1],"--fixture"));IOServiceClose(c);IOObjectRelease(service);return result;
}
