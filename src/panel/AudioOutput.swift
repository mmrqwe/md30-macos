import AudioToolbox
import CoreAudio
import Foundation
import IOKit

struct MD30Audio {
    static let uid = "local.md30.usb.04b4.5030" as CFString
    static let system = AudioObjectID(kAudioObjectSystemObject)
    private static let integerModeSelector: AudioObjectPropertySelector = 0x6d33696e

    struct StatusError: LocalizedError {
        let action: String
        let status: OSStatus

        var errorDescription: String? { "\(action)失败（\(status)）" }
    }

    static func check(_ status: OSStatus, _ action: String) throws {
        if status != noErr {
            throw StatusError(action: action, status: status)
        }
    }

    static func usbMatching() -> NSMutableDictionary? {
        guard let match = IOServiceMatching("IOUSBHostDevice") as NSMutableDictionary? else { return nil }
        match["idVendor"] = 0x04b4
        match["idProduct"] = 0x5030
        return match
    }

    static func connected() -> Bool {
        guard let match = usbMatching() else { return false }
        let device = IOServiceGetMatchingService(kIOMainPortDefault, match)
        guard device != IO_OBJECT_NULL else { return false }
        IOObjectRelease(device)
        return true
    }

    static func deviceID() -> AudioObjectID {
        var property = address(kAudioHardwarePropertyTranslateUIDToDevice)
        let qualifier = uid
        var device = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let status = withUnsafePointer(to: qualifier) {
            AudioObjectGetPropertyData(system, &property, UInt32(MemoryLayout<CFString>.size),
                                       $0, &size, &device)
        }
        return status == noErr ? device : AudioObjectID(kAudioObjectUnknown)
    }

    static func sampleRate(of device: AudioObjectID) -> Int {
        Int(read(device, kAudioDevicePropertyNominalSampleRate, 0.0))
    }

    static func supportedRates(of device: AudioObjectID) -> [Int] {
        guard device != kAudioObjectUnknown else { return [] }
        var property = address(kAudioDevicePropertyAvailableNominalSampleRates)
        let stride = UInt32(MemoryLayout<AudioValueRange>.stride)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(device, &property, 0, nil, &size) == noErr,
              size > 0, size % stride == 0 else { return [] }
        var ranges = [AudioValueRange](repeating: AudioValueRange(), count: Int(size / stride))
        let status = ranges.withUnsafeMutableBytes {
            AudioObjectGetPropertyData(device, &property, 0, nil, &size, $0.baseAddress!)
        }
        guard status == noErr else { return [] }
        return ranges.prefix(Int(size / stride)).compactMap { range in
            guard range.mMinimum == range.mMaximum else { return nil }
            return Int(exactly: range.mMinimum)
        }.sorted()
    }

    static func isDefault(_ device: AudioObjectID) -> Bool {
        guard device != kAudioObjectUnknown else { return false }
        return read(system, kAudioHardwarePropertyDefaultOutputDevice, AudioObjectID(0)) == device
    }

    static func isAlive(_ device: AudioObjectID) -> Bool {
        read(device, kAudioDevicePropertyDeviceIsAlive, UInt32(0)) != 0
    }

    static func hogPID(_ device: AudioObjectID) -> Int32 {
        read(device, kAudioDevicePropertyHogMode, Int32(-1))
    }

    static func integerModeAllowed(_ device: AudioObjectID) -> Bool? {
        guard device != kAudioObjectUnknown else { return nil }
        var property = address(integerModeSelector)
        var value: Unmanaged<CFBoolean>?
        var size = UInt32(MemoryLayout.size(ofValue: value))
        guard AudioObjectHasProperty(device, &property),
              AudioObjectGetPropertyData(device, &property, 0, nil, &size, &value) == noErr,
              let value else { return nil }
        return CFBooleanGetValue(value.takeRetainedValue())
    }

    static func setIntegerModeAllowed(_ enabled: Bool, of device: AudioObjectID) throws {
        var property = address(integerModeSelector)
        let value = Unmanaged.passUnretained((enabled ? kCFBooleanTrue : kCFBooleanFalse)!)
        let size = UInt32(MemoryLayout.size(ofValue: value))
        try withUnsafePointer(to: value) {
            try check(AudioObjectSetPropertyData(device, &property, 0, nil, size, $0), "设置整数独占")
        }
    }

    static func integerBits(_ device: AudioObjectID) -> Int {
        guard device != kAudioObjectUnknown else { return 0 }
        var property = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams,
            mScope: kAudioDevicePropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
        var stream = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout.size(ofValue: stream))
        guard AudioObjectGetPropertyData(device, &property, 0, nil, &size, &stream) == noErr else { return 0 }
        let format = read(stream, kAudioStreamPropertyPhysicalFormat, AudioStreamBasicDescription())
        guard format.mFormatFlags & kAudioFormatFlagIsNonMixable != 0 else { return 0 }
        return Int(format.mBitsPerChannel)
    }

    static func setRate(_ rate: Int, of device: AudioObjectID) throws {
        var value = Float64(rate)
        var property = address(kAudioDevicePropertyNominalSampleRate)
        try check(AudioObjectSetPropertyData(device, &property, 0, nil,
                                             UInt32(MemoryLayout<Float64>.size), &value), "切换采样率")
    }

    static func makeDefault(_ device: AudioObjectID) throws {
        var value = device
        var property = address(kAudioHardwarePropertyDefaultOutputDevice)
        try check(AudioObjectSetPropertyData(system, &property, 0, nil,
                                             UInt32(MemoryLayout<AudioObjectID>.size), &value), "设置系统输出")
    }

    private static func address(_ selector: AudioObjectPropertySelector) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal,
                                   mElement: kAudioObjectPropertyElementMain)
    }

    private static func read<T>(_ device: AudioObjectID, _ selector: AudioObjectPropertySelector,
                                _ fallback: T) -> T {
        guard device != kAudioObjectUnknown else { return fallback }
        var property = address(selector)
        var value = fallback
        var size = UInt32(MemoryLayout<T>.size)
        let status = withUnsafeMutableBytes(of: &value) {
            AudioObjectGetPropertyData(device, &property, 0, nil, &size, $0.baseAddress!)
        }
        return status == noErr ? value : fallback
    }
}
