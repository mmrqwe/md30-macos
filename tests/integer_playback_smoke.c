#include "md30_audio_ids.h"

#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static const AudioObjectPropertyAddress physical = {
    kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain,
};
static const AudioObjectPropertyAddress hog = {
    kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain,
};

static AudioObjectID find_device(void)
{
    const AudioObjectPropertyAddress key = {
        kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain,
    };
    CFStringRef uid = CFSTR(MD30_AUDIO_UID);
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &key, sizeof(uid), &uid, &size, &device);
    return device;
}

static bool set_format(AudioObjectID stream, const AudioStreamBasicDescription *format)
{
    OSStatus status = AudioObjectSetPropertyData(stream, &physical, 0, NULL, sizeof(*format), format);
    if (status != noErr) {
        fprintf(stderr, "Set physical format: %d\n", status);
        return false;
    }
    for (int attempt = 0; attempt < 80; ++attempt) {
        AudioStreamBasicDescription received;
        UInt32 size = sizeof(received);
        status = AudioObjectGetPropertyData(stream, &physical, 0, NULL, &size, &received);
        if (!status && received.mFormatFlags == format->mFormatFlags &&
            received.mSampleRate == format->mSampleRate && received.mBitsPerChannel == format->mBitsPerChannel) {
            return true;
        }
        usleep(25000);
    }
    fputs("Physical format did not change\n", stderr);
    return false;
}

typedef struct {
    unsigned bits;
    double rate;
    atomic_uint frames;
    atomic_int bad_buffer;
} Tone;

static OSStatus playback(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *input,
                         const AudioTimeStamp *input_time, AudioBufferList *output,
                         const AudioTimeStamp *output_time, void *context)
{
    (void)device; (void)now; (void)input; (void)input_time; (void)output_time;
    Tone *tone = context;
    const unsigned width = tone->bits / 8;
    if (output->mNumberBuffers != 1 || output->mBuffers[0].mNumberChannels != 2 ||
        output->mBuffers[0].mDataByteSize % (2 * width) != 0) {
        atomic_store(&tone->bad_buffer, 1);
        return kAudioDeviceUnsupportedFormatError;
    }
    const unsigned frames = output->mBuffers[0].mDataByteSize / (2 * width);
    const unsigned first = atomic_fetch_add(&tone->frames, frames);
    UInt8 *bytes = output->mBuffers[0].mData;
    for (unsigned frame = 0; frame < frames; ++frame) {
        const double time = (first + frame) / tone->rate;
        const double fade = fmax(0, fmin(1, fmin(time, 0.8 - time) / 0.02));
        for (unsigned channel = 0; channel < 2; ++channel) {
            const int32_t amplitude = (int32_t)(ldexp(0.04 * fade, tone->bits - 1) *
                sin(2 * M_PI * (channel ? 997 : 440) * time));
            const uint32_t word = (uint32_t)amplitude | (fade > 0 ? (frame + channel) & 1 : 0);
            for (unsigned byte = 0; byte < width; ++byte) {
                *bytes++ = (UInt8)(word >> (8 * byte));
            }
        }
    }
    return noErr;
}

static bool play(AudioObjectID device, AudioObjectID stream, const AudioStreamBasicDescription *format)
{
    Tone tone = {.bits = format->mBitsPerChannel, .rate = format->mSampleRate};
    if (!set_format(stream, format)) {
        return false;
    }
    AudioDeviceIOProcID callback = NULL;
    OSStatus status = AudioDeviceCreateIOProcID(device, playback, &tone, &callback);
    if (!status) {
        status = AudioDeviceStart(device, callback);
        if (!status) {
            sleep(2);
            status = AudioDeviceStop(device, callback);
        }
        AudioDeviceDestroyIOProcID(device, callback);
    }
    const unsigned frames = atomic_load(&tone.frames);
    printf("Core Audio IOProc: %u-bit at %.0f Hz, %u frames, status %d\n",
           tone.bits, tone.rate, frames, status);
    return !status && !atomic_load(&tone.bad_buffer) && frames > tone.rate;
}

int main(void)
{
    const AudioObjectID device = find_device();
    if (!device) {
        return 1;
    }
    const AudioObjectPropertyAddress streams = {
        kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain,
    };
    AudioObjectID stream;
    UInt32 size = sizeof(stream);
    if (AudioObjectGetPropertyData(device, &streams, 0, NULL, &size, &stream) != noErr) {
        return 1;
    }
    AudioStreamBasicDescription original;
    size = sizeof(original);
    if (AudioObjectGetPropertyData(stream, &physical, 0, NULL, &size, &original) != noErr) {
        return 1;
    }
    pid_t owner = -1;
    if (AudioObjectSetPropertyData(device, &hog, 0, NULL, sizeof(owner), &owner) != noErr) {
        fputs("Hog acquisition failed\n", stderr);
        return 1;
    }
    size = sizeof(owner);
    AudioObjectGetPropertyData(device, &hog, 0, NULL, &size, &owner);
    if (owner != getpid()) {
        fprintf(stderr, "Unexpected hog owner %d (expected %d)\n", owner, getpid());
        return 1;
    }
    printf("Core Audio granted exclusive device access to player PID %d\n", owner);
    bool ok = true;
    for (unsigned bits = 16; bits <= 32 && ok; bits += 8) {
        AudioStreamBasicDescription format = {48000, kAudioFormatLinearPCM,
            kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonMixable,
            bits / 4, 1, bits / 4, 2, bits, 0};
        ok = play(device, stream, &format);
    }
    ok = set_format(stream, &original) && ok;
    return AudioObjectSetPropertyData(device, &hog, 0, NULL, sizeof(owner), &owner) == noErr && ok ? 0 : 1;
}
