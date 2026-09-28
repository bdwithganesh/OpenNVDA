/* Mock <mach/mach.h> for tests/nvkmd_macos (Linux host). */
#pragma once
#include <IOKit/IOKitLib.h>
task_port_t mach_task_self(void);
