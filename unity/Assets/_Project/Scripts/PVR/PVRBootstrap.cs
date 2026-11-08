using System.Text;
using UnityEngine;

namespace CardboardXR.PVR
{
    /// <summary>
    /// Runtime bootstrap: creates the PVRLink + decoder + FloatingWindow stack
    /// inside the CardboardXR Core scene without touching project scenes.
    /// Also feeds the link's pose from the stereo rig's camera each frame.
    /// </summary>
    public class PVRBootstrap : MonoBehaviour
    {
        PVRDecoder _decoder;

        void Start()
        {
            var go = new GameObject("PVRLinkObject");
            go.AddComponent<UnityMainThread>();
            var link = go.AddComponent<PVRLink>();
            DontDestroyOnLoad(go);

            _decoder = new PVRDecoder();
            link.OnHeaderNals += nals =>
            {
                Debug.Log("[PVRBootstrap] header NALs " + nals.Length +
                          " bytes; configuring decoder 1280x720");
                _decoder.Configure(1280, 720, nals);
            };

            var cam = Camera.main;
            if (cam == null) cam = FindFirstObjectByType<Camera>();
            if (cam == null) { Debug.LogError("[PVRBootstrap] no camera"); return; }

            var fwGo = new GameObject("FloatingWindow");
            var fw = fwGo.AddComponent<FloatingWindow>();
            fw.Setup(cam, link, _decoder);
            Debug.Log("[PVRBootstrap] floating window ready above dock");
        }

        void LateUpdate()
        {
            // Feed head orientation to the board (room-space camera quat).
            var cam = Camera.main;
            if (cam != null && PVRLink.Instance != null)
                PVRLink.Instance.HeadQuat = cam.transform.rotation;
        }
    }
}
