// lwlogin: log in at the login window by typing the automatic-login password
// (decoded from /etc/kcpassword, the system's own copy; nothing new is stored
// and nothing is printed). For GPU tests after a WindowServer restart, where
// loginwindow does not repeat the boot-time automatic login.
// The login window runs secure input, which ignores synthetic CGEvents, so
// the keys come from a virtual HID keyboard (IOHIDUserDevice), like hardware.
// Build: clang -O2 -framework IOKit -framework CoreFoundation lwlogin.c -o lwlogin
//        codesign -s - -f --entitlements lwlogin.entitlements lwlogin
// Use (root): lwlogin
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
// IOHIDUserDevice.h is not in the public SDK; the functions are IOKit exports
typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
extern IOHIDUserDeviceRef IOHIDUserDeviceCreateWithProperties(CFAllocatorRef, CFDictionaryRef, IOOptionBits);
extern IOReturn IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef, const uint8_t *, CFIndex);
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// boot keyboard: modifier byte, reserved, 6 key codes
static const uint8_t kDesc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

// HID usage (and shift) for printable ASCII on a US layout
static bool usage(char c, uint8_t *u, bool *shift) {
    static const char *lower = "abcdefghijklmnopqrstuvwxyz";
    static const char *digits = "1234567890";
    static const char *shiftDigits = "!@#$%^&*()";
    const char *p;
    *shift = false;
    if ((p = strchr(lower, c)) && c) { *u = (uint8_t)(0x04 + (p - lower)); return true; }
    if (c >= 'A' && c <= 'Z') { *u = (uint8_t)(0x04 + c - 'A'); *shift = true; return true; }
    if ((p = strchr(digits, c)) && c) { *u = (uint8_t)(0x1E + (p - digits)); return true; }
    if ((p = strchr(shiftDigits, c)) && c) { *u = (uint8_t)(0x1E + (p - shiftDigits)); *shift = true; return true; }
    static const struct { char c; uint8_t u; bool s; } other[] = {
        {' ', 0x2C, 0}, {'-', 0x2D, 0}, {'_', 0x2D, 1}, {'=', 0x2E, 0}, {'+', 0x2E, 1}, {'[', 0x2F, 0}, {'{', 0x2F, 1},
        {']', 0x30, 0}, {'}', 0x30, 1}, {'\\', 0x31, 0}, {'|', 0x31, 1}, {';', 0x33, 0}, {':', 0x33, 1}, {'\'', 0x34, 0},
        {'"', 0x34, 1}, {'`', 0x35, 0}, {'~', 0x35, 1}, {',', 0x36, 0}, {'<', 0x36, 1}, {'.', 0x37, 0}, {'>', 0x37, 1},
        {'/', 0x38, 0}, {'?', 0x38, 1},
    };
    for (size_t i = 0; i < sizeof other / sizeof other[0]; i++)
        if (other[i].c == c) { *u = other[i].u; *shift = other[i].s; return true; }
    return false;
}

static void send(IOHIDUserDeviceRef d, uint8_t mod, uint8_t key) {
    uint8_t r[8] = {mod, 0, key, 0, 0, 0, 0, 0};
    IOHIDUserDeviceHandleReport(d, r, sizeof r);
    usleep(20000);
}

int main(void) {
    static const unsigned char k[11] = {0x7D, 0x89, 0x52, 0x23, 0xD2, 0xBC, 0xDD, 0xEA, 0xA3, 0xB9, 0x1F};
    unsigned char buf[256];
    FILE *f = fopen("/etc/kcpassword", "rb");
    if (!f) { fprintf(stderr, "lwlogin: no /etc/kcpassword\n"); return 1; }
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    size_t len = 0;
    for (; len < n; len++) {
        buf[len] ^= k[len % 11];
        if (!buf[len]) break;
    }
    if (!len || len == n) { fprintf(stderr, "lwlogin: kcpassword not readable\n"); return 1; }
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFDataRef desc = CFDataCreate(NULL, kDesc, sizeof kDesc);
    CFDictionarySetValue(props, CFSTR(kIOHIDReportDescriptorKey), desc);
    int vid = 0x05ac, pid = 0x0250;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid), p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFDictionarySetValue(props, CFSTR(kIOHIDVendorIDKey), v);
    CFDictionarySetValue(props, CFSTR(kIOHIDProductIDKey), p);
    CFDictionarySetValue(props, CFSTR(kIOHIDProductKey), CFSTR("lwlogin virtual keyboard"));
    IOHIDUserDeviceRef d = IOHIDUserDeviceCreateWithProperties(NULL, props, 0);
    if (!d) { fprintf(stderr, "lwlogin: no virtual HID device (entitlement?)\n"); return 1; }
    sleep(2);                                   // let the HID system match the new keyboard
    size_t typed = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t u; bool sh;
        if (!usage((char)buf[i], &u, &sh)) continue;
        send(d, sh ? 0x02 : 0, u);
        send(d, 0, 0);
        typed++;
    }
    memset(buf, 0, sizeof buf);
    send(d, 0, 0x28);                           // Return
    send(d, 0, 0);
    sleep(1);
    CFRelease(d);
    printf("lwlogin: typed %zu of %zu characters and Return\n", typed, len);
    return typed == len ? 0 : 1;
}
