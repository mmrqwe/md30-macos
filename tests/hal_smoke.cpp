#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

namespace {
AudioServerPlugInDriverRef driver;
AudioObjectID device;

void check(bool valid, const char* operation)
{
    if (!valid) {
        std::fprintf(stderr, "HAL smoke test failed: %s\n", operation);
        std::exit(1);
    }
}

OSStatus propertyChanged(AudioServerPlugInHostRef, AudioObjectID, UInt32,
                         const AudioObjectPropertyAddress*)
{
    return noErr;
}

OSStatus readStorage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef* result)
{
    *result = nullptr;
    return noErr;
}

OSStatus writeStorage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef)
{
    return noErr;
}

OSStatus deleteFromStorage(AudioServerPlugInHostRef, CFStringRef)
{
    return noErr;
}

OSStatus configurationChange(AudioServerPlugInHostRef, AudioObjectID object,
                             UInt64 action, void* info)
{
    return (*driver)->PerformDeviceConfigurationChange(driver, object, action, info);
}

template<typename T>
T read(AudioObjectID object, AudioObjectPropertySelector selector,
       AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    AudioObjectPropertyAddress property{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    T result{};
    check((*driver)->GetPropertyData(driver, object, 0, &property, 0, nullptr,
              sizeof(result), &size, &result) == noErr && size == sizeof(result), "read object property");
    return result;
}

void configure(Float64 rate)
{
    AudioObjectPropertyAddress property{kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    check((*driver)->SetPropertyData(driver, device, 0, &property, 0, nullptr,
                  sizeof(rate), &rate) == noErr, "request sample rate");
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (read<Float64>(device, kAudioDevicePropertyNominalSampleRate) == rate) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(false, "confirm sample rate");
}

void play(AudioObjectID stream, UInt32 client, Float64 rate)
{
    constexpr UInt32 frames = 256;
    std::array<float, frames * 2> silence{};
    check((*driver)->StartIO(driver, device, client) == noErr, "start I/O and claim the MD30");
    AudioServerPlugInIOCycleInfo cycle{};
    const auto start = std::chrono::steady_clock::now();
    for (UInt32 frame = 0; frame < rate * 2; frame += frames) {
        cycle.mOutputTime.mSampleTime = frame;
        check((*driver)->DoIOOperation(driver, device, stream, client,
                  kAudioServerPlugInIOOperationWriteMix, frames, &cycle, silence.data(), nullptr) == noErr,
              "submit stereo audio to driver");
        if (frame == static_cast<UInt32>(rate / 2) / frames * frames) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        std::this_thread::sleep_until(start + std::chrono::duration<double>((frame + frames) / rate));
    }
    check((*driver)->StopIO(driver, device, client) == noErr, "stop I/O and release USB");
    std::printf("HAL adapter completed real MD30 StartIO/WriteMix/StopIO at %.0f Hz\n", rate);
}
}

int main(int argc, char** argv)
{
    check(argc == 2, "provide path to the MD30 bundle executable");
    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        std::fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    auto factory = reinterpret_cast<void* (*)(CFAllocatorRef, CFUUIDRef)>(dlsym(module, "MD30Factory"));
    check(factory != nullptr, "load factory");
    driver = static_cast<AudioServerPlugInDriverRef>(factory(nullptr, kAudioServerPlugInTypeUUID));
    check(driver != nullptr, "initialize factory");
    static AudioServerPlugInHostInterface host{
        propertyChanged, readStorage, writeStorage, deleteFromStorage, configurationChange
    };
    check((*driver)->Initialize(driver, &host) == noErr, "initialize driver");
    device = read<AudioObjectID>(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList);
    check(read<UInt32>(device, kAudioDevicePropertyDeviceIsAlive) == 1, "detect real MD30");
    check(read<UInt32>(device, kAudioDevicePropertyTransportType) == kAudioDeviceTransportTypeUSB,
          "publish USB transport");
    const auto stream = read<AudioObjectID>(device, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    AudioServerPlugInClientInfo client{};
    client.mClientID = 1;
    client.mProcessID = getpid();
    client.mIsNativeEndian = true;
    check((*driver)->AddDeviceClient(driver, device, &client) == noErr, "add audio client");
    for (Float64 rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
        configure(rate);
        const auto format = read<AudioStreamBasicDescription>(stream, kAudioStreamPropertyPhysicalFormat);
        check(format.mSampleRate == rate && format.mChannelsPerFrame == 2 && format.mBytesPerFrame == 8,
              "publish active stereo format");
        play(stream, client.mClientID, rate);
    }
    check((*driver)->RemoveDeviceClient(driver, device, &client) == noErr, "remove audio client");
}
