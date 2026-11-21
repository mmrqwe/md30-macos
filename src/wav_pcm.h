#ifndef MD30_WAV_PCM_H
#define MD30_WAV_PCM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *file;
    uint32_t rate;
    uint16_t bits;
    uint16_t sample_bytes;
    uint32_t frames_left;
} MD30Wav;

bool md30_wav_open(MD30Wav *wav, const char *path);
bool md30_wav_read(MD30Wav *wav, uint8_t *output, size_t frames);

#endif
