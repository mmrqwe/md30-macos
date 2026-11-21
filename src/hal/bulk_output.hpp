#pragma once

#include "usb_device.h"

#include <array>
#include <cstdint>

class BulkOutput {
public:
    static constexpr size_t blockFrames = 256;
    using Samples = std::array<int32_t, blockFrames * 2>;

    static constexpr size_t queueDepth(unsigned int rate)
    {
        return (rate * 24 + blockFrames * 1000 - 1) / (blockFrames * 1000);
    }

    BulkOutput(const MD30Connection& connection, unsigned int rate);
    ~BulkOutput();
    BulkOutput(const BulkOutput&) = delete;
    BulkOutput& operator=(const BulkOutput&) = delete;

    IOReturn status() const { return status_; }
    Samples* waitForBuffer();
    void submit();
    size_t completed() const { return completed_; }
    uint64_t longestGap() const { return longestGap_; }

private:
    static constexpr size_t maxTransfers = 18;
    struct Transfer {
        Samples samples{};
        BulkOutput* output = nullptr;
        bool pending = false;
    };

    static void completion(void* context, IOReturn status, void* bytes);
    void receive(Transfer& transfer, IOReturn status, size_t bytes);

    IOUSBInterfaceInterface** interface_;
    const UInt8 pipe_;
    const size_t depth_;
    CFRunLoopSourceRef source_ = nullptr;
    IOReturn status_ = kIOReturnSuccess;
    size_t pending_ = 0;
    size_t completed_ = 0;
    uint64_t previousCompletion_ = 0;
    uint64_t longestGap_ = 0;
    bool stopping_ = false;
    Transfer* ready_ = nullptr;
    std::array<Transfer, maxTransfers> transfers_;
};
