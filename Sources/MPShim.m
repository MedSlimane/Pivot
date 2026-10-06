#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <dlfcn.h>
#import <objc/message.h>

// MonitorPanel (private framework) rotation shim.
// Probed on this machine: MPDisplay initWithCGSDisplayID: / setOrientation:
// work under SIP with no entitlement. setOrientation: takes degrees
// (0/90/180/270), applies immediately, fires screen-params notification.

static void *MPShimPanel(void) {
    static void *handle;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        handle = dlopen("/System/Library/PrivateFrameworks/MonitorPanel.framework/MonitorPanel", RTLD_NOW);
    });
    return handle;
}

static id MPShimMakeDisplay(uint32_t displayID) {
    if (!MPShimPanel()) { return nil; }
    Class c = NSClassFromString(@"MPDisplay");
    if (!c) { return nil; }
    id d = ((id (*)(id, SEL, uint32_t))objc_msgSend)([c alloc], sel_registerName("initWithCGSDisplayID:"), displayID);
    return d;
}

int MPShimCanChangeOrientation(uint32_t displayID) {
    id d = MPShimMakeDisplay(displayID);
    if (!d) { return -1; }
    return ((BOOL (*)(id, SEL))objc_msgSend)(d, sel_registerName("canChangeOrientation")) ? 1 : 0;
}

int32_t MPShimGetOrientation(uint32_t displayID) {
    id d = MPShimMakeDisplay(displayID);
    if (!d) { return -1; }
    return ((int32_t (*)(id, SEL))objc_msgSend)(d, sel_registerName("orientation"));
}

int MPShimSetOrientation(uint32_t displayID, int32_t degrees) {
    id d = MPShimMakeDisplay(displayID);
    if (!d) { return -1; }
    ((void (*)(id, SEL, int32_t))objc_msgSend)(d, sel_registerName("setOrientation:"), degrees);
    return 0;
}
