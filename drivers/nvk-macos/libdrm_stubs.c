/* macOS has no DRM. The common Vulkan runtime is built with HAVE_LIBDRM (its
 * headers are needed by NAK's bindings), so provide inert libdrm entry points:
 * no DRM devices, every DRM call fails with -ENODEV. NVK on macOS goes
 * through nvkmd_macos (NVGspControl) instead. */
#include <errno.h>
#include <stdint.h>
#include <xf86drm.h>

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices) { return 0; }
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) { return -ENODEV; }
void drmFreeDevice(drmDevicePtr *device) { }
void drmFreeDevices(drmDevicePtr devices[], int count) { }
int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b) { return 0; }
int drmGetCap(int fd, uint64_t capability, uint64_t *value) { return -ENODEV; }
int drmIoctl(int fd, unsigned long request, void *arg) { errno = ENODEV; return -1; }
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle) { return -ENODEV; }
int drmSyncobjDestroy(int fd, uint32_t handle) { return -ENODEV; }
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd) { return -ENODEV; }
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle) { return -ENODEV; }
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd) { return -ENODEV; }
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd) { return -ENODEV; }
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
                   unsigned flags, uint32_t *first_signaled) { return -ENODEV; }
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count) { return -ENODEV; }
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count) { return -ENODEV; }
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
                             uint32_t handle_count) { return -ENODEV; }
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
                           int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{ return -ENODEV; }
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{ return -ENODEV; }
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count,
                     uint32_t flags) { return -ENODEV; }
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
                       uint64_t src_point, uint32_t flags) { return -ENODEV; }
