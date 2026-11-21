#ifndef MD30_USB_DEVICE_H
#define MD30_USB_DEVICE_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    IOUSBDeviceInterface **device;
    IOUSBInterfaceInterface **interface;
    UInt8 previous_configuration;
    UInt8 playback_pipe;
    bool device_open;
    bool interface_open;
    bool configuration_changed;
} MD30Connection;

bool md30_usb_ok(IOReturn status, const char *operation);
bool md30_query_interface(io_service_t service, CFUUIDRef client_id, CFUUIDRef interface_id,
                          void **result);
io_service_t md30_find_device(void);
bool md30_open_connection(MD30Connection *connection);
void md30_close_connection(MD30Connection *connection);

#ifdef __cplusplus
}
#endif

#endif
