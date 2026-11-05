//
//  Shaders.metal
//  NuosView
//
//  Matching shaders for NuosRenderer:
//    - passthroughVertex/yuvFragment : fullscreen YUV -> RGB passthrough (2D fallback)
//    - timewarpVertex/videoFragment  : rotation-only timewarp of the video layer
//    - distortVertex/compositeFragment : SBS stereo, barrel pre-distortion,
//      passthrough backdrop + virtual video, per-eye offset
//

#include <metal_stdlib>

using namespace metal;
constexpr sampler linearSampler(coord::normalized, address::clamp_to_edge, filter::linear);
using namespace metal;

// Must match Renderer.swift's Uniforms struct layout.
struct Uniforms {
    float4 warpQuat;   // (w, x, y, z) timewarp delta
    float  k1;
    float  k2;
    float  dim;
    int    videoOn;
    float  pad0;
    float  pad1;
    float  pad2;
};

// Distortion mesh vertex from Renderer.buildDistortionMesh().
struct MeshVertex {
    float2 pos;   // [0,1] horizontal position (splits eyes at 0.5)
    float2 uv;    // pre-distorted texture uv (already k1/k2 applied on CPU)
    float  eye;   // 0 = left, 1 = right
};

// ---------------------------------------------------------------------------
// YUV -> RGB (BT.709 limited range, 420f biplanar)
// ---------------------------------------------------------------------------

float3 yuv2rgb(float y, float2 cbcr) {
    // BT.709 full-range-ish approximation used by ARKit 420f buffers.
    float cb = cbcr.x - 0.5;
    float cr = cbcr.y - 0.5;
    float r = y + 1.5748 * cr;
    float g = y - 0.1873 * cb - 0.4681 * cr;
    float b = y + 1.8556 * cb;
    return clamp(float3(r, g, b), 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// 2D passthrough fallback (fullscreen triangle strip, procedural quad)
// ---------------------------------------------------------------------------

vertex float4 passthroughVertex(uint vid [[vertex_id]]) {
    // 4-vertex strip: (-1,-1) (1,-1) (-1,1) (1,1)
    float2 p[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };
    return float4(p[vid], 0.0, 1.0);
}

fragment float4 yuvFragment(float4 ndc [[position]],
                            texture2d<float> yTex [[texture(0)]],
                            texture2d<float> cbcrTex [[texture(1)]],
                            constant float &dim [[buffer(0)]]) {
    float2 uv = float2((ndc.x + 1.0) * 0.5, 1.0 - (ndc.y + 1.0) * 0.5);
    float y = yTex.sample(linearSampler, uv).r;
    float2 cbcr = cbcrTex.sample(linearSampler, uv).rg;
    float3 rgb = yuv2rgb(y, cbcr);
    return float4(rgb * dim, 1.0);
}

// ---------------------------------------------------------------------------
// Rotation-only timewarp (video layer)
// ---------------------------------------------------------------------------

/// Rotate a 2D direction by the yaw/pitch implied by quaternion q.
/// We only warp rotationally; per PROTOCOL.md translation is never warped.
float2 timewarpUV(float2 uv, float4 q) {
    // Convert uv direction to a ray, rotate by q's yaw+pitch, reproject.
    // Small-angle form: apply yaw (y axis) and pitch (x axis) from q.
    float yaw = atan2(2.0 * (q.w * q.y + q.z * q.x), 1.0 - 2.0 * (q.y * q.y + q.x * q.x));
    float pitch = asin(clamp(2.0 * (q.w * q.x - q.z * q.y), -1.0, 1.0));
    // Warp by the negative of the angular delta; scale relates fov to uv span.
    // Approximate: 90 deg fov per eye half of the screen.
    float fovScale = 0.5 / M_PI_F; // uv shift per radian (90deg half-screen approx)
    float2 shifted = uv + float2(-yaw * fovScale, pitch * fovScale * 0.5);
    // Reject off-texture samples (black border).
    if (shifted.x < 0.0 || shifted.x > 1.0 || shifted.y < 0.0 || shifted.y > 1.0) {
        return float2(-1.0);
    }
    return shifted;
}

struct TimewarpOut {
    float4 position [[position]];
    float2 uv;
};

vertex TimewarpOut timewarpVertex(uint vid [[vertex_id]],
                                  constant Uniforms &u [[buffer(1)]]) {
    float2 p[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };
    float2 uv0[4] = { float2(0.0, 1.0), float2(1.0, 1.0), float2(0.0, 0.0), float2(1.0, 0.0) };
    TimewarpOut o;
    o.position = float4(p[vid], 0.0, 1.0);
    o.uv = timewarpUV(uv0[vid], u.warpQuat);
    return o;
}

fragment float4 videoFragment(TimewarpOut in [[stage_in]],
                              texture2d<float> yTex [[texture(0)]],
                              texture2d<float> cbcrTex [[texture(1)]]) {
    if (in.uv.x < 0.0) return float4(0.0, 0.0, 0.0, 1.0);
    float y = yTex.sample(linearSampler, in.uv).r;
    float2 cbcr = cbcrTex.sample(linearSampler, in.uv).rg;
    return float4(yuv2rgb(y, cbcr), 1.0);
}

// ---------------------------------------------------------------------------
// Composite: SBS stereo + barrel pre-distortion + passthrough backdrop
// ---------------------------------------------------------------------------

struct CompositeOut {
    float4 position [[position]];
    float2 uv;   // (pre-distorted) source uv
    float  eye;
};

vertex CompositeOut distortVertex(const device MeshVertex *mesh [[buffer(0)]],
                                  constant Uniforms &u [[buffer(1)]],
                                  uint vid [[vertex_id]]) {
    MeshVertex m = mesh[vid];
    CompositeOut o;
    // mesh pos is [0,1] x [0,1] (top-left origin); map to NDC.
    float x = m.pos.x * 2.0 - 1.0;
    float y = 1.0 - m.pos.y * 2.0;
    o.position = float4(x, y, 0.0, 1.0);
    // Barrel pre-distortion around the eye center so HMD optics re-straighten
    // the image: uv_d = 0.5 + r*(1 + k1*r2 + k2*r2^2).
    float2 r = m.uv - float2(0.5);
    float r2 = dot(r, r);
    float scale = 1.0 + u.k1 * r2 + u.k2 * r2 * r2;
    float2 uvd = float2(0.5) + r * scale;
    o.uv = uvd;
    o.eye = m.eye;
    return o;
}

fragment float4 compositeFragment(CompositeOut in [[stage_in]],
                                  constant Uniforms &u [[buffer(0)]],
                                  texture2d<float> passY [[texture(0)]],
                                  texture2d<float> passCbCr [[texture(1)]],
                                  texture2d<float> videoY [[texture(2)]],
                                  texture2d<float> videoCbCr [[texture(3)]]) {
    // Per-eye offset: the video frame is a single wide image containing both
    // eyes side by side (board renders SBS per nuos_session_t per-eye size).
    // Left eye samples the left half, right eye the right half; both eyes
    // look at the same pre-distorted uv, offset into the SBS source.
    float2 uv = in.uv;
    float halfW = 0.5;
    float eyeX = (in.eye < 0.5) ? 0.0 : halfW;          // left half or right half
    float2 videoUV = float2(eyeX + uv.x * halfW, uv.y);

    float3 passthroughRGB = float3(0.0);
    if (u.dim > 0.0) {
        float py = passY.sample(linearSampler, uv).r;
        float2 pcb = passCbCr.sample(linearSampler, uv).rg;
        passthroughRGB = yuv2rgb(py, pcb) * u.dim;
    }

    float3 rgb = passthroughRGB;
    if (u.videoOn != 0 && videoY.get_width() > 0) {
        float vy = videoY.sample(linearSampler, videoUV).r;
        float2 vcb = videoCbCr.sample(linearSampler, videoUV).rg;
        float3 videoRGB = yuv2rgb(vy, vcb);
        // Video layer replaces the passthrough where it is non-black;
        // (simple additive-free composite; real alpha keying can come later)
        rgb = videoRGB;
    }
    return float4(rgb, 1.0);
}
