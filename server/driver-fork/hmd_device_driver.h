#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "UDPsocket.h"
#include "openvr_driver.h"

struct TOpenTrack {
  double X;
  double Y;
  double Z;
  double Yaw;
  double Pitch;
  double Roll;
};

// NuOS v2: 32-byte hands block appended after TOpenTrack (80 bytes total).
struct TVRHands {
  uint32_t flags;  // bit0 left present, bit1 right present
  float lpinch;
  float lgrip;
  float rpinch;
  float rgrip;
  uint32_t buttons;
};

struct OpenTrackDriverConfiguration {
  int32_t window_x;
  int32_t window_y;

  int32_t window_width;
  int32_t window_height;

  int32_t render_width;
  int32_t render_height;
};

// Shared pose/hands state written by the HMD's UDP loop, read by controllers.
extern std::atomic<double> g_yaw_deg, g_pitch_deg, g_roll_deg;
extern std::atomic<uint32_t> g_hand_flags;
extern std::atomic<float> g_lpinch, g_lgrip, g_rpinch, g_rgrip;

class OpenTrackDisplayComponent : public vr::IVRDisplayComponent {
public:
  OpenTrackDisplayComponent(OpenTrackDriverConfiguration config);

  virtual bool IsDisplayOnDesktop() override;
  virtual bool IsDisplayRealDisplay() override;
  virtual void GetRecommendedRenderTargetSize(uint32_t *pnWidth,
                                              uint32_t *pnHeight) override;
  virtual void GetEyeOutputViewport(vr::EVREye eEye, uint32_t *pnX,
                                    uint32_t *pnY, uint32_t *pnWidth,
                                    uint32_t *pnHeight) override;
  virtual void GetProjectionRaw(vr::EVREye eEye, float *pfLeft, float *pfRight,
                                float *pfTop, float *pfBottom) override;
  virtual vr::DistortionCoordinates_t
  ComputeDistortion(vr::EVREye eEye, float fU, float fV) override;
  virtual void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth,
                               uint32_t *pnHeight) override;

private:
  OpenTrackDriverConfiguration config_;
};

class OpenTrackDeviceDriver : public vr::ITrackedDeviceServerDriver {
public:
  OpenTrackDeviceDriver();

  const std::string &GetSerialNumber();

  void RunFrame();

  virtual vr::EVRInitError Activate(uint32_t unObjectId) override;
  virtual void Deactivate() override;
  virtual void EnterStandby() override;
  virtual void *GetComponent(const char *pchComponentNameAndVersion) override;
  virtual void DebugRequest(const char *pchRequest, char *pchResponseBuffer,
                            uint32_t unResponseBufferSize) override;
  virtual vr::DriverPose_t GetPose() override;

private:
  vr::TrackedDeviceIndex_t device_id_;

  std::unique_ptr<OpenTrackDisplayComponent> display_component_;

  std::string serial_number_;
  std::string model_number_;

  UDPsocket socket_;

  std::thread socket_thread_;

  double Yaw_, Pitch_, Roll_;
  double pX_, pY_, pZ_;

  void SocketReadLoop();
};

// NuOS v2: pinch-driven controller (trigger + squeeze), aimed with the head.
class OpenTrackControllerDriver : public vr::ITrackedDeviceServerDriver {
public:
  OpenTrackControllerDriver(bool right);

  const std::string &GetSerialNumber();
  void RunFrame();

  virtual vr::EVRInitError Activate(uint32_t unObjectId) override;
  virtual void Deactivate() override;
  virtual void EnterStandby() override;
  virtual void *GetComponent(const char *pchComponentNameAndVersion) override;
  virtual void DebugRequest(const char *pchRequest, char *pchResponseBuffer,
                            uint32_t unResponseBufferSize) override;
  virtual vr::DriverPose_t GetPose() override;

private:
  bool right_;
  vr::TrackedDeviceIndex_t device_id_;
  std::string serial_number_;
  vr::VRDriverInput::ScalarComponentHandle_t trigger_ = 0;
  vr::VRDriverInput::ScalarComponentHandle_t squeeze_ = 0;
  vr::VRDriverInput::BooleanComponentHandle_t menu_ = 0;
  bool squeeze_state_ = false;
};
