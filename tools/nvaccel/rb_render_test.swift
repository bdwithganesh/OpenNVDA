// rb_render_test: SwiftUI ImageRenderer (RenderBox on Metal, the renderer IconServices uses for
// Tahoe app icons) draws a red circle on a blue rounded rect; counts visible / red / blue pixels.
// Blank Dock icons (1 Oct 2026): every RenderBox target stayed zero on the RTX.
// Usage: rb_render_test [blob]   (blob: general bezier paths instead of analytic shapes)
// Build: swiftc -O -parse-as-library rb_render_test.swift -o rb_render_test
import SwiftUI
import AppKit

// a general vector path (RenderBox fills these with path_exterior / stencil, not analytically)
struct Blob: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.midX, y: r.minY))
        p.addCurve(to: CGPoint(x: r.maxX, y: r.midY), control1: CGPoint(x: r.maxX * 0.9, y: r.minY), control2: CGPoint(x: r.maxX, y: r.midY * 0.4))
        p.addCurve(to: CGPoint(x: r.midX, y: r.maxY), control1: CGPoint(x: r.maxX, y: r.maxY * 0.9), control2: CGPoint(x: r.midX * 1.3, y: r.maxY))
        p.addCurve(to: CGPoint(x: r.minX, y: r.midY), control1: CGPoint(x: r.midX * 0.5, y: r.maxY), control2: CGPoint(x: r.minX, y: r.maxY * 0.8))
        p.addCurve(to: CGPoint(x: r.midX, y: r.minY), control1: CGPoint(x: r.minX, y: r.midY * 0.3), control2: CGPoint(x: r.midX * 0.4, y: r.minY))
        p.closeSubpath()
        return p
    }
}

struct Scene: View {
    let blob: Bool
    var body: some View {
        ZStack {
            if blob {
                Blob().fill(Color(red: 0, green: 0, blue: 1))
                Blob().fill(Color(red: 1, green: 0, blue: 0)).frame(width: 64, height: 64)
            } else {
                RoundedRectangle(cornerRadius: 24).fill(Color(red: 0, green: 0, blue: 1))
                Circle().fill(Color(red: 1, green: 0, blue: 0)).frame(width: 64, height: 64)
            }
        }
        .frame(width: 128, height: 128)
    }
}

@MainActor func run() -> Int32 {
    let blob = CommandLine.arguments.contains("blob")
    let r = ImageRenderer(content: Scene(blob: blob))
    r.scale = 1
    guard let cg = r.cgImage else { print("rb_render_test: no image"); return 2 }
    let w = cg.width, h = cg.height
    var px = [UInt8](repeating: 0, count: w * h * 4)
    let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
    var vis = 0, red = 0, blue = 0
    for i in 0..<(w * h) {
        let r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2], a = px[i * 4 + 3]
        if a > 8 { vis += 1 }
        if r > 200 && g < 60 && b < 60 && a > 200 { red += 1 }
        if b > 200 && r < 60 && g < 60 && a > 200 { blue += 1 }
    }
    if let dir = ProcessInfo.processInfo.environment["RB_PNG"] {
        let rep = NSBitmapImageRep(cgImage: cg)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: dir + "/rb_render_test.png"))
    }
    // circle ~ pi*32^2 = 3217 px, rounded rect ~ 16384 - corners
    let ok = blob ? vis > 9000 && red > 1500 && blue > 6000 : vis > 15000 && red > 2900 && blue > 11000
    print("rb_render_test\(blob ? " blob" : "") \(w)x\(h) on \(MTLCreateSystemDefaultDevice()?.name ?? "-"): vis \(vis) red \(red) blue \(blue) \(ok ? "PASS" : "FAIL")")
    return ok ? 0 : 1
}

@main struct Main {
    @MainActor static func main() { exit(run()) }
}
