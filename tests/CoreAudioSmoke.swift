import CoreAudio
import Foundation

@main
struct CoreAudioSmoke {
    static func main() async throws {
        let device = MD30Audio.deviceID()
        guard device != kAudioObjectUnknown, MD30Audio.isAlive(device), MD30Audio.connected() else {
            throw MD30Audio.StatusError(action: "查找系统中的 MD30", status: -1)
        }

        let originalRate = MD30Audio.sampleRate(of: device)
        defer { try? MD30Audio.setRate(originalRate, of: device) }
        let rates = MD30Audio.supportedRates(of: device)
        guard rates == [44100, 48000, 88200, 96000, 176400, 192000] else {
            throw MD30Audio.StatusError(action: "核对驱动公布的采样率", status: -1)
        }
        for rate in rates {
            try MD30Audio.setRate(rate, of: device)
            for _ in 0..<100 {
                if MD30Audio.sampleRate(of: device) == rate { break }
                try await Task.sleep(for: .milliseconds(10))
            }
            guard MD30Audio.sampleRate(of: device) == rate else {
                throw MD30Audio.StatusError(action: "验证 \(rate) Hz", status: -1)
            }
            print("System Core Audio device \(device): \(rate) Hz")
            try await TestTone.play(rate: rate)
            print("Core Audio test tone submitted to MUSILAND MD30 at device rate \(rate) Hz")
        }
    }
}
