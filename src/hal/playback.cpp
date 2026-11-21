#include "playback.hpp"
#include "pcm_words.h"
#include "usb_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <os/log.h>
#include <pthread/qos.h>

namespace {
int32_t toPCM(float value)
{
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<int32_t>(std::clamp(static_cast<double>(value) * 2147483648.0,
                                           static_cast<double>(INT32_MIN), static_cast<double>(INT32_MAX)));
}
}

Playback::Playback(const std::shared_ptr<aspl::Device>& device) : device_(device) {}

float Playback::Frame::sample(size_t channel) const
{
    float value;
    std::memcpy(&value, bytes + channel * sizeof(value), sizeof(value));
    return value;
}

Playback::~Playback()
{
    OnStopIO();
}

OSStatus Playback::OnStartIO()
{
    std::lock_guard lock(transportLock_);
    const auto status = owner_ ? kAudioHardwareNoError : startTransport();
    ioActive_ = status == kAudioHardwareNoError;
    return status;
}

void Playback::OnStopIO()
{
    std::lock_guard lock(transportLock_);
    ioActive_ = false;
    stopTransport();
}

OSStatus Playback::startTransport()
{
    const auto device = device_.lock();
    if (!device || !md30_open_connection(&connection_)) {
        os_log_error(OS_LOG_DEFAULT, "MD30 could not claim its USB device");
        return kAudioHardwareNotReadyError;
    }
    const auto rate = static_cast<unsigned int>(device->GetNominalSampleRate());
    if (!md30_start_stream(connection_.device, rate)) {
        stopTransport();
        return kAudioHardwareNotReadyError;
    }

    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    overflowFrames_.store(0, std::memory_order_relaxed);
    clockScale_.store(1, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread(&Playback::run, this, rate);
    } catch (const std::system_error&) {
        stopTransport();
        return kAudioHardwareNotReadyError;
    }
    return kAudioHardwareNoError;
}

void Playback::stopTransport()
{
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    if (connection_.device) {
        md30_stop_stream(connection_.device);
        md30_close_connection(&connection_);
    }
}

void Playback::OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>&, Float64, Float64,
                                  const void* samples, UInt32 byteCount)
{
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t head = write_.load(std::memory_order_relaxed);
    const uint64_t tail = read_.load(std::memory_order_acquire);
    const unsigned bits = integerBits_.load(std::memory_order_relaxed);
    const size_t frameBytes = bits ? bits / 4 : sizeof(Frame);
    const size_t incoming = byteCount / frameBytes;
    const size_t count = std::min<uint64_t>(incoming, capacity - (head - tail));
    if (count < incoming) {
        overflowFrames_.fetch_add(incoming - count, std::memory_order_relaxed);
    }
    const size_t start = head % capacity;
    const size_t first = std::min(count, capacity - start);
    const auto* bytes = static_cast<const unsigned char*>(samples);

    auto* segment = reinterpret_cast<uint8_t*>(ring_.data() + start);
    auto* wrap = reinterpret_cast<uint8_t*>(ring_.data());
    std::memcpy(segment, bytes, first * frameBytes);
    std::memcpy(wrap, bytes + first * frameBytes, (count - first) * frameBytes);
    md30_expand_pcm(segment, first * 2, frameBytes / 2);
    md30_expand_pcm(wrap, (count - first) * 2, frameBytes / 2);
    write_.store(head + count, std::memory_order_release);
}

void Playback::run(unsigned int rate)
{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const auto integerBits = integerBits_.load(std::memory_order_relaxed);
    const size_t target = rate / 25; // Approximately 40 ms of host scheduling headroom.
    const size_t warmup = rate / 40 / blockFrames + 1;
    const double blockTime = blockFrames / static_cast<double>(rate);
    BulkOutput usb(connection_, rate);
    size_t blocks = 0;
    size_t maxReady = 0;
    unsigned int underruns = 0;
    bool primed = false;
    double buffered = target;
    double drift = 0;

    while (running_.load(std::memory_order_acquire) && usb.status() == kIOReturnSuccess) {
        auto* buffer = usb.waitForBuffer();
        if (!buffer || !running_.load(std::memory_order_acquire)) {
            continue;
        }
        auto& samples = *buffer;
        const uint64_t tail = read_.load(std::memory_order_relaxed);
        const size_t ready = write_.load(std::memory_order_acquire) - tail;
        maxReady = std::max(maxReady, ready);
        if (primed && ready < blockFrames) {
            primed = false;
            ++underruns;
        }
        if (!primed && usb.completed() >= warmup && ready >= target) {
            primed = true;
            buffered = ready;
        }
        if (primed) {
            // Filter Core Audio callback bursts and steer the host clock from FIFO fill.
            // Each incoming frame is consumed once, at the DAC’s own rate.
            buffered += std::min(1.0, blockTime / 0.25) * (ready - buffered);
            const double error = (buffered - target) / rate;
            drift = std::clamp(drift + error * blockTime / 30, -0.002, 0.002);
            const double step = 1 + std::clamp(drift + error / 3, -0.004, 0.004);
            clockScale_.store(step, std::memory_order_relaxed);
            if (integerBits) {
                const size_t start = tail % capacity;
                const size_t first = std::min(blockFrames, capacity - start);
                std::memcpy(samples.data(), ring_.data() + start, first * sizeof(Frame));
                std::memcpy(samples.data() + first * 2, ring_.data(), (blockFrames - first) * sizeof(Frame));
            } else {
                for (size_t frame = 0; frame < blockFrames; ++frame) {
                    const auto& value = ring_[(tail + frame) % capacity];
                    samples[frame * 2] = toPCM(value.sample(0));
                    samples[frame * 2 + 1] = toPCM(value.sample(1));
                }
            }
            read_.store(tail + blockFrames, std::memory_order_release);
        } else {
            samples.fill(0);
        }

        usb.submit();
        ++blocks;
    }
    if (usb.status() != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "MD30 USB audio write failed (%{public}x)", usb.status());
        running_.store(false, std::memory_order_release);
    }
    os_log(OS_LOG_DEFAULT, "MD30 buffer at %{public}u Hz, integer bits %{public}u: %{public}u underruns, %{public}llu dropped frames (%{public}zu USB blocks, peak %{public}zu queued frames, max USB completion gap %{public}llu us)",
           rate, integerBits, underruns, overflowFrames_.load(std::memory_order_relaxed), blocks, maxReady,
           usb.longestGap() / 1000);
}
