#include "usb_device.h"
#include "usb_runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static bool read_meters(MD30Connection *connection, UInt16 usb[2], UInt16 mix[2])
{
    return md30_read_peak(connection->device, 2, usb)
        && md30_read_peak(connection->device, 3, usb + 1)
        && md30_read_peak(connection->device, 6, mix)
        && md30_read_peak(connection->device, 7, mix + 1);
}

static bool test_channel(MD30Connection *connection, unsigned int rate, unsigned int channel)
{
    enum { block_frames = 256 };
    const unsigned int initial_blocks = rate / block_frames / 2;
    const unsigned int tone_blocks = (rate * 2) / block_frames;
    const unsigned int test_blocks = initial_blocks + tone_blocks;
    IOUSBInterfaceInterface **interface = connection->interface;

    if (!md30_start_stream(connection->device, rate)) {
        return false;
    }
    bool success = true;
    UInt16 usb[2] = {0}, mix[2] = {0};
    uint64_t tone_start = 0;
    for (unsigned int block = 0; success && block < test_blocks; ++block) {
        if (block == initial_blocks) {
            // Reading the peak meters clears peaks left by the preceding stream.
            success = read_meters(connection, usb, mix);
            if (!success) {
                break;
            }
            tone_start = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        }
        int32_t samples[block_frames * 2] = {0};
        if (block >= initial_blocks) {
            for (unsigned int frame = 0; frame < block_frames; ++frame) {
                const double time = (block * block_frames + frame) / (double)rate;
                samples[frame * 2 + channel] = (int32_t)(171798691 * sin(2 * M_PI * 440 * time));
            }
        }
        success = (*interface)->WritePipeTO(interface, connection->playback_pipe, samples,
                                             sizeof(samples), 200, 400) == kIOReturnSuccess;
    }

    const uint64_t tone_end = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    const double elapsed = (tone_end - tone_start) / 1e9;
    const double observed_rate = tone_blocks * block_frames / elapsed;
    success = success && read_meters(connection, usb, mix);
    success = md30_stop_stream(connection->device) && success;

    printf("%u Hz, observed %.1f Hz, %s: USB L=%u R=%u, analog mix L=%u R=%u\n",
           rate, observed_rate, channel == 0 ? "left" : "right", usb[0], usb[1], mix[0], mix[1]);
    return success && fabs(observed_rate - rate) <= rate * 0.015
        && usb[channel] > 3500 && mix[channel] > 3500
        && usb[1 - channel] < 64 && mix[1 - channel] < 64;
}

int main(void)
{
    MD30Connection connection;
    if (!md30_open_connection(&connection)) {
        return 1;
    }
    size_t rate_count;
    const MD30SampleRate *rates = md30_sample_rates(&rate_count);
    bool success = true;
    for (size_t index = 0; index < rate_count; ++index) {
        const unsigned int rate = rates[index].hertz;
        const bool left_ok = test_channel(&connection, rate, 0);
        const bool right_ok = test_channel(&connection, rate, 1);
        success = left_ok && right_ok && success;
    }
    md30_close_connection(&connection);
    return success ? 0 : 1;
}
