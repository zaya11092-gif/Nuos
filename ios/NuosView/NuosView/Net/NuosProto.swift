//
//  NuosProto.swift
//  NuosView
//
//  Swift mirror of proto/nuos_proto.h (wire protocol v1).
//  All multi-byte integers are LITTLE-ENDIAN on the wire; all structs are
//  byte-packed with no padding. Encode/decode is done manually into [UInt8]
//  so endianness and layout are explicit.
//

import Foundation

// ---------------------------------------------------------------------------
// Constants (must match nuos_proto.h exactly)
// ---------------------------------------------------------------------------

enum NuosProto {
    static let protoVersion: UInt16 = 1

    // Ports
    static let portVideo: UInt16 = 48010        // udp board -> phone
    static let portTelemetry: UInt16 = 48011    // udp phone -> board (pose, hands, stat)
    static let portControl: UInt16 = 48012      // tcp board listens; phone connects
    static let mdnsServiceType = "_nuos-xr._tcp"

    // Wire constants
    static let mtuDefault = 1500
    /// NUOS_VIDEO_PAYLOAD_MAX = MTU - sizeof(hdr) - sizeof(video) = 1500 - 8 - 48
    static let videoPayloadMax = mtuDefault - hdrSize - videoSize // 1444
    static let fecGroup = 8
    static let handJoints = 21
    static let maxHands = 2

    // Struct sizes (asserted at load)
    static let hdrSize = 8
    static let poseSize = 64
    static let handsSize = 516
    static let videoSize = 48
    static let fecSize = 12
    static let statSize = 28
    static let helloSize = 8
    static let sessionSize = 12

    // Packet types (nuos_pkt_type)
    enum PktType: UInt8 {
        case pose  = 1
        case hands = 2
        case video = 3
        case fec   = 4
        case stat  = 5
    }

    // Control TLV types (nuos_ctl_type)
    enum CtlType: UInt8 {
        case hello    = 1
        case session  = 2
        case stop     = 3
        case ping     = 4
        case pong     = 5
        case statReq  = 6
    }

    // Caps (nuos_caps)
    struct Caps: OptionSet {
        let rawValue: UInt32
        static let hevc        = Caps(rawValue: 1 << 0)
        static let fec         = Caps(rawValue: 1 << 1)
        static let hands       = Caps(rawValue: 1 << 2)
        static let passthrough = Caps(rawValue: 1 << 3)
        static let ultrawide   = Caps(rawValue: 1 << 4)
    }

    // Pose flags
    static let poseFlagUltrawidePassthrough: UInt8 = 1 << 0
    // Video flags
    static let videoFlagIDR: UInt8 = 1 << 0
    // Stat flags
    static let statFlagThermalThrottle: UInt8 = 1 << 0

    static let magic: [UInt8] = [UInt8(ascii: "N"), UInt8(ascii: "U"), UInt8(ascii: "1")]

    /// Vision joint ordering per PROTOCOL.md: wrist first, then thumb(4),
    /// index(4), middle(4), ring(4), little(4). Must be mirrored exactly on
    /// the board (platform/src/nuos-handbus).
    static let handJointOrder: [String] = [
        "wrist",
        "thumbCMC", "thumbMP", "thumbIP", "thumbTIP",
        "indexMCP", "indexPIP", "indexDIP", "indexTIP",
        "middleMCP", "middlePIP", "middleDIP", "middleTIP",
        "ringMCP", "ringPIP", "ringDIP", "ringTIP",
        "littleMCP", "littlePIP", "littleDIP", "littleTIP",
    ]

    /// Byte offset in the wire image where a sanity check is cheap.
    static func validateStructSizes() {
        // Computed sizes are compile-time constants here; the encode/decode
        // paths below index with fixed offsets derived from them, so a
        // mismatch would trap immediately. These asserts document intent.
        assert(hdrSize == 8)
        assert(poseSize == 64)
        assert(handsSize == 516)
        assert(videoSize == 48)
        assert(fecSize == 12)
        assert(statSize == 28)
        assert(helloSize == 8)
        assert(sessionSize == 12)
        assert(handJointOrder.count == handJoints)
        assert(videoPayloadMax == 1436)
    }
}

// ---------------------------------------------------------------------------
// Little-endian codec primitives
// ---------------------------------------------------------------------------

struct NuosWriter {
    private(set) var bytes: [UInt8] = []

    mutating func u8(_ v: UInt8)  { bytes.append(v) }
    mutating func u16(_ v: UInt16) { bytes.append(UInt8(v & 0xff)); bytes.append(UInt8(v >> 8)) }
    mutating func u32(_ v: UInt32) {
        bytes.append(UInt8(v & 0xff))
        bytes.append(UInt8((v >> 8) & 0xff))
        bytes.append(UInt8((v >> 16) & 0xff))
        bytes.append(UInt8((v >> 24) & 0xff))
    }
    mutating func u64(_ v: UInt64) {
        for shift in stride(from: 0, to: 64, by: 8) { bytes.append(UInt8((v >> UInt64(shift)) & 0xff)) }
    }
    mutating func i16(_ v: Int16) { u16(UInt16(bitPattern: v)) }
    mutating func f32(_ v: Float) { u32(v.bitPattern) }

    mutating func raw(_ data: [UInt8]) { bytes.append(contentsOf: data) }
    mutating func zeros(_ n: Int) { bytes.append(contentsOf: repeatElement(0, count: n)) }
}

struct NuosReader {
    let bytes: [UInt8]
    var offset = 0

    init(_ bytes: [UInt8]) { self.bytes = bytes }

    mutating func u8() -> UInt8 {
        let v = bytes[offset]; offset += 1; return v
    }
    mutating func u16() -> UInt16 {
        let v = UInt16(bytes[offset]) | (UInt16(bytes[offset + 1]) << 8)
        offset += 2; return v
    }
    mutating func u32() -> UInt32 {
        var v: UInt32 = 0
        for i in 0..<4 { v |= UInt32(bytes[offset + i]) << (UInt32(i) * 8) }
        offset += 4; return v
    }
    mutating func u64() -> UInt64 {
        var v: UInt64 = 0
        for i in 0..<8 { v |= UInt64(bytes[offset + i]) << (UInt64(i) * 8) }
        offset += 8; return v
    }
    mutating func i16() -> Int16 { Int16(bitPattern: u16()) }
    mutating func f32() -> Float { Float(bitPattern: u32()) }

    mutating func raw(_ n: Int) -> [UInt8] {
        let v = Array(bytes[offset..<(offset + n)]); offset += n; return v
    }
    mutating func skip(_ n: Int) { offset += n }

    var remaining: Int { bytes.count - offset }
}

// ---------------------------------------------------------------------------
// nuos_hdr_t — 8 bytes
// ---------------------------------------------------------------------------

struct NuosHdr {
    var type: NuosProto.PktType
    var seq: UInt16
    var len: UInt16

    static func decode(_ r: inout NuosReader) -> NuosHdr? {
        guard r.remaining >= NuosProto.hdrSize else { return nil }
        let m = r.raw(3)
        guard m == NuosProto.magic else { return nil }
        let rawType = r.u8()
        guard let type = NuosProto.PktType(rawValue: rawType),
              rawType >= NuosProto.PktType.pose.rawValue,
              rawType <= NuosProto.PktType.stat.rawValue else { return nil }
        let seq = r.u16()
        let len = r.u16()
        return NuosHdr(type: type, seq: seq, len: len)
    }

    func encode(into w: inout NuosWriter) {
        w.raw(NuosProto.magic)
        w.u8(type.rawValue)
        w.u16(seq)
        w.u16(len)
    }
}

// ---------------------------------------------------------------------------
// nuos_pose_t — 64 bytes
// ---------------------------------------------------------------------------

struct NuosPose {
    var tMonoNs: UInt64
    var pos: (Float, Float, Float)
    var quat: (Float, Float, Float, Float)   // w x y z
    var vel: (Float, Float, Float)
    var angVel: (Float, Float, Float)
    var tracking: UInt8   // 0 none, 1 limited, 2 normal
    var flags: UInt8      // bit0: ultrawide passthrough active
    var reserved: UInt16

    init() {
        tMonoNs = 0
        pos = (0, 0, 0); quat = (1, 0, 0, 0)
        vel = (0, 0, 0); angVel = (0, 0, 0)
        tracking = 0; flags = 0; reserved = 0
    }

    func encode() -> [UInt8] {
        var w = NuosWriter()
        w.u64(tMonoNs)
        w.f32(pos.0); w.f32(pos.1); w.f32(pos.2)
        w.f32(quat.0); w.f32(quat.1); w.f32(quat.2); w.f32(quat.3)
        w.f32(vel.0); w.f32(vel.1); w.f32(vel.2)
        w.f32(angVel.0); w.f32(angVel.1); w.f32(angVel.2)
        w.u8(tracking)
        w.u8(flags)
        w.u16(reserved)
        assert(w.bytes.count == NuosProto.poseSize)
        return w.bytes
    }
}

// ---------------------------------------------------------------------------
// nuos_hands_t — 516 bytes
// ---------------------------------------------------------------------------

struct NuosHands {
    var tMonoNs: UInt64
    var present: UInt8          // bit0: left, bit1: right
    var left: [[Float]]         // [21][3]
    var right: [[Float]]        // [21][3]

    init() {
        tMonoNs = 0
        present = 0
        left = Array(repeating: [0, 0, 0], count: NuosProto.handJoints)
        right = Array(repeating: [0, 0, 0], count: NuosProto.handJoints)
    }

    func encode() -> [UInt8] {
        var w = NuosWriter()
        w.u64(tMonoNs)
        w.u8(present)
        w.zeros(3) // reserved[3]
        assert(left.count == NuosProto.handJoints && right.count == NuosProto.handJoints)
        for j in left  { w.f32(j[0]); w.f32(j[1]); w.f32(j[2]) }
        for j in right { w.f32(j[0]); w.f32(j[1]); w.f32(j[2]) }
        assert(w.bytes.count == NuosProto.handsSize)
        return w.bytes
    }
}

// ---------------------------------------------------------------------------
// nuos_video_t — 48 bytes (header of a video datagram; raw HEVC follows)
// ---------------------------------------------------------------------------

struct NuosVideo {
    var tMonoNs: UInt64      // board compose time
    var frameSeq: UInt32
    var fragIdx: UInt16      // 0-based
    var fragCount: UInt16
    var payloadLen: UInt16   // raw bytes following this struct
    var flags: UInt8         // bit0: frame contains IDR
    var reserved: UInt8
    var renderPos: (Float, Float, Float)
    var renderQuat: (Float, Float, Float, Float)

    static func decode(_ r: inout NuosReader) -> NuosVideo? {
        guard r.remaining >= NuosProto.videoSize else { return nil }
        var v = NuosVideo()
        v.tMonoNs = r.u64()
        v.frameSeq = r.u32()
        v.fragIdx = r.u16()
        v.fragCount = r.u16()
        v.payloadLen = r.u16()
        v.flags = r.u8()
        v.reserved = r.u8()
        v.renderPos = (r.f32(), r.f32(), r.f32())
        v.renderQuat = (r.f32(), r.f32(), r.f32(), r.f32())
        assert(r.offset == NuosProto.videoSize)
        return v
    }
}

// ---------------------------------------------------------------------------
// nuos_fec_t — 12 bytes
// ---------------------------------------------------------------------------

struct NuosFec {
    var frameSeq: UInt32
    var groupStart: UInt16
    var groupCount: UInt16
    var payloadLen: UInt16   // length of the XOR recovery payload
    var reserved: UInt16

    static func decode(_ r: inout NuosReader) -> NuosFec? {
        guard r.remaining >= NuosProto.fecSize else { return nil }
        var f = NuosFec()
        f.frameSeq = r.u32()
        f.groupStart = r.u16()
        f.groupCount = r.u16()
        f.payloadLen = r.u16()
        f.reserved = r.u16()
        assert(r.offset == NuosProto.fecSize)
        return f
    }
}

// ---------------------------------------------------------------------------
// nuos_stat_t — 32 bytes
// ---------------------------------------------------------------------------

struct NuosStat {
    var tMonoNs: UInt64
    var fpsX10: UInt32
    var bitrateKbps: UInt32
    var lossPpm: UInt32
    var rttMsX10: Int16     // -1 if unknown
    var tempCX10: Int16     // -1 if unknown
    var batteryPct: UInt8   // 0-100, 255 = unknown
    var flags: UInt8        // bit0: thermal-throttle hint
    var reserved: [UInt8]   // [2]

    init() {
        tMonoNs = 0; fpsX10 = 0; bitrateKbps = 0; lossPpm = 0
        rttMsX10 = -1; tempCX10 = -1
        batteryPct = 255; flags = 0; reserved = [0, 0]
    }

    static func decode(_ r: inout NuosReader) -> NuosStat? {
        guard r.remaining >= NuosProto.statSize else { return nil }
        var s = NuosStat()
        s.tMonoNs = r.u64()
        s.fpsX10 = r.u32()
        s.bitrateKbps = r.u32()
        s.lossPpm = r.u32()
        s.rttMsX10 = r.i16()
        s.tempCX10 = r.i16()
        s.batteryPct = r.u8()
        s.flags = r.u8()
        s.reserved = r.raw(2)
        assert(r.offset == NuosProto.statSize)
        return s
    }

    func encode() -> [UInt8] {
        var w = NuosWriter()
        w.u64(tMonoNs)
        w.u32(fpsX10)
        w.u32(bitrateKbps)
        w.u32(lossPpm)
        w.i16(rttMsX10)
        w.i16(tempCX10)
        w.u8(batteryPct)
        w.u8(flags)
        w.raw(reserved)
        assert(w.bytes.count == NuosProto.statSize)
        return w.bytes
    }
}

// ---------------------------------------------------------------------------
// TCP control TLVs — frame = [type u8][len u16 LE][payload len bytes]
// ---------------------------------------------------------------------------

enum NuosCtl {
    /// Split a byte stream into TLV frames. Returns frames parsed so far and
    /// the number of bytes consumed (remainder is a partial frame).
    static func parseFrames(_ data: [UInt8]) -> (frames: [(type: NuosCtl.CtlType, payload: [UInt8])], consumed: Int) {
        var frames: [(NuosCtl.CtlType, [UInt8])] = []
        var off = 0
        while off + 3 <= data.count {
            guard let type = NuosCtl.CtlType(rawValue: data[off]) else { break }
            let len = Int(data[off + 1]) | (Int(data[off + 2]) << 8)
            if off + 3 + len > data.count { break }
            frames.append((type, Array(data[(off + 3)..<(off + 3 + len)])))
            off += 3 + len
        }
        return (frames, off)
    }

    static func frame(type: NuosCtl.CtlType, payload: [UInt8]) -> [UInt8] {
        var out: [UInt8] = [type.rawValue]
        out.append(UInt8(payload.count & 0xff))
        out.append(UInt8((payload.count >> 8) & 0xff))
        out.append(contentsOf: payload)
        return out
    }
}

// nuos_hello_t — 8 bytes
struct NuosHello {
    var protoVersion: UInt16
    var device: UInt8       // 0 = board, 1 = phone
    var reserved: UInt8
    var caps: UInt32

    static let devicePhone: UInt8 = 1
    static let deviceBoard: UInt8 = 0

    func encode() -> [UInt8] {
        var w = NuosWriter()
        w.u16(protoVersion)
        w.u8(device)
        w.u8(reserved)
        w.u32(caps)
        assert(w.bytes.count == NuosProto.helloSize)
        return w.bytes
    }

    static func decode(_ payload: [UInt8]) -> NuosHello? {
        guard payload.count == NuosProto.helloSize else { return nil }
        var r = NuosReader(payload)
        var h = NuosHello()
        h.protoVersion = r.u16()
        h.device = r.u8()
        h.reserved = r.u8()
        h.caps = r.u32()
        return h
    }
}

// nuos_session_t — 12 bytes
struct NuosSession {
    var fps: UInt8          // 30, 60
    var codec: UInt8        // 0 = HEVC
    var passthrough: UInt8  // 0 off, 1 on
    var reserved: UInt8
    var width: UInt16       // per-eye
    var height: UInt16      // per-eye
    var bitrateKbps: UInt32

    func encode() -> [UInt8] {
        var w = NuosWriter()
        w.u8(fps)
        w.u8(codec)
        w.u8(passthrough)
        w.u8(reserved)
        w.u16(width)
        w.u16(height)
        w.u32(bitrateKbps)
        assert(w.bytes.count == NuosProto.sessionSize)
        return w.bytes
    }

    static func decode(_ payload: [UInt8]) -> NuosSession? {
        guard payload.count == NuosProto.sessionSize else { return nil }
        var r = NuosReader(payload)
        var s = NuosSession()
        s.fps = r.u8()
        s.codec = r.u8()
        s.passthrough = r.u8()
        s.reserved = r.u8()
        s.width = r.u16()
        s.height = r.u16()
        s.bitrateKbps = r.u32()
        return s
    }
}

// ---------------------------------------------------------------------------
// Timewarp helpers
// ---------------------------------------------------------------------------

enum NuosMath {
    /// Quaternion multiply: q = a * b, (w, x, y, z) convention.
    static func quatMul(_ a: (Float, Float, Float, Float), _ b: (Float, Float, Float, Float))
        -> (Float, Float, Float, Float) {
        let (aw, ax, ay, az) = a
        let (bw, bx, by, bz) = b
        return (
            aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw
        )
    }

    /// Quaternion conjugate.
    static func quatConj(_ q: (Float, Float, Float, Float)) -> (Float, Float, Float, Float) {
        (q.0, -q.1, -q.2, -q.3)
    }

    /// Rotation-only timewarp delta per PROTOCOL.md rule 6:
    /// delta = latest_pose ∘ render_pose⁻¹.
    static func timewarpDelta(latest: (Float, Float, Float, Float),
                              render: (Float, Float, Float, Float))
        -> (Float, Float, Float, Float) {
        quatMul(latest, quatConj(render))
    }
}
