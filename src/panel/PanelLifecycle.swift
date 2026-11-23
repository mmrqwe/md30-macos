import AppKit
import Foundation
import os

@MainActor
final class PanelLifecycle: NSObject, NSApplicationDelegate {
    private let log = Logger(subsystem: "local.md30.panel", category: "USB")
    private var timer: Timer?
    private var wasMissing = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        checkConnection()
        let monitor = Timer(timeInterval: 0.5, target: self, selector: #selector(checkConnection),
                            userInfo: nil, repeats: true)
        RunLoop.main.add(monitor, forMode: .common)
        timer = monitor
    }

    func applicationWillTerminate(_ notification: Notification) {
        timer?.invalidate()
        timer = nil
    }

    @objc private func checkConnection() {
        let missing = !MD30Audio.connected()
        // Confirm the loss across two checks so a brief USB reset can settle.
        if missing && wasMissing {
            log.info("MD30 unavailable on USB; closing the panel")
            NSApp.terminate(nil)
        }
        wasMissing = missing
    }
}
