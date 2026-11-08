#include "hmd_device_driver.h"

static const char *OPENTRACK_MAIN_SETTINGS_SECTION = "driver_opentrack";

// Shared state (HMD socket loop writes, controllers read).
std::atomic<double> g_yaw_deg{0}, g_pitch_deg{0}, g_roll_deg{0};
std::atomic<uint32_t> g_hand_flags{0};
std::atomic<float> g_lpinch{0}, g_lgrip{0}, g_rpinch{0}, g_rgrip{0};

inline vr::HmdQuaternion_t NewHmdQuaternion(double w, double x, double y,
                                            double z) {
  vr::HmdQuaternion_t quat;
  quat.w = w;
  quat.x = x;
  quat.y = y;
  quat.z = z;
  return quat;
}

inline vr::HmdQuaternion_t EulerAngleToQuaternion(double Yaw, double Pitch,
                                                  double Roll) {
  vr::HmdQuaternion_t q;
  double cy = std::cos(Yaw * 0.5);
  double sy = std::sin(Yaw * 0.5);
  double cp = std::cos(Pitch * 0.5);
  double sp = std::sin(Pitch * 0.5);
  double cr = std::cos(Roll * 0.5);
  double sr = std::sin(Roll * 0.5);

  q.w = cr * cp * cy + sr * sp * sy;
  q.x = sr * cp * cy - cr * sp * sy;
  q.y = cr * sp * cy + sr * cp * sy;
  q.z = cr * cp * sy - sr * sp * cy;

  return q;
}

double DegToRad(double f) { return f * (3.14159265358979323846 / 180); }

OpenTrackDisplayComponent::OpenTrackDisplayComponent(
    OpenTrackDriverConfiguration config) {
  config_ = config;
};

bool OpenTrackDisplayComponent::IsDisplayOnDesktop() { return true; }

bool OpenTrackDisplayComponent::IsDisplayRealDisplay() { return false; }

void OpenTrackDisplayComponent::GetRecommendedRenderTargetSize(
    uint32_t *pnWidth, uint32_t *pnHeight) {
  *pnWidth = config_.render_width;
  *pnHeight = config_.render_height;
}

void OpenTrackDisplayComponent::GetEyeOutputViewport(vr::EVREye eEye,
                                                     uint32_t *pnX,
                                                     uint32_t *pnY,
                                                     uint32_t *pnWidth,
                                                     uint32_t *pnHeight) {
  *pnY = 0;
  *pnWidth = config_.window_width / 2;
  *pnHeight = config_.window_height;
  if (eEye == vr::Eye_Left) {
    *pnX = 0;
  } else {
    *pnX = config_.window_width / 2;
  }
}

void OpenTrackDisplayComponent::GetProjectionRaw(vr::EVREye eEye, float *pfLeft,
                                                 float *pfRight, float *pfTop,
                                                 float *pfBottom) {
  *pfLeft = -1.0;
  *pfRight = 1.0;
  *pfTop = -1.0;
  *pfBottom = 1.0;
}

vr::DistortionCoordinates_t
OpenTrackDisplayComponent::ComputeDistortion(vr::EVREye eEye, float fU,
                                             float fV) {
  vr::DistortionCoordinates_t coordinates{};
  coordinates.rfBlue[0] = fU;
  coordinates.rfBlue[1] = fV;
  coordinates.rfGreen[0] = fU;
  coordinates.rfGreen[1] = fV;
  coordinates.rfRed[0] = fU;
  coordinates.rfRed[1] = fV;
  return coordinates;
}

void OpenTrackDisplayComponent::GetWindowBounds(int32_t *pnX, int32_t *pnY,
                                                uint32_t *pnWidth,
                                                uint32_t *pnHeight) {
  *pnX = config_.window_x;
  *pnY = config_.window_y;
  *pnWidth = config_.window_width;
  *pnHeight = config_.window_height;
}

OpenTrackDeviceDriver::OpenTrackDeviceDriver() {
  char serial_number[1024];
  vr::VRSettings()->GetString(OPENTRACK_MAIN_SETTINGS_SECTION, "serial_number",
                              serial_number, sizeof(serial_number));
  serial_number_ = serial_number;

  char model_number[1024];
  vr::VRSettings()->GetString(OPENTRACK_MAIN_SETTINGS_SECTION, "model_number",
                              model_number, sizeof(model_number));
  model_number_ = model_number;

  OpenTrackDriverConfiguration config{};

  config.window_x =
      vr::VRSettings()->GetInt32(OPENTRACK_MAIN_SETTINGS_SECTION, "window_x");
  config.window_y =
      vr::VRSettings()->GetInt32(OPENTRACK_MAIN_SETTINGS_SECTION, "window_y");

  config.window_width = vr::VRSettings()->GetInt32(
      OPENTRACK_MAIN_SETTINGS_SECTION, "window_width");
  config.window_height = vr::VRSettings()->GetInt32(
      OPENTRACK_MAIN_SETTINGS_SECTION, "window_height");

  config.render_width = vr::VRSettings()->GetInt32(
      OPENTRACK_MAIN_SETTINGS_SECTION, "render_width");
  config.render_height = vr::VRSettings()->GetInt32(
      OPENTRACK_MAIN_SETTINGS_SECTION, "render_height");

  display_component_ = std::make_unique<OpenTrackDisplayComponent>(config);
}

const std::string &OpenTrackDeviceDriver::GetSerialNumber() {
  return serial_number_;
}

void OpenTrackDeviceDriver::RunFrame() {
  if (device_id_ != vr::k_unTrackedDeviceIndexInvalid) {
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(
        device_id_, GetPose(), sizeof(vr::DriverPose_t));
  }
}

vr::EVRInitError OpenTrackDeviceDriver::Activate(uint32_t unObjectId) {
  device_id_ = unObjectId;

  vr::PropertyContainerHandle_t container =
      vr::VRProperties()->TrackedDeviceToPropertyContainer(device_id_);

  vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String,
                                        model_number_.c_str());

  const float ipd = vr::VRSettings()->GetFloat(vr::k_pch_SteamVR_Section,
                                               vr::k_pch_SteamVR_IPD_Float);
  vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float,
                                       ipd);

  vr::VRProperties()->SetFloatProperty(
      container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
  vr::VRProperties()->SetFloatProperty(
      container, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.11f);
  vr::VRProperties()->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool,
                                      false);
  vr::VRProperties()->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool,
                                      true);

  if (socket_.open() < 0) {
    vr::VRDriverLog()->Log("failed to open UDP socket");
    return vr::VRInitError_Driver_Failed;
  }
  if (socket_.bind(4242) < 0) {
    vr::VRDriverLog()->Log("failed to bind UDP socket");
    return vr::VRInitError_Driver_Failed;
  }

  socket_thread_ = std::thread(&OpenTrackDeviceDriver::SocketReadLoop, this);

  return vr::VRInitError_None;
}

void OpenTrackDeviceDriver::Deactivate() {
  socket_.close();
  socket_thread_.join();

  device_id_ = vr::k_unTrackedDeviceIndexInvalid;
}

void OpenTrackDeviceDriver::EnterStandby() {
  // Do nothing.
}

void *
OpenTrackDeviceDriver::GetComponent(const char *pchComponentNameAndVersion) {
  if (std::strcmp(pchComponentNameAndVersion,
                  vr::IVRDisplayComponent_Version) == 0) {
    return display_component_.get();
  }

  return nullptr;
}

void OpenTrackDeviceDriver::DebugRequest(const char *pchRequest,
                                         char *pchResponseBuffer,
                                         uint32_t unResponseBufferSize) {
  if (unResponseBufferSize >= 1)
    pchResponseBuffer[0] = 0;
}

vr::DriverPose_t OpenTrackDeviceDriver::GetPose() {
  vr::DriverPose_t pose = {0};

  if (!socket_.is_closed()) {
    pose.poseIsValid = true;
    pose.result = vr::TrackingResult_Running_OK;
    pose.deviceIsConnected = true;
  } else {
    pose.poseIsValid = false;
    pose.result = vr::TrackingResult_Uninitialized;
    pose.deviceIsConnected = false;
  }

  pose.qWorldFromDriverRotation = NewHmdQuaternion(1, 0, 0, 0);
  pose.qDriverFromHeadRotation = NewHmdQuaternion(1, 0, 0, 0);
  pose.shouldApplyHeadModel = true;
  pose.poseTimeOffset = 0;
  pose.willDriftInYaw = false;

  pose.qRotation = EulerAngleToQuaternion(Roll_, -Yaw_, Pitch_);

  pose.vecPosition[0] = pX_ * 0.01;
  pose.vecPosition[1] = pZ_ * 0.01;
  pose.vecPosition[2] = pY_ * 0.01;

  return pose;
}

void OpenTrackDeviceDriver::SocketReadLoop() {
  while (!socket_.is_closed()) {
    UDPsocket::IPv4 addr;
    std::string msg;
    if (socket_.recv(msg, addr) >= 0 &&
        msg.length() >= sizeof(TOpenTrack) + sizeof(TVRHands)) {
      TOpenTrack data;
      std::memcpy(&data, msg.c_str(), sizeof(TOpenTrack));
      TVRHands hands;
      std::memcpy(&hands, msg.c_str() + sizeof(TOpenTrack), sizeof(TVRHands));
      Yaw_ = DegToRad(data.Yaw);
      Pitch_ = DegToRad(data.Pitch);
      Roll_ = DegToRad(data.Roll);
      g_yaw_deg = data.Yaw;
      g_pitch_deg = data.Pitch;
      g_roll_deg = data.Roll;
      pX_ = data.X;
      pY_ = data.Y;
      pZ_ = data.Z;
      g_hand_flags = hands.flags;
      g_lpinch = hands.lpinch;
      g_lgrip = hands.lgrip;
      g_rpinch = hands.rpinch;
      g_rgrip = hands.rgrip;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

// ------------------------------------------------- NuOS v2: controllers
OpenTrackControllerDriver::OpenTrackControllerDriver(bool right)
    : right_(right) {
  serial_number_ = right ? "nuos_ctrl_right" : "nuos_ctrl_left";
}

const std::string &OpenTrackControllerDriver::GetSerialNumber() {
  return serial_number_;
}

void OpenTrackControllerDriver::RunFrame() {
  if (device_id_ == vr::k_unTrackedDeviceIndexInvalid) return;
  vr::VRServerDriverHost()->TrackedDevicePoseUpdated(
      device_id_, GetPose(), sizeof(vr::DriverPose_t));

  float pinch = right_ ? g_rpinch : g_lpinch;
  if (trigger_ != 0)
    vr::VRDriverInput()->UpdateScalarComponent(trigger_, pinch, 0.0);
  bool squeeze_now = pinch > 0.75f;
  if (squeeze_state_ != squeeze_now && squeeze_ != 0) {
    vr::VRDriverInput()->UpdateBooleanComponent(squeeze_, squeeze_now, 0.0);
    squeeze_state_ = squeeze_now;
  }
}

vr::EVRInitError OpenTrackControllerDriver::Activate(uint32_t unObjectId) {
  device_id_ = unObjectId;
  vr::PropertyContainerHandle_t container =
      vr::VRProperties()->TrackedDeviceToPropertyContainer(device_id_);

  vr::VRProperties()->SetStringProperty(
      container, vr::Prop_ModelNumber_String,
      right_ ? "NuOS Right Hand" : "NuOS Left Hand");
  vr::VRProperties()->SetStringProperty(
      container, vr::Prop_ControllerType_String, "vive_controller");
  vr::VRProperties()->SetStringProperty(
      container, vr::Prop_TrackingSystemName_String, "driver_opentrack");
  vr::VRProperties()->SetInt32Property(
      container, vr::Prop_ControllerHandSelectionPriority_Int32, right_ ? 1 : 0);
  vr::VRProperties()->SetBoolProperty(
      container, vr::Prop_NeverTracked_Bool, false);

  vr::EVRInputError e;
  e = vr::VRDriverInput()->CreateScalarComponent(
      container, "/input/trigger/x", &trigger_,
      vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
  if (e != vr::EVRInputError_None) return vr::VRInitError_Driver_Failed;
  e = vr::VRDriverInput()->CreateScalarComponent(
      container, "/input/squeeze/x", &squeeze_,
      vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
  if (e != vr::EVRInputError_None) return vr::VRInitError_Driver_Failed;
  e = vr::VRDriverInput()->CreateBooleanComponent(
      container, "/input/menu/click", &menu_);
  if (e != vr::EVRInputError_None) return vr::VRInitError_Driver_Failed;

  return vr::VRInitError_None;
}

void OpenTrackControllerDriver::Deactivate() {
  device_id_ = vr::k_unTrackedDeviceIndexInvalid;
}

void OpenTrackControllerDriver::EnterStandby() {}

void *OpenTrackControllerDriver::GetComponent(
    const char *pchComponentNameAndVersion) {
  return nullptr;
}

void OpenTrackControllerDriver::DebugRequest(const char *pchRequest,
                                             char *pchResponseBuffer,
                                             uint32_t unResponseBufferSize) {
  if (unResponseBufferSize >= 1) pchResponseBuffer[0] = 0;
}

vr::DriverPose_t OpenTrackControllerDriver::GetPose() {
  vr::DriverPose_t pose = {0};
  pose.poseIsValid = true;
  pose.result = vr::TrackingResult_Running_OK;
  pose.deviceIsConnected = true;
  pose.qWorldFromDriverRotation = NewHmdQuaternion(1, 0, 0, 0);
  pose.qDriverFromHeadRotation = NewHmdQuaternion(1, 0, 0, 0);
  pose.shouldApplyHeadModel = false;

  // Aim with the head: rotation = head, position = head + head_rot * offset.
  double yaw = DegToRad(g_yaw_deg), pitch = DegToRad(g_pitch_deg),
         roll = DegToRad(g_roll_deg);
  pose.qRotation = EulerAngleToQuaternion(roll, pitch, yaw);

  double ox = right_ ? 0.15 : -0.15, oy = -0.12, oz = -0.30;
  double cy = std::cos(yaw), sy = std::sin(yaw);
  double cp = std::cos(pitch), sp = std::sin(pitch);
  // rotate offset by yaw then pitch (approx, enough for hold-in-front)
  double x1 = ox * cy + oz * sy;
  double z1 = -ox * sy + oz * cy;
  double y2 = oy * cp - z1 * sp;
  double z2 = oy * sp + z1 * cp;
  pose.vecPosition[0] = x1;
  pose.vecPosition[1] = y2;
  pose.vecPosition[2] = z2;

  return pose;
}
