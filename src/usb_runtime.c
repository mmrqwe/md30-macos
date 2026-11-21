#include "usb_runtime.h"

#include <CommonCrypto/CommonDigest.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool vendor_transfer(IOUSBDeviceInterface **device, UInt8 type, UInt8 request,
                            UInt16 value, void *bytes, UInt16 length)
{
    IOUSBDevRequestTO transfer = {
        .bmRequestType = type,
        .bRequest = request,
        .wValue = value,
        .wLength = length,
        .pData = bytes,
        .noDataTimeout = 1000,
        .completionTimeout = 2000,
    };
    const IOReturn result = (*device)->DeviceRequestTO(device, &transfer);
    if (result != kIOReturnSuccess || transfer.wLenDone != length) {
        fprintf(stderr, "USB request %02x/%02x at %04x: status 0x%08x, %u/%u bytes\n",
                type, request, value, (unsigned int)result, (unsigned int)transfer.wLenDone, length);
        return false;
    }
    return true;
}

bool md30_load_runtime(IOUSBDeviceInterface **device, const char *driver_path)
{
    // Embedded MD30-specific FX2 image in the x64 MlCyMD_win7.sys 2.0.1.0 release.
    enum { image_offset = 0x12350, image_size = 0x17eb, chunk_size = 64 };
    static const UInt8 expected[CC_SHA256_DIGEST_LENGTH] = {
        0x40, 0x2e, 0xad, 0x41, 0x2e, 0xb1, 0xe7, 0x04,
        0xcb, 0x15, 0xbc, 0x50, 0xb4, 0xe9, 0x4d, 0x16,
        0xef, 0xc5, 0xcb, 0xd5, 0xa5, 0x1b, 0x7a, 0x4c,
        0x6a, 0xb1, 0x70, 0xf2, 0x63, 0xf4, 0xfd, 0xa5,
    };
    FILE *file = fopen(driver_path, "rb");
    if (!file) {
        perror(driver_path);
        return false;
    }

    UInt8 image[image_size], digest[sizeof(expected)];
    const bool extracted = fseek(file, image_offset, SEEK_SET) == 0
                        && fread(image, sizeof(image), 1, file) == 1;
    fclose(file);
    if (!extracted || !CC_SHA256(image, sizeof(image), digest)
        || memcmp(digest, expected, sizeof(expected)) != 0) {
        fputs("Expected the matching official x64 MlCyMD_win7.sys 2.0.1.0\n", stderr);
        return false;
    }

    // CPUCS holds the CPU in reset while A0 writes the volatile program RAM.
    UInt8 reset = 1;
    if (!vendor_transfer(device, 0x40, 0xa0, 0xe600, &reset, 1)) {
        return false;
    }
    for (UInt16 offset = 0; offset < sizeof(image); offset += chunk_size) {
        const UInt16 remaining = sizeof(image) - offset;
        const UInt16 length = remaining < chunk_size ? remaining : chunk_size;
        if (!vendor_transfer(device, 0x40, 0xa0, offset, image + offset, length)) {
            fputs("RAM upload incomplete. Power-cycle MD30 to restore its startup state.\n", stderr);
            return false;
        }
    }
    reset = 0;
    if (!vendor_transfer(device, 0x40, 0xa0, 0xe600, &reset, 1)) {
        fputs("RAM startup was not acknowledged. Check whether the MD30 re-enumerated.\n", stderr);
        return false;
    }
    printf("Loaded the official MD30 runtime (%zu bytes) into RAM\n", sizeof(image));
    return true;
}

bool md30_probe_runtime(IOUSBDeviceInterface **device)
{
    UInt8 b5, b6;
    const bool got_b5 = vendor_transfer(device, 0xc0, 0xb5, 0, &b5, 1);
    const bool got_b6 = vendor_transfer(device, 0xc0, 0xb6, 0, &b6, 1);
    if (got_b5 && got_b6) {
        printf("MD30 runtime responses: B5=%02x B6=%02x\n", b5, b6);
    }
    return got_b5 && got_b6;
}

enum {
    sample_rate_44100 = 44100,
    channels = 2,
    block_frames = 512
};

// Clock codes from the MD30 Windows driver's sample-rate switch.
static const MD30SampleRate sample_rates[] = {
    {44100, 0}, {48000, 1}, {88200, 2}, {96000, 3}, {176400, 4}, {192000, 5},
};

const MD30SampleRate *md30_sample_rates(size_t *count)
{
    *count = sizeof(sample_rates) / sizeof(sample_rates[0]);
    return sample_rates;
}

static bool select_clock(IOUSBDeviceInterface **device, unsigned int rate)
{
    size_t rate_count;
    const MD30SampleRate *rates = md30_sample_rates(&rate_count);
    for (size_t index = 0; index < rate_count; ++index) {
        if (rates[index].hertz == rate) {
            UInt8 command[] = {0, rates[index].clock_code};
            return vendor_transfer(device, 0x40, 0xb4, 0x36, command, sizeof(command));
        }
    }
    return false;
}

static bool send_stream_command(IOUSBDeviceInterface **device, UInt8 request, UInt8 value)
{
    return vendor_transfer(device, 0x40, request, 0, &value, 1);
}

bool md30_stop_stream(IOUSBDeviceInterface **device)
{
    const bool stopped = send_stream_command(device, 0xb0, 0);
    const bool cleared = send_stream_command(device, 0xb2, 0);
    return stopped && cleared;
}

bool md30_read_peak(IOUSBDeviceInterface **device, UInt8 reg, UInt16 *peak)
{
    UInt8 bytes[2];
    if (!vendor_transfer(device, 0x40, 0xb4, 0x36, &reg, 1) ||
        !vendor_transfer(device, 0xc0, 0xb3, 0x36, bytes, sizeof(bytes))) {
        return false;
    }
    *peak = ((UInt16)bytes[0] << 8) | bytes[1];
    return true;
}

static bool prepare_analog_mix(IOUSBDeviceInterface **device)
{
    // Reproduce the Windows startup writes for the USB and analog mixer stages.
    UInt8 analog_routes[] = {1, 0xc0};
    if (!vendor_transfer(device, 0x40, 0xb4, 0x36, analog_routes, sizeof(analog_routes))) {
        return false;
    }
    for (UInt8 channel = 2; channel <= 7; ++channel) {
        UInt8 unity[] = {channel, 0x80, 0};
        if (!vendor_transfer(device, 0x40, 0xb4, 0x36, unity, sizeof(unity))) {
            return false;
        }
    }
    return true;
}

bool md30_start_stream(IOUSBDeviceInterface **device, unsigned int rate)
{
    return select_clock(device, rate) && prepare_analog_mix(device) && send_stream_command(device, 0xb2, 0)
        && send_stream_command(device, 0xb0, 1);
}

static void fill_test_tone(int32_t *samples, unsigned int start_frame)
{
    for (unsigned int frame = 0; frame < block_frames; ++frame) {
        const double time = (start_frame + frame) / (double)sample_rate_44100;
        const double left = time - 0.25;
        const double right = time - 1.25;
        const double left_fade = fmax(0, fmin(1, fmin(left, 0.8 - left) / 0.02));
        const double right_fade = fmax(0, fmin(1, fmin(right, 0.8 - right) / 0.02));

        // Low-level signed 32-bit PCM with alternating left and right tones.
        samples[frame * channels] = (int32_t)(85000000 * left_fade * sin(2 * M_PI * 440 * left));
        samples[frame * channels + 1] = (int32_t)(85000000 * right_fade * sin(2 * M_PI * 997 * right));
    }
}

static bool transfer_audio(IOUSBDeviceInterface **device, IOUSBInterfaceInterface **interface,
                           UInt8 pipe, unsigned int rate, bool tone)
{
    enum { warmup_blocks = 16, measured_blocks = 128 };
    int32_t samples[block_frames * channels] = {0};
    const unsigned int total_blocks = tone ? (rate * 3 + block_frames - 1) / block_frames
                                           : warmup_blocks + measured_blocks;
    bool success = md30_start_stream(device, rate);
    uint64_t start = 0;
    for (unsigned int block = 0; success && block < total_blocks; ++block) {
        if (block == warmup_blocks) {
            start = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        }
        if (tone) {
            fill_test_tone(samples, block * block_frames);
        }
        const IOReturn status = (*interface)->WritePipeTO(interface, pipe, samples, sizeof(samples),
                                                          400, 800);
        if (status != kIOReturnSuccess) {
            fprintf(stderr, "USB audio write %u failed (0x%08x)\n", block + 1, (unsigned int)status);
            success = false;
        }
    }

    if (success && tone) {
        printf("Sent a low-level 44.1 kHz left/right test tone to MD30 over USB\n");
    } else if (success) {
        const double seconds = (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - start) / 1e9;
        const double throughput = measured_blocks * sizeof(samples) / seconds;
        const double expected = rate * (channels * sizeof(samples[0]));
        printf("%u Hz: %.0f bytes/s (expected %.0f)\n", rate, throughput, expected);
        success = fabs(throughput - expected) <= expected * 0.02;
    }
    return md30_stop_stream(device) && success;
}

bool md30_test_stream(IOUSBDeviceInterface **device, IOUSBInterfaceInterface **interface,
                      UInt8 pipe, bool tone)
{
    if (tone) {
        return transfer_audio(device, interface, pipe, sample_rate_44100, true);
    }
    size_t rate_count;
    const MD30SampleRate *rates = md30_sample_rates(&rate_count);
    bool success = true;
    for (size_t index = 0; index < rate_count; ++index) {
        success = transfer_audio(device, interface, pipe, rates[index].hertz, false) && success;
    }
    return select_clock(device, sample_rate_44100) && success;
}
