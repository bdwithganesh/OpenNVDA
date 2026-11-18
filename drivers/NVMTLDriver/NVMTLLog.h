// 0.8.14: the driver's log lines, public. NSLog's %@ arguments show up as
// <private> in the unified log on Tahoe, which hid every selector, kernel and
// format name we needed when debugging other processes.
//
// 0.9.18: format the message through initWithFormat:arguments: instead of
// stringWithFormat: so clang's -Wformat-security (promoted to an error by the
// os_log headers) does not reject every literal call site through the macro.
#pragma once
#import <Foundation/Foundation.h>
#include <os/log.h>
#include <stdarg.h>

static inline void NVMTLPublicLog(NSString *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    os_log(OS_LOG_DEFAULT, "%{public}s", s.UTF8String);
}

#define NSLog(fmt, ...) NVMTLPublicLog((fmt), ##__VA_ARGS__)
