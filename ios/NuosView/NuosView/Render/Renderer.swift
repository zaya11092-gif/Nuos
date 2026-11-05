//
//  Renderer.swift
//  NuosView
//
//  MTKView 60 Hz fixed (CAMetalDisplayLayer / CAFrameRateRange) draw loop:
//    1. Passthrough backdrop: latest ARFrame YCbCr pixelBuffer, dimmed.
//    2. Virtual video layer: HEVC decoder output with rotation-only timewarp
//       (delta = latestPose ∘ renderPose⁻¹ from nuos_video_t render_quat).
//    3. Stereo side-by-side with barrel pre-distortion mesh, per-eye offset,
//       adjustable k1/k2.
//

import ARKit
import Metal
import MetalKit
import simd

final class NuosRenderer: NSObject, MTKViewDelegate {
    // Tunables surfaced to DiagnosticsView
    var distortionK1: Float = 0.15
    var distortionK2: Float = 0.02
    var passthroughEnabled = true
    var videoEnabled = true
    var stereoEnabled = true
    var interEyeDistanceMeters: Float = 0.0625

    // Sources
    var trackingEngine: TrackingEngine?
    var decoder: HEVCDecoder?
    var link: NuosLink?

    private var device: MTLDevice!
    private var queue: MTLCommandQueue!
    private var pipelineYUV: MTLRenderPipelineState?
    private var pipelineVideo: MTLRenderPipelineState?
    private var pipelineComposite: MTLRenderPipelineState?
    private var passthroughYTexture: MTLTexture?
    private var passthroughCbCrTexture: MTLTexture?
    private var videoYTexture: MTLTexture?
    private var videoCbCrTexture: MTLTexture?

    // Latest decoded CVPixelBuffer bridged into a Metal texture.
    private var lastDecodedPixelBuffer: CVPixelBuffer?
    private var latestFrame: NuosVideoFrame?

    private let renderQueue = DispatchQueue(label: "nuos.render")
    private var fpsCount = 0
    private var fpsWindowStart = Date()

    // Distortion mesh: 2 quads (one per eye), grid SUBDIV x SUBDIV.
    private let meshSubdiv = 16
    private var meshVertexBuffer: MTLBuffer?
    private var meshIndexBuffer: MTLBuffer?
    private var meshIndexCount = 0

    init?(mtkView: MTKView) {
        guard let device = MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else { return nil }
        super.init()
        self.device = device
        self.queue = queue

        do {
            let lib = try makeLibrary(device: device)
            pipelineYUV = try buildPipeline(device: device, lib: lib,
                                            vertex: "passthroughVertex",
                                            fragment: "yuvFragment")
            pipelineVideo = try buildPipeline(device: device, lib: lib,
                                              vertex: "timewarpVertex",
                                              fragment: "videoFragment")
            pipelineComposite = try buildPipeline(device: device, lib: lib,
                                                  vertex: "distortVertex",
                                                  fragment: "compositeFragment")
        } catch {
            return nil
        }

        buildDistortionMesh()
        mtkView.device = device
        // 60 Hz fixed: preferredFramesPerSecond drives CAMetalLayer's display
        // sync (iOS CAMetalLayer has no public frameRateRange; CAFrameRateRange
        // applies to macOS CAMetalDisplayLayer — the fixed 60 is equivalent).
        mtkView.preferredFramesPerSecond = 60
        mtkView.framebufferOnly = true
        mtkView.enableSetNeedsDisplay = false
        mtkView.colorPixelFormat = .bgra8Unorm
        mtkView.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
    }

    private func makeLibrary(device: MTLDevice) throws -> MTLLibrary {
        // Shaders.metal is compiled into the default library by Xcode.
        let bundle = Bundle.main
        if let url = bundle.url(forResource: "default", withExtension: "metallib") {
            return try device.makeLibrary(URL: url)
        }
        // Fallback: compile source (project.yml should bundle Shaders.metal).
        guard let path = bundle.path(forResource: "Shaders", ofType: "metal") else {
            throw NSError(domain: "nuos.render", code: 1)
        }
        let src = try String(contentsOfFile: path, encoding: .utf8)
        return try device.makeLibrary(source: src, options: nil)
    }

    private func buildPipeline(device: MTLDevice, lib: MTLLibrary,
                               vertex: String, fragment: String) throws -> MTLRenderPipelineState {
        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = lib.makeFunction(name: vertex)
        desc.fragmentFunction = lib.makeFunction(name: fragment)
        desc.colorAttachments[0].pixelFormat = .bgra8Unorm
        return try device.makeRenderPipelineState(descriptor: desc)
    }

    // MARK: - Mesh

    private func buildDistortionMesh() {
        // SBS stereo grid. The barrel pre-distortion itself is applied in the
        // vertex shader from the k1/k2 uniforms so the sliders take effect on
        // the next frame (see distortVertex in Shaders.metal):
        //   uv_d = 0.5 + r * (1 + k1*|r|^2 + k2*|r|^4), r = uv - (0.5,0.5)
        // Vertex layout: pos = on-screen quadrant coords, uv = eye-local
        // undistorted source uv, eye = 0 (left) / 1 (right).
        let n = meshSubdiv
        struct V {
            var pos: SIMD2<Float>
            var uv: SIMD2<Float>
            var eye: Float
        }
        var verts: [V] = []
        var indices: [UInt16] = []

        func appendEye(eye: Int) {
            let base = UInt16(verts.count)
            let x0 = eye == 0 ? Float(0) : Float(0.5)
            let x1 = eye == 0 ? Float(0.5) : Float(1)
            let w = x1 - x0
            for gy in 0...n {
                let v = Float(gy) / Float(n)
                for gx in 0...n {
                    let u = Float(gx) / Float(n)
                    let x = x0 + u * w
                    verts.append(V(pos: SIMD2<Float>(x, v),
                                   uv: SIMD2<Float>(u, v),
                                   eye: Float(eye)))
                }
            }
            let stride = UInt16(n + 1)
            for gy in 0..<n {
                for gx in 0..<n {
                    let a = base + UInt16(gy) * stride + UInt16(gx)
                    let b = a + 1
                    let c = a + stride
                    let d = c + 1
                    indices.append(contentsOf: [a, b, c, b, d, c])
                }
            }
        }
        appendEye(eye: 0)
        appendEye(eye: 1)
        meshIndexCount = indices.count
        meshVertexBuffer = device.makeBuffer(bytes: verts, length: verts.count * MemoryLayout<V>.stride)
        meshIndexBuffer = device.makeBuffer(bytes: indices, length: indices.count * MemoryLayout<UInt16>.stride)
    }

    // MARK: - Inputs

    func setDecodedPixelBuffer(_ pb: CVPixelBuffer) {
        renderQueue.async { [weak self] in
            self?.lastDecodedPixelBuffer = pb
        }
    }

    func setLatestFrame(_ frame: NuosVideoFrame) {
        renderQueue.async { [weak self] in
            self?.latestFrame = frame
        }
    }

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        renderQueue.async { [weak self] in
            self?.drawSync(in: view)
        }
    }

    private func drawSync(in view: MTKView) {
        guard let drawable = view.currentDrawable,
              let pass = view.currentRenderPassDescriptor,
              let cmd = queue.makeCommandBuffer() else { return }

        // 1. Update textures from latest sources.
        updateTextures()

        // Track render fps.
        fpsCount += 1
        let now = Date()
        let dt = now.timeIntervalSince(fpsWindowStart)
        if dt >= 1.0 {
            fpsCount = 0
            fpsWindowStart = now
        }

        guard let encoder = cmd.makeRenderCommandEncoder(descriptor: pass) else {
            cmd.commit()
            return
        }

        if stereoEnabled, let vb = meshVertexBuffer, let ib = meshIndexBuffer,
           let pl = pipelineComposite {
            encoder.setRenderPipelineState(pl)
            encoder.setVertexBuffer(vb, offset: 0, index: 0)

            // Rotation-only timewarp per PROTOCOL.md rule 6:
            // delta = latest_pose ∘ render_pose⁻¹, no positional warp.
            var warp = SIMD4<Float>(1, 0, 0, 0)
            if let frame = latestFrame {
                let latest = currentPoseQuat()
                let delta = NuosMath.timewarpDelta(latest: latest, render: frame.renderQuat)
                warp = SIMD4<Float>(delta.0, delta.1, delta.2, delta.3)
            }
            var uniforms = Uniforms(warpQuat: warp, k1: distortionK1, k2: distortionK2,
                                    dim: passthroughEnabled ? 0.35 : 0.0,
                                    videoOn: videoEnabled ? 1 : 0,
                                    pad0: 0, pad1: 0, pad2: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 1)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.stride, index: 0)
            // Textures: 0/1 = passthrough Y/CbCr, 2/3 = decoded video Y/CbCr.
            if let t = passthroughYTexture { encoder.setFragmentTexture(t, index: 0) }
            if let t = passthroughCbCrTexture { encoder.setFragmentTexture(t, index: 1) }
            if let t = videoYTexture { encoder.setFragmentTexture(t, index: 2) }
            if let t = videoCbCrTexture { encoder.setFragmentTexture(t, index: 3) }
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: meshIndexCount,
                                          indexType: .uint16, indexBuffer: ib,
                                          indexBufferOffset: 0)
        } else if let pl = pipelineYUV {
            // Non-stereo 2D fallback: passthrough full-screen quad.
            encoder.setRenderPipelineState(pl)
            var dim: Float = 0.35
            encoder.setFragmentBytes(&dim, length: MemoryLayout<Float>.stride, index: 0)
            if let t = passthroughYTexture { encoder.setFragmentTexture(t, index: 0) }
            if let t = passthroughCbCrTexture { encoder.setFragmentTexture(t, index: 1) }
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }

        encoder.endEncoding()
        cmd.present(drawable)
        cmd.commit()
    }

    private func updateTextures() {
        // Passthrough: ARFrame pixelBuffer is 420f (two planes: Y, CbCr).
        if let pb = trackingEngine?.latestPixelBuffer {
            let (y, cbcr) = makeYUVTextures(from: pb)
            passthroughYTexture = y
            passthroughCbCrTexture = cbcr
        }
        // Video: decoder output is also 420f; same two-plane treatment.
        if let pb = lastDecodedPixelBuffer {
            let (y, cbcr) = makeYUVTextures(from: pb)
            videoYTexture = y
            videoCbCrTexture = cbcr
        }
    }

    private var yuvCache: CVMetalTextureCache?
    private func ensureTextureCache() -> CVMetalTextureCache? {
        if yuvCache == nil {
            CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &yuvCache)
        }
        return yuvCache
    }

    /// Y plane (.r8) + CbCr plane (.rg8) textures for the passthrough buffer.
    private func makeYUVTextures(from pb: CVPixelBuffer) -> (MTLTexture?, MTLTexture?) {
        guard let cache = ensureTextureCache() else { return (nil, nil) }
        var yTex: CVMetalTexture?
        var cbcrTex: CVMetalTexture?
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, pb, nil, .r8Unorm,
            CVPixelBufferGetWidthOfPlane(pb, 0), CVPixelBufferGetHeightOfPlane(pb, 0), 0, &yTex)
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, pb, nil, .rg8Unorm,
            CVPixelBufferGetWidthOfPlane(pb, 1), CVPixelBufferGetHeightOfPlane(pb, 1), 1, &cbcrTex)
        return (yTex.flatMap { CVMetalTextureGetTexture($0) },
                cbcrTex.flatMap { CVMetalTextureGetTexture($0) })
    }

    private func currentPoseQuat() -> (Float, Float, Float, Float) {
        guard let frame = trackingEngine?.latestARFrame else { return (1, 0, 0, 0) }
        let q = simd_quatf(frame.camera.transform)
        return (q.vector.w, q.vector.x, q.vector.y, q.vector.z)
    }
}

// Uniform block shared with Shaders.metal (must match layout there).
struct Uniforms {
    var warpQuat: SIMD4<Float>  // (w, x, y, z)
    var k1: Float
    var k2: Float
    var dim: Float
    var videoOn: Int32
    var pad0: Float
    var pad1: Float
    var pad2: Float
}
