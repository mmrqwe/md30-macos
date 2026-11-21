import AppKit
import Foundation

let arguments = Array(CommandLine.arguments.dropFirst())
guard arguments.count == 2, let source = NSImage(contentsOfFile: arguments[0]) else {
    fputs("Usage: render-app-icon.swift ICON.svg OUTPUT.iconset\n", stderr)
    exit(1)
}
let iconset = URL(fileURLWithPath: arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

func png(pixels: Int) -> Data? {
    guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
            bytesPerRow: 0, bitsPerPixel: 0),
          let context = NSGraphicsContext(bitmapImageRep: bitmap) else { return nil }
    NSGraphicsContext.saveGraphicsState()
    defer { NSGraphicsContext.restoreGraphicsState() }
    NSGraphicsContext.current = context
    context.imageInterpolation = .high
    source.draw(in: NSRect(x: 0, y: 0, width: pixels, height: pixels), from: .zero,
                operation: .copy, fraction: 1, respectFlipped: false, hints: nil)
    return bitmap.representation(using: .png, properties: [:])
}

for side in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        guard let data = png(pixels: side * scale) else {
            fputs("Could not render the app icon at \(side * scale) pixels.\n", stderr)
            exit(1)
        }
        let suffix = scale == 2 ? "@2x" : ""
        try data.write(to: iconset.appendingPathComponent("icon_\(side)x\(side)\(suffix).png"))
    }
}
