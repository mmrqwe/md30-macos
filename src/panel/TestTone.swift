import AudioToolbox
import Foundation

enum TestTone {
    static func play(rate: Int) async throws {
        guard rate > 0 else {
            throw MD30Audio.StatusError(action: "读取采样率", status: -1)
        }
        var format = AudioStreamBasicDescription(
            mSampleRate: Double(rate), mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagsNativeFloatPacked,
            mBytesPerPacket: 8, mFramesPerPacket: 1, mBytesPerFrame: 8,
            mChannelsPerFrame: 2, mBitsPerChannel: 32, mReserved: 0
        )
        var output: AudioQueueRef?
        try MD30Audio.check(AudioQueueNewOutput(&format, { _, _, _ in }, nil, nil, nil, 0, &output),
                            "创建测试音播放器")
        guard let output else { return }
        defer { AudioQueueDispose(output, true) }

        let selectStatus = withUnsafePointer(to: MD30Audio.uid) {
            AudioQueueSetProperty(output, kAudioQueueProperty_CurrentDevice, $0,
                                  UInt32(MemoryLayout<CFString>.size))
        }
        try MD30Audio.check(selectStatus, "选择 MD30")

        let bytesPerBuffer = UInt32(rate * Int(format.mBytesPerFrame))
        for second in 0..<3 {
            var buffer: AudioQueueBufferRef?
            try MD30Audio.check(AudioQueueAllocateBuffer(output, bytesPerBuffer, &buffer), "分配音频缓冲区")
            guard let buffer else { throw MD30Audio.StatusError(action: "分配音频缓冲区", status: -1) }
            let samples = buffer.pointee.mAudioData.bindMemory(to: Float32.self, capacity: rate * 2)
            for frame in 0..<rate {
                let time = Double(second) + Double(frame) / format.mSampleRate
                samples[frame * 2] = sample(time - 0.25, frequency: 440)
                samples[frame * 2 + 1] = sample(time - 1.25, frequency: 997)
            }
            buffer.pointee.mAudioDataByteSize = bytesPerBuffer
            try MD30Audio.check(AudioQueueEnqueueBuffer(output, buffer, 0, nil), "提交测试音")
        }
        try MD30Audio.check(AudioQueueStart(output, nil), "播放测试音")
        try await Task.sleep(for: .milliseconds(3300))
    }

    static func runUSBTool(_ arguments: [String]) async throws -> String {
        guard let tool = Bundle.main.url(forAuxiliaryExecutable: "md30-usb") else {
            throw MD30Audio.StatusError(action: "查找 USB 工具", status: -1)
        }
        return try await Task.detached(priority: .userInitiated) {
            let process = Process()
            let pipe = Pipe()
            process.executableURL = tool
            process.arguments = arguments
            process.standardOutput = pipe
            process.standardError = pipe
            try process.run()
            let result = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            let message = String(decoding: result, as: UTF8.self)
            guard process.terminationStatus == 0 else {
                throw USBFailure(message: message.trimmingCharacters(in: .whitespacesAndNewlines))
            }
            return message
        }.value
    }

    private struct USBFailure: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    private static func sample(_ time: Double, frequency: Double) -> Float32 {
        let gain = max(0, min(1, min(time, 0.8 - time) / 0.02))
        return Float32(0.04 * gain * sin(2 * .pi * frequency * time))
    }
}
