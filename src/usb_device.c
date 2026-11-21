#include "usb_device.h"

#include <IOKit/IOCFPlugIn.h>
#include <mach/mach_error.h>
#include <stdio.h>

bool md30_usb_ok(IOReturn status, const char *operation)
{
    if (status == kIOReturnSuccess) {
        return true;
    }
    fprintf(stderr, "%s: 0x%08x (%s)\n", operation, (unsigned int)status, mach_error_string(status));
    return false;
}

bool md30_query_interface(io_service_t service, CFUUIDRef client_id, CFUUIDRef interface_id,
                            void **result)
{
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score;
    if (!md30_usb_ok(IOCreatePlugInInterfaceForService(service, client_id, kIOCFPlugInInterfaceID,
                                                    &plugin, &score), "Create USB plug-in")) {
        return false;
    }
    HRESULT status = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(interface_id), result);
    IODestroyPlugInInterface(plugin);
    return md30_usb_ok(status, "Query USB interface");
}

io_service_t md30_find_device(void)
{
    const int vendor = 0x04b4;
    const int product = 0x5030;
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    CFNumberRef vendor_number = CFNumberCreate(NULL, kCFNumberIntType, &vendor);
    CFNumberRef product_number = CFNumberCreate(NULL, kCFNumberIntType, &product);
    CFDictionarySetValue(match, CFSTR("idVendor"), vendor_number);
    CFDictionarySetValue(match, CFSTR("idProduct"), product_number);
    CFRelease(vendor_number);
    CFRelease(product_number);
    return IOServiceGetMatchingService(kIOMainPortDefault, match);
}

static bool find_playback_pipe(MD30Connection *connection)
{
    IOUSBInterfaceInterface **interface = connection->interface;
    UInt8 count;
    if (!md30_usb_ok((*interface)->GetNumEndpoints(interface, &count), "Read USB endpoints")) {
        return false;
    }
    for (unsigned int index = 1; index <= count; ++index) {
        UInt8 direction, number, type, interval;
        UInt16 max_packet;
        if (!md30_usb_ok((*interface)->GetPipeProperties(interface, index, &direction, &number,
                                                        &type, &max_packet, &interval),
                         "Read USB endpoint")) {
            return false;
        }
        if (direction == kUSBOut && number == 2 && type == kUSBBulk && max_packet == 512) {
            connection->playback_pipe = index;
            return true;
        }
    }
    return false;
}

static bool open_playback_interface(MD30Connection *connection)
{
    IOUSBDeviceInterface **device = connection->device;
    IOUSBFindInterfaceRequest request = {kUSBVendorSpecificClass, 0xff, 0xff, 0};
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (!md30_usb_ok((*device)->CreateInterfaceIterator(device, &request, &iterator),
                     "Find USB playback interface")) {
        return false;
    }
    io_service_t service = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (!service) {
        fputs("MD30 vendor interface unavailable after configuration\n", stderr);
        return false;
    }
    const bool queried = md30_query_interface(service, kIOUSBInterfaceUserClientTypeID,
                                              kIOUSBInterfaceInterfaceID942,
                                              (void **)&connection->interface);
    IOObjectRelease(service);
    if (!queried) {
        return false;
    }
    IOUSBInterfaceInterface **interface = connection->interface;
    connection->interface_open = md30_usb_ok((*interface)->USBInterfaceOpen(interface),
                                              "Open MD30 playback interface");
    return connection->interface_open && find_playback_pipe(connection);
}

static bool configure_device(MD30Connection *connection)
{
    IOUSBDeviceInterface **device = connection->device;
    connection->device_open = md30_usb_ok((*device)->USBDeviceOpen(device), "Open MD30 device");
    if (!connection->device_open ||
        !md30_usb_ok((*device)->GetConfiguration(device, &connection->previous_configuration),
                     "Read USB configuration")) {
        return false;
    }
    if (connection->previous_configuration != 1) {
        connection->configuration_changed = md30_usb_ok((*device)->SetConfiguration(device, 1),
                                                        "Set USB configuration");
        if (!connection->configuration_changed) {
            return false;
        }
    }
    return open_playback_interface(connection);
}

bool md30_open_connection(MD30Connection *connection)
{
    *connection = (MD30Connection){0};
    io_service_t service = md30_find_device();
    if (!service) {
        return false;
    }
    const bool queried = md30_query_interface(service, kIOUSBDeviceUserClientTypeID,
                                              kIOUSBDeviceInterfaceID942,
                                              (void **)&connection->device);
    IOObjectRelease(service);
    if (queried && configure_device(connection)) {
        return true;
    }
    md30_close_connection(connection);
    return false;
}

void md30_close_connection(MD30Connection *connection)
{
    IOUSBInterfaceInterface **interface = connection->interface;
    IOUSBDeviceInterface **device = connection->device;
    if (interface) {
        if (connection->interface_open) {
            (*interface)->USBInterfaceClose(interface);
        }
        (*interface)->Release(interface);
    }
    if (device) {
        if (connection->configuration_changed) {
            (*device)->SetConfiguration(device, connection->previous_configuration);
        }
        if (connection->device_open) {
            (*device)->USBDeviceClose(device);
        }
        (*device)->Release(device);
    }
    *connection = (MD30Connection){0};
}
