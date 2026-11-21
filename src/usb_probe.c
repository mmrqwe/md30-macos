#include "exclusive_play.h"
#include "exclusive_owner.h"
#include "usb_device.h"
#include "usb_runtime.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_error.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum probe_mode {
    describe_device,
    claim_pipes,
    query_runtime,
    load_runtime,
    measure_stream,
    play_tone
};

static bool describe_pipes(IOUSBInterfaceInterface **interface, UInt8 *playback_pipe)
{
    UInt8 count;
    if (!md30_usb_ok((*interface)->GetNumEndpoints(interface, &count), "Read number of endpoints")) {
        return false;
    }
    for (unsigned int pipe = 1; pipe <= count; ++pipe) {
        UInt8 direction, number, transfer_type, interval;
        UInt16 max_packet;
        if (!md30_usb_ok((*interface)->GetPipeProperties(interface, pipe, &direction, &number,
                                                      &transfer_type, &max_packet, &interval),
                       "Read pipe properties")) {
            return false;
        }
        printf("Pipe %u  endpoint 0x%02x  transfer type %u  max packet %u\n",
               pipe, number | (direction == kUSBIn ? 0x80 : 0), transfer_type, max_packet);
        if (number == 2 && direction == kUSBOut && transfer_type == kUSBBulk && max_packet == 512) {
            *playback_pipe = pipe;
        }
    }
    return true;
}

static bool claim_interface(IOUSBDeviceInterface **device, enum probe_mode mode)
{
    IOUSBFindInterfaceRequest request = {kUSBVendorSpecificClass, 0xff, 0xff, 0};
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (!md30_usb_ok((*device)->CreateInterfaceIterator(device, &request, &iterator),
                   "Find MD30 vendor interface")) {
        return false;
    }
    io_service_t service = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (!service) {
        fputs("MD30 vendor interface unavailable after configuration\n", stderr);
        return false;
    }

    IOUSBInterfaceInterface **interface = NULL;
    bool queried = md30_query_interface(service, kIOUSBInterfaceUserClientTypeID,
                                   kIOUSBInterfaceInterfaceID942, (void **)&interface);
    IOObjectRelease(service);
    if (!queried) {
        return false;
    }
    bool opened = md30_usb_ok((*interface)->USBInterfaceOpen(interface), "Open MD30 vendor interface");
    UInt8 playback_pipe = 0;
    bool result = opened && describe_pipes(interface, &playback_pipe);
    if (result && (mode == measure_stream || mode == play_tone)) {
        if (playback_pipe) {
            result = md30_test_stream(device, interface, playback_pipe, mode == play_tone);
        } else {
            fputs("MD30 playback endpoint 0x02 is unavailable\n", stderr);
            result = false;
        }
    }
    if (opened) {
        result = md30_usb_ok((*interface)->USBInterfaceClose(interface), "Close MD30 vendor interface")
                 && result;
    }
    (*interface)->Release(interface);
    return result;
}

static bool verify_access(IOUSBDeviceInterface **device, UInt8 previous, UInt8 selected,
                          enum probe_mode mode, const char *driver_path)
{
    if (!md30_usb_ok((*device)->USBDeviceOpen(device), "Open MD30 USB device")) {
        return false;
    }
    const bool changed = previous != selected;
    const bool configured = !changed ||
        md30_usb_ok((*device)->SetConfiguration(device, selected), "Select MD30 USB configuration");
    bool result = false;
    if (configured) {
        switch (mode) {
        case query_runtime:
            result = md30_probe_runtime(device);
            break;
        case load_runtime:
            result = md30_load_runtime(device, driver_path);
            break;
        default:
            result = claim_interface(device, mode);
            break;
        }
    }
    if (changed && configured) {
        result = md30_usb_ok((*device)->SetConfiguration(device, previous), "Restore USB configuration")
                 && result;
    }
    return md30_usb_ok((*device)->USBDeviceClose(device), "Close MD30 USB device") && result;
}

int main(int argc, char **argv)
{
    enum probe_mode mode = describe_device;
    if (argc > 1) {
        if (argc == 2 && strcmp(argv[1], "--audio-ready") == 0) {
            alarm(2);
            return md30_coreaudio_ready() ? 0 : 1;
        }
        if (argc == 3 && (strcmp(argv[1], "--exclusive-play") == 0 ||
                          strcmp(argv[1], "--verify-pcm") == 0)) {
            return md30_play_wav(argv[2], strcmp(argv[1], "--verify-pcm") == 0) ? 0 : 1;
        }
        if (argc == 2 && strcmp(argv[1], "--claim") == 0) {
            mode = claim_pipes;
        } else if (argc == 2 && strcmp(argv[1], "--probe-runtime") == 0) {
            mode = query_runtime;
        } else if (argc == 3 && strcmp(argv[1], "--load-runtime") == 0) {
            mode = load_runtime;
        } else if (argc == 2 && strcmp(argv[1], "--probe-stream") == 0) {
            mode = measure_stream;
        } else if (argc == 2 && strcmp(argv[1], "--tone") == 0) {
            mode = play_tone;
        } else {
            fprintf(stderr, "Usage: %s [--claim | --probe-runtime | --load-runtime DRIVER.sys | --probe-stream | --tone | --exclusive-play FILE.wav | --verify-pcm FILE.wav | --audio-ready]\n", argv[0]);
            return 2;
        }
    }
    io_service_t service = md30_find_device();
    if (!service) {
        fputs("MUSILAND MD30 (04b4:5030) was not found\n", stderr);
        return 1;
    }

    IOUSBDeviceInterface **device = NULL;
    bool queried = md30_query_interface(service, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID942,
                                   (void **)&device);
    IOObjectRelease(service);
    if (!queried) {
        return 1;
    }

    UInt8 current;
    IOUSBConfigurationDescriptorPtr descriptor = NULL;
    bool result = md30_usb_ok((*device)->GetConfiguration(device, &current), "Read active configuration")
                  && md30_usb_ok((*device)->GetConfigurationDescriptorPtr(device, 0, &descriptor),
                               "Read MD30 configuration descriptor");
    if (result) {
        printf("MUSILAND MD30 (04b4:5030)  active configuration %u  available configuration %u\n",
               current, descriptor->bConfigurationValue);
        if (mode != describe_device) {
            result = verify_access(device, current, descriptor->bConfigurationValue, mode, mode == load_runtime ? argv[2] : NULL);
        }
    }

    (*device)->Release(device);
    return result ? 0 : 1;
}
