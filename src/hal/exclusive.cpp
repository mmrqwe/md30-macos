#include "playback.hpp"

#include <cerrno>
#include <csignal>

CFPropertyListRef Playback::ExclusiveOwner()
{
    std::lock_guard lock(transportLock_);
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &owner_);
}

OSStatus Playback::SetExclusiveOwner(pid_t client, bool acquire)
{
    std::lock_guard lock(transportLock_);
    if (owner_ && owner_ != client) {
        return kAudioDevicePermissionsError;
    }
    if (acquire && !owner_) {
        stopTransport();
        owner_ = client;
    } else if (!acquire && owner_) {
        releaseExclusive();
    }
    return kAudioHardwareNoError;
}

void Playback::ReapExclusiveOwner()
{
    std::lock_guard lock(transportLock_);
    if (owner_ && kill(owner_, 0) == -1 && errno == ESRCH) {
        releaseExclusive();
    }
}

void Playback::releaseExclusive()
{
    owner_ = 0;
    if (ioActive_) {
        startTransport();
    }
}
