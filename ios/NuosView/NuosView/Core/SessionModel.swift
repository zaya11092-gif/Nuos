//
//  SessionModel.swift
//  NuosView
//
//  Session state machine: idle -> browsing -> connecting -> streaming.
//  Owns the NuosLink and drives reconnection on loss (exponential backoff).
//

import Foundation
import Combine
import Network

enum SessionState: String, Equatable {
    case idle       // not connected, not searching
    case browsing   // mDNS browsing for boards
    case connecting // TCP HELLO/SESSION handshake in flight
    case streaming  // SESSION sent, video flowing
}

final class SessionModel: ObservableObject {
    @Published var state: SessionState = .idle
    @Published var boards: [NuosDiscoveredBoard] = []
    @Published var selectedBoardID: String?
    @Published var statusMessage = "Idle"
    @Published var stats = LinkStats()
    @Published var lastError: String?

    // Session parameters (nuos_session_t)
    @Published var fps: UInt8 = 60
    @Published var perEyeWidth: UInt16 = 1280
    @Published var perEyeHeight: UInt16 = 800
    @Published var bitrateKbps: UInt32 = 15_000
    @Published var passthrough: Bool = true

    // Reconnect
    private var reconnectAttempt = 0
    private var reconnectTimer: Timer?
    private var wantConnected = false

    let link = NuosLink()
    let discovery = NuosDiscovery()
    /// Live tracking pipeline while streaming (set by StreamView).
    weak var trackingEngine: TrackingEngine?
    /// Live renderer while streaming (set by StreamView) — k1/k2 etc.
    weak var renderer: NuosRenderer?
    private var cancellables = Set<AnyCancellable>()

    init() {
        NuosProto.validateStructSizes()

        discovery.$boards
            .receive(on: DispatchQueue.main)
            .sink { [weak self] boards in
                guard let self else { return }
                self.boards = boards
                if self.state == .browsing, boards.isEmpty == false, self.selectedBoardID == nil {
                    self.selectedBoardID = boards.first?.id
                }
                if self.state == .browsing {
                    self.statusMessage = boards.isEmpty
                        ? "Searching for NuOS boards (_nuos-xr._tcp)…"
                        : "Found \(boards.count) board(s)"
                }
            }
            .store(in: &cancellables)

        link.onControlEvent = { [weak self] msg in
            DispatchQueue.main.async { self?.statusMessage = msg }
        }

        link.onBoardHello = { [weak self] _ in
            DispatchQueue.main.async { self?.beginStreaming() }
        }

        link.onBoardStat = { [weak self] stat in
            DispatchQueue.main.async {
                var s = self?.stats ?? LinkStats()
                s.boardStat = stat
                self?.stats = s
                // STAT flags bit0 = "please reduce": hands back off to 15 Hz.
                let throttle = (stat.flags & NuosProto.statFlagThermalThrottle) != 0
                self?.trackingEngine?.handTracker?.setThermalThrottled(throttle)
            }
        }

        link.onTcpClosed = { [weak self] in
            DispatchQueue.main.async { self?.handleConnectionLost() }
        }

        // 1 Hz PING + STAT per PROTOCOL.md while streaming.
        let heartbeat = Timer.publish(every: 1.0, on: .main, in: .common).autoconnect()
        heartbeat
            .sink { [weak self] _ in
                guard let self, self.state == .streaming else { return }
                self.link.sendPing()
                self.link.sendStat()
            }
            .store(in: &cancellables)
    }

    // MARK: - State transitions

    func startBrowsing() {
        wantConnected = true
        setState(.browsing, message: "Searching for NuOS boards (_nuos-xr._tcp)…")
        discovery.start()
    }

    func stopEverything() {
        wantConnected = false
        reconnectTimer?.invalidate()
        reconnectTimer = nil
        reconnectAttempt = 0
        link.disconnect()
        discovery.stop()
        setState(.idle, message: "Idle")
    }

    func connect(to board: NuosDiscoveredBoard) {
        guard state != .connecting, state != .streaming else { return }
        selectedBoardID = board.id
        setState(.connecting, message: "Connecting to \(board.name)…")
        link.connectControl(to: controlEndpoint(for: board))
    }

    /// Manual connect by IP (fallback for networks with multicast filtering).
    func connectToIP(_ host: String) {
        guard state != .connecting, state != .streaming else { return }
        let trimmed = host.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return }
        wantConnected = true
        selectedBoardID = trimmed
        setState(.connecting, message: "Connecting to \(trimmed)…")
        let port = NWEndpoint.Port(rawValue: NuosProto.portControl)!
        link.connectControl(to: NWEndpoint.hostPort(host: .init(trimmed), port: port))
    }

    /// Endpoint for the TCP control channel: prefer a resolved sockaddr from
    /// mDNS, otherwise host name + control port.
    private func controlEndpoint(for board: NuosDiscoveredBoard) -> NWEndpoint {
        if let ep = board.endpoints.first {
            return ep
        }
        let host = board.host ?? "10.66.0.1"
        let port = NWEndpoint.Port(rawValue: UInt16(clamping: board.port)) ?? NWEndpoint.Port(rawValue: NuosProto.portControl)!
        return NWEndpoint.hostPort(host: .init(host), port: port)
    }

    /// Called once the TCP HELLO exchange succeeds; sends SESSION and moves
    /// to streaming.
    func beginStreaming() {
        guard state == .connecting else { return }
        let session = NuosSession(
            fps: fps,
            codec: 0, // HEVC
            passthrough: passthrough ? 1 : 0,
            reserved: 0,
            width: perEyeWidth,
            height: perEyeHeight,
            bitrateKbps: bitrateKbps)
        link.sendSession(session)
        setState(.streaming, message: "Streaming (\(fps) fps, \(perEyeWidth)x\(perEyeHeight)/eye)")
    }

    // MARK: - Reconnection

    private func handleConnectionLost() {
        guard wantConnected else {
            setState(.idle, message: "Disconnected")
            return
        }
        setState(.browsing, message: "Link lost — reconnecting…")
        reconnectAttempt += 1
        let delay = min(5.0, 0.5 * pow(2.0, Double(min(reconnectAttempt, 4))))
        reconnectTimer?.invalidate()
        reconnectTimer = Timer.scheduledTimer(withTimeInterval: delay, repeats: false) { [weak self] _ in
            self?.attemptReconnect()
        }
    }

    private func attemptReconnect() {
        guard wantConnected else { return }
        // Re-browse; when a board shows up, connect automatically.
        if !discovery.isBrowsing { discovery.start() }
        if state == .browsing, let board = boards.first {
            reconnectAttempt = 0
            connect(to: board)
        }
    }

    private func setState(_ s: SessionState, message: String) {
        state = s
        statusMessage = message
        if s != .streaming {
            var st = stats
            st.tcpState = s == .idle ? "idle" : st.tcpState
            stats = st
        }
    }
}
