#include "device_provider.h"

vr::EVRInitError
OpenTrackDeviceProvider::Init(vr::IVRDriverContext *pDriverContext) {
  VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);

  opentrack_device_ = std::make_unique<OpenTrackDeviceDriver>();
  left_controller_ = std::make_unique<OpenTrackControllerDriver>(false);
  right_controller_ = std::make_unique<OpenTrackControllerDriver>(true);

  if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
          opentrack_device_->GetSerialNumber().c_str(),
          vr::TrackedDeviceClass_HMD, opentrack_device_.get())) {
    vr::VRDriverLog()->Log("failed to create OpenTrack HMD device.");
    return vr::VRInitError_Driver_Unknown;
  }

  if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
          left_controller_->GetSerialNumber().c_str(),
          vr::TrackedDeviceClass_Controller, left_controller_.get())) {
    vr::VRDriverLog()->Log("failed to create left controller.");
  }

  if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
          right_controller_->GetSerialNumber().c_str(),
          vr::TrackedDeviceClass_Controller, right_controller_.get())) {
    vr::VRDriverLog()->Log("failed to create right controller.");
  }

  return vr::VRInitError_None;
}

void OpenTrackDeviceProvider::Cleanup() {
  opentrack_device_ = nullptr;
  left_controller_ = nullptr;
  right_controller_ = nullptr;

  VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

const char *const *OpenTrackDeviceProvider::GetInterfaceVersions() {
  return vr::k_InterfaceVersions;
}

void OpenTrackDeviceProvider::RunFrame() {
  if (opentrack_device_ != nullptr) {
    opentrack_device_->RunFrame();
  }
  if (left_controller_ != nullptr) {
    left_controller_->RunFrame();
  }
  if (right_controller_ != nullptr) {
    right_controller_->RunFrame();
  }

  vr::VREvent_t vrevent{};
  while (vr::VRServerDriverHost()->PollNextEvent(&vrevent,
                                                 sizeof(vr::VREvent_t))) {
    // no events handled in v1
  }
}

bool OpenTrackDeviceProvider::ShouldBlockStandbyMode() {
  return false;
}

void OpenTrackDeviceProvider::EnterStandby() {
  // Do nothing.
}

void OpenTrackDeviceProvider::LeaveStandby() {
  // Do nothing.
}
