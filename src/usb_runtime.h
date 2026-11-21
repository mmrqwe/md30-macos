#ifndef MD30_USB_RUNTIME_H
#define MD30_USB_RUNTIME_H

#include <IOKit/usb/IOUSBLib.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int hertz;
    UInt8 clock_code;
} MD30SampleRate;

const MD30SampleRate *md30_sample_rates(size_t *count);
bool md30_load_runtime(IOUSBDeviceInterface **device, const char *driver_path);
bool md30_probe_runtime(IOUSBDeviceInterface **device);
bool md30_start_stream(IOUSBDeviceInterface **device, unsigned int rate);
bool md30_stop_stream(IOUSBDeviceInterface **device);
bool md30_read_peak(IOUSBDeviceInterface **device, UInt8 reg, UInt16 *peak);
bool md30_test_stream(IOUSBDeviceInterface **device, IOUSBInterfaceInterface **interface,
                      UInt8 pipe, bool tone);

#ifdef __cplusplus
}
#endif

#endif
