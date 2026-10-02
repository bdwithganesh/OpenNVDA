/* Mock IOKit user API for tests/nvkmd_macos (Linux host; not Apple headers).
 * Declares only what nvkmd_macos.c uses; nvkmd_macos_mock_check.c implements
 * it as an in-memory NVGspControl user client. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int kern_return_t;
typedef unsigned int mach_port_t;
typedef mach_port_t io_object_t, io_connect_t, io_service_t, task_port_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
typedef unsigned int IOOptionBits;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFNumber *CFNumberRef;
typedef const struct __CFBoolean *CFBooleanRef;
typedef unsigned long CFTypeID;
typedef unsigned char Boolean;
#define kCFAllocatorDefault ((CFAllocatorRef)0)
enum { kCFNumberSInt64Type = 4, kCFStringEncodingUTF8 = 0x08000100 };

#define IO_OBJECT_NULL ((io_object_t)0)
#define KERN_SUCCESS 0
#define kIOReturnSuccess 0
#define kIOReturnTimeout ((int)0xe00002d6)
#define kIOReturnUnsupported ((int)0xe00002c7)
#define kIOReturnNotPermitted ((int)0xe00002e2)
#define kIOReturnBadArgument ((int)0xe00002c2)
#define kIOReturnOffline ((int)0xe00002d0)
#define kIOReturnNoMemory ((int)0xe00002bd)
#define kIOMapAnywhere 0x00000001

extern const mach_port_t kIOMainPortDefault;

CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type,
                            io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallMethod(mach_port_t connection, uint32_t selector,
                                  const uint64_t *input, uint32_t inputCnt,
                                  const void *inputStruct, size_t inputStructCnt,
                                  uint64_t *output, uint32_t *outputCnt,
                                  void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType,
                                   task_port_t intoTask, mach_vm_address_t *atAddress,
                                   mach_vm_size_t *ofSize, IOOptionBits options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType,
                                     task_port_t fromTask, mach_vm_address_t atAddress);

/* CoreFoundation subset (real IOKitLib.h pulls in CoreFoundation) */
CFStringRef CFStringCreateWithCString(CFAllocatorRef a, const char *s, uint32_t enc);
CFTypeRef IORegistryEntryCreateCFProperty(io_object_t entry, CFStringRef key,
                                          CFAllocatorRef allocator, IOOptionBits options);
CFTypeID CFGetTypeID(CFTypeRef cf);
CFTypeID CFNumberGetTypeID(void);
CFTypeID CFBooleanGetTypeID(void);
Boolean CFNumberGetValue(CFNumberRef number, int type, void *value);
Boolean CFBooleanGetValue(CFBooleanRef b);
void CFRelease(CFTypeRef cf);
