// rb_scene_test: SwiftUI scenes through ImageRenderer (RenderBox on Metal), the features Tahoe's
// layered app icons use: vector paths, gradients, blur, shadows, Liquid Glass. Renders one scene
// to a PNG; with a reference PNG (the same scene rendered with no Metal device, i.e. RenderBox's
// CPU path on the same OS: DYLD_INSERT_LIBRARIES=nvlab-nometal.dylib) it compares pixel by pixel.
// Usage: rb_scene_test <scene> <out.png> [ref.png]
//   scenes: blob lgrad rgrad blur shadow glass text all
// Build (Tahoe SDK): swiftc -O -parse-as-library rb_scene_test.swift -o rb_scene_test
import SwiftUI
import AppKit

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
    let name: String
    var body: some View {
        ZStack {
            switch name {
            case "blob":
                Blob().fill(.blue)
            case "lgrad":
                RoundedRectangle(cornerRadius: 28).fill(LinearGradient(colors: [.red, .yellow, .blue], startPoint: .top, endPoint: .bottom))
            case "rgrad":
                Circle().fill(RadialGradient(colors: [.white, .blue, .black], center: .center, startRadius: 4, endRadius: 60))
            case "blur":
                Blob().fill(.orange).padding(20).blur(radius: 8)
            case "shadow":
                RoundedRectangle(cornerRadius: 24).fill(.white).padding(24).shadow(color: .black.opacity(0.6), radius: 10, x: 0, y: 6)
            case "glass":
                ZStack {
                    LinearGradient(colors: [.purple, .orange], startPoint: .topLeading, endPoint: .bottomTrailing)
#if compiler(>=6.2)
                    Text("Aa").font(.system(size: 40, weight: .bold)).foregroundStyle(.white)
                        .frame(width: 96, height: 96)
                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 26))
#endif
                }
            case "text":
                Text(">_").font(.system(size: 56, weight: .semibold, design: .monospaced)).foregroundStyle(.green)
            default:   // all
                ZStack {
                    RoundedRectangle(cornerRadius: 28).fill(LinearGradient(colors: [.blue, .cyan], startPoint: .top, endPoint: .bottom))
                    Blob().fill(.white.opacity(0.8)).padding(30).shadow(radius: 6)
                    Text("A").font(.system(size: 30, weight: .heavy)).foregroundStyle(.red)
                }
            }
        }
        .frame(width: 128, height: 128)
    }
}

func pixels(_ cg: CGImage) -> [UInt8] {
    let w = cg.width, h = cg.height
    var px = [UInt8](repeating: 0, count: w * h * 4)
    let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
    return px
}

@MainActor func run() -> Int32 {
    let args = CommandLine.arguments
    guard args.count >= 3 else { print("usage: rb_scene_test <scene> <out.png> [ref.png]"); return 2 }
    let r = ImageRenderer(content: Scene(name: args[1]))
    r.scale = 1
    guard let cg = r.cgImage else { print("rb_scene_test \(args[1]): no image"); return 2 }
    try? NSBitmapImageRep(cgImage: cg).representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: args[2]))
    let px = pixels(cg)
    let vis = stride(from: 3, to: px.count, by: 4).filter { px[$0] > 8 }.count
    let dev = MTLCreateSystemDefaultDevice()?.name ?? "no Metal (CPU)"
    guard args.count >= 4, let ref = NSImage(contentsOfFile: args[3])?.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
        print("rb_scene_test \(args[1]) on \(dev): vis \(vis) -> \(args[2])")
        return 0
    }
    let rp = pixels(ref)
    guard rp.count == px.count else { print("rb_scene_test \(args[1]): size differs from the reference"); return 1 }
    var maxd = 0, sum = 0, off = 0
    for i in 0..<(px.count / 4) {
        var d = 0
        for k in 0..<4 { d = max(d, abs(Int(px[i * 4 + k]) - Int(rp[i * 4 + k]))) }
        maxd = max(maxd, d); sum += d
        if d > 24 { off += 1 }
    }
    let n = px.count / 4
    let pct = Double(off) * 100 / Double(n)
    let ok = pct < 1.0
    print(String(format: "rb_scene_test %@ on %@: vis %d, vs reference max %d mean %.2f, %.2f%% pixels off by >24: %@",
                 args[1], dev, vis, maxd, Double(sum) / Double(n), pct, ok ? "PASS" : "FAIL"))
    return ok ? 0 : 1
}

@main struct Main {
    @MainActor static func main() { exit(run()) }
}
