//
//  NuosLink.swift
//  NuosView
//
//  Transport for one NuOS session:
//    - TCP control to board :48012 — HELLO/SESSION/STOP/PING TLV framing.
//    - UDP send socket to board :48011 (pose / hands / stat).
//    - UDP listener on phone :48010 for video fragments, with reassembly by
//      frame_seq/frag_idx, XOR FEC recovery, and a 2-frame jitter buffer.
//

import Foundation
import Network

// ---------------------------------------------------------------------------
// Statistics surfaced to the UI
// ---------------------------------------------------------------------------

struct LinkStats: Observable, Equatable {
    var tcpState: String = "idle"
    var udpRxVideoBytes: UInt64 = 0
    var framesAssembled: UInt64 = 0
    var framesDroppedStale: UInt64 = 0
    var framesIncomplete: UInt64 = 0
    var fecRecovered: UInt64 = 0
    var datagramsLost: UInt64 = 0
    var rttMs: Double = -1
    var boardStat: NuosStat?
}

// ---------------------------------------------------------------------------
// Assembled video frame
// ---------------------------------------------------------------------------

struct NuosVideoFrame {
    var frameSeq: UInt32
    var idr: Bool
    var tMonoNs: UInt64
    var renderPos: (Float, Float, Float)
    var renderQuat: (Float, Float, Float, Float)
    var data: [UInt8]
}

// ---------------------------------------------------------------------------
// NuosLink
// ---------------------------------------------------------------------------

final class NuosLink {
    // Callbacks (invoked on internal queue; marshal as needed)
    var onVideoFrame: ((NuosVideoFrame) -> Void)?
    var onBoardStat: ((NuosStat) -> Void)?
    var onControlEvent: ((String) -> Void)?   // human-readable state changes
    var onBoardHello: ((UInt32) -> Void)?     // board HELLO received (caps)
    var onTcpClosed: (() -> Void)?

    private let queue = DispatchQueue(label: "nuos.link")

    // TCP control
    private var tcp: NWConnection?
    private var tcpRxBuffer: [UInt8] = []
    private var boardCaps: UInt32 = 0
    private var helloSent = false

    // UDP send (pose/hands/stat -> board :48011)
    private var udpSend: NWConnection?

    // UDP receive (video frags -> us :48010)
    private var udpListener: NWListener?
    private var udpRecvConns: [NWConnection] = []

    private var boardEndpoint: NWEndpoint?
    private let params: NWParameters

    // Session negotiation
    private(set) var session: NuosSession?

    // Reassembly state
    private var pendingSeq: UInt32?
    private var pendingFragCount: UInt16 = 0
    private var pendingIDR = false
    private var pendingT: UInt64 = 0
    private var pendingRenderPos = (0.0 as Float, 0.0 as Float, 0.0 as Float)
    private var pendingRenderQuat = (1.0 as Float, 0.0 as Float, 0.0 as Float, 0.0 as Float)
    private var pendingChunks: [UInt16: [UInt8]] = [:]
    private var pendingMissing: Set<UInt16> = []
    private var pendingFEC: [UInt16: (frameSeq: UInt32, start: UInt16, count: UInt16, payload: [UInt8])] = [:]

    // 2-frame jitter buffer: seq of completed frames, newest first; only the
    // newest fully-assembled frame is presented each vsync (PROTOCOL.md rule 5).
    private var jitterBuffer: [NuosVideoFrame] = []
    private let jitterDepth = 2

    // PING bookkeeping
    private var pingSeq: UInt16 = 0
    private var pingTimes: [UInt16: DispatchTime] = [:]

    private(set) var stats = LinkStats()

    // Counters for rate tracking
    private var poseSeq: UInt16 = 0
    private var handsSeq: UInt16 = 0
    private var statSeq: UInt16 = 0

    init() {
        let p = NWParameters()
        p.allowLocalEndpointReuse = true
        params = p
    }

    // MARK: - Control TCP

    /// Open TCP control to the board. `endpoint` may be host:port or a
    /// pre-resolved endpoint from mDNS.
    func connectControl(to endpoint: NWEndpoint) {
        queue.async { [weak self] in
            guard let self else { return }
            self.teardownTCP()
            self.boardEndpoint = endpoint
            let conn = NWConnection(to: endpoint, using: .tcp)
            self.tcp = conn
            conn.stateUpdateHandler = { [weak self] state in
                self?.queue.async { self?.tcpState(state) }
            }
            conn.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
                self?.queue.async { self?.tcpReceived(data: data, isComplete: isComplete, error: error) }
            }
            conn.start(queue: self.queue)
            self.stats.tcpState = "connecting"
        }
    }

    func disconnect() {
        queue.async { [weak self] in
            guard let self else { return }
            // Politely tell the board we are leaving (best-effort).
            self.sendCtl(type: .stop, payload: [])
            self.teardownTCP()
            self.teardownUDP()
            self.stats.tcpState = "closed"
        }
    }

    private func teardownTCP() {
        tcp?.cancel()
        tcp = nil
        tcpRxBuffer = []
        helloSent = false
        boardCaps = 0
    }

    private func teardownUDP() {
        udpSend?.cancel()
        udpSend = nil
        udpListener?.cancel()
        udpListener = nil
        for c in udpRecvConns { c.cancel() }
        udpRecvConns.removeAll()
    }

    private func tcpState(_ state: NWConnection.State) {
        switch state {
        case .ready:
            stats.tcpState = "connected"
            // HELLO must be the first frame from each side (nuos_proto.h).
            sendHello()
            onControlEvent?("control connected")
        case .failed(let err):
            stats.tcpState = "failed: \(err)"
            onControlEvent?("control failed: \(err)")
            onTcpClosed?()
        case .cancelled:
            stats.tcpState = "closed"
            onTcpClosed?()
        case .waiting(let err):
            stats.tcpState = "waiting: \(err)"
        default:
            break
        }
    }

    private func sendHello() {
        guard !helloSent else { return }
        helloSent = true
        var caps: NuosProto.Caps = [.hevc, .fec, .hands, .passthrough]
        if UltraWideProbe.shared.probePassed { caps.insert(.ultrawide) }
        let hello = NuosHello(protoVersion: NuosProto.protoVersion,
                              device: NuosHello.devicePhone,
                              reserved: 0,
                              caps: caps.rawValue)
        sendCtl(type: .hello, payload: hello.encode())
    }

    /// Send SESSION after HELLO exchange completes (fps, res, bitrate).
    func sendSession(_ session: NuosSession) {
        queue.async { [weak self] in
            guard let self else { return }
            self.session = session
            self.sendCtl(type: .session, payload: session.encode())
            // Start the UDP paths once streaming is requested.
            self.setupUDPSend()
            self.setupUDPRecv()
        }
    }

    private func sendCtl(type: NuosCtl.CtlType, payload: [UInt8]) {
        guard let tcp, tcp.state == .ready else { return }
        let frame = NuosCtl.frame(type: type, payload: payload)
        tcp.send(content: frame, completion: .contentProcessed { _ in })
    }

    private func tcpReceived(data: Data?, isComplete: Bool, error: NWError?) {
        if let data {
            tcpRxBuffer.append(contentsOf: [UInt8](data))
            processTCPBuffer()
        }
        if isComplete {
            stats.tcpState = "closed by board"
            onControlEvent?("control closed by board")
            onTcpClosed?()
            teardownTCP()
            return
        }
        guard error == nil else {
            stats.tcpState = "rx error: \(String(describing: error))"
            return
        }
        tcp?.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
            self?.queue.async { self?.tcpReceived(data: data, isComplete: isComplete, error: error) }
        }
    }

    private func processTCPBuffer() {
        let (frames, consumed) = NuosCtl.parseFrames(tcpRxBuffer)
        tcpRxBuffer.removeFirst(consumed)
        for (type, payload) in frames {
            switch type {
            case .hello:
                if let h = NuosHello.decode(payload) {
                    guard h.protoVersion == NuosProto.protoVersion else {
                        onControlEvent?("proto version mismatch: board \(h.protoVersion) != \(NuosProto.protoVersion); dropping session")
                        sendCtl(type: .stop, payload: [])
                        teardownTCP()
                        teardownUDP()
                        return
                    }
                    boardCaps = h.caps
                    onControlEvent?("board HELLO caps=0x\(String(h.caps, radix: 16))")
                    onBoardHello?(h.caps)
                }
            case .session:
                if let s = NuosSession.decode(payload) {
                    session = s
                    onControlEvent?("board reconfigured session \(s.fps)fps \(s.width)x\(s.height)")
                }
            case .stop:
                onControlEvent?("board sent STOP")
                teardownTCP()
                teardownUDP()
                onTcpClosed?()
            case .ping:
                // Reply immediately with PONG echoing the payload.
                sendCtl(type: .pong, payload: payload)
            case .pong:
                if payload.count >= 2 {
                    let seq = UInt16(payload[0]) | (UInt16(payload[1]) << 8)
                    if let t0 = pingTimes.removeValue(forKey: seq) {
                        let rtt = Double(DispatchTime.now().uptimeNanoseconds - t0.uptimeNanoseconds) / 1e6
                        stats.rttMs = rtt
                    }
                }
            case .statReq:
                sendStat() // we emit our stat at 1 Hz on demand
            }
        }
    }

    /// Send a PING (payload: seq u16 LE + 6 pad bytes) and remember the time.
    func sendPing() {
        queue.async { [weak self] in
            guard let self else { return }
            let seq = self.pingSeq &+ 1
            self.pingSeq = seq
            var payload: [UInt8] = [UInt8(seq & 0xff), UInt8(seq >> 8)]
            payload.append(contentsOf: repeatElement(0, count: 6))
            self.pingTimes[seq] = DispatchTime.now()
            self.sendCtl(type: .ping, payload: payload)
            if self.pingTimes.count > 8 { // prune unanswered
                let oldest = self.pingTimes.keys.min() ?? 0
                self.pingTimes.removeValue(forKey: oldest)
            }
        }
    }

    // MARK: - UDP send (telemetry)

    private func setupUDPSend() {
        guard udpSend == nil, let board = boardEndpoint else { return }
        // Board telemetry port from the TCP endpoint host, fixed port 48011.
        let host: NWEndpoint.Host
        switch board {
        case .hostPort(let h, _): host = h
        case .service(_, _, _, _):
            host = "10.66.0.1" // fallback: board AP address
        @unknown default:
            host = "10.66.0.1"
        }
        guard let port = NWEndpoint.Port(rawValue: NuosProto.portTelemetry) else { return }
        let conn = NWConnection(host: host, port: port, using: .udp)
        conn.stateUpdateHandler = { [weak self] state in
            if case .ready = state { self?.queue.async { self?.stats.tcpState += "" /* noop */ } }
        }
        conn.start(queue: queue)
        udpSend = conn
    }

    func sendPose(_ pose: NuosPose, seq: UInt16? = nil) {
        guard let udpSend, udpSend.state == .ready else { return }
        let s = seq ?? (poseSeq &+= 1)
        let hdr = NuosHdr(type: .pose, seq: s, len: UInt16(NuosProto.poseSize))
        var w = NuosWriter()
        hdr.encode(into: &w)
        w.raw(pose.encode())
        udpSend.send(content: Data(w.bytes), completion: .contentProcessed { _ in })
    }

    func sendHands(_ hands: NuosHands, seq: UInt16? = nil) {
        guard let udpSend, udpSend.state == .ready else { return }
        let s = seq ?? (handsSeq &+= 1)
        let hdr = NuosHdr(type: .hands, seq: s, len: UInt16(NuosProto.handsSize))
        var w = NuosWriter()
        hdr.encode(into: &w)
        w.raw(hands.encode())
        udpSend.send(content: Data(w.bytes), completion: .contentProcessed { _ in })
    }

    func sendStat() {
        queue.async { [weak self] in
            guard let self, let udpSend = self.udpSend, udpSend.state == .ready else { return }
            var s = NuosStat()
            s.tMonoNs = machAbsoluteTimeNanos()
            statSeq &+= 1
            let hdr = NuosHdr(type: .stat, seq: statSeq, len: UInt16(NuosProto.statSize))
            var w = NuosWriter()
            hdr.encode(into: &w)
            w.raw(s.encode())
            udpSend.send(content: Data(w.bytes), completion: .contentProcessed { _ in })
        }
    }

    // MARK: - UDP receive (video)

    private func setupUDPRecv() {
        guard udpListener == nil else { return }
        guard let port = NWEndpoint.Port(rawValue: NuosProto.portVideo) else { return }
        let p = NWParameters.udp
        p.allowLocalEndpointReuse = true
        guard let listener = try? NWListener(using: p, on: port) else {
            onControlEvent?("cannot bind video UDP :\(NuosProto.portVideo)")
            return
        }
        listener.newConnectionHandler = { [weak self] conn in
            guard let self else { return }
            self.queue.async {
                self.udpRecvConns.append(conn)
                self.recvLoop(conn)
            }
            conn.start(queue: self.queue)
        }
        listener.stateUpdateHandler = { [weak self] state in
            if case .failed(let err) = state {
                self?.queue.async { self?.onControlEvent?("video UDP failed: \(err)") }
            }
        }
        listener.start(queue: queue)
        udpListener = listener
    }

    private func recvLoop(_ conn: NWConnection) {
        conn.receiveMessage { [weak self, weak conn] data, _, _, error in
            guard let self else { return }
            if let data {
                self.queue.async { self.handleVideoDatagram([UInt8](data)) }
            }
            if error == nil, let conn {
                self.recvLoop(conn)
            }
        }
    }

    private func handleVideoDatagram(_ bytes: [UInt8]) {
        var r = NuosReader(bytes)
        guard let hdr = NuosHdr.decode(&r) else { return }
        switch hdr.type {
        case .video:
            guard let v = NuosVideo.decode(&r) else { return }
            stats.udpRxVideoBytes &+= UInt64(r.remaining)
            ingestFragment(v, payload: r.raw(min(Int(v.payloadLen), r.remaining)))
        case .fec:
            guard let f = NuosFec.decode(&r) else { return }
            ingestFEC(f, payload: r.raw(min(Int(f.payloadLen), r.remaining)))
        case .stat:
            if let s = NuosStat.decode(&r) {
                stats.boardStat = s
                onBoardStat?(s)
            }
        default:
            break
        }
    }

    // MARK: - Reassembly + FEC

    private func ingestFragment(_ v: NuosVideo, payload: [UInt8]) {
        let seq = v.frameSeq
        if seq != pendingSeq {
            // New frame: if the old one never completed, count it lost.
            if pendingSeq != nil, !pendingMissing.isEmpty {
                stats.framesIncomplete &+= 1
            }
            startPending(v)
        }
        pendingChunks[v.fragIdx] = payload
        pendingMissing.remove(v.fragIdx)
        tryComplete()
    }

    private func ingestFEC(_ f: NuosFec, payload: [UInt8]) {
        pendingFEC[f.groupStart] = (f.frameSeq, f.groupStart, f.groupCount, payload)
        tryComplete()
    }

    private func startPending(_ v: NuosVideo) {
        pendingSeq = v.frameSeq
        pendingFragCount = v.fragCount
        pendingIDR = (v.flags & NuosProto.videoFlagIDR) != 0
        pendingT = v.tMonoNs
        pendingRenderPos = v.renderPos
        pendingRenderQuat = v.renderQuat
        pendingChunks = [:]
        pendingMissing = Set(0..<v.fragCount)
        pendingFEC = [:]
    }

    private func tryComplete() {
        guard pendingSeq != nil, !pendingMissing.isEmpty else {
            if pendingSeq != nil { emitCompleted() }
            return
        }
        // XOR FEC recovery: only when exactly one fragment of a group is
        // missing and we hold the FEC fragment for that group (PROTOCOL.md 4).
        var recovered: [UInt16: [UInt8]] = [:]
        for missing in pendingMissing {
            let groupStart = missing - (missing % NuosProto.fecGroup)
            guard let fec = pendingFEC[groupStart] else { continue }
            let groupEnd = min(Int(groupStart + fec.count), Int(pendingFragCount))
            let groupMembers = UInt16(groupStart)..<UInt16(groupEnd)
            let missingInGroup = groupMembers.filter { pendingMissing.contains($0) }
            guard missingInGroup == [missing] else { continue }
            // XOR of all present payloads, padded to fec payload length.
            var acc = fec.payload
            for idx in groupMembers where idx != missing {
                guard let chunk = pendingChunks[idx] else { continue }
                for i in 0..<min(chunk.count, acc.count) { acc[i] ^= chunk[i] }
            }
            // Trim trailing zeros? Cannot know exact original length, so keep
            // padded length; HEVC RBSP tolerates trailing zero bytes only
            // outside NAL boundaries, so instead mark the recovered length as
            // the FEC payload length. (Sender XORs zero-padded payloads.)
            recovered[missing] = acc
        }
        guard !recovered.isEmpty else { return }
        for (idx, payload) in recovered {
            pendingChunks[idx] = payload
            pendingMissing.remove(idx)
            stats.fecRecovered &+= 1
        }
        if pendingMissing.isEmpty { emitCompleted() }
    }

    private func emitCompleted() {
        guard let seq = pendingSeq, pendingMissing.isEmpty else { return }
        var data: [UInt8] = []
        data.reserveCapacity(Int(pendingFragCount) * NuosProto.videoPayloadMax)
        for idx in 0..<pendingFragCount {
            guard let chunk = pendingChunks[idx] else { return }
            data.append(contentsOf: chunk)
        }
        let frame = NuosVideoFrame(frameSeq: seq,
                                   idr: pendingIDR,
                                   tMonoNs: pendingT,
                                   renderPos: pendingRenderPos,
                                   renderQuat: pendingRenderQuat,
                                   data: data)
        stats.framesAssembled &+= 1
        pushToJitterBuffer(frame)
        // Reset for the next frame.
        pendingSeq = nil
        pendingChunks = [:]
        pendingMissing = []
        pendingFEC = [:]
    }

    private func pushToJitterBuffer(_ frame: NuosVideoFrame) {
        // Estimate loss from gaps in frame_seq (wraps at UInt32.max).
        if let lastSeq = lastAssembledSeq {
            let delta = frame.frameSeq &- lastSeq
            if delta > 1 && delta < 1024 {
                stats.datagramsLost &+= UInt64(delta - 1)
            }
        }
        lastAssembledSeq = frame.frameSeq

        jitterBuffer.append(frame)
        // Keep at most `jitterDepth` (2) frames; older ones are stale.
        while jitterBuffer.count > jitterDepth {
            jitterBuffer.removeFirst()
            stats.framesDroppedStale &+= 1
        }
    }

    private var lastAssembledSeq: UInt32?

    /// Called each vsync from the renderer: newest fully-assembled frame,
    /// nil if none (PROTOCOL.md rule 5).
    func latestFrame() -> NuosVideoFrame? {
        // Synchronize minimally; called from render queue.
        queue.sync { jitterBuffer.last }
    }

    /// Drop everything (e.g. after a session renegotiation or IDR request).
    func resetVideo() {
        queue.sync {
            jitterBuffer.removeAll()
            pendingSeq = nil
            pendingChunks = [:]
            pendingMissing = []
            pendingFEC = [:]
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func machAbsoluteTimeNanos() -> UInt64 {
    // iPhone timebase is already 1 ns per tick.
    DispatchTime.now().uptimeNanoseconds
}
