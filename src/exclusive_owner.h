#ifndef MD30_EXCLUSIVE_OWNER_H
#define MD30_EXCLUSIVE_OWNER_H

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>

bool md30_acquire_exclusive(AudioObjectID *device);
bool md30_release_exclusive(AudioObjectID device);
bool md30_coreaudio_ready(void);

#endif
