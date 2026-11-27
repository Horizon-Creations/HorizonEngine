// Encodes a folder of frame_NNNNN.ppm (what a camera flight writes, src/HE_Editor/EditorFlythrough.h) into an H.264 MP4 with
// AVFoundation, for Macs without ffmpeg. scripts/he_flythrough.py calls it; by hand:
//   swift scripts/he_video_encode.swift <frames dir> <fps> <out.mp4>
import AVFoundation
import CoreVideo
import Foundation

func fail(_ s: String) -> Never { FileHandle.standardError.write((s + "\n").data(using: .utf8)!); exit(1) }

let args = CommandLine.arguments
if args.count < 4 { fail("usage: he_video_encode.swift <frames dir> <fps> <out.mp4>") }
let dir = URL(fileURLWithPath: args[1])
guard let fps = Int32(args[2]), fps > 0 else { fail("fps must be a positive whole number") }
let out = URL(fileURLWithPath: args[3])

let frames = (try? FileManager.default.contentsOfDirectory(atPath: dir.path))?
    .filter { $0.hasPrefix("frame_") && $0.hasSuffix(".ppm") }.sorted() ?? []
if frames.isEmpty { fail("no frame_*.ppm in \(dir.path)") }

// P6, maxval 255: three header tokens after the magic, then one whitespace byte, then width * height * 3 bytes.
func readPPM(_ url: URL) -> (Int, Int, Data)? {
    guard let d = try? Data(contentsOf: url), d.count > 2, d[0] == 0x50, d[1] == 0x36 else { return nil }
    var vals: [Int] = []
    var i = 2
    while vals.count < 3 && i < d.count {
        while i < d.count && (d[i] == 0x20 || d[i] == 0x0A || d[i] == 0x0D || d[i] == 0x09) { i += 1 }
        var v = 0
        while i < d.count && d[i] >= 0x30 && d[i] <= 0x39 { v = v * 10 + Int(d[i] - 0x30); i += 1 }
        vals.append(v)
    }
    i += 1
    let w = vals[0], h = vals[1]
    guard vals.count == 3, vals[2] == 255, d.count - i >= w * h * 3 else { return nil }
    return (w, h, d.subdata(in: i..<(i + w * h * 3)))
}

guard let first = readPPM(dir.appendingPathComponent(frames[0])) else { fail("\(frames[0]) is not a P6 PPM") }
let (w, h) = (first.0 & ~1, first.1 & ~1)   // H.264 wants even sides
try? FileManager.default.removeItem(at: out)
guard let writer = try? AVAssetWriter(outputURL: out, fileType: .mp4) else { fail("cannot write \(out.path)") }
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: w, AVVideoHeightKey: h,
    AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: w * h * 6],
])
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: w,
    kCVPixelBufferHeightKey as String: h,
])
writer.add(input)
writer.startWriting()
writer.startSession(atSourceTime: .zero)

for (n, name) in frames.enumerated() {
    guard let (fw, _, rgb) = readPPM(dir.appendingPathComponent(name)) else { fail("\(name) is not a P6 PPM") }
    while !input.isReadyForMoreMediaData { Thread.sleep(forTimeInterval: 0.002) }
    guard let pool = adaptor.pixelBufferPool else { fail("no pixel buffer pool") }
    var pb: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pb)
    guard let buf = pb else { fail("no pixel buffer") }
    CVPixelBufferLockBaseAddress(buf, [])
    let base = CVPixelBufferGetBaseAddress(buf)!.assumingMemoryBound(to: UInt8.self)
    let stride = CVPixelBufferGetBytesPerRow(buf)
    rgb.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
        for y in 0..<h {
            let row = base + y * stride
            for x in 0..<w {
                let s = (y * fw + x) * 3
                row[x * 4 + 0] = src[s + 2]; row[x * 4 + 1] = src[s + 1]; row[x * 4 + 2] = src[s]; row[x * 4 + 3] = 255
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(buf, [])
    if !adaptor.append(buf, withPresentationTime: CMTime(value: CMTimeValue(n), timescale: fps)) { fail("frame \(n): \(writer.error?.localizedDescription ?? "append failed")") }
}
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
if writer.status != .completed { fail("encoding failed: \(writer.error?.localizedDescription ?? "unknown")") }
print("\(frames.count) frames, \(w)x\(h) at \(fps) fps -> \(out.path)")
