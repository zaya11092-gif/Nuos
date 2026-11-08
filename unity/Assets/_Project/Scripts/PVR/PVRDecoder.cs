using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace CardboardXR.PVR
{
    /// <summary>
    /// HEVC AU decoder bridging to VideoToolbox (iOS native plugin).
    /// Editor/other platforms: renders an animated test pattern instead, so
    /// the FloatingWindow pipeline is verifiable without a device.
    /// </summary>
    public class PVRDecoder : IDisposable
    {
        IntPtr _handle;
        Texture2D _texture;
        int _w, _h;
        bool _paramsSent;

#if UNITY_IOS && !UNITY_EDITOR
        [DllImport("__Internal")] static extern IntPtr pvrdec_create(int w, int h);
        [DllImport("__Internal")] static extern int pvrdec_params(IntPtr h,
            byte[] vps, int vl, byte[] sps, int sl, byte[] pps, int pl);
        [DllImport("__Internal")] static extern int pvrdec_feed(IntPtr h,
            byte[] au, int len);
        [DllImport("__Internal")] static extern IntPtr pvrdec_texture(IntPtr h,
            out int w, out int h2);
        [DllImport("__Internal")] static extern void pvrdec_destroy(IntPtr h);
#else
        static IntPtr pvrdec_create(int w, int h) => (IntPtr)1;
        static int pvrdec_params(IntPtr h, string v, int a, string s, int b,
            string p, int c) => 0;
        static int pvrdec_feed(IntPtr h, byte[] au, int len) => 0;
        static IntPtr pvrdec_texture(IntPtr h, out int w, out int h2)
        { w = 0; h2 = 0; return IntPtr.Zero; }
        static void pvrdec_destroy(IntPtr h) { }
        static bool EditorMode => true;
#endif

        public int DecodedFrames { get; private set; }
        public int DecodeErrors { get; private set; }

        public void Configure(int width, int height, byte[] headerNals)
        {
#if UNITY_IOS && !UNITY_EDITOR
            _handle = pvrdec_create(width, height);
#endif
            // Split HEADER_NALS (Annex-B) into VPS/SPS/PPS by NAL type.
            var nals = SplitByType(headerNals);
            byte[] vps = nals.Find(n => n.type == 32).data ?? Array.Empty<byte>();
            byte[] sps = nals.Find(n => n.type == 33).data ?? Array.Empty<byte>();
            byte[] pps = nals.Find(n => n.type == 34).data ?? Array.Empty<byte>();
            if (vps.Length == 0 || sps.Length == 0 || pps.Length == 0)
            {
                Debug.LogWarning("[PVRDecoder] parameter sets incomplete");
                return;
            }
#if UNITY_IOS && !UNITY_EDITOR
            int st = pvrdec_params(_handle, vps, vps.Length,
                sps, sps.Length, pps, pps.Length);
#else
            int st = 0;
#endif
            Debug.Log($"[PVRDecoder] params sent: {st} (vps {vps.Length}, sps {sps.Length}, pps {pps.Length})");
        }

        static System.Collections.Generic.List<(int type, byte[] data)> SplitByType(
            byte[] annexB)
        {
            var res = new System.Collections.Generic.List<(int, byte[])>();
            int i = 0, start = -1;
            while (i < annexB.Length - 3)
            {
                bool sc3 = annexB[i] == 0 && annexB[i + 1] == 0 && annexB[i + 2] == 1;
                bool sc4 = annexB[i] == 0 && annexB[i + 1] == 0 &&
                           annexB[i + 2] == 0 && annexB[i + 3] == 1;
                if (sc3 || sc4)
                {
                    if (start >= 0)
                    {
                        int type = (annexB[start] >> 1) & 0x3f;
                        res.Add((type, annexB[start..(i - (sc4 ? 0 : 0))]));
                    }
                    start = i + (sc4 ? 4 : 3);
                    i = start;
                }
                else i++;
            }
            if (start >= 0)
            {
                int type = (annexB[start] >> 1) & 0x3f;
                res.Add((type, annexB[start..annexB.Length]));
            }
            return res;
        }

        /// <summary>Feed one access unit; call GetTexture() after to fetch.</summary>
        public bool Feed(byte[] au)
        {
#if UNITY_IOS && !UNITY_EDITOR
            if (_handle == IntPtr.Zero) return false;
            int r = pvrdec_feed(_handle, au, au.Length);
            if (r == 1) { DecodedFrames++; return true; }
            DecodeErrors++;
            return false;
#else
            DecodedFrames++;
            return true;
#endif
        }

        /// <summary>Latest decoded frame as a Unity texture (or test pattern).</summary>
        public Texture2D GetTexture()
        {
#if UNITY_IOS && !UNITY_EDITOR
            int w = 0, h = 0;
            IntPtr texPtr = pvrdec_texture(_handle, out w, out h);
            if (texPtr == IntPtr.Zero) return _texture;
            if (_texture == null || _texture.width != w || _texture.height != h)
            {
                _texture = Texture2D.CreateExternalTexture(
                    w, h, TextureFormat.BGRA32, false, false, texPtr);
                _w = w; _h = h;
            }
            else if (_w != w || _h != h || _texture.GetNativeTexturePtr() != texPtr)
            {
                _texture.UpdateExternalTexture(texPtr);
            }
            return _texture;
#else
            return EditorTestPattern();
#endif
        }

#if !UNITY_IOS || UNITY_EDITOR
        Texture2D EditorTestPattern()
        {
            if (_texture == null)
            {
                _texture = new Texture2D(640, 360, TextureFormat.RGBA32, false);
                _w = 640; _h = 360;
            }
            float t = Time.time;
            for (int y = 0; y < _h; y += 4)
            {
                for (int x = 0; x < _w; x += 4)
                {
                    float v = 0.5f + 0.5f * Mathf.Sin(x * 0.02f + t * 2f)
                                      * Mathf.Cos(y * 0.03f + t);
                    var c = new Color(v * 0.3f, v, 1f - v);
                    _texture.SetPixels(x, y, 4, 4, new[] { c, c, c, c });
                }
            }
            _texture.Apply();
            return _texture;
        }
#endif

        public void Dispose()
        {
#if UNITY_IOS && !UNITY_EDITOR
            if (_handle != IntPtr.Zero) pvrdec_destroy(_handle);
            _handle = IntPtr.Zero;
#endif
        }
    }
}
