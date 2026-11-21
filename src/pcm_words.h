#ifndef MD30_PCM_WORDS_H
#define MD30_PCM_WORDS_H

#include <stdint.h>
#include <string.h>

// Expand packed little-endian PCM in place into the MD30's left-aligned words.
static inline void md30_expand_pcm(uint8_t *output, size_t samples, size_t bytes)
{
    if (bytes < 4) {
        for (size_t sample = samples; sample > 0; --sample) {
            uint8_t *word = output + 4 * (sample - 1);
            memmove(word + (4 - bytes), output + bytes * (sample - 1), bytes);
            memset(word, 0, 4 - bytes);
        }
    }
}

#endif
