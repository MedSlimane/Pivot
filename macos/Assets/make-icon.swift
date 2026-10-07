import AppKit

// Renders Pivot's 1024x1024 icon source: dark slate squircle + white
// rotate-arrows glyph (arrow.triangle.2.circlepath).
// Usage: swift make-icon.swift  (writes icon-1024.png next to this script)

let S: CGFloat = 1024
let img = NSImage(size: NSSize(width: S, height: S))
img.lockFocus()
NSColor.clear.setFill()
NSRect(x: 0, y: 0, width: S, height: S).fill()

let bg = NSBezierPath(roundedRect: NSRect(x: 0, y: 0, width: S, height: S), xRadius: S * 0.24, yRadius: S * 0.24)
let top = NSColor(srgbRed: 0.24, green: 0.28, blue: 0.34, alpha: 1.0)
let bottom = NSColor(srgbRed: 0.09, green: 0.11, blue: 0.15, alpha: 1.0)
NSGradient(colors: [top, bottom])!.draw(in: bg, angle: 90)

NSGraphicsContext.saveGraphicsState()
bg.setClip()
let sheenTop = NSColor(white: 1.0, alpha: 0.12)
let sheenBottom = NSColor(white: 1.0, alpha: 0.0)
NSGradient(colors: [sheenBottom, sheenTop])!.draw(
    in: NSRect(x: 0, y: S * 0.45, width: S, height: S * 0.55), angle: 90)
NSGraphicsContext.restoreGraphicsState()

if let sym = NSImage(systemSymbolName: "arrow.triangle.2.circlepath", accessibilityDescription: nil),
   let configured = sym.withSymbolConfiguration(
       NSImage.SymbolConfiguration(pointSize: S * 0.42, weight: .medium)) {
    let tinted = NSImage(size: configured.size)
    tinted.lockFocus()
    NSColor(srgbRed: 0.93, green: 0.96, blue: 1.0, alpha: 1.0).set()
    let r = NSRect(origin: .zero, size: configured.size)
    r.fill()
    configured.draw(in: r, from: .zero, operation: .destinationIn, fraction: 1.0)
    tinted.unlockFocus()

    let shadow = NSShadow()
    shadow.shadowColor = NSColor(white: 0.0, alpha: 0.35)
    shadow.shadowBlurRadius = S * 0.03
    shadow.shadowOffset = NSSize(width: 0, height: -S * 0.015)
    shadow.set()

    let origin = NSPoint(x: (S - configured.size.width) / 2,
                         y: (S - configured.size.height) / 2)
    tinted.draw(at: origin, from: .zero, operation: .sourceOver, fraction: 1.0)
    NSShadow().set()
}

img.unlockFocus()

guard let tiff = img.tiffRepresentation,
      let rep = NSBitmapImageRep(data: tiff),
      let png = rep.representation(using: .png, properties: [:]) else {
    fputs("ERROR: failed to render PNG\n", stderr)
    exit(1)
}
let out = URL(fileURLWithPath: "icon-1024.png")
try png.write(to: out)
print("Wrote \(out.path) (\(png.count) bytes)")
