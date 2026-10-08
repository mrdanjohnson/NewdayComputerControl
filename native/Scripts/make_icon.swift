import AppKit
let root = URL(fileURLWithPath: CommandLine.arguments[1])
try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
for (size, filename) in [(16,"icon_16x16.png"),(32,"icon_16x16@2x.png"),(32,"icon_32x32.png"),(64,"icon_32x32@2x.png"),(128,"icon_128x128.png"),(256,"icon_128x128@2x.png"),(256,"icon_256x256.png"),(512,"icon_256x256@2x.png"),(512,"icon_512x512.png"),(1024,"icon_512x512@2x.png")] {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState(); NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    let scale = CGFloat(size) / 1024
    let transform = AffineTransform(scale: scale); (transform as NSAffineTransform).concat()
    let base = NSBezierPath(roundedRect: NSRect(x: 70, y: 70, width: 884, height: 884), xRadius: 196, yRadius: 196)
    NSGradient(starting: NSColor(calibratedRed: 0.06, green: 0.18, blue: 0.23, alpha: 1), ending: NSColor(calibratedRed: 0.08, green: 0.54, blue: 0.57, alpha: 1))!.draw(in: base, angle: 65)
    let chip = NSBezierPath(roundedRect: NSRect(x: 325,y: 325,width: 374,height: 374), xRadius: 58,yRadius: 58)
    NSColor.white.withAlphaComponent(0.92).setStroke(); chip.lineWidth = 24; chip.stroke()
    for value in [400,512,624] {
        for (a,b) in [(NSPoint(x: value,y: 230),NSPoint(x: value,y: 320)),(NSPoint(x:value,y:704),NSPoint(x:value,y:794)),(NSPoint(x:230,y:value),NSPoint(x:320,y:value)),(NSPoint(x:704,y:value),NSPoint(x:794,y:value))] {
            let line = NSBezierPath(); line.move(to: a); line.line(to: b); line.lineWidth = 20; line.lineCapStyle = .round; line.stroke()
        }
    }
    let check = NSBezierPath(); check.move(to: NSPoint(x:416,y:515)); check.line(to:NSPoint(x:483,y:448)); check.line(to:NSPoint(x:612,y:590)); check.lineWidth = 40; check.lineCapStyle = .round; check.lineJoinStyle = .round; check.stroke()
    NSGraphicsContext.restoreGraphicsState()
    try bitmap.representation(using: .png, properties: [:])!.write(to: root.appendingPathComponent(filename))
}
