// IOAccelDisplayPipe (IOAcceleratorFamily2) for NVAccelerator's display pipe.
// Written by hand from the 14.8.9 vtable dump (docs/b4/ioaf2-vtables.txt,
// "IOAccelDisplayPipe", 309 qwords): the virtuals it adds after IOService
// are slots 266..305 in this order. Return types are what the family's
// code does for the ones we override; the rest stay void * (never call
// those). The object size is not in the dump, so the base gets a padded
// size that is larger than the real one (NVAccelDisplayPipe logs the real
// size from the metaclass at init).
#ifndef IOAF2_PIPE_HPP
#define IOAF2_PIPE_HPP
#include "IOAF2.hpp"

class IOAccelDisplayPipeTransaction2;
class IOAccelEvent;
class IOFramebuffer;
struct IOAccelDisplayPipeScaler;

class IOAccelDisplayPipe : public IOService {
    OSDeclareDefaultStructors(IOAccelDisplayPipe)
public:
    virtual void free();   // [18]
    virtual bool init(IOGraphicsAccelerator2 *, IOAccelDisplayMachine *, IOFramebuffer *, unsigned int);   // [266]
    virtual void *initFramebufferResource(unsigned int, IOAccelResource2 *);   // [267]
    virtual void *destroyFramebufferResource(unsigned int, IOAccelResource2 *);   // [268]
    virtual void *displayModeWillChange();   // [269]
    virtual void *displayModeDidChange();   // [270]
    virtual void enableVBLInterrupt();   // [271]
    virtual void disableVBLInterrupt();   // [272]
    virtual void *enableTransactionInterrupt();   // [273]
    virtual void *disableTransactionInterrupt();   // [274]
    virtual void *newDisplayPipeTransaction();   // [275]
    virtual IOReturn validateTransaction(IOAccelDisplayPipeTransaction2 *);   // [276]
    virtual void *performTransaction(IOAccelDisplayPipeTransaction2 *);   // [277]
    virtual bool isTransactionComplete(IOAccelDisplayPipeTransaction2 *);   // [278]
    virtual IOReturn submitTransaction(IOAccelDisplayPipeTransaction2 *);   // [279]
    virtual void *getDisplayModePipeScalerSetup(IOAccelDisplayPipeScaler *);   // [280]
    virtual void *createWorkLoop();   // [281]
    virtual void *copyCapabilities();   // [282]
    virtual void *beginTransaction(IOAccelEvent *);   // [283]
    virtual void *signalTransactionComplete(IOAccelEvent *);   // [284]
    virtual void *framebufferTerminated();   // [285]
    virtual void *wsaaEnterDefer(int);   // [286]
    virtual void *wsaaWillExitDefer(int);   // [287]
    virtual void *wsaaDidExitDefer(int);   // [288]
    virtual void *wsaaWillEnterDefer(int);   // [289]
    virtual void *wsaaDidEnterDefer(int);   // [290]
    virtual void *willPowerOff();   // [291]
    virtual void *didPowerOn();   // [292]
    virtual void *logTransactionTimeoutDiagnosisReport();   // [293]
    virtual void *getStampIndex() const;   // [294]
    virtual void *triage(char **, unsigned long long *);   // [295]
    virtual void *_RESERVEDIOAccelDisplayPipe1();   // [296]
    virtual void *_RESERVEDIOAccelDisplayPipe2();   // [297]
    virtual void *_RESERVEDIOAccelDisplayPipe3();   // [298]
    virtual void *_RESERVEDIOAccelDisplayPipe4();   // [299]
    virtual void *_RESERVEDIOAccelDisplayPipe5();   // [300]
    virtual void *_RESERVEDIOAccelDisplayPipe6();   // [301]
    virtual void *_RESERVEDIOAccelDisplayPipe7();   // [302]
    virtual void *framebuffer_will_power_off();   // [303]
    virtual void *framebuffer_did_power_on();   // [304]
    virtual void *framebuffer_terminated();   // [305]
protected:
    uint8_t _nvData[0x1000 - sizeof(IOService)];
};
#endif
