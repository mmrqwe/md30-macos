import AppKit
import IOKit
import os

@main
struct USBWatch {
    private static let panelID = "local.md30.panel"
    private static let log = Logger(subsystem: "local.md30.autolaunch", category: "USB")

    static func main() {
        guard let match = MD30Audio.usbMatching(),
              let port = IONotificationPortCreate(kIOMainPortDefault) else {
            log.error("Could not create the MD30 USB notification port")
            exit(1)
        }
        IONotificationPortSetDispatchQueue(port, .main)
        var iterator: io_iterator_t = IO_OBJECT_NULL
        let status = IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, match,
            { _, iterator in USBWatch.attached(iterator) }, nil, &iterator)
        guard status == KERN_SUCCESS else {
            log.error("Could not register the MD30 USB match (\(status))")
            exit(1)
        }
        attached(iterator) // Also drains the initial matches to arm future notifications.
        dispatchMain()
    }

    private static func attached(_ iterator: io_iterator_t) {
        var matched = false
        while case let service = IOIteratorNext(iterator), service != IO_OBJECT_NULL {
            matched = true
            IOObjectRelease(service)
        }
        guard matched, NSRunningApplication.runningApplications(withBundleIdentifier: panelID).isEmpty else {
            return
        }
        let url = URL(fileURLWithPath: "/Applications/MD30 Panel.app", isDirectory: true)
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = false
        NSWorkspace.shared.openApplication(at: url, configuration: configuration) { _, error in
            if let error {
                log.error("Could not open MD30 Panel (\(error.localizedDescription, privacy: .public))")
            } else {
                log.info("Opened MD30 Panel after USB match")
            }
        }
    }
}
