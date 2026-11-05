//
//  HEVCDecoder.swift
//  NuosView
//
//  Hardware-accelerated low-latency HEVC decode via VTDecompressionSession.
//  Input: one fully-assembled access unit per frame (from NuosLink), Annex-B.
//  Output: CVPixelBuffer handed to the Renderer's virtual video layer.
//

import Foundation
import CoreMedia
import CoreVideo
import VideoToolbox

final class HEVCDecoder {
    var onDecodedFrame: ((CVPixelBuffer) -> Void)?

    private(set) var decodeErrors: Int = 0
    private(set) var decodedFps: Double = 0

    private var session: VTDecompressionSession?
    private var formatDesc: CMVideoFormatDescription?
    private var lastFrameSeq: UInt32?

    private var decodedCount = 0
    private var fpsWindowStart = Date()

    // MARK: - Format description from VPS/SPS/PPS

    /// Build (or rebuild) the format description from the parameter-set NALs
    /// contained in an IDR access unit. Annex-B start codes (00 00 00 01).
    func ensureFormat(annexB: [UInt8]) {
        guard formatDesc == nil else { return }
        let units = Self.splitAnnexB(annexB)
        // NAL types for HEVC: VPS=32, SPS=33, PPS=34 (nal_type = (b0>>1)&0x3f).
        let paramSets = units.filter { unit in
            guard let first = unit.first else { return false }
            let type = (first >> 1) & 0x3f
            return type == 32 || type == 33 || type == 34
        }
        guard paramSets.count >= 3 else { return }

        var sizes: [Int] = paramSets.map { $0.count }
        // Copy parameter sets into one contiguous buffer whose lifetime spans
        // the format-desc creation call; pointers must stay valid during it.
        var flat: [UInt8] = []
        var offsets: [Int] = []
        for unit in paramSets { offsets.append(flat.count); flat.append(contentsOf: unit) }

        var desc: CMVideoFormatDescription?
        let status = flat.withUnsafeBufferPointer { buf -> OSStatus in
            guard let base = buf.baseAddress else { return -1 }
            var pointers: [UnsafePointer<UInt8>] = offsets.map { base + $0 }
            return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                allocator: nil,
                parameterSetCount: pointers.count,
                parameterSetPointers: &pointers,
                parameterSetSizes: &sizes,
                nalUnitHeaderLength: 4,
                extensions: nil,
                formatDescriptionOut: &desc)
        }
        guard status == noErr, let desc else {
            decodeErrors += 1
            return
        }
        formatDesc = desc
    }

    // MARK: - Decode

    /// Decode one assembled access unit (Annex-B start codes from NuosLink).
    func decode(frameSeq: UInt32, annexB: [UInt8]) {
        guard let desc = formatDesc else { return }
        if session == nil { createSession(format: desc) }
        guard let session else { return }

        // Convert to 4-byte length-prefixed NALs as the format desc expects.
        let data = Self.convertAnnexBToLengthPrefixed(annexB)
        guard !data.isEmpty else { return }

        var cmData: CMBlockBuffer?
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: nil, memoryBlock: nil, blockLength: data.count,
            blockAllocator: nil, customBlockSource: nil,
            offsetToData: 0, dataLength: data.count, flags: 0,
            blockBufferOut: &cmData)
        guard status == kCMBlockBufferNoErr, let blockBuffer = cmData else {
            decodeErrors += 1
            return
        }
        data.withUnsafeBytes { raw in
            _ = CMBlockBufferReplaceDataBytes(
                with: raw.baseAddress!, blockBuffer: blockBuffer,
                offsetIntoDestination: 0, dataLength: data.count)
        }

        var sampleBuffer: CMSampleBuffer?
        let timing = CMSampleTimingInfo(
            duration: .invalid, presentationTimeStamp: .invalid, decodeTimeStamp: .invalid)
        status = CMSampleBufferCreateReady(
            allocator: nil,
            dataBuffer: blockBuffer,
            formatDescription: desc,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: [timing],
            sampleSizeEntryCount: 1,
            sampleSizeArray: [data.count],
            sampleBufferOut: &sampleBuffer)
        guard status == noErr, let sb = sampleBuffer else {
            decodeErrors += 1
            return
        }

        let flags: VTDecodeFrameFlags = [VTDecodeFrameFlags._EnableAsynchronousDecompression,
                                         VTDecodeFrameFlags._1xRealTimePlayback]
        VTDecompressionSessionDecodeFrame(session, sampleBuffer: sb, flags: flags,
                                          infoFlagsOut: nil) { _, _, _, _, _, _ in }
        lastFrameSeq = frameSeq
    }

    private func createSession(format desc: CMVideoFormatDescription) {
        let decoderSpec: [CFString: Any] = [
            kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: true,
        ]
        let dstAttrs: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: [kCVPixelFormatType_420YpCbCr8BiPlanarFullRange],
            kCVPixelBufferMetalCompatibilityKey: true,
        ]
        // The C output callback cannot capture self; route frames through a
        // static registry keyed by the refCon pointer we pass at creation.
        let refCon = Unmanaged.passUnretained(self).toOpaque()
        Self.register(self, for: refCon)
        let record = VTDecompressionOutputCallbackRecord(
            callback: Self.outputCallback,
            decompressionOutputRefCon: refCon)
        var sessionOut: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: nil,
            formatDescription: desc,
            decoderSpecification: decoderSpec as CFDictionary,
            imageBufferAttributes: dstAttrs as CFDictionary,
            outputCallback: &record,
            decompressionSessionOut: &sessionOut)
        guard status == noErr, let s = sessionOut else {
            decodeErrors += 1
            Self.unregister(refCon)
            return
        }
        // Low-latency decode is requested per-frame via the
        // _1xRealTimePlayback flag; no session-level real-time property is
        // exposed by this SDK.
        session = s
    }

    private func handleOutput(status: OSStatus, pixelBuffer pb: CVImageBuffer?) {
        guard status == noErr, let pb else {
            decodeErrors += 1
            return
        }
        decodedCount += 1
        let now = Date()
        let dt = now.timeIntervalSince(fpsWindowStart)
        if dt >= 1.0 {
            decodedFps = Double(decodedCount) / dt
            decodedCount = 0
            fpsWindowStart = now
        }
        onDecodedFrame?(pb)
    }

    // MARK: - Output callback registry

    private static let registryLock = NSLock()
    private static var registry: [UnsafeMutableRawPointer: HEVCDecoder] = [:]

    private static func register(_ decoder: HEVCDecoder, for key: UnsafeMutableRawPointer) {
        registryLock.lock()
        registry[key] = decoder
        registryLock.unlock()
    }

    private static func unregister(_ key: UnsafeMutableRawPointer) {
        registryLock.lock()
        registry.removeValue(forKey: key)
        registryLock.unlock()
    }

    private static let outputCallback: VTDecompressionOutputCallback =
        { refCon, _, status, _, imageBuffer, _, _ in
        guard let refCon else { return }
        registryLock.lock()
        let decoder = registry[refCon]
        registryLock.unlock()
        decoder?.handleOutput(status: status, pixelBuffer: imageBuffer)
    }

    func invalidate() {
        if let session { VTDecompressionSessionInvalidate(session) }
        session = nil
        formatDesc = nil
        Self.unregister(Unmanaged.passUnretained(self).toOpaque())
    }

    // MARK: - Annex-B helpers

    static func splitAnnexB(_ data: [UInt8]) -> [[UInt8]] {
        var units: [[UInt8]] = []
        var current: [UInt8] = []
        var i = 0
        func isStart3(_ i: Int) -> Bool {
            i + 3 <= data.count && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1
        }
        func isStart4(_ i: Int) -> Bool {
            i + 4 <= data.count && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1
        }
        while i < data.count {
            if isStart3(i) {
                if !current.isEmpty { units.append(current); current = [] }
                i += 3
            } else if isStart4(i) {
                if !current.isEmpty { units.append(current); current = [] }
                i += 4
            } else {
                current.append(data[i])
                i += 1
            }
        }
        if !current.isEmpty { units.append(current) }
        return units
    }

    /// Convert Annex-B start codes to 4-byte big-endian length prefixes
    /// (VTDecompression expects length-prefixed NALs per the format desc).
    static func convertAnnexBToLengthPrefixed(_ annexB: [UInt8]) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(annexB.count)
        for u in splitAnnexB(annexB) {
            let n = UInt32(u.count)
            out.append(UInt8((n >> 24) & 0xff))
            out.append(UInt8((n >> 16) & 0xff))
            out.append(UInt8((n >> 8) & 0xff))
            out.append(UInt8(n & 0xff))
            out.append(contentsOf: u)
        }
        return out
    }
}
