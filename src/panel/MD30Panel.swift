import AppKit
import SwiftUI

@main
struct MD30PanelApp: App {
    @NSApplicationDelegateAdaptor(PanelLifecycle.self) private var lifecycle

    var body: some Scene {
        MenuBarExtra {
            PanelView()
        } label: {
            Label {
                Text("MD30")
            } icon: {
                MD30Icon.menu
            }
            .labelStyle(.titleAndIcon)
        }
        .menuBarExtraStyle(.window)
    }
}

private struct PanelView: View {
    @State private var connected = false
    @State private var device: UInt32 = 0
    @State private var alive = false
    @State private var rate = 0
    @State private var supportedRates: [Int] = []
    @State private var isDefault = false
    @State private var exclusiveAllowed: Bool?
    @State private var exclusivePID: Int32 = -1
    @State private var integerBits = 0
    @State private var busy = false
    @State private var notice = ""
    @State private var failed = false

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack(spacing: 14) {
                MD30Icon.panel
                    .foregroundStyle(Color.accentColor)
                    .frame(width: 46, height: 46)
                    .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 12))
                VStack(alignment: .leading, spacing: 4) {
                    Text("MUSILAND MD30").font(.title2.weight(.semibold))
                    status(connected ? "USB 已连接" : "等待 USB 连接", active: connected)
                }
            }

            GroupBox {
                VStack(alignment: .leading, spacing: 12) {
                    HStack {
                        Text("系统音频驱动")
                        Spacer()
                        status(driverStatus, active: alive)
                    }
                    Divider()
                    HStack {
                        Text("系统采样率")
                        Spacer()
                        Picker("采样率", selection: Binding(get: { rate }, set: selectRate)) {
                            if !supportedRates.contains(rate) { Text("—").tag(rate) }
                            ForEach(supportedRates, id: \.self) { hertz in
                                Text(rateLabel(hertz)).tag(hertz)
                            }
                        }
                        .labelsHidden()
                        .pickerStyle(.menu)
                        .frame(width: 145)
                        .disabled(!alive || controlsBlocked || supportedRates.isEmpty)
                    }
                }
                .padding(8)
            }

            HStack {
                Button(isDefault ? "已用作系统输出" : "用作系统输出", action: makeDefault)
                    .disabled(!alive || isDefault || controlsBlocked)
                Spacer()
                if busy { ProgressView().controlSize(.small) }
                Button("播放测试音", action: play)
                    .buttonStyle(.borderedProminent)
                    .disabled(!connected || controlsBlocked)
            }

            GroupBox {
                VStack(alignment: .leading, spacing: 6) {
                    Toggle("允许播放器使用整数独占", isOn: Binding(
                        get: { exclusiveAllowed ?? false }, set: setExclusiveAllowed))
                        .disabled(exclusiveAllowed == nil || exclusivePID > 0 || busy)
                    Text(exclusiveStatus).font(.footnote).foregroundStyle(.secondary)
                }
                .padding(8)
            }

            HStack {
                Button("载入原厂启动代码…", action: chooseRuntime).disabled(!connected || controlsBlocked)
                Spacer()
                Button("音频 MIDI 设置", action: openAudioMIDISetup)
                Button("退出") { NSApp.terminate(nil) }
            }
            .controlSize(.small)

            if !notice.isEmpty {
                Text(notice)
                    .font(.footnote)
                    .foregroundStyle(failed ? Color.red : Color.secondary)
                    .textSelection(.enabled)
                    .lineLimit(4)
            }
        }
        .padding(24)
        .frame(width: 465)
        .task {
            while !Task.isCancelled {
                refresh()
                try? await Task.sleep(for: .seconds(1))
            }
        }
    }

    private var controlsBlocked: Bool { busy || exclusivePID > 0 || integerBits > 0 }

    private var exclusiveStatus: String {
        if exclusiveAllowed == nil { return "驱动尚未提供整数独占" }
        if exclusivePID > 0 {
            return integerBits > 0 ? "播放器已独占 · \(integerBits) 位整数 PCM" : "播放器已独占 · 浮点 PCM"
        }
        if exclusiveAllowed == false { return "整数独占已关闭" }
        return "在播放器中选择 MD30 并开启独占与整数输出"
    }

    private func setExclusiveAllowed(_ enabled: Bool) {
        perform { try MD30Audio.setIntegerModeAllowed(enabled, of: device) }
    }

    private var driverStatus: String {
        if device == 0 { return "尚未加载" }
        return alive ? "可用" : "等待设备"
    }

    private func status(_ text: String, active: Bool) -> some View {
        Label(text, systemImage: active ? "checkmark.circle.fill" : "circle")
            .font(.callout)
            .foregroundStyle(active ? Color.green : Color.secondary)
    }

    private func refresh() {
        connected = MD30Audio.connected()
        device = MD30Audio.deviceID()
        alive = MD30Audio.isAlive(device)
        rate = MD30Audio.sampleRate(of: device)
        supportedRates = MD30Audio.supportedRates(of: device)
        isDefault = MD30Audio.isDefault(device)
        exclusiveAllowed = MD30Audio.integerModeAllowed(device)
        exclusivePID = MD30Audio.hogPID(device)
        integerBits = MD30Audio.integerBits(device)
    }

    private func rateLabel(_ hertz: Int) -> String {
        let kilohertz = Double(hertz) / 1000
        return kilohertz.formatted(.number.precision(.fractionLength(0...1))) + " kHz"
    }

    private func selectRate(_ value: Int) {
        perform { try MD30Audio.setRate(value, of: device) }
    }

    private func makeDefault() {
        perform { try MD30Audio.makeDefault(device) }
    }

    private func play() {
        let useCoreAudio = alive
        run {
            if useCoreAudio {
                try await TestTone.play(rate: MD30Audio.sampleRate(of: device))
            } else {
                _ = try await TestTone.runUSBTool(["--tone"])
            }
            return "测试音已送出，顺序为左声道、右声道。"
        }
    }

    private func chooseRuntime() {
        let picker = NSOpenPanel()
        picker.message = "选择原厂 2.0.1.0 的 x64 MlCyMD_win7.sys。"
        picker.canChooseDirectories = false
        guard picker.runModal() == .OK, let file = picker.url else { return }
        run {
            _ = try await TestTone.runUSBTool(["--load-runtime", file.path])
            return "原厂启动代码已载入设备 RAM。"
        }
    }

    private func openAudioMIDISetup() {
        NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Applications/Utilities/Audio MIDI Setup.app"))
    }

    private func perform(_ action: () throws -> Void) {
        do {
            try action()
            notice = ""
        } catch {
            notice = error.localizedDescription
            failed = true
        }
        refresh()
    }

    private func run(_ action: @escaping @MainActor () async throws -> String) {
        busy = true
        notice = ""
        Task {
            defer {
                busy = false
                refresh()
            }
            do {
                notice = try await action()
                failed = false
            } catch {
                notice = error.localizedDescription
                failed = true
            }
        }
    }
}
