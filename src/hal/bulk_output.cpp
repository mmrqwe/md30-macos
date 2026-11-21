#include "bulk_output.hpp"

#include <algorithm>
#include <time.h>

BulkOutput::BulkOutput(const MD30Connection& connection, unsigned int rate)
    : interface_(connection.interface), pipe_(connection.playback_pipe),
      depth_(std::min(queueDepth(rate), maxTransfers))
{
    status_ = (*interface_)->CreateInterfaceAsyncEventSource(interface_, &source_);
    if (status_ == kIOReturnSuccess) {
        CFRunLoopAddSource(CFRunLoopGetCurrent(), source_, kCFRunLoopDefaultMode);
        for (auto& transfer : transfers_) {
            transfer.output = this;
        }
    }
}

BulkOutput::~BulkOutput()
{
    if (!source_) {
        return;
    }
    stopping_ = true;
    if (pending_) {
        (*interface_)->AbortPipe(interface_, pipe_);
        const uint64_t deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) + 450000000;
        while (pending_ && clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        }
    }
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source_, kCFRunLoopDefaultMode);
    CFRunLoopSourceInvalidate(source_);
    CFRelease(source_);
}

BulkOutput::Samples* BulkOutput::waitForBuffer()
{
    if (pending_ == depth_) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    }
    if (status_ != kIOReturnSuccess) {
        return nullptr;
    }
    for (size_t i = 0; i < depth_; ++i) {
        if (!transfers_[i].pending) {
            ready_ = &transfers_[i];
            return &ready_->samples;
        }
    }
    return nullptr;
}

void BulkOutput::submit()
{
    status_ = (*interface_)->WritePipeAsyncTO(interface_, pipe_, ready_->samples.data(),
                                            sizeof(ready_->samples), 200, 400, completion, ready_);
    if (status_ == kIOReturnSuccess) {
        ready_->pending = true;
        ++pending_;
    }
}

void BulkOutput::completion(void* context, IOReturn status, void* bytes)
{
    auto& transfer = *static_cast<Transfer*>(context);
    transfer.output->receive(transfer, status, reinterpret_cast<uintptr_t>(bytes));
}

void BulkOutput::receive(Transfer& transfer, IOReturn status, size_t bytes)
{
    transfer.pending = false;
    --pending_;
    if (stopping_) {
        return;
    }
    if (status != kIOReturnSuccess || bytes != sizeof(transfer.samples)) {
        status_ = status != kIOReturnSuccess ? status : kIOReturnUnderrun;
        return;
    }
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if (completed_++ > 100) {
        longestGap_ = std::max(longestGap_, now - previousCompletion_);
    }
    previousCompletion_ = now;
}
