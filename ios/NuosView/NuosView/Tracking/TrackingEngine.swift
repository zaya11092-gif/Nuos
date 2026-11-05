//
//  TrackingEngine.swift
//  NuosView
//
//  ARKit world tracking, gravity-aligned (ARWorldTrackingConfiguration).
//  Each camera frame (~60 Hz on iPhone 15):
//    - builds a NuosPose (pos/quat/vel/angVel/tracking) and sends it as a
//      NUOS_PKT_POSE datagram over the telemetry UDP socket,
//    - hands the ARFrame to HandTracker (Vision, throttled) and to the
//      renderer's passthrough backdrop buffer.
//

import ARKit
import Foundation
import simd

final class TrackingEngine: NSObject, ObservableObject, ARSessionDelegate {
    @Published var trackingState: String = "not running"
    @Published var poseFps: Double = 0
    @Published var lastError: String?

    weak var sessionModel: SessionModel?

    /// Latest captured frame's pixelBuffer for the passthrough backdrop
    /// (owned by render thread reads; assign on session queue).
    private(set) var latestPixelBuffer: CVPixelBuffer?
    private(set) var latestARFrame: ARFrame?

    /// Hands throttle (Vision 30 Hz, 15 Hz when thermal throttled).
    var handTracker: HandTracker?

    private var poseSeq: UInt16 = 0
    private var lastCamTransform: simd_float4x4?
    private var lastCamTime: Double?
    private var lastPoseTime = Date()
    private var poseCount = 0
    private var fpsWindowStart = Date()
    private let linkQueue = DispatchQueue(label: "nuos.tracking")

    func start(session: ARSession) {
        guard ARWorldTrackingConfiguration.isSupported else {
            lastError = "ARWorldTrackingConfiguration unsupported"
            return
        }
        let config = ARWorldTrackingConfiguration()
        config.worldAlignment = .gravity
        // On iPhone 15 the rear ultra-wide is ARKit's second camera when
        // worldTracking uses it internally; we do NOT run a second capture
        // session (see UltraWideProbe notes).
        session.delegate = self
        session.run(config, options: [.resetTracking, .removeExistingAnchors])
        trackingState = "starting"
    }

    func stop(session: ARSession) {
        session.pause()
        trackingState = "paused"
        latestPixelBuffer = nil
        latestARFrame = nil
    }

    // MARK: - ARSessionDelegate

    func session(_ session: ARSession, didUpdate frame: ARFrame) {
        latestARFrame = frame
        latestPixelBuffer = frame.capturedImage

        // Feed Vision-based hand tracking (internally throttled).
        handTracker?.process(arFrame: frame)

        sendPose(from: frame)
    }

    func session(_ session: ARSession, didFailWithError error: Error) {
        lastError = error.localizedDescription
        trackingState = "failed: \(error.localizedDescription)"
    }

    func sessionWasInterrupted(_ session: ARSession) {
        trackingState = "interrupted"
    }

    func sessionInterruptionEnded(_ session: ARSession) {
        trackingState = "resuming"
        let config = ARWorldTrackingConfiguration()
        config.worldAlignment = .gravity
        session.run(config, options: [.resetTracking])
    }

    func session(_ session: ARSession, cameraDidChangeTrackingState camera: ARCamera) {
        switch camera.trackingState {
        case .normal:
            trackingState = "normal"
        case .limited(let reason):
            trackingState = "limited (\(reason))"
        case .notAvailable:
            trackingState = "not available"
        }
    }

    // MARK: - Pose transmission

    private func sendPose(from frame: ARFrame) {
        guard let sessionModel = sessionModel, sessionModel.state == .streaming else { return }
        let link = sessionModel.link

        var pose = NuosPose()
        pose.tMonoNs = DispatchTime.now().uptimeNanoseconds

        let cam = frame.camera
        let t = cam.transform
        let p = simd_make_float3(t.columns.3)
        pose.pos = (p.x, p.y, p.z)

        // ARKit transform -> quaternion (w x y z).
        let q = simd_quatf(t)
        pose.quat = (q.vector.w, q.vector.x, q.vector.y, q.vector.z)

        // ARCamera.velocity/angularVelocity are not exposed by this SDK;
        // estimate both from the previous frame's transform (finite diff).
        let now = frame.timestamp
        if let prevT = lastCamTransform, let prevTime = lastCamTime {
            let dt = Float(now - prevTime)
            if dt > 0.0001 {
                let pPrev = simd_make_float3(prevT.columns.3)
                let vel = (p - pPrev) / dt
                pose.vel = (vel.x, vel.y, vel.z)
                let dq = q * simd_quatf(prevT).inverse
                let angRate = dq.angle / dt
                let axis = dq.axis
                pose.angVel = (axis.x * angRate, axis.y * angRate, axis.z * angRate)
            }
        }
        lastCamTransform = t
        lastCamTime = now

        switch cam.trackingState {
        case .normal: pose.tracking = 2
        case .limited: pose.tracking = 1
        case .notAvailable: pose.tracking = 0
        }

        if UltraWideProbe.shared.experimentalPassthroughActive {
            pose.flags |= NuosProto.poseFlagUltrawidePassthrough
        }

        linkQueue.async { [weak self] in
            guard let self else { return }
            self.poseSeq &+= 1
            link.sendPose(pose, seq: self.poseSeq)
            self.trackFps()
        }
    }

    private func trackFps() {
        poseCount += 1
        let now = Date()
        let dt = now.timeIntervalSince(fpsWindowStart)
        if dt >= 1.0 {
            poseFps = Double(poseCount) / dt
            poseCount = 0
            fpsWindowStart = now
        }
        _ = lastPoseTime
        lastPoseTime = now
    }
}
