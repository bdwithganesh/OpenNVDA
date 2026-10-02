// 0.8.14: the driver's log lines, public. NSLog's %@ arguments show up as
// <private> in the unified log on Tahoe, which hid every selector, kernel and
// format name we needed when debugging other processes.
#pragma once
#import <Foundation/Foundation.h>
#include <os/log.h>
#define NSLog(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "%{public}s", [[NSString stringWithFormat:(fmt), ##__VA_ARGS__] UTF8String])
