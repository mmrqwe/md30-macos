#pragma once

#include "bulk_output.hpp"
#include <aspl/Device.hpp>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>

class Playback final : public aspl::ControlRequestHandler, public aspl::IORequestHandler {
public:
    explicit Playback(const std::shared_ptr<aspl::Device>& device);
    ~Playback() override;
    OSStatus OnStartIO() override;
    void OnStopIO() override;
    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>&, Float64, Float64,
                            const void* samples, UInt32 byteCount) override;
    CFPropertyListRef ExclusiveOwner();
    OSStatus SetExclusiveOwner(pid_t client, bool acquire);
    void ReapExclusiveOwner();
    void SetIntegerBits(unsigned bits) { integerBits_.store(bits); }
    double ClockScale() const { return clockScale_.load(std::memory_order_relaxed); }

private:
    static constexpr size_t capacity = 65536;
    static constexpr size_t blockFrames = BulkOutput::blockFrames;
    struct Frame {
        uint8_t bytes[8];
        float sample(size_t channel) const;
    };
    static_assert(sizeof(Frame) == sizeof(float) * 2);

    OSStatus startTransport();
    void stopTransport();
    void releaseExclusive();
    void run(unsigned int rate);

    std::weak_ptr<aspl::Device> device_;
    std::mutex transportLock_;
    bool ioActive_ = false;
    pid_t owner_ = 0;
    MD30Connection connection_{};
    std::array<Frame, capacity> ring_{};
    std::atomic<uint64_t> write_{0};
    std::atomic<uint64_t> read_{0};
    std::atomic<uint64_t> overflowFrames_{0};
    std::atomic<unsigned> integerBits_{0};
    std::atomic<double> clockScale_{1};
    std::atomic<bool> running_{false};
    std::thread worker_;
};
