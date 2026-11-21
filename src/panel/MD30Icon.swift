import AppKit
import SwiftUI

@MainActor
enum MD30Icon {
    static let menu = image(side: 18)
    static let panel = image(side: 25)

    private static func image(side: CGFloat) -> Image {
        guard let url = Bundle.main.url(forResource: "MD30", withExtension: "svg"),
              let image = NSImage(contentsOf: url) else {
            return Image(systemName: "hifispeaker.fill")
        }
        image.size = NSSize(width: side, height: side)
        image.isTemplate = true
        return Image(nsImage: image)
    }
}
