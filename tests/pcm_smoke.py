#!/usr/bin/env python3
"""Verify MD30 hardware writes against independently generated 32-bit words."""
import hashlib
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + (b'\0' if len(data) % 2 else b'')


def fixture(path, rate, bits, container):
    width = container // 8
    samples = bytearray()
    expected = hashlib.sha256()
    frames = rate + 19  # Exercise the partial final USB block.
    for frame in range(frames):
        time = frame / rate
        fade = min(1, time / 0.02, (frames - 1 - frame) / rate / 0.02)
        for channel, pitch in enumerate((440, 997)):
            value = 0 if fade == 0 else int((1 << (bits - 1)) * 0.04 * fade * math.sin(math.tau * pitch * time))
            if fade > 0:
                value = (value & ~1) | ((frame + channel) & 1)
            samples.extend((value << (container - bits)).to_bytes(width, 'little', signed=True))
            expected.update(struct.pack('<i', value << (32 - bits)))
    fmt = struct.pack('<HHIIHH', 1 if container == bits else 0xfffe, 2,
                      rate, rate * width * 2, width * 2, container)
    if container != bits:
        fmt += struct.pack('<HHI', 22, bits, 3) + bytes.fromhex('0100000000001000800000aa00389b71')
    wav = b'WAVE' + chunk(b'JUNK', b'x') + chunk(b'fmt ', fmt) + chunk(b'data', samples)
    path.write_bytes(chunk(b'RIFF', wav))
    return expected.hexdigest(), frames


def check_path(tool, path, rate, bits, container):
    expected, frames = fixture(path, rate, bits, container)
    result = subprocess.run([tool, '--verify-pcm', str(path)], check=True,
                            text=True, stdout=subprocess.PIPE)
    print(result.stdout, end='', flush=True)
    assert f'{bits}-bit PCM, {rate} Hz, {frames} stereo frames sent' in result.stdout
    assert f'USB PCM SHA-256 {expected}' in result.stdout
    peaks = re.search(r'Peaks USB L=(\d+) R=(\d+), analog mix L=(\d+) R=(\d+)', result.stdout)
    assert peaks and all(1800 < int(peak) < 3400 for peak in peaks.groups())


def main():
    tool = sys.argv[1]
    rates = [int(rate) for rate in sys.argv[2:]] or [44100, 48000, 88200, 96000, 176400, 192000]
    with tempfile.TemporaryDirectory(prefix='md30-pcm-') as directory:
        path = Path(directory) / 'fixture.wav'
        for rate in rates:
            for bits in (16, 24, 32):
                check_path(tool, path, rate, bits, bits)
        check_path(tool, path, rates[0], 24, 32)
        path.write_bytes(path.read_bytes()[:-1])
        result = subprocess.run([tool, '--verify-pcm', path], capture_output=True, text=True)
        assert result.returncode != 0 and 'Expected a stereo WAV' in result.stderr


if __name__ == '__main__':
    main()
