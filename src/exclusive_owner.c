#include "exclusive_owner.h"
#include "md30_audio_ids.h"

#include <stdio.h>
#include <unistd.h>

static const AudioObjectPropertyAddress owner_property = {
    MD30_EXCLUSIVE_OWNER, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain,
};

static AudioObjectID find_device(void)
{
    const AudioObjectPropertyAddress lookup = {
        kAudioHardwarePropertyTranslateUIDToDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain,
    };
    const CFStringRef uid = CFSTR(MD30_AUDIO_UID);
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &lookup,
                                                       sizeof(uid), &uid, &size, &device);
    return status == noErr ? device : kAudioObjectUnknown;
}

bool md30_coreaudio_ready(void)
{
    const AudioObjectID device = find_device();
    if (device == kAudioObjectUnknown || !AudioObjectHasProperty(device, &owner_property)) {
        return false;
    }
    const AudioObjectPropertyAddress volume = {
        kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain,
    };
    Boolean settable = false;
    return AudioObjectIsPropertySettable(device, &volume, &settable) == noErr && settable;
}

static bool request_owner(AudioObjectID device, bool acquire)
{
    const int pid = acquire ? getpid() : -getpid();
    const CFNumberRef value = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    if (!value) {
        return false;
    }
    const OSStatus status = AudioObjectSetPropertyData(device, &owner_property, 0, NULL,
                                                       sizeof(value), &value);
    CFRelease(value);
    if (status != noErr) {
        fprintf(stderr, "%s MD30 exclusive access failed (%d)\n", acquire ? "Requesting" : "Releasing", status);
        return false;
    }
    return true;
}

bool md30_acquire_exclusive(AudioObjectID *device)
{
    *device = find_device();
    if (*device == kAudioObjectUnknown || !AudioObjectHasProperty(*device, &owner_property)) {
        // Standalone operation without the HAL driver still requires an exclusive IOKit open.
        *device = kAudioObjectUnknown;
        return true;
    }
    return request_owner(*device, true);
}

bool md30_release_exclusive(AudioObjectID device)
{
    return device == kAudioObjectUnknown || request_owner(device, false);
}
