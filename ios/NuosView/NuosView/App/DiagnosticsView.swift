//
//  DiagnosticsView.swift
//  NuosView
//
//  Runtime diagnostics: UltraWideProbe results (isMultiCamSupported +
//  supportedMultiCamSessionConfigurations per AVCaptureDevice), ARKit
//  tracking state, fps counters, link stats, battery/thermal; toggles for
//  passthrough and hands; barrel distortion k1/k2 sliders.
//

import ARKit
import SwiftUI

struct DiagnosticsView: View {
    @EnvironmentObject var sessionModel: SessionModel
    @Environment(\.dismiss) private var dismiss
    @StateObject private var probe = UltraWideProbe.shared
    @State private var thermalState = ProcessInfo.processInfo.thermalState
    @State private var thermalTimer: Timer?

    var body: some View {
        NavigationStack {
            List {
                sectionUltraWide
                sectionTracking
                sectionCounters
                sectionLink
                sectionPowerThermal
                sectionToggles
                sectionDistortion
            }
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button("Back") { dismiss() }
                }
            }
            .navigationTitle("Diagnostics")
            .onAppear {
                probe.runProbe()
                thermalTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { _ in
                    thermalState = ProcessInfo.processInfo.thermalState
                }
            }
            .onDisappear {
                thermalTimer?.invalidate()
                thermalTimer = nil
            }
        }
    }

    // MARK: - UltraWideProbe

    private var sectionUltraWide: some View {
        Section("UltraWide probe (AVCaptureMultiCamSession)") {
            LabeledRow("isMultiCamSupported", probe.isMultiCamSupported ? "yes" : "no")
            LabeledRow("probePassed", probe.probePassed ? "yes (ULTRAWIDE cap advertised)" : "no (cap withheld)")
            LabeledRow("configurations", "\(probe.supportedConfigs.count)")
            ForEach(probe.supportedConfigs) { row in
                Text(row.description)
                    .font(.system(size: 11, design: .monospaced))
            }
            Text(probe.notes)
                .font(.caption)
                .foregroundStyle(.secondary)
            Button("Re-run probe") { probe.runProbe() }
            Button("Experimental simultaneous-capture validation") {
                probe.runExperimentalValidation(arSessionRunning: sessionModel.state == .streaming)
            }
            .disabled(!probe.probePassed)
            Text("Note: ARKit owns the camera during streaming; the experimental path is off by default and refuses to run while an ARSession is active.")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
    }

    // MARK: - Tracking

    private var sectionTracking: some View {
        Section("ARKit tracking") {
            if let te = sessionModel.trackingEngine {
                LabeledRow("tracking state", te.trackingState)
                LabeledRow("pose fps", String(format: "%.1f", te.poseFps))
                if let err = te.lastError {
                    LabeledRow("error", err)
                }
            } else {
                LabeledRow("tracking", "not running")
            }
            LabeledRow("hands mask", sessionModel.trackingEngine?.handTracker?.lastDetectedMask.description ?? "—")
        }
    }

    // MARK: - Counters

    private var sectionCounters: some View {
        Section("Counters") {
            LabeledRow("frames assembled", "\(sessionModel.stats.framesAssembled)")
            LabeledRow("frames dropped (stale)", "\(sessionModel.stats.framesDroppedStale)")
            LabeledRow("frames incomplete", "\(sessionModel.stats.framesIncomplete)")
            LabeledRow("FEC recovered", "\(sessionModel.stats.fecRecovered)")
            LabeledRow("datagrams lost (est.)", "\(sessionModel.stats.datagramsLost)")
            LabeledRow("video rx bytes", "\(sessionModel.stats.udpRxVideoBytes)")
            if let dec = sessionModel.decoder {
                LabeledRow("frames decoded", "\(dec.decodedCount)")
                LabeledRow("decode errors", "\(dec.decodeErrors)")
            } else {
                LabeledRow("decoder", "not created")
            }
        }
    }

    // MARK: - Link

    private var sectionLink: some View {
        Section("Link") {
            LabeledRow("control", sessionModel.stats.tcpState)
            LabeledRow("rtt", sessionModel.stats.rttMs >= 0 ? String(format: "%.1f ms", sessionModel.stats.rttMs) : "unknown")
            if let s = sessionModel.stats.boardStat {
                LabeledRow("board fps", String(format: "%.1f", Double(s.fpsX10) / 10))
                LabeledRow("board bitrate", "\(s.bitrateKbps) kbps")
                LabeledRow("board loss", "\(s.lossPpm) ppm")
                LabeledRow("board temp", s.tempCX10 >= 0 ? String(format: "%.1f °C", Double(s.tempCX10) / 10) : "unknown")
            } else {
                LabeledRow("board stat", "not received")
            }
        }
    }

    // MARK: - Power / thermal

    private var sectionPowerThermal: some View {
        Section("Battery / thermal (phone)") {
            LabeledRow("thermal state", "\(thermalState)")
        }
    }

    // MARK: - Toggles

    private var sectionToggles: some View {
        Section("Toggles") {
            Toggle("Passthrough backdrop", isOn: Binding(
                get: { sessionModel.renderer?.passthroughEnabled ?? sessionModel.passthrough },
                set: { on in
                    sessionModel.renderer?.passthroughEnabled = on
                    sessionModel.passthrough = on
                }))
            Toggle("Hands tracking", isOn: Binding(
                get: { sessionModel.trackingEngine?.handTracker?.enabled ?? true },
                set: { on in
                    sessionModel.trackingEngine?.handTracker?.enabled = on
                }))
        }
    }

    // MARK: - Distortion

    private var sectionDistortion: some View {
        Section("Barrel distortion") {
            KSlider(label: "k1") { v in
                sessionModel.renderer?.distortionK1 = v
            }
            KSlider(label: "k2") { v in
                sessionModel.renderer?.distortionK2 = v
            }
            Text("Sliders apply on the next rendered frame (the renderer reads k1/k2 per draw).")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }
}

private struct KSlider: View {
    let label: String
    let onChange: (Float) -> Void
    @State private var value: Float = 0

    var body: some View {
        VStack(alignment: .leading) {
            Text("\(label): \(String(format: "%.3f", value))")
                .font(.system(size: 12, design: .monospaced))
            Slider(value: $value, in: -0.5...0.5) { editing in
                if !editing { onChange(value) }
            }
            .onChange(of: value) { _, new in onChange(new) }
        }
    }
}

private struct LabeledRow: View {
    let label: String
    let value: String

    init(_ label: String, _ value: String) {
        self.label = label
        self.value = value
    }

    var body: some View {
        HStack {
            Text(label)
            Spacer()
            Text(value)
                .foregroundStyle(.secondary)
                .font(.system(size: 12, design: .monospaced))
        }
    }
}
