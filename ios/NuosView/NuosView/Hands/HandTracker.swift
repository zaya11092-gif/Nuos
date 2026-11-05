//
//  HandTracker.swift
//  NuosView
//
//  Vision-based hand tracking on ARFrames:
//    - VNDetectHumanHandPoseRequest, maximumHandCount = 2
//    - 30 Hz throttle; 15 Hz when thermal throttling is requested
//      (STAT flags bit0, per PROTOCOL.md)
//    - joints ordered per NuosProto.handJointOrder (wrist, thumb(4),
//      index(4), middle(4), ring(4), little(4)), x/y normalized image
//      coords (y down, top-left origin), z = relative depth proxy
//    - emits NUOS_PKT_HANDS on the telemetry socket
//

import ARKit
import Foundation
import Vision

final class HandTracker {
    @Published var handsFps: Double = 0
    @Published var lastDetectedMask: UInt8 = 0
    /// Hands on/off toggle (DiagnosticsView).
    var enabled = true

    weak var sessionModel: SessionModel?

    private let request = VNDetectHumanHandPoseRequest()
    private let handlerQueue = DispatchQueue(label: "nuos.hands")
    private let sendQueue = DispatchQueue(label: "nuos.hands.send")

    private var lastRunTime = Date.distantPast
    private var thermalThrottled = false
    private var jointSeq: UInt16 = 0
    private var sentCount = 0
    private var fpsWindowStart = Date()

    // Canonical joint order per PROTOCOL.md; index into Vision's joint dict.
    private static let jointNames: [VNHumanHandPoseObservation.JointName] = [
        .wrist,
        .thumbCMC, .thumbMP, .thumbIP, .thumbTip,
        .indexMCP, .indexPIP, .indexDIP, .indexTip,
        .middleMCP, .middlePIP, .middleDIP, .middleTip,
        .ringMCP, .ringPIP, .ringDIP, .ringTip,
        .littleMCP, .littlePIP, .littleDIP, .littleTip,
    ]

    init() {
        request.maximumHandCount = 2
        request.revision = VNDetectHumanHandPoseRequestRevision1
    }

    /// Called from ARSessionDelegate on every ARFrame; internally throttled.
    func process(arFrame: ARFrame) {
        guard enabled else { return }
        guard let sessionModel = sessionModel, sessionModel.state == .streaming else { return }
        let now = Date()
        let minInterval: TimeInterval = thermalThrottled ? 1.0 / 15.0 : 1.0 / 30.0
        guard now.timeIntervalSince(lastRunTime) >= minInterval else { return }
        lastRunTime = now

        let pixelBuffer = arFrame.capturedImage
        handlerQueue.async { [weak self] in
            guard let self else { return }
            let handler = VNImageRequestHandler(cvPixelBuffer: pixelBuffer, orientation: .right, options: [:])
            do {
                try handler.perform([self.request])
            } catch {
                return
            }
            guard let observations = self.request.results, !observations.isEmpty else {
                self.lastDetectedMask = 0
                return
            }

            var hands = NuosHands()
            hands.tMonoNs = DispatchTime.now().uptimeNanoseconds

            var leftPresent = false
            var rightPresent = false
            for obs in observations {
                guard let joints = try? obs.recognizedPoints(.all) else { continue }
                // Vision returns points in normalized image space with origin
                // at bottom-left and y up; convert to top-left origin, y down
                // as required by PROTOCOL.md.
                var pts = Array(repeating: [Float](repeating: 0, count: 3), count: NuosProto.handJoints)
                for (i, name) in Self.jointNames.enumerated() {
                    if let p = joints[name], p.confidence > 0.3 {
                        pts[i][0] = Float(p.location.x)
                        pts[i][1] = Float(1.0 - p.location.y) // y down
                        // VNRecognizedPoint exposes no depth on this SDK; keep wire [x,y,z]
                        // format (PROTOCOL.md) with z = 0.
                        pts[i][2] = 0
                    }
                }
                // VNHumanHandPoseObservation does not expose chirality, so
                // assign by observation order: first detected hand -> left,
                // second -> right. Good enough for UI pointing.
                if !leftPresent {
                    leftPresent = true
                    hands.left = pts
                } else {
                    rightPresent = true
                    hands.right = pts
                }
            }

            hands.present = (leftPresent ? 0b01 : 0) | (rightPresent ? 0b10 : 0)
            self.lastDetectedMask = hands.present
            guard hands.present != 0 else { return }

            self.sendQueue.async { [weak self] in
                guard let self else { return }
                self.jointSeq &+= 1
                self.sentCount += 1
                let now2 = Date()
                let dt = now2.timeIntervalSince(self.fpsWindowStart)
                if dt >= 1.0 {
                    self.handsFps = Double(self.sentCount) / dt
                    self.sentCount = 0
                    self.fpsWindowStart = now2
                }
                self.sessionModel?.link.sendHands(hands, seq: self.jointSeq)
            }
        }
    }

    /// STAT flags bit0 from the board ("please reduce") backs hands off to
    /// 15 Hz per PROTOCOL.md.
    func setThermalThrottled(_ throttled: Bool) {
        thermalThrottled = throttled
    }
}
