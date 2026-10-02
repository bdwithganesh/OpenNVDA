// Fixed cursor PIO point initialization, NVIDIA MoveCursorC3 ordering.
#pragma once
#include "nvdisplay_admin.hpp"
static inline bool cursor_pio_free(io_connect_t c) {
    const uint64_t end=ns_now()+100000000;uint32_t space=0;
    do{
        if(peek(c,0x6d8008,1,&space))return false;
        if(space&0x3f)return true;
        usleep(1000);
    }while(ns_now()<end);
    return false;
}
static inline bool cursor_pio_point(io_connect_t c,uint32_t point) {
    if(!cursor_pio_free(c))return false;
    const uint64_t position[]={0x6d8208,point},update[]={0x6d8200,0};
    if(IOConnectCallScalarMethod(c,9,position,2,nullptr,nullptr) || !cursor_pio_free(c) ||
        IOConnectCallScalarMethod(c,9,update,2,nullptr,nullptr) || !cursor_pio_free(c))return false;
    uint32_t actual=0;
    const bool ok=!peek(c,0x6d8208,1,&actual) && actual==point;
    printf("cursor PIO point=%08x fetch/ASSY=%s PIO-ARM=unpublished\n",point,ok?"yes":"no");
    return ok;
}
