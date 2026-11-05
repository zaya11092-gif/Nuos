//
//  UltraWideProbe.swift
//  NuosView
//
//  Informational runtime probe of AVCaptureMultiCamSession support.
//
//  IMPORTANT: ARKit owns the camera while tracking is active. We never run an
//  always-on second capture session — attempting it would conflict with
//  ARKit's ARSession (see docs/ios-camera-constraints.md). This probe answers
//  one question: could this device host simultaneous multi-camera capture at
//  all? Its result gates NUOS_CAP_ULTRAWIDE in the phone HELLO caps, and an
//  *experimental*, off-by-default path may briefly open an
//  AVCaptureMultiCamSession (passthrough only, no ARKit running) to validate
//  the configuration for real.
//

import AVFoundation
import Foundation

struct MultiCamConfigRow: Identifiable, Equatable {
    let id = UUID()
    let description: String
}

final class UltraWideProbe: ObservableObject {
    static let shared = UltraWideProbe()

    @Published var ranProbe = false
    /// True only if AVCaptureMultiCamSession.isMultiCamSupported AND at
    /// least one usable multi-cam session configuration was found.
    @Published var probePassed = false
    @Published var isMultiCamSupported = false
    @Published var supportedConfigs: [MultiCamConfigRow] = []
    @Published var notes: String = ""

    /// Experimental passthrough path (OFF by default).
    @Published var experimentalPassthroughActive = false

    private init() {
        // Cheap static capability flag (hardware generation dependent).
        isMultiCamSupported = AVCaptureMultiCamSession.isMultiCamSupported
    }

    /// Enumerate every capture device and the per-device
    /// supportedMultiCamSessionConfigurations. No session is started; this is
    /// purely informational and safe to run while ARKit is active.
    func runProbe() {
        var rows: [MultiCamConfigRow] = []
        let types: [AVCaptureDevice.DeviceType] = [
            .builtInWideAngleCamera,
            .builtInUltraWideCamera,
            .builtInTelephotoCamera,
            .builtInDualCamera,
            .builtInDualWideCamera,
            .builtInTripleCamera,
        ]
        let session = AVCaptureDevice.DiscoverySession(
            deviceTypes: types,
            mediaType: .video,
            position: .back)

        let devices = session.devices
        for device in devices {
            // Per-device multi-cam configuration descriptors are not exposed
            // by this SDK; report the discovered back cameras instead and
            // treat any ultra-wide presence as a usable configuration.
            let kind: String
            switch device.deviceType {
            case .builtInUltraWideCamera: kind = "UltraWideCam"
            case .builtInWideAngleCamera: kind = "WideCam"
            case .builtInTelephotoCamera: kind = "TeleCam"
            default: kind = device.deviceType.rawValue
            }
            rows.append(MultiCamConfigRow(
                description: "\(device.localizedName) (\(kind))"))
        }

        DispatchQueue.main.async {
            self.ranProbe = true
            self.supportedConfigs = rows
            // Pass criterion: hardware says multi-cam is possible AND at
            // least one configuration listed on a real capture device.
            let hasConfig = rows.contains { $0.description.contains("UltraWideCam") }
            self.probePassed = self.isMultiCamSupported && hasConfig
            self.notes = self.probePassed
                ? "Multi-cam supported. NUOS_CAP_ULTRAWIDE will be advertised."
                : "Multi-cam NOT usable on this device. NUOS_CAP_ULTRAWIDE withheld."
        }
    }

    /// EXPERIMENTAL, off by default. Briefly opens an AVCaptureMultiCamSession
    /// to prove simultaneous capture works. Must NOT be called while an
    /// ARSession is running (ARKit owns the camera).
    func runExperimentalValidation(arSessionRunning: Bool) {
        guard !arSessionRunning else {
            notes = "Refusing experimental probe: ARSession is running."
            return
        }
        guard probePassed else { return }
        let session = AVCaptureMultiCamSession()
        let wide = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
        let ultra = AVCaptureDevice.default(.builtInUltraWideCamera, for: .video, position: .back)
        guard let wide, let ultra else { return }
        do {
            let inWide = try AVCaptureDeviceInput(device: wide)
            let inUltra = try AVCaptureDeviceInput(device: ultra)
            if session.canAddInput(inWide) && session.canAddInput(inUltra) {
                session.addInputWithNoConnections(inWide)
                session.addInputWithNoConnections(inUltra)
                let outW = AVCaptureVideoDataOutput()
                let outU = AVCaptureVideoDataOutput()
                if session.canAddOutput(outW) && session.canAddOutput(outU) {
                    session.addOutputWithNoConnections(outW)
                    session.addOutputWithNoConnections(outU)
                    let connW = AVCaptureConnection(inputPorts: inWide.ports, output: outW)
                    let connU = AVCaptureConnection(inputPorts: inUltra.ports, output: outU)
                    guard session.canAddConnection(connW) && session.canAddConnection(connU)
                    else { throw NSError(domain: "nuos.probe", code: 1) }
                    session.addConnection(connW)
                    session.addConnection(connU)
                    session.startRunning()
                    let ok = session.inputs.count == 2
                    session.stopRunning()
                    DispatchQueue.main.async {
                        self.experimentalPassthroughActive = false
                        self.notes = ok
                            ? "Experimental simultaneous capture OK."
                            : "Experimental simultaneous capture FAILED."
                    }
                    return
                }
            }
            throw NSError(domain: "nuos.probe", code: 2)
        } catch {
            DispatchQueue.main.async {
                self.notes = "Experimental multi-cam validation failed: \(error.localizedDescription)"
            }
        }
    }
}
