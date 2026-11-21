#include "playback.hpp"
#include "output_format.hpp"
#include "md30_audio_ids.h"
#include "usb_runtime.h"

#include <aspl/Driver.hpp>
#include <aspl/Plugin.hpp>
#include <aspl/Stream.hpp>
#include <dispatch/dispatch.h>
#include <algorithm>

namespace {
class OutputDevice final : public aspl::Device {
public:
    using aspl::Device::Device;
    UInt32 GetTransportType() const override { return kAudioDeviceTransportTypeUSB; }
    UInt32 GetLatency() const override
    {
        const auto rate = static_cast<unsigned int>(GetNominalSampleRate());
        return rate / 25 + 1024 + BulkOutput::queueDepth(rate) * BulkOutput::blockFrames;
    }

    unsigned IntegerBits() const { return integerBits_.load(); }
    bool IntegerEnabled() const { return integerEnabled_.load(); }

    void SetPlayback(const std::shared_ptr<Playback>& playback, const std::shared_ptr<aspl::Stream>& stream)
    {
        stream_ = stream;
        RegisterCustomProperty(MD30_INTEGER_ENABLED,
            [this]() -> CFPropertyListRef { return CFRetain(IntegerEnabled() ? kCFBooleanTrue : kCFBooleanFalse); },
            std::function<void(CFPropertyListRef)>([](CFPropertyListRef) {}));
        playback_ = playback;
        RegisterCustomProperty(MD30_EXCLUSIVE_OWNER,
            [playback] { return playback->ExclusiveOwner(); },
            std::function<void(CFPropertyListRef)>([](CFPropertyListRef) {}));
    }

    OSStatus SetPropertyData(AudioObjectID object, pid_t client,
                             const AudioObjectPropertyAddress* address, UInt32 qualifierSize,
                             const void* qualifier, UInt32 size, const void* data) override
    {
        if (object != GetID() || (address->mSelector != MD30_EXCLUSIVE_OWNER &&
                                  address->mSelector != MD30_INTEGER_ENABLED)) {
            return Device::SetPropertyData(object, client, address, qualifierSize, qualifier, size, data);
        }
        if (!data || size != sizeof(CFPropertyListRef)) {
            return kAudioHardwareBadPropertySizeError;
        }
        auto value = *static_cast<const CFPropertyListRef*>(data);
        if (address->mSelector == MD30_INTEGER_ENABLED) {
            if (!value || CFGetTypeID(value) != CFBooleanGetTypeID()) {
                return kAudioHardwareIllegalOperationError;
            }
            const bool enabled = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
            {
                std::lock_guard lock(formatLock_);
                integerEnabled_.store(enabled);
            }
            if (!enabled && IntegerBits()) {
                RequestConfigurationChange([this] {
                    if (!IntegerEnabled()) {
                        ApplyFormat(md30Format(GetNominalSampleRate()));
                    }
                });
            }
            NotifyPropertyChanged(MD30_INTEGER_ENABLED);
            stream_->NotifyPropertiesChanged({kAudioStreamPropertyAvailablePhysicalFormats,
                                              kAudioStreamPropertyAvailableVirtualFormats});
            return kAudioHardwareNoError;
        }
        int request = 0;
        if (client <= 0 || !value || CFGetTypeID(value) != CFNumberGetTypeID() ||
            !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &request) ||
            (request != client && request != -client)) {
            return kAudioDevicePermissionsError;
        }
        return playback_->SetExclusiveOwner(client, request > 0);
    }

    OSStatus ApplyFormat(const AudioStreamBasicDescription& value)
    {
        std::lock_guard lock(formatLock_);
        const unsigned bits = value.mFormatFlags & kAudioFormatFlagIsNonMixable ? value.mBitsPerChannel : 0;
        if (bits && !IntegerEnabled()) {
            return kAudioDeviceUnsupportedFormatError;
        }
        const bool rateChanged = GetNominalSampleRate() != value.mSampleRate;
        if (rateChanged) {
            const auto status = SetNominalSampleRateImpl(value.mSampleRate);
            if (status != noErr) {
                return status;
            }
            NotifyPropertyChanged(kAudioDevicePropertyNominalSampleRate);
        }
        integerBits_.store(bits);
        stream_->NotifyPropertyChanged(kAudioStreamPropertyVirtualFormat);
        stream_->NotifyPropertyChanged(kAudioStreamPropertyPhysicalFormat);
        return noErr;
    }

protected:
    OSStatus StartIOImpl(UInt32 client, UInt32 startCount) override
    {
        if (!startCount) {
            playback_->SetIntegerBits(IntegerBits());
        }
        const auto status = Device::StartIOImpl(client, startCount);
        if (!startCount && status == noErr) {
            clock_.reset(GetNominalSampleRate());
        }
        return status;
    }

    OSStatus GetZeroTimeStampImpl(UInt32, Float64* sample, UInt64* host, UInt64* seed) override
    {
        clock_.timestamp(GetZeroTimeStampPeriod(), playback_->ClockScale(), sample, host, seed);
        return noErr;
    }

    OSStatus WillDoIOOperationImpl(UInt32 client, UInt32 operation, Boolean* willDo, Boolean* inPlace) override
    {
        if (IntegerBits() && operation == kAudioServerPlugInIOOperationProcessMix) {
            *willDo = false;
            *inPlace = false;
            return noErr;
        }
        return Device::WillDoIOOperationImpl(client, operation, willDo, inPlace);
    }

private:
    std::shared_ptr<Playback> playback_;
    std::shared_ptr<aspl::Stream> stream_;
    std::mutex formatLock_;
    std::atomic<unsigned> integerBits_{0};
    std::atomic<bool> integerEnabled_{true};
    PlaybackClock clock_;
};

class OutputStream final : public aspl::Stream {
public:
    OutputStream(const std::shared_ptr<aspl::Context>& context,
                 const std::shared_ptr<OutputDevice>& device, const aspl::StreamParameters& params)
        : aspl::Stream(context, device, params), device_(device) {}

    AudioStreamBasicDescription GetPhysicalFormat() const override
    {
        const auto device = device_.lock();
        return device ? md30Format(device->GetNominalSampleRate(), device->IntegerBits()) : md30Format(44100);
    }

    AudioStreamBasicDescription GetVirtualFormat() const override { return GetPhysicalFormat(); }

    std::vector<AudioStreamRangedDescription> GetAvailablePhysicalFormats() const override
    {
        auto formats = Stream::GetAvailablePhysicalFormats();
        if (const auto device = device_.lock(); device && !device->IntegerEnabled()) {
            formats.erase(std::remove_if(formats.begin(), formats.end(), [](const auto& entry) {
                return entry.mFormat.mFormatFlags & kAudioFormatFlagIsNonMixable;
            }), formats.end());
        }
        return formats;
    }

    std::vector<AudioStreamRangedDescription> GetAvailableVirtualFormats() const override
    {
        return GetAvailablePhysicalFormats();
    }

protected:
    OSStatus SetPhysicalFormatImpl(const AudioStreamBasicDescription& value) override
    {
        const auto device = device_.lock();
        return device ? device->ApplyFormat(value)
                      : kAudioHardwareBadDeviceError;
    }

    OSStatus SetVirtualFormatImpl(const AudioStreamBasicDescription& value) override
    {
        return SetPhysicalFormatImpl(value);
    }

private:
    std::weak_ptr<OutputDevice> device_;
};

bool attached()
{
    io_service_t service = md30_find_device();
    if (!service) {
        return false;
    }
    IOObjectRelease(service);
    return true;
}

class ConnectionMonitor final : public aspl::DriverRequestHandler {
public:
    ConnectionMonitor(std::shared_ptr<aspl::Device> device, std::shared_ptr<Playback> playback)
        : device_(std::move(device)), playback_(std::move(playback)) {}
    ~ConnectionMonitor() override
    {
        if (timer_) {
            dispatch_source_cancel(timer_);
            dispatch_sync_f(queue_, nullptr, [](void*) {});
            dispatch_release(timer_);
            dispatch_release(queue_);
        }
    }

    OSStatus OnInitialize() override
    {
        queue_ = dispatch_queue_create("local.md30.connection", DISPATCH_QUEUE_SERIAL);
        timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
        dispatch_set_context(timer_, this);
        dispatch_source_set_event_handler_f(timer_, [](void* context) {
            auto& monitor = *static_cast<ConnectionMonitor*>(context);
            auto& device = monitor.device_;
            monitor.playback_->ReapExclusiveOwner();
            const bool connected = attached();
            if (connected != device->GetIsAlive()) {
                device->SetIsAlive(connected);
            }
        });
        dispatch_source_set_timer(timer_, DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC, 100 * NSEC_PER_MSEC);
        dispatch_resume(timer_);
        return kAudioHardwareNoError;
    }

private:
    const std::shared_ptr<aspl::Device> device_;
    const std::shared_ptr<Playback> playback_;
    dispatch_queue_t queue_ = nullptr;
    dispatch_source_t timer_ = nullptr;
};

std::shared_ptr<aspl::Driver> makeDriver()
{
    auto tracer = std::make_shared<aspl::Tracer>(aspl::Tracer::Mode::Noop);
    auto context = std::make_shared<aspl::Context>(tracer);
    aspl::DeviceParameters parameters;
    parameters.Name = "MUSILAND MD30";
    parameters.Manufacturer = "MUSILAND";
    parameters.DeviceUID = MD30_AUDIO_UID;
    parameters.ModelUID = "local.md30.model.5030";
    parameters.ConfigurationApplicationBundleID = "local.md30.panel";
    parameters.ZeroTimeStampPeriod = 512;
    auto device = std::make_shared<OutputDevice>(context, parameters);
    device->SetIsAlive(attached());
    size_t rateCount;
    const MD30SampleRate* rates = md30_sample_rates(&rateCount);
    std::vector<AudioValueRange> rateRanges;
    std::vector<AudioStreamRangedDescription> formats;
    for (size_t index = 0; index < rateCount; ++index) {
        const auto rate = static_cast<Float64>(rates[index].hertz);
        rateRanges.push_back({rate, rate});
        for (unsigned bits : {0, 16, 24, 32}) {
            formats.push_back({md30Format(rate, bits), {rate, rate}});
        }
    }
    device->SetAvailableSampleRatesAsync(std::move(rateRanges));

    aspl::StreamParameters streamParams;
    streamParams.Direction = aspl::Direction::Output;
    streamParams.Format = md30Format(44100);
    auto stream = std::make_shared<OutputStream>(context, device, streamParams);
    device->AddStreamAsync(stream);
    stream->SetAvailablePhysicalFormatsAsync(formats);
    stream->AttachVolumeControl(device->AddVolumeControlAsync(kAudioObjectPropertyScopeOutput));
    stream->AttachMuteControl(device->AddMuteControlAsync(kAudioObjectPropertyScopeOutput));

    auto playback = std::make_shared<Playback>(device);
    device->SetPlayback(playback, stream);
    device->SetControlHandler(playback);
    device->SetIOHandler(playback);
    auto plugin = std::make_shared<aspl::Plugin>(context);
    plugin->AddDevice(device);
    auto driver = std::make_shared<aspl::Driver>(context, plugin);
    driver->SetDriverHandler(std::make_shared<ConnectionMonitor>(device, playback));
    return driver;
}
}

extern "C" __attribute__((visibility("default")))
void* MD30Factory(CFAllocatorRef, CFUUIDRef type)
{
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }
    static auto driver = makeDriver();
    return driver->GetReference();
}
