//
//  ContentView.swift
//  NuosView
//
//  Connect screen (mDNS board list + manual board entry) and the streaming
//  view (MTKView render surface) with a HUD overlay of live stats.
//

import ARKit
import MetalKit
import SwiftUI

struct ContentView: View {
    @EnvironmentObject var sessionModel: SessionModel
    @State private var showDiagnostics = false
    @State private var showStream = false

    var body: some View {
        NavigationStack {
            switch sessionModel.state {
            case .idle, .browsing, .connecting:
                connectScreen
            case .streaming:
                streamingView
            }
        }
        .navigationViewStyle(.stack)
    }

    // MARK: - Connect screen

    private var connectScreen: some View {
        VStack(spacing: 16) {
            Text("NuOS View")
                .font(.largeTitle.bold())

            Text(sessionModel.statusMessage)
                .font(.subheadline)
                .foregroundStyle(.secondary)

            if sessionModel.state == .idle {
                Button("Find Boards") {
                    sessionModel.startBrowsing()
                }
                .buttonStyle(.borderedProminent)
            } else {
                ProgressView()
            }

            List(sessionModel.boards) { board in
                Button {
                    sessionModel.connect(to: board)
                } label: {
                    VStack(alignment: .leading) {
                        Text(board.name).font(.headline)
                        Text("proto \(board.txt["proto"] ?? "?") — \(board.endpoints.count) addr")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .listStyle(.plain)
            .frame(maxHeight: 240)

            Button("Diagnostics") {
                showDiagnostics = true
            }
            .sheet(isPresented: $showDiagnostics) {
                DiagnosticsView()
                    .environmentObject(sessionModel)
            }

            if sessionModel.state != .idle {
                Button("Stop", role: .destructive) {
                    sessionModel.stopEverything()
                }
            }
        }
        .padding()
    }

    // MARK: - Streaming

    private var streamingView: some View {
        ZStack(alignment: .topTrailing) {
            if ARWorldTrackingConfiguration.isSupported {
                StreamView(sessionModel: sessionModel)
                    .ignoresSafeArea()
            } else {
                Color.black.ignoresSafeArea()
                Text("ARKit world tracking unavailable on this device")
                    .foregroundStyle(.white)
            }

            hud
        }
    }

    private var hud: some View {
        VStack(alignment: .trailing, spacing: 4) {
            Text("state: \(sessionModel.state.rawValue)")
            Text("link: \(sessionModel.stats.tcpState)")
            if let stat = sessionModel.stats.boardStat {
                Text("board: \(String(format: "%.1f", Double(stat.fpsX10) / 10)) fps  \(stat.bitrateKbps) kbps")
                Text("loss: \(stat.lossPpm) ppm  batt: \(stat.batteryPct == 255 ? "?" : "\(stat.batteryPct)")%")
            }
            Text("rtt: \(sessionModel.stats.rttMs >= 0 ? String(format: "%.1f ms", sessionModel.stats.rttMs) : "?")")
            HStack {
                Button("Diag") { showDiagnostics = true }
                    .buttonStyle(.bordered)
                Button("Disconnect", role: .destructive) {
                    sessionModel.stopEverything()
                }
                .buttonStyle(.bordered)
            }
        }
        .font(.system(size: 11, design: .monospaced))
        .padding(8)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))
        .padding(8)
        .sheet(isPresented: $showDiagnostics) {
            DiagnosticsView()
                .environmentObject(sessionModel)
        }
    }
}

// MARK: - MTKView wrapper

struct StreamView: UIViewRepresentable {
    let sessionModel: SessionModel

    func makeCoordinator() -> Coordinator {
        Coordinator(sessionModel: sessionModel)
    }

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView()
        guard let renderer = NuosRenderer(mtkView: view) else { return view }
        context.coordinator.renderer = renderer
        context.coordinator.start(view: view)
        view.delegate = renderer
        return view
    }

    func updateUIView(_ uiView: MTKView, context: Context) {}

    final class Coordinator {
        weak var sessionModel: SessionModel?
        var renderer: NuosRenderer?
        let trackingEngine = TrackingEngine()
        let handTracker = HandTracker()
        let decoder = HEVCDecoder()
        private var sessionStarted = false

        init(sessionModel: SessionModel) {
            self.sessionModel = sessionModel
            trackingEngine.sessionModel = sessionModel
            handTracker.sessionModel = sessionModel
            trackingEngine.handTracker = handTracker

            // Assembled frames (newest of the 2-frame jitter buffer) feed
            // decode + the renderer's timewarp source.
            sessionModel.link.onVideoFrame = { [weak self] frame in
                guard let self else { return }
                self.decoder.decode(frameSeq: frame.frameSeq, annexB: frame.data)
                self.renderer?.setLatestFrame(frame)
            }
            decoder.onDecodedFrame = { [weak self] pb in
                self?.renderer?.setDecodedPixelBuffer(pb)
            }
        }

        func start(view: MTKView) {
            guard let renderer else { return }
            renderer.trackingEngine = trackingEngine
            renderer.decoder = decoder
            if !sessionStarted, let sessionModel = sessionModel {
                trackingEngine.start(session: ARSession())
                sessionModel.trackingEngine = trackingEngine
                sessionModel.renderer = renderer
                sessionStarted = true
            }
        }
    }
}
