#include "wav_pcm.h"
#include "pcm_words.h"

#include <string.h>

static uint16_t read16(const uint8_t *bytes)
{
    return bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t read32(const uint8_t *bytes)
{
    return read16(bytes) | (uint32_t)read16(bytes + 2) << 16;
}

static bool read_format(MD30Wav *wav, uint32_t size)
{
    uint8_t format[40];
    if (size < 16 || fread(format, 1, size < 40 ? size : 40, wav->file) != (size < 40 ? size : 40)) {
        return false;
    }
    const uint16_t type = read16(format);
    const uint16_t container = read16(format + 14);
    if (read16(format + 2) != 2 || (container != 16 && container != 24 && container != 32)) {
        return false;
    }

    uint16_t bits = container;
    if (type == 0xfffe) {
        const uint8_t pcm_guid[] = {1, 0, 0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113};
        if (size < 40 || read16(format + 16) < 22 || read16(format + 16) > size - 18 ||
            memcmp(format + 24, pcm_guid, sizeof(pcm_guid)) != 0) {
            return false;
        }
        bits = read16(format + 18);
    } else if (type != 1) {
        return false;
    }
    if (bits > container || (bits != 16 && bits != 24 && bits != 32) ||
        read16(format + 12) != container / 4) {
        return false;
    }
    wav->rate = read32(format + 4);
    wav->bits = bits;
    wav->sample_bytes = container / 8;
    return (uint64_t)wav->rate * 2 * wav->sample_bytes == read32(format + 8);
}

static bool parse(MD30Wav *wav)
{
    FILE *file = wav->file;
    uint8_t header[12];
    if (fseeko(file, 0, SEEK_END) != 0) {
        return false;
    }
    const off_t file_size = ftello(file);
    if (fseeko(file, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }
    const off_t end = 8 + (off_t)read32(header + 4);
    if (end > file_size) {
        return false;
    }
    off_t data = 0;
    uint32_t data_size = 0;
    for (off_t offset = 12; offset <= end - 8; ) {
        if (fread(header, 1, 8, file) != 8) {
            return false;
        }
        const uint32_t size = read32(header + 4);
        const off_t payload = offset + 8;
        offset = payload + size + (size & 1);
        if (offset > end) {
            return false;
        }
        if (!wav->sample_bytes && memcmp(header, "fmt ", 4) == 0 && !read_format(wav, size)) {
            return false;
        }
        if (!data && memcmp(header, "data", 4) == 0) {
            data = payload;
            data_size = size;
        }
        if (data && wav->sample_bytes) {
            const uint16_t frame_bytes = 2 * wav->sample_bytes;
            wav->frames_left = data_size / frame_bytes;
            return wav->frames_left && data_size % frame_bytes == 0 && fseeko(file, data, SEEK_SET) == 0;
        }
        if (fseeko(file, offset, SEEK_SET) != 0) {
            return false;
        }
    }
    return false;
}

bool md30_wav_open(MD30Wav *wav, const char *path)
{
    *wav = (MD30Wav){.file = fopen(path, "rb")};
    if (!wav->file) {
        perror(path);
        return false;
    }
    if (parse(wav)) {
        return true;
    }
    fclose(wav->file);
    fputs("Expected a stereo WAV with 16-, 24- or 32-bit integer PCM\n", stderr);
    return false;
}

bool md30_wav_read(MD30Wav *wav, uint8_t *output, size_t frames)
{
    const size_t samples = 2 * frames;
    const size_t bytes = wav->sample_bytes;
    if (frames > wav->frames_left || fread(output, bytes, samples, wav->file) != samples) {
        fputs("Could not read the complete PCM block\n", stderr);
        return false;
    }
    md30_expand_pcm(output, samples, bytes);
    wav->frames_left -= (uint32_t)frames;
    return true;
}
