#pragma once

#include <aspl/Device.hpp>
#include <mach/mach_time.h>

inline AudioStreamBasicDescription md30Format(Float64 rate, unsigned bits = 0)
{
    const UInt32 bytes = bits ? bits / 4 : 8;
    const UInt32 flags = bits ? kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonMixable
                              : kAudioFormatFlagsNativeFloatPacked;
    return {rate, kAudioFormatLinearPCM, flags, bytes, 1, bytes, 2, bits ? bits : 32, 0};
}

class PlaybackClock {
public:
    void reset(Float64 rate)
    {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        ticksPerFrame_ = 1e9 * timebase.denom / timebase.numer / rate;
        hostTime_ = mach_absolute_time();
        sampleTime_ = 0;
        ++seed_;
    }

    void timestamp(UInt32 period, double scale, Float64* sample, UInt64* host, UInt64* seed)
    {
        if (!hostTime_) {
            reset(44100);
        }
        const auto ticks = static_cast<UInt64>(period * ticksPerFrame_ * scale);
        const auto elapsed = mach_absolute_time() - hostTime_;
        const auto periods = elapsed / ticks;
        hostTime_ += periods * ticks;
        sampleTime_ += periods * period;
        *sample = sampleTime_;
        *host = hostTime_;
        *seed = seed_;
    }

private:
    double ticksPerFrame_ = 1;
    UInt64 hostTime_ = 0;
    Float64 sampleTime_ = 0;
    UInt64 seed_ = 1;
};
