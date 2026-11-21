#include "exclusive_play.h"
#include "exclusive_owner.h"
#include "usb_device.h"
#include "usb_runtime.h"
#include "wav_pcm.h"

#include <CommonCrypto/CommonDigest.h>
#include <signal.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t cancelled;

static void cancel_playback(int signal_number)
{
    (void)signal_number;
    cancelled = 1;
}

static bool send(MD30Connection *connection, void *data, UInt32 bytes)
{
    IOUSBInterfaceInterface **interface = connection->interface;
    const IOReturn status = (*interface)->WritePipeTO(interface, connection->playback_pipe,
                                                    data, bytes, 300, 500);
    return cancelled || md30_usb_ok(status, "Send PCM to MD30");
}

static bool read_meters(MD30Connection *connection, UInt16 peaks[4])
{
    return md30_read_peak(connection->device, 2, peaks)
        && md30_read_peak(connection->device, 3, peaks + 1)
        && md30_read_peak(connection->device, 6, peaks + 2)
        && md30_read_peak(connection->device, 7, peaks + 3);
}

static bool stream(MD30Connection *connection, MD30Wav *wav, bool verify)
{
    enum { block_frames = 256, frame_bytes = 8 };
    uint8_t block[block_frames * frame_bytes] = {0};
    CC_SHA256_CTX digest;
    if (verify) {
        CC_SHA256_Init(&digest);
    }
    // Keep USB packets full, including the last block. Silence primes and drains the FIFO.
    const unsigned int lead_blocks = wav->rate / 10 / block_frames + 1;
    const unsigned int drain_blocks = wav->rate / 4 / block_frames + 1;
    for (unsigned int i = 0; !cancelled && i < lead_blocks; ++i) {
        if (!send(connection, block, sizeof(block))) {
            return false;
        }
    }
    UInt16 peaks[4] = {0};
    if (verify && !cancelled && !read_meters(connection, peaks)) {
        return false;
    }
    const uint32_t total = wav->frames_left;
    const uint64_t start = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    while (!cancelled && wav->frames_left) {
        const size_t frames = wav->frames_left < block_frames ? wav->frames_left : block_frames;
        const size_t bytes = frames * frame_bytes;
        if (!md30_wav_read(wav, block, frames)) {
            return false;
        }
        memset(block + bytes, 0, sizeof(block) - bytes);
        if (!send(connection, block, sizeof(block))) {
            return false;
        }
        if (verify) {
            CC_SHA256_Update(&digest, block, (CC_LONG)bytes);
        }
    }
    const double elapsed = (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - start) / 1e9;
    memset(block, 0, sizeof(block));
    for (unsigned int i = 0; !cancelled && i < drain_blocks; ++i) {
        if (verify && i == 0 && !read_meters(connection, peaks)) {
            return false;
        }
        if (!send(connection, block, sizeof(block))) {
            return false;
        }
    }
    if (cancelled) {
        return true;
    }
    printf("%u-bit PCM, %u Hz, %u stereo frames sent in %.3f s\n", wav->bits, wav->rate, total, elapsed);
    if (verify) {
        printf("Peaks USB L=%u R=%u, analog mix L=%u R=%u\n", peaks[0], peaks[1], peaks[2], peaks[3]);
        unsigned char hash[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(hash, &digest);
        fputs("USB PCM SHA-256 ", stdout);
        for (size_t i = 0; i < sizeof(hash); ++i) {
            printf("%02x", hash[i]);
        }
        putchar('\n');
    }
    return true;
}

bool md30_play_wav(const char *path, bool verify)
{
    MD30Wav wav;
    if (!md30_wav_open(&wav, path)) {
        return false;
    }
    size_t count;
    const MD30SampleRate *rates = md30_sample_rates(&count);
    size_t index = 0;
    while (index < count && rates[index].hertz != wav.rate) {
        ++index;
    }
    if (index == count) {
        fprintf(stderr, "MD30 does not support %u Hz\n", wav.rate);
        fclose(wav.file);
        return false;
    }

    struct sigaction action = {.sa_handler = cancel_playback};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    AudioObjectID owner;
    if (!md30_acquire_exclusive(&owner)) {
        fclose(wav.file);
        return false;
    }
    MD30Connection connection;
    bool ok = cancelled;
    if (!cancelled && md30_open_connection(&connection)) {
        ok = md30_start_stream(connection.device, wav.rate) && stream(&connection, &wav, verify);
        ok = md30_stop_stream(connection.device) && (ok || cancelled);
        md30_close_connection(&connection);
    }
    fclose(wav.file);
    return md30_release_exclusive(owner) && ok;
}
