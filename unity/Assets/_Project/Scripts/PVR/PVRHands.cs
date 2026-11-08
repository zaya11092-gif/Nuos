using CardboardXR.Hands;
using UnityEngine;
using UnityEngine.XR.Hands;

namespace CardboardXR.PVR
{
    /// <summary>
    /// Feeds hand pinch values from CardboardXR's hand feeds to PVRLink so
    /// they become SteamVR controller inputs (trigger/squeeze) in VR mode.
    /// </summary>
    public class PVRHands : MonoBehaviour
    {
        HandSubsystemFeed[] _feeds;
        float _rescan;

        void Update()
        {
            if (Time.unscaledTime < _rescan) return;
            _rescan = Time.unscaledTime + 2f;
            _feeds = FindObjectsByType<HandSubsystemFeed>(
                FindObjectsSortMode.None);
        }

        void LateUpdate()
        {
            if (_feeds == null) return;
            foreach (var f in _feeds)
            {
                switch (f.TrackedHandedness)
                {
                    case Handedness.Left:
                        PVRLink.LeftPinch = f.PinchValue;
                        PVRLink.LeftPresent = f.PinchValue > 0f;
                        break;
                    case Handedness.Right:
                        PVRLink.RightPinch = f.PinchValue;
                        PVRLink.RightPresent = f.PinchValue > 0f;
                        break;
                }
            }
        }
    }
}
